#include "renderer.h"

#include "models.h"
#include "resources.h"
#include "utility.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <tiny_gltf_v3.h> // Replace with fastgltf eventually?
#include <tracy/Tracy.hpp>
#include <unordered_map>

#define VOLK_IMPLEMENTATION
#include <Volk/volk.h>

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

void Renderer::initialize(SDL_Window* sdlWindow)
{
	ZoneScopedN("Initialize");

	window = sdlWindow;

	scene.initialize(InitialDrawBufferSize);
	nodeRenderStack.reserve(128);
	stagedDrawCommands.reserve(InitialDrawBufferSize);
	stagedRenderItems.reserve(InitialDrawBufferSize);

	if (volkInitialize() != VK_SUCCESS) { throw RenderError("Error initializing Volk."); }

	createVulkanInstance();

	if (!SDL_Vulkan_CreateSurface(window, vulkanInstance, nullptr, &surface))
	{
		throw RenderError("Vulkan surface creation failed!\n\n" + std::string(SDL_GetError()));
	}

	physicalDevice = selectPhysicalDevice();
	selectGraphicsQueue();
	createDevice();
	initializeVMA();
	createSwapchain();
	createShaders();
	createDescriptorSets();
	pipeline = createGraphicsPipeline();
	createSyncResources();
	createCommandBuffers();

	for (auto& resource : frameResources)
	{
		recreateFrameResourceDrawBuffers(resource, InitialDrawBufferSize);
	}

	createFallbackTexture();
}

// Really only set up for a single model currently
void Renderer::loadModel(const Model& model)
{
	// Upload images and samplers
	std::vector<uint32_t> modelImageIDs = uploadImages(model.images);
	std::vector<uint32_t> modelSamplerIDs = uploadSamplers(model.samplers);

	// Remapping base counts
	const uint32_t texturesBase = static_cast<uint32_t>(textures.size());
	const uint32_t materialsBase = static_cast<uint32_t>(materials.size());
	const uint32_t meshesBase = static_cast<uint32_t>(meshes.size());
	const uint32_t nodesBase = static_cast<uint32_t>(scene.size());

	// Remap textures
	for (const ModelTexture& modelTexture : model.textures)
	{
		textures.push_back({
			.imageID = modelImageIDs[modelTexture.image],
			.samplerID =
				modelTexture.sampler == InvalidIndex ? textures[0].samplerID : modelSamplerIDs[modelTexture.sampler],
		});
	}

	// Remap materials
	for (const Material& modelMaterial : model.materials)
	{
		materials.push_back({
			.baseColor = modelMaterial.baseColor,
			.textureID = modelMaterial.textureID == InvalidIndex ? 0 : modelMaterial.textureID + texturesBase + 1,
		});
	}

	// Remap meshes
	for (const Mesh& modelMesh : model.meshes)
	{
		Mesh mesh = modelMesh;
		for (SubMesh& subMesh : mesh.subMeshes)
		{
			subMesh.materialID = subMesh.materialID == InvalidIndex ? 1 : subMesh.materialID + materialsBase + 1;
		}

		meshes.push_back(std::move(mesh));
	}

	// Vertex buffer and index buffer size in bytes
	const size_t vertexBufferBytes = model.vertices.size() * sizeof(Vertex);
	const size_t indexBufferBytes = model.indices.size() * sizeof(uint32_t);

	// Staging buffers
	GPUBuffer vertexStagingBuffer =
		createBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vertexBufferBytes, true, VMA_MEMORY_USAGE_AUTO);
	if (!vertexStagingBuffer.buffer) { throw RenderError("Failed to create vertex staging buffer."); }

	GPUBuffer indexStagingBuffer =
		createBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, indexBufferBytes, true, VMA_MEMORY_USAGE_AUTO);
	if (!indexStagingBuffer.buffer) { throw RenderError("Failed to create index staging buffer."); }

	// Device-local buffers
	GPUBuffer vertexBuffer = createBuffer(
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		vertexBufferBytes,
		false,
		VMA_MEMORY_USAGE_AUTO
	);
	if (!vertexBuffer.buffer) { throw RenderError("Failed to create vertex buffer."); }

	vertexBufferID = addBuffer(vertexBuffer);
	mapCopyBufferData(vertexStagingBuffer, 0, model.vertices.data(), vertexBufferBytes);

	GPUBuffer indexBuffer = createBuffer(
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBufferBytes, false, VMA_MEMORY_USAGE_AUTO
	);
	if (!indexBuffer.buffer) { throw RenderError("Failed to create index buffer."); }

	indexBufferID = addBuffer(indexBuffer);
	mapCopyBufferData(indexStagingBuffer, 0, model.indices.data(), indexBufferBytes);

	// Copy staged data to VRAM
	VkCommandBuffer geometryCommandBuffer = startTransientCommandBuffer();

	VkBufferCopy bufferCopyVertices{.srcOffset = 0, .dstOffset = 0, .size = vertexBufferBytes};
	vkCmdCopyBuffer(geometryCommandBuffer, vertexStagingBuffer.buffer, vertexBuffer.buffer, 1, &bufferCopyVertices);

	VkBufferCopy bufferCopyIndices{.srcOffset = 0, .dstOffset = 0, .size = indexBufferBytes};
	vkCmdCopyBuffer(geometryCommandBuffer, indexStagingBuffer.buffer, indexBuffer.buffer, 1, &bufferCopyIndices);

	submitTransientCommandBuffer(geometryCommandBuffer);

	// Staging cleanup
	vmaDestroyBuffer(vmaAllocator, vertexStagingBuffer.buffer, vertexStagingBuffer.allocation);
	vmaDestroyBuffer(vmaAllocator, indexStagingBuffer.buffer, indexStagingBuffer.allocation);

	// Texture descriptors
	updateTextureDescriptors();

	// Material buffer
	const size_t materialDataBytes = materials.size() * sizeof(Material);
	GPUBuffer materialBuffer = createBuffer(
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		materialDataBytes,
		true,
		VMA_MEMORY_USAGE_AUTO
	);
	if (!materialBuffer.buffer) { throw RenderError("Failed to create material buffer."); }

	materialBufferID = addBuffer(materialBuffer);
	mapCopyBufferData(materialBuffer, 0, materials.data(), materialDataBytes);

	// Remap nodes / add to scene
	for (const ModelNode& modelNode : model.nodes)
	{
		auto [node, nodeID] = scene.createNode();
		node.setTransform(modelNode.transform);
		node.meshID = modelNode.mesh == InvalidIndex ? 0 : modelNode.mesh + meshesBase + 1;
		node.parentID = modelNode.parent == InvalidIndex ? 0 : modelNode.parent + nodesBase + 1;
		node.firstChildID = modelNode.firstChild == InvalidIndex ? 0 : modelNode.firstChild + nodesBase + 1;
		node.nextSiblingID = modelNode.nextSibling == InvalidIndex ? 0 : modelNode.nextSibling + nodesBase + 1;
	}

	for (uint32_t rootIndex : model.rootNodes)
	{
		uint32_t nodeID = rootIndex + nodesBase + 1;

		if (!rootNodeID) { rootNodeID = nodeID; }
		else
		{
			scene.getNode(lastRootNodeID).nextSiblingID = nodeID;
		}

		lastRootNodeID = nodeID;
	}
}

void Renderer::render(const glm::mat4& viewProjectionMatrix)
{
	ZoneScopedN("Render");

	// Check swapchain validity
	if (requireSwapchainRecreation)
	{
		ZoneScopedN("Recreate Swapchain");

		vkDeviceWaitIdle(device);
		destroySwapchain();
		createSwapchain();
		requireSwapchainRecreation = false;
	}

	// Determine frame resource index and timeline semaphore values
	const uint32_t frameResourceIndex = frameIndex % MaxFramesInFlight;
	const uint64_t signalValue = nextSignalValue;
	const uint64_t waitValue = signalValue - MaxFramesInFlight;

	// Ensure it's safe to start recording commands for this frame resource
	{
		ZoneScopedN("Wait Semaphores");

		VkSemaphoreWaitInfo waitInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			.semaphoreCount = 1,
			.pSemaphores = &timelineSemaphore,
			.pValues = &waitValue,
		};
		vkWaitSemaphores(device, &waitInfo, UINT64_MAX);
	}

	// Reset the command pool for this frame resource
	FrameResources& frameResource = frameResources[frameResourceIndex];
	vkResetCommandPool(device, frameResource.commandPool, 0);

	// Acquire next swapchain image
	VkSemaphore imageAcquiredSemaphore = frameResource.imageAcquiredSemaphore;
	uint32_t swapchainImageIndex;
	VkResult acquireResult;
	{
		ZoneScopedN("Acquire Swapchain Image");

		acquireResult = vkAcquireNextImageKHR(
			device, swapchain, UINT64_MAX, imageAcquiredSemaphore, VK_NULL_HANDLE, &swapchainImageIndex
		);
	}

	// Handle swapchain recreation if needed
	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR)
	{
		recreateImageAcquiredSemaphore(frameResource);
		requireSwapchainRecreation = true;
		return;
	}
	else if (acquireResult != VK_SUCCESS) { throw RenderError("Failed to acquire next swapchain image."); }

	// Image acquired, increment frame index and timeline signal value
	++frameIndex;
	++nextSignalValue;

	// Traverse scene and record MDI draw commands
	{
		ZoneScopedN("Traverse Scene");

		nodeRenderStack.clear();
		stagedDrawCommands.clear();
		stagedRenderItems.clear();

		uint32_t nodeID = rootNodeID;
		while (nodeID)
		{
			Node& node = scene.getNode(nodeID);
			nodeRenderStack.push_back({&node, glm::mat4(1.0f)});
			nodeID = node.nextSiblingID;
		}

		while (!nodeRenderStack.empty())
		{
			auto [node, parentTransform] = nodeRenderStack.back();
			nodeRenderStack.pop_back();
			glm::mat4 worldMatrix = parentTransform * node->getTransform();

			// Draw the nodes mesh! (if it has one)
			if (node->meshID)
			{
				Mesh& mesh = meshes[node->meshID - 1];

				for (SubMesh& subMesh : mesh.subMeshes)
				{
					// Indirect draw command
					stagedDrawCommands.push_back(
						VkDrawIndexedIndirectCommand{
							.indexCount = static_cast<uint32_t>(subMesh.indexCount),
							.instanceCount = 1,
							.firstIndex = static_cast<uint32_t>(subMesh.indexStart),
							.vertexOffset = static_cast<int32_t>(subMesh.vertexStart),
							.firstInstance = static_cast<uint32_t>(stagedDrawCommands.size()),
						}
					);

					// Per render-item data
					stagedRenderItems.push_back(
						RenderItem{
							.worldMatrix = worldMatrix,
							.normalMatrix = glm::transpose(glm::inverse(glm::mat3(worldMatrix))),
							.materialIndex = subMesh.materialID - 1,
						}
					);
				}
			}

			// Push children to stack for processing
			uint32_t childNodeID = node->firstChildID;
			while (childNodeID)
			{
				Node& child = scene.getNode(childNodeID);
				nodeRenderStack.push_back({&child, worldMatrix});
				childNodeID = child.nextSiblingID;
			}
		}
	}

	// Grow the draw buffers for this frame resource (if the scene outgrew them)
	const size_t drawCount = stagedDrawCommands.size();
	if (drawCount > frameResource.drawCapacity)
	{
		ZoneScopedN("Recreate Draw Buffers");
		recreateFrameResourceDrawBuffers(
			frameResource, std::max(drawCount + InitialDrawBufferSize, frameResource.drawCapacity * 2)
		);
	}

	// Upload staged draws into the buffers for this frame resource
	{
		ZoneScopedN("Upload Staged Draws");

		std::memcpy(
			frameResource.indirectDrawPointer, stagedDrawCommands.data(), drawCount * sizeof(VkDrawIndexedIndirectCommand)
		);
		std::memcpy(frameResource.renderItemPointer, stagedRenderItems.data(), drawCount * sizeof(RenderItem));
	}

	// Record commands for this frame resource
	{
		ZoneScopedN("Record Commands");

		VkCommandBufferBeginInfo commandBufferBeginInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vkBeginCommandBuffer(frameResource.commandBuffer, &commandBufferBeginInfo);

		// Transition the color and depth images
		std::vector<VkImageMemoryBarrier2> imageBarriers{
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				.srcAccessMask = 0,
				.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.image = swapchainImages[swapchainImageIndex],
				.subresourceRange{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1
				},
			},
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
				.srcAccessMask = 0,
				.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
				.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
				.image = depthImage,
				.subresourceRange{
					.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1
				},
			}
		};

		VkDependencyInfo dependencyInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size()),
			.pImageMemoryBarriers = imageBarriers.data(),
		};
		vkCmdPipelineBarrier2(frameResource.commandBuffer, &dependencyInfo);

		// Setup attachment info
		VkRenderingAttachmentInfo colorAttachmentInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = swapchainImageViews[swapchainImageIndex],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,	 // clear the image to start
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE, // keep image for presentation
			.clearValue{.color{{0.01f, 0.01f, 0.01f, 1.0f}}},
		};
		VkRenderingAttachmentInfo depthAttachmentInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = depthImageView,
			.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,		 // clear the depth buffer to start
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, // don't care after rendering
			.clearValue{.depthStencil{1.0f, 0}},
		};

		// Setup rendering info
		VkRenderingInfo renderingInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea{.offset{0, 0}, .extent{swapchainWidth, swapchainHeight}},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &colorAttachmentInfo,
			.pDepthAttachment = &depthAttachmentInfo,
		};

		// Setup frame data
		vkCmdBindDescriptorSets(
			frameResource.commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipelineLayout,
			0,
			1,
			&globalDescriptorSet,
			0,
			nullptr
		);

		// Frame constants
		FrameConstants frameConstants;
		GPUBuffer& vertexBuffer = buffers[vertexBufferID - 1];
		GPUBuffer& materialBuffer = buffers[materialBufferID - 1];
		frameConstants.vertexBufferAddress = vertexBuffer.deviceAddress;
		frameConstants.materialBufferAddress = materialBuffer.deviceAddress;
		frameConstants.renderItemsBufferAddress = frameResource.renderItemBuffer.deviceAddress;
		frameConstants.viewProjection = viewProjectionMatrix;

		// Written immediately to cmd buffer
		vkCmdPushConstants(
			frameResource.commandBuffer,
			pipelineLayout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			0,
			sizeof(FrameConstants),
			&frameConstants
		);

		// Bind index buffer
		GPUBuffer& indexBuffer = buffers[indexBufferID - 1];
		vkCmdBindIndexBuffer(frameResource.commandBuffer, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

		// Begin dynamic rendering
		vkCmdBeginRendering(frameResource.commandBuffer, &renderingInfo);
		{
			// Set the viewport dynamically
			VkViewport viewport{
				.x = 0,
				.y = static_cast<float>(swapchainHeight),
				.width = static_cast<float>(swapchainWidth),
				.height = -static_cast<float>(swapchainHeight),
				.minDepth = 0,
				.maxDepth = 1,
			};
			vkCmdSetViewport(frameResource.commandBuffer, 0, 1, &viewport);

			// Set the scissor dynamically
			VkRect2D scissor{
				.offset{0, 0},
				.extent{swapchainWidth, swapchainHeight},
			};
			vkCmdSetScissor(frameResource.commandBuffer, 0, 1, &scissor);

			// Bind the graphics pipeline
			vkCmdBindPipeline(frameResource.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

			// Draw everything!
			vkCmdDrawIndexedIndirect(
				frameResource.commandBuffer,
				frameResource.indirectDrawBuffer.buffer,
				0,
				static_cast<uint32_t>(drawCount),
				sizeof(VkDrawIndexedIndirectCommand)
			);
		}
		// End dynamic rendering
		vkCmdEndRendering(frameResource.commandBuffer);

		// Transition the color attachment to presentation layout
		VkImageMemoryBarrier2 presentationBarrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
			.dstAccessMask = 0,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.image = swapchainImages[swapchainImageIndex],
			.subresourceRange{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
		};
		VkDependencyInfo presentationDependencyInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &presentationBarrier,
		};
		vkCmdPipelineBarrier2(frameResource.commandBuffer, &presentationDependencyInfo);

		// Finish recording commands
		if (vkEndCommandBuffer(frameResource.commandBuffer) != VK_SUCCESS)
		{
			throw RenderError("Failed to record command buffer.");
		}
	}

	// Submit the graphics queue
	{
		ZoneScopedN("Submit Queue");

		// Ensure swapchain image is ready for rendering by waiting on the image-acquired semaphore
		VkSemaphoreSubmitInfo imageAcquireWaitInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = imageAcquiredSemaphore,
			.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		};

		// Signal that the image is ready for presentation
		std::vector<VkSemaphoreSubmitInfo> semaphoreSignalInfos{
			// Signal the render-complete binary semaphore for this swapchain image
			{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = renderCompleteSemaphores[swapchainImageIndex],
				.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
			},
			// Signal the timeline semaphore
			{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = timelineSemaphore,
				.value = signalValue,
				.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			}
		};

		// Submit the command buffer to the graphics queue
		VkCommandBufferSubmitInfo commandBufferSubmitInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = frameResource.commandBuffer,
		};
		VkSubmitInfo2 submitInfo{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = 1,
			.pWaitSemaphoreInfos = &imageAcquireWaitInfo,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &commandBufferSubmitInfo,
			.signalSemaphoreInfoCount = static_cast<uint32_t>(semaphoreSignalInfos.size()),
			.pSignalSemaphoreInfos = semaphoreSignalInfos.data(),
		};
		if (vkQueueSubmit2(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
		{
			throw RenderError("Failed to submit command buffer.");
		}
	}

	// Present the swapchain image!
	{
		ZoneScopedN("Present");

		VkPresentInfoKHR presentInfo{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &renderCompleteSemaphores[swapchainImageIndex],
			.swapchainCount = 1,
			.pSwapchains = &swapchain,
			.pImageIndices = &swapchainImageIndex,
			.pResults = nullptr,
		};
		const VkResult presentResult = vkQueuePresentKHR(graphicsQueue, &presentInfo);

		// Handle swapchain recreation if needed
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
		{
			requireSwapchainRecreation = true;
		}
		else if (presentResult != VK_SUCCESS) { throw RenderError("Failed to present swapchain image."); }
	}
}

void Renderer::shutdown()
{
	// Flush GPU first (if device exists)
	if (device) { vkDeviceWaitIdle(device); }

	// Descriptor layouts and pool
	if (globalDescriptorSetLayout) { vkDestroyDescriptorSetLayout(device, globalDescriptorSetLayout, nullptr); }
	if (descriptorPool) { vkDestroyDescriptorPool(device, descriptorPool, nullptr); }

	for (auto& image : images)
	{
		vkDestroyImageView(device, image.imageView, nullptr);
		vkDestroyImage(device, image.image, nullptr);
		vmaFreeMemory(vmaAllocator, image.allocation);
	}
	images.clear();

	for (VkSampler sampler : samplers)
	{
		vkDestroySampler(device, sampler, nullptr);
	}
	samplers.clear();

	for (auto& buffer : buffers)
	{
		vkDestroyBuffer(device, buffer.buffer, nullptr);
		vmaFreeMemory(vmaAllocator, buffer.allocation);
	}
	buffers.clear();

	// Frame/sync resources
	if (timelineSemaphore)
	{
		vkDestroySemaphore(device, timelineSemaphore, nullptr);
		timelineSemaphore = VK_NULL_HANDLE;
	}

	if (transientCommandPool)
	{
		vkDestroyCommandPool(device, transientCommandPool, nullptr);
		transientCommandPool = VK_NULL_HANDLE;
	}

	for (FrameResources& frameResource : frameResources)
	{
		if (frameResource.imageAcquiredSemaphore)
		{
			vkDestroySemaphore(device, frameResource.imageAcquiredSemaphore, nullptr);
			frameResource.imageAcquiredSemaphore = VK_NULL_HANDLE;
		}
		// Destroying command pool implicity frees command buffers allocated from it
		if (frameResource.commandPool)
		{
			vkDestroyCommandPool(device, frameResource.commandPool, nullptr);
			frameResource.commandPool = VK_NULL_HANDLE;
			frameResource.commandBuffer = VK_NULL_HANDLE;
		}

		if (frameResource.indirectDrawBuffer.buffer)
		{
			vmaUnmapMemory(vmaAllocator, frameResource.indirectDrawBuffer.allocation);
			vkDestroyBuffer(device, frameResource.indirectDrawBuffer.buffer, nullptr);
			vmaFreeMemory(vmaAllocator, frameResource.indirectDrawBuffer.allocation);
		}

		if (frameResource.renderItemBuffer.buffer)
		{
			vmaUnmapMemory(vmaAllocator, frameResource.renderItemBuffer.allocation);
			vkDestroyBuffer(device, frameResource.renderItemBuffer.buffer, nullptr);
			vmaFreeMemory(vmaAllocator, frameResource.renderItemBuffer.allocation);
		}
	}

	// Pipeline
	if (pipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		pipelineLayout = VK_NULL_HANDLE;
	}
	if (pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
	}

	// Shaders
	if (vertShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(device, vertShader, nullptr);
		vertShader = VK_NULL_HANDLE;
	}
	if (fragShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(device, fragShader, nullptr);
		fragShader = VK_NULL_HANDLE;
	}

	destroySwapchain();

	if (vmaAllocator)
	{
		vmaDestroyAllocator(vmaAllocator);
		vmaAllocator = nullptr;
	}

	if (surface)
	{
		vkDestroySurfaceKHR(vulkanInstance, surface, nullptr);
		surface = VK_NULL_HANDLE;
	}

	if (device)
	{
		vkDestroyDevice(device, nullptr);
		device = VK_NULL_HANDLE;
	}

	if (debugMessenger)
	{
		vkDestroyDebugUtilsMessengerEXT(vulkanInstance, debugMessenger, nullptr);
		debugMessenger = VK_NULL_HANDLE;
	}

	if (vulkanInstance)
	{
		vkDestroyInstance(vulkanInstance, nullptr);
		vulkanInstance = VK_NULL_HANDLE;
	}
	volkFinalize();
}

void Renderer::invalidateSwapchain()
{ requireSwapchainRecreation = true; }

// Debug callback for Vulkan validation layers
VKAPI_ATTR VkBool32 VKAPI_CALL Renderer::debugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void*
)
{
	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		std::cerr << "Vulkan validation layer: " << pCallbackData->pMessage << std::endl;
	}

	return VK_FALSE;
}

void Renderer::createVulkanInstance()
{
	ZoneScopedN("Create VK Instance");

	VkApplicationInfo applicationInfo{
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pNext = nullptr,
		.pApplicationName = "Vulkan Renderer",
		.apiVersion = VulkanAPIVersion,
	};

	// Extensions
	uint32_t instanceExtensionCount = 0;
	const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&instanceExtensionCount);
	std::vector<const char*> requestedExtensions{
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	};
	for (uint32_t i = 0; i < instanceExtensionCount; ++i)
	{
		requestedExtensions.push_back(extensions[i]);
	}

	// Layers
	std::vector<const char*> requestedLayers{
		"VK_LAYER_KHRONOS_validation",
	};

	VkDebugUtilsMessengerCreateInfoEXT debugInfo{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
						   | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
		.pfnUserCallback = debugCallback
	};

	VkInstanceCreateInfo instanceCreateInfo{
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = &debugInfo,
		.pApplicationInfo = &applicationInfo,
		.enabledLayerCount = static_cast<uint32_t>(requestedLayers.size()),
		.ppEnabledLayerNames = requestedLayers.data(),
		.enabledExtensionCount = static_cast<uint32_t>(requestedExtensions.size()),
		.ppEnabledExtensionNames = requestedExtensions.data(),
	};

	if (vkCreateInstance(&instanceCreateInfo, nullptr, &vulkanInstance) != VK_SUCCESS)
	{
		throw RenderError("Failed to create Vulkan instance.");
	}

	volkLoadInstance(vulkanInstance);

	// Create debug messenger
	if (vkCreateDebugUtilsMessengerEXT(vulkanInstance, &debugInfo, nullptr, &debugMessenger) != VK_SUCCESS)
	{
		throw RenderError("Failed to create debug messenger.");
	}
}

// Defaults to first device, but will try to find a discrete GPU if available.
// - Replace with a more sophisticated selection algorithm at some point!
VkPhysicalDevice Renderer::selectPhysicalDevice()
{
	uint32_t physicalDeviceCount = 0;
	vkEnumeratePhysicalDevices(vulkanInstance, &physicalDeviceCount, nullptr);

	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	vkEnumeratePhysicalDevices(vulkanInstance, &physicalDeviceCount, physicalDevices.data());

	if (physicalDeviceCount == 0) { throw RenderError("No physical devices found."); }

	// Default to first device
	VkPhysicalDevice selectedDevice = physicalDevices[0];

	// Try to find a discrete GPU
	for (const auto& currentDevice : physicalDevices)
	{
		VkPhysicalDeviceProperties deviceProperties{};
		vkGetPhysicalDeviceProperties(currentDevice, &deviceProperties);

		if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			selectedDevice = currentDevice;
			break;
		}
	}

	// Print selected device name
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(selectedDevice, &props);
	std::cout << "Selected physical device: " << props.deviceName << std::endl;

	// Ensure the requested swapchain format is supported
	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(selectedDevice, surface, &formatCount, nullptr);

	std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(selectedDevice, surface, &formatCount, surfaceFormats.data());

	bool formatSupported = std::ranges::contains(surfaceFormats, swapchainFormat, &VkSurfaceFormatKHR::format);
	if (!formatSupported) { throw RenderError("Requested swapchain format not supported by the selected device."); }

	return selectedDevice;
}

void Renderer::selectGraphicsQueue()
{
	// Get queue family count
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &queueFamilyCount, nullptr);

	// Get queue family properties
	std::vector<VkQueueFamilyProperties2> queueFamilies(queueFamilyCount, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
	vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &queueFamilyCount, queueFamilies.data());

	// Select one that supports both graphics and presentation
	for (uint32_t i = 0; i < queueFamilyCount; ++i)
	{
		VkBool32 presentSupport = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupport);

		const VkQueueFamilyProperties2& queueFamily = queueFamilies[i];
		if (presentSupport == VK_TRUE && queueFamily.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			graphicsQueueFamilyIndex = i;
			return;
		}
	}

	throw RenderError("No suitable graphics queue found.");
}

void Renderer::createDevice()
{
	ZoneScopedN("Create Device");

	// Get supported features
	VkPhysicalDeviceVulkan14Features supportedFeatures14{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = nullptr
	};
	VkPhysicalDeviceVulkan13Features supportedFeatures13{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &supportedFeatures14
	};
	VkPhysicalDeviceVulkan12Features supportedFeatures12{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supportedFeatures13
	};
	VkPhysicalDeviceFeatures2 supportedFeatures{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supportedFeatures12
	};
	vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures);

	// Check for required features
	if (!supportedFeatures13.dynamicRendering || !supportedFeatures13.synchronization2
		|| !supportedFeatures12.timelineSemaphore || !supportedFeatures12.bufferDeviceAddress
		|| !supportedFeatures12.scalarBlockLayout || !supportedFeatures12.descriptorIndexing
		|| !supportedFeatures12.descriptorBindingSampledImageUpdateAfterBind
		|| !supportedFeatures12.descriptorBindingPartiallyBound || !supportedFeatures12.runtimeDescriptorArray
		|| !supportedFeatures12.shaderSampledImageArrayNonUniformIndexing || !supportedFeatures.features.shaderInt64
		|| !supportedFeatures.features.multiDrawIndirect || !supportedFeatures.features.drawIndirectFirstInstance)
	{
		throw RenderError("Physical device does not support required features.");
	}

	// Enable required features
	VkPhysicalDeviceVulkan14Features enabledFeatures14{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
		.pNext = nullptr,
	};
	VkPhysicalDeviceVulkan13Features enabledFeatures13{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = &enabledFeatures14,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE,
	};
	VkPhysicalDeviceVulkan12Features enabledFeatures12{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &enabledFeatures13,
		.descriptorIndexing = VK_TRUE,
		.shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
		.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
		.descriptorBindingPartiallyBound = VK_TRUE,
		.runtimeDescriptorArray = VK_TRUE,
		.scalarBlockLayout = VK_TRUE,
		.timelineSemaphore = VK_TRUE,
		.bufferDeviceAddress = VK_TRUE,
	};
	VkPhysicalDeviceFeatures2 enabledFeatures{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &enabledFeatures12,
		.features{
			.multiDrawIndirect = VK_TRUE,
			.drawIndirectFirstInstance = VK_TRUE,
			.shaderInt64 = VK_TRUE,
		}
	};

	// Device extensions
	const std::vector<const char*> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

	// Request queues
	std::vector<float> queuePriorities{1.0f};
	VkDeviceQueueCreateInfo graphicsQueueInfo{
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = graphicsQueueFamilyIndex,
		.queueCount = 1,
		.pQueuePriorities = queuePriorities.data(),
	};

	// Create logical device
	VkDeviceCreateInfo deviceCreateInfo{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &enabledFeatures,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &graphicsQueueInfo,
		.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
		.ppEnabledExtensionNames = deviceExtensions.data(),
		.pEnabledFeatures = nullptr,
	};

	if (vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS)
	{
		throw RenderError("Failed to create logical device.");
	}

	// Get the graphics queue
	vkGetDeviceQueue(device, graphicsQueueFamilyIndex, 0, &graphicsQueue);
	if (!graphicsQueue) { throw RenderError("Failed to get graphics queue."); }
}

void Renderer::initializeVMA()
{
	ZoneScopedN("Initialize VMA");

	VmaVulkanFunctions vmaFunctionInfo{};
	VmaAllocatorCreateInfo vmaAllocatorInfo{
		.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
		.physicalDevice = physicalDevice,
		.device = device,
		.pVulkanFunctions = &vmaFunctionInfo,
		.instance = vulkanInstance,
		.vulkanApiVersion = VulkanAPIVersion
	};

	vmaImportVulkanFunctionsFromVolk(&vmaAllocatorInfo, &vmaFunctionInfo);

	if (vmaCreateAllocator(&vmaAllocatorInfo, &vmaAllocator) != VK_SUCCESS)
	{
		throw RenderError("Failed to create VMA allocator.");
	}
}

void Renderer::createSwapchain()
{
	ZoneScopedN("Create Swapchain");

	int width, height;
	if (!SDL_GetWindowSizeInPixels(window, &width, &height)) { throw RenderError("Error getting window size."); }

	// Get surface capabilities
	VkSurfaceCapabilitiesKHR surfaceCapabilities{};
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &surfaceCapabilities) != VK_SUCCESS)
	{
		throw RenderError("Failed to get surface capabilities.");
	}

	// Clamp swapchain extent to surface capabilities
	swapchainWidth = std::clamp(
		static_cast<uint32_t>(width), surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width
	);
	swapchainHeight = std::clamp(
		static_cast<uint32_t>(height), surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height
	);

	// Should never actually happen, but just in case
	if (swapchainWidth == 0 || swapchainHeight == 0) { throw RenderError("Invalid swapchain dimensions."); }

	// Determine the number of images in the swapchain
	uint32_t requestedImageCount = std::max(2u, surfaceCapabilities.minImageCount);
	if (surfaceCapabilities.maxImageCount > 0)
	{
		requestedImageCount = std::min(requestedImageCount, surfaceCapabilities.maxImageCount);
	}

	// Create swapchain
	VkSwapchainCreateInfoKHR swapchainCreateInfo{
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = surface,
		.minImageCount = requestedImageCount,
		.imageFormat = swapchainFormat,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.imageExtent{.width = swapchainWidth, .height = swapchainHeight},
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = surfaceCapabilities.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR
	};

	if (vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain) != VK_SUCCESS)
	{
		throw RenderError("Failed to create swapchain.");
	}

	// Get the swapchain images
	uint32_t swapchainImageCount = 0;
	vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, nullptr);
	swapchainImages.resize(swapchainImageCount);
	vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages.data());
	swapchainImageViews.resize(swapchainImageCount);

	// Create image view into each swapchain image
	for (size_t i = 0; i < swapchainImages.size(); ++i)
	{
		VkImageViewCreateInfo imageViewInfo{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = swapchainImages[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = swapchainFormat,
			.subresourceRange{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};

		if (vkCreateImageView(device, &imageViewInfo, nullptr, &swapchainImageViews[i]) != VK_SUCCESS)
		{
			throw RenderError(std::format("Failed to create image view for swapchain image {}.", i));
		}
	}

	// Create semaphore for each swapchain image
	renderCompleteSemaphores.resize(swapchainImages.size());
	for (size_t i = 0; i < renderCompleteSemaphores.size(); ++i)
	{
		VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

		if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderCompleteSemaphores[i]) != VK_SUCCESS)
		{
			throw RenderError(std::format("Failed to create render-complete semaphore for swapchain image {}.", i));
		}
	}

	// Create swapchain depth image
	VkImageCreateInfo depthImageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = depthFormat,
		.extent{.width = swapchainWidth, .height = swapchainHeight, .depth = 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};

	VmaAllocationCreateInfo depthAllocationInfo{
		.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	if (vmaCreateImage(vmaAllocator, &depthImageInfo, &depthAllocationInfo, &depthImage, &depthImageAllocation, nullptr)
		!= VK_SUCCESS)
	{
		throw RenderError("Failed to create depth image.");
	}

	// Create image view for depth image
	VkImageViewCreateInfo depthImageViewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = depthImage,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = depthFormat,
		.subresourceRange{
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1
		}
	};

	if (vkCreateImageView(device, &depthImageViewInfo, nullptr, &depthImageView) != VK_SUCCESS)
	{
		throw RenderError("Failed to create image view for depth image.");
	}
}

void Renderer::destroySwapchain()
{
	ZoneScopedN("Destroy Swapchain");

	for (VkImageView imageView : swapchainImageViews)
	{
		if (imageView != VK_NULL_HANDLE) { vkDestroyImageView(device, imageView, nullptr); }
	}
	swapchainImageViews.clear();

	for (VkSemaphore& semaphore : renderCompleteSemaphores)
	{
		if (semaphore != VK_NULL_HANDLE) { vkDestroySemaphore(device, semaphore, nullptr); }
	}
	renderCompleteSemaphores.clear();
	swapchainImages.clear();

	if (swapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(device, swapchain, nullptr);
		swapchain = VK_NULL_HANDLE;
	}

	if (depthImageView != VK_NULL_HANDLE)
	{
		vkDestroyImageView(device, depthImageView, nullptr);
		depthImageView = VK_NULL_HANDLE;
	}
	if (depthImage != VK_NULL_HANDLE)
	{
		vmaDestroyImage(vmaAllocator, depthImage, depthImageAllocation);
		depthImage = VK_NULL_HANDLE;
	}
}

// Create a shader module from a GLSL shader file using shaderc
VkShaderModule Renderer::createShaderModule(const std::string& filename, shaderc_shader_kind kind) const
{
	ZoneScopedN("Create Shader Module");

	// Read shader source from file
	const std::filesystem::path shaderPath = std::filesystem::path{SHADER_DIR} / filename;
	std::string shaderSource = readTextFile(shaderPath);

	if (shaderSource.empty()) { throw RenderError("Failed to read shader source for \"" + filename + "\"."); }

	// Compile shader source to SPIR-V using shaderc
	std::cout << "Compiling shader: " << shaderPath << std::endl;

	shaderc::Compiler compiler;
	shaderc::CompileOptions options;

	options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_4);
	options.SetTargetSpirv(shaderc_spirv_version_1_6);
	options.SetOptimizationLevel(shaderc_optimization_level_performance);

	shaderc::CompilationResult result = compiler.CompileGlslToSpv(shaderSource, kind, filename.c_str(), options);

	if (result.GetCompilationStatus() != shaderc_compilation_status_success)
	{
		std::cerr << "Shader compilation failed for \"" << filename << "\": " << result.GetErrorMessage() << std::endl;
		throw RenderError("Failed to compile shader: " + filename + ".\n\n" + result.GetErrorMessage());
	}

	// Create shader module from SPIR-V
	const size_t spirvSize = (result.cend() - result.cbegin()) * sizeof(uint32_t);

	VkShaderModuleCreateInfo shaderModuleInfo{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = spirvSize,
		.pCode = result.cbegin(),
	};

	VkShaderModule shaderModule = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &shaderModuleInfo, nullptr, &shaderModule) != VK_SUCCESS)
	{
		throw RenderError("Failed to create shader module for \"" + filename + "\".");
	}

	return shaderModule;
}

void Renderer::createShaders()
{
	ZoneScopedN("Create Shaders");

	// Vertex shader
	vertShader = createShaderModule("shader.vert", shaderc_vertex_shader);

	// Fragment shader
	fragShader = createShaderModule("shader.frag", shaderc_fragment_shader);
}

VkPipeline Renderer::createGraphicsPipeline()
{
	ZoneScopedN("Create Graphics Pipeline");

	// Push constants
	VkPushConstantRange pushConstantRange{
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(FrameConstants),
	};

	std::array<VkDescriptorSetLayout, 1> descriptorSetLayouts{globalDescriptorSetLayout};

	// Create pipeline layout
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = static_cast<uint32_t>(descriptorSetLayouts.size()),
		.pSetLayouts = descriptorSetLayouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pushConstantRange,
	};

	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
	{
		throw RenderError("Failed to create pipeline layout.");
	}

	// Shader stages
	const char* entryPoint = "main";
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages{
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertShader,
			.pName = entryPoint,
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragShader,
			.pName = entryPoint,
		}
	};

	// Vertex input state (vertex pulling)
	VkPipelineVertexInputStateCreateInfo vertexInputInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	// Input assembly state
	VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	// Depth stencil state
	VkPipelineDepthStencilStateCreateInfo depthStencilInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.stencilTestEnable = VK_FALSE,
	};

	// Viewport & scissor state (dynamic)
	VkPipelineViewportStateCreateInfo viewportInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = nullptr,
		.scissorCount = 1,
		.pScissors = nullptr,
	};

	// Rasterization state
	VkPipelineRasterizationStateCreateInfo rasterizationInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};

	// Multisample state (none)
	VkPipelineMultisampleStateCreateInfo multisampleInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	// Color blend state (alpha-blending disabled)
	VkPipelineColorBlendAttachmentState colorBlendAttachment{
		.blendEnable = VK_FALSE,
		.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo colorBlendInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &colorBlendAttachment,
	};

	// Dynamic state (viewport & scissor)
	std::vector<VkDynamicState> dynamicStates{
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamicStateInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
		.pDynamicStates = dynamicStates.data(),
	};

	// Dynamic rendering info
	VkPipelineRenderingCreateInfo renderingInfo{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &swapchainFormat,
		.depthAttachmentFormat = depthFormat,
	};

	// Finally, create the graphics pipeline
	VkGraphicsPipelineCreateInfo pipelineInfo{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &renderingInfo,
		.stageCount = static_cast<uint32_t>(shaderStages.size()),
		.pStages = shaderStages.data(),
		.pVertexInputState = &vertexInputInfo,
		.pInputAssemblyState = &inputAssemblyInfo,
		.pViewportState = &viewportInfo,
		.pRasterizationState = &rasterizationInfo,
		.pMultisampleState = &multisampleInfo,
		.pDepthStencilState = &depthStencilInfo,
		.pColorBlendState = &colorBlendInfo,
		.pDynamicState = &dynamicStateInfo,
		.layout = pipelineLayout,
		.renderPass = VK_NULL_HANDLE,
	};

	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
	{
		throw RenderError("Failed to create graphics pipeline.");
	}

	return pipeline;
}

void Renderer::createSyncResources()
{
	ZoneScopedN("Create Sync Resources");

	// Create timeline semaphore
	VkSemaphoreTypeCreateInfo timelineSemaphoreInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		.initialValue = MaxFramesInFlight,
	};
	VkSemaphoreCreateInfo semaphoreInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &timelineSemaphoreInfo,
	};
	if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &timelineSemaphore) != VK_SUCCESS)
	{
		throw RenderError("Failed to create timeline semaphore.");
	}

	// Per-frame binary semaphore for image acquisition
	for (FrameResources& frameResource : frameResources)
	{
		VkSemaphoreCreateInfo imageAcquiredSemaphoreInfo{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		if (vkCreateSemaphore(device, &imageAcquiredSemaphoreInfo, nullptr, &frameResource.imageAcquiredSemaphore)
			!= VK_SUCCESS)
		{
			throw RenderError("Failed to create per-frame image-acquired semaphore.");
		}
	}
}

void Renderer::recreateImageAcquiredSemaphore(FrameResources& frameResource)
{
	ZoneScopedN("Recreate Image Acquired Semaphore");

	// Destroy existing semaphore
	vkDestroySemaphore(device, frameResource.imageAcquiredSemaphore, nullptr);
	frameResource.imageAcquiredSemaphore = VK_NULL_HANDLE;

	// Create a new one
	VkSemaphoreCreateInfo semaphoreInfo{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frameResource.imageAcquiredSemaphore) != VK_SUCCESS)
	{
		throw RenderError("Failed to recreate image-acquired semaphore.");
	}
}

void Renderer::createCommandBuffers()
{
	ZoneScopedN("Create Command Buffers");

	// Transient command pool
	VkCommandPoolCreateInfo transientPoolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = graphicsQueueFamilyIndex,
	};
	if (vkCreateCommandPool(device, &transientPoolInfo, nullptr, &transientCommandPool) != VK_SUCCESS)
	{
		throw RenderError("Failed to create transient command pool.");
	}

	// Per-frame command pools/buffers
	for (FrameResources& frameResource : frameResources)
	{
		// Create per-frame command pool
		VkCommandPoolCreateInfo commandPoolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.queueFamilyIndex = graphicsQueueFamilyIndex,
		};
		if (vkCreateCommandPool(device, &commandPoolInfo, nullptr, &frameResource.commandPool) != VK_SUCCESS)
		{
			throw RenderError("Failed to create per-frame command pool.");
		}

		// Create per-frame command buffer
		VkCommandBufferAllocateInfo commandBufferInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = frameResource.commandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		if (vkAllocateCommandBuffers(device, &commandBufferInfo, &frameResource.commandBuffer) != VK_SUCCESS)
		{
			throw RenderError("Failed to allocate per-frame command buffer.");
		}
	}
}

VkCommandBuffer Renderer::startTransientCommandBuffer()
{
	ZoneScopedN("Start Transient Command Buffer");

	// Allocate
	VkCommandBufferAllocateInfo allocationInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = transientCommandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device, &allocationInfo, &commandBuffer) != VK_SUCCESS)
	{
		throw RenderError("Failed to allocate transient command buffer.");
	}

	// Begin
	VkCommandBufferBeginInfo beginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
	{
		vkFreeCommandBuffers(device, transientCommandPool, 1, &commandBuffer);
		throw RenderError("Failed to begin transient command buffer.");
	}

	return commandBuffer;
}

void Renderer::submitTransientCommandBuffer(VkCommandBuffer commandBuffer)
{
	ZoneScopedN("Submit Transient Command Buffer");

	// Finish recording commands
	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) { throw RenderError("Failed to record transient command buffer."); }

	// Submit the command buffer to the graphics queue (transfer queue eventually?)
	VkCommandBufferSubmitInfo commandBufferSubmitInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = commandBuffer,
	};
	VkSubmitInfo2 submitInfo{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &commandBufferSubmitInfo,
	};
	if (vkQueueSubmit2(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
	{
		throw RenderError("Failed to submit transient command buffer.");
	}
	// Wait and clean up buffer (use better sync method eventually)
	vkQueueWaitIdle(graphicsQueue);
	vkFreeCommandBuffers(device, transientCommandPool, 1, &commandBuffer);
}

std::pair<uint32_t, GPUBuffer>
Renderer::createImage(VkCommandBuffer commandBuffer, unsigned char* imageData, uint32_t width, uint32_t height, int channels)
{
	ZoneScopedN("Create Image");

	VkFormat imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
	VmaAllocationCreateInfo allocationInfo{.usage = VMA_MEMORY_USAGE_AUTO};
	GPUImage gpuImage;

	// Create image
	VkImageCreateInfo imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = imageFormat,
		.extent{width, height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vmaCreateImage(vmaAllocator, &imageInfo, &allocationInfo, &gpuImage.image, &gpuImage.allocation, nullptr)
		!= VK_SUCCESS)
	{
		throw RenderError("Failed to create image.");
	}

	// Create image view
	VkImageViewCreateInfo imageViewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = gpuImage.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = imageFormat,
		.subresourceRange{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1
		}
	};
	if (vkCreateImageView(device, &imageViewInfo, nullptr, &gpuImage.imageView) != VK_SUCCESS)
	{
		throw RenderError("Failed to create image view.");
	}

	// Transition image to transfer-DST
	VkImageMemoryBarrier2 transferBarrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_NONE,
		.srcAccessMask = VK_ACCESS_2_NONE,
		.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.image = gpuImage.image,
		.subresourceRange{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1
		},
	};
	VkDependencyInfo transferDependencyInfo{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &transferBarrier,
	};
	vkCmdPipelineBarrier2(commandBuffer, &transferDependencyInfo);

	// Create staging buffer and issue record copy operation
	const size_t byteSize = width * height * channels;
	GPUBuffer stagingBuffer =
		createBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, byteSize, true, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
	mapCopyBufferData(stagingBuffer, 0, imageData, byteSize);

	// Record command to make final copy to image in GPU memory
	VkBufferImageCopy bufferImageCopy{
		.imageSubresource =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		.imageExtent = {width, height, 1},
	};
	vkCmdCopyBufferToImage(
		commandBuffer, stagingBuffer.buffer, gpuImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bufferImageCopy
	);

	// Transition image for shader read/sampling
	VkImageMemoryBarrier2 shaderReadBarrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.image = gpuImage.image,
		.subresourceRange{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1
		},
	};
	VkDependencyInfo shaderReadDependencyInfo{
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &shaderReadBarrier,
	};
	vkCmdPipelineBarrier2(commandBuffer, &shaderReadDependencyInfo);

	images.push_back(gpuImage);

	// Image ID is 1-based (0 is NULL, ID - 1 is index)
	const uint32_t imageID = static_cast<uint32_t>(images.size());
	return {imageID, stagingBuffer};
}

GPUBuffer Renderer::createBuffer(VkBufferUsageFlags usage, size_t byteSize, bool mappable, VmaMemoryUsage memoryUsage)
{
	ZoneScopedN("Create GPUBuffer");

	GPUBuffer gpuBuffer;

	// Create buffer and vma allocation
	VkBufferCreateInfo bufferInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = byteSize,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VmaAllocationCreateInfo allocationInfo{
		.flags = mappable ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0u,
		.usage = memoryUsage,
	};
	if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocationInfo, &gpuBuffer.buffer, &gpuBuffer.allocation, nullptr)
		!= VK_SUCCESS)
	{
		throw RenderError("Failed to create buffer.");
	}

	// BDA send device pointer
	if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
	{
		VkBufferDeviceAddressInfo bdaInfo{
			.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
			.buffer = gpuBuffer.buffer,
		};
		gpuBuffer.deviceAddress = vkGetBufferDeviceAddress(device, &bdaInfo);
	}

	return gpuBuffer;
}

void Renderer::mapCopyBufferData(const GPUBuffer& buffer, size_t bufferOffset, const void* data, size_t byteSize)
{
	ZoneScopedN("Map Copy Buffer Data");

	void* bufferPtr = nullptr;
	if (vmaMapMemory(vmaAllocator, buffer.allocation, &bufferPtr) != VK_SUCCESS)
	{
		throw RenderError("Failed to map buffer memory.");
	}

	// Copy into the mapped range at given offset
	std::memcpy(static_cast<char*>(bufferPtr) + bufferOffset, data, byteSize);

	vmaUnmapMemory(vmaAllocator, buffer.allocation);
}

void Renderer::createFallbackTexture()
{
	ZoneScopedN("Create Fallback Texture");

	// Fallback image
	uint32_t whitePixelData = 0xFFFFFFFF;
	Image whitePixel{
		.width = 1,
		.height = 1,
		.channels = 4,
		.data = reinterpret_cast<unsigned char*>(&whitePixelData),
	};

	VkCommandBuffer fallbackImageCommandBuffer = startTransientCommandBuffer();
	auto [whitePixelID, whitePixelStagingBuffer] =
		createImage(fallbackImageCommandBuffer, whitePixel.data, whitePixel.width, whitePixel.height, whitePixel.channels);
	fallbackImageID = whitePixelID;
	submitTransientCommandBuffer(fallbackImageCommandBuffer);
	vmaDestroyBuffer(vmaAllocator, whitePixelStagingBuffer.buffer, whitePixelStagingBuffer.allocation);

	// Fallback texture sampler
	VkSamplerCreateInfo samplerInfo{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.compareEnable = VK_FALSE,
	};
	VkSampler sampler = VK_NULL_HANDLE;
	if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
	{
		throw RenderError("Failed to create texture sampler.");
	}

	// Store sampler
	samplers.push_back(sampler);
	uint32_t fallbackSamplerID = static_cast<uint32_t>(samplers.size());

	// Store texture
	textures.push_back(Texture{.imageID = fallbackImageID, .samplerID = fallbackSamplerID});

	// Store material
	materials.push_back(Material{.baseColor = glm::vec4(1.0f), .textureID = 0});
}

std::vector<uint32_t> Renderer::uploadImages(const std::vector<Image>& cpuImages)
{
	ZoneScopedN("Upload Images");

	VkCommandBuffer commandBuffer = startTransientCommandBuffer();

	std::vector<GPUBuffer> stagingBuffers;
	stagingBuffers.reserve(cpuImages.size());

	std::vector<uint32_t> imageIDs(cpuImages.size(), fallbackImageID);

	// Upload images to GPU textures
	for (uint32_t i = 0; i < cpuImages.size(); ++i)
	{
		const Image& image = cpuImages[i];

		if (image.data)
		{
			auto [imageID, imageStagingBuffer] = createImage(commandBuffer, image.data, image.width, image.height, 4);

			imageIDs[i] = imageID;
			stagingBuffers.push_back(imageStagingBuffer);
		}
	}

	submitTransientCommandBuffer(commandBuffer);

	// Cleanup staging buffers
	for (const GPUBuffer& buffer : stagingBuffers)
	{
		vmaDestroyBuffer(vmaAllocator, buffer.buffer, buffer.allocation);
	}

	std::cout << std::format("Uploaded {} images.", imageIDs.size()) << std::endl;
	return imageIDs;
}

std::vector<uint32_t> Renderer::uploadSamplers(const std::vector<ModelSampler>& modelSamplers)
{
	ZoneScopedN("Load Samplers");

	std::vector<uint32_t> samplerIDs(modelSamplers.size());

	for (uint32_t i = 0; i < modelSamplers.size(); ++i)
	{
		const ModelSampler& modelSampler = modelSamplers[i];

		static const std::unordered_map<int32_t, std::tuple<VkFilter, VkSamplerMipmapMode, float>> filterMap{
			{TG3_TEXTURE_FILTER_NEAREST, {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, 0.25f}},
			{TG3_TEXTURE_FILTER_LINEAR, {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, 0.25f}},
			{TG3_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST,
			 {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_LOD_CLAMP_NONE}},
			{TG3_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR,
			 {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_LOD_CLAMP_NONE}},
			{TG3_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST,
			 {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_LOD_CLAMP_NONE}},
			{TG3_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR, {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_LOD_CLAMP_NONE}},
		};

		static const std::unordered_map<int32_t, VkSamplerAddressMode> wrapMap{
			{TG3_TEXTURE_WRAP_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT},
			{TG3_TEXTURE_WRAP_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
			{TG3_TEXTURE_WRAP_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT},
		};

		VkSamplerCreateInfo samplerInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter =
				(modelSampler.magFilter == -1) ? VK_FILTER_LINEAR : std::get<0>(filterMap.at(modelSampler.magFilter)),
			.minFilter =
				(modelSampler.minFilter == -1) ? VK_FILTER_LINEAR : std::get<0>(filterMap.at(modelSampler.minFilter)),
			.mipmapMode = (modelSampler.minFilter == -1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
														 : std::get<1>(filterMap.at(modelSampler.minFilter)),
			.addressModeU = (modelSampler.wrapU == -1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : wrapMap.at(modelSampler.wrapU),
			.addressModeV = (modelSampler.wrapV == -1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : wrapMap.at(modelSampler.wrapV),
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.compareEnable = VK_FALSE,
			.minLod = 0.0f,
			.maxLod = (modelSampler.minFilter == -1) ? VK_LOD_CLAMP_NONE : std::get<2>(filterMap.at(modelSampler.minFilter)),
		};

		VkSampler sampler = VK_NULL_HANDLE;
		if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
		{
			std::cerr << "Failed to create texture sampler." << std::endl;
			samplerIDs[i] = textures[0].samplerID;
		}
		else
		{
			samplers.push_back(sampler);
			samplerIDs[i] = static_cast<uint32_t>(samplers.size());
		}
	}

	std::cout << std::format("Loaded {} samplers.", samplerIDs.size()) << std::endl;
	return samplerIDs;
}

uint32_t Renderer::addBuffer(const GPUBuffer& buffer)
{
	buffers.push_back(buffer);
	return static_cast<uint32_t>(buffers.size());
}

void Renderer::createDescriptorSets()
{
	ZoneScopedN("Create Descriptor Sets");

	// Descriptor pool
	std::array<VkDescriptorPoolSize, 1> poolSizes{VkDescriptorPoolSize{
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = MaxTextures,
	}};

	VkDescriptorPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
		.maxSets = 1,
		.poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
		.pPoolSizes = poolSizes.data(),
	};
	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
	{
		throw RenderError("Failed to create descriptor pool.");
	}

	// Descriptor set layout
	std::array<VkDescriptorSetLayoutBinding, 1> bindings{VkDescriptorSetLayoutBinding{
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = MaxTextures,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	}};

	std::array<VkDescriptorBindingFlags, 1> flags;
	flags[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(flags.size()),
		.pBindingFlags = flags.data(),
	};

	VkDescriptorSetLayoutCreateInfo layoutInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = &flagsInfo,
		.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
		.bindingCount = static_cast<uint32_t>(bindings.size()),
		.pBindings = bindings.data(),
	};
	if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &globalDescriptorSetLayout) != VK_SUCCESS)
	{
		throw RenderError("Failed to create descriptor set layout.");
	}

	// Descriptor sets (only 1 for now)
	VkDescriptorSetAllocateInfo descriptorSetAllocationInfo{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &globalDescriptorSetLayout,
	};
	if (vkAllocateDescriptorSets(device, &descriptorSetAllocationInfo, &globalDescriptorSet) != VK_SUCCESS)
	{
		throw RenderError("Failed to allocate descriptor set.");
	}
}

void Renderer::updateTextureDescriptors()
{
	ZoneScopedN("Update Texture Descriptors");

	std::vector<VkDescriptorImageInfo> imageDescriptors;
	imageDescriptors.reserve(textures.size());

	for (const Texture& texture : textures)
	{
		imageDescriptors.push_back(
			{.sampler = samplers[texture.samplerID - 1],
			 .imageView = images[texture.imageID - 1].imageView,
			 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
		);
	}

	VkWriteDescriptorSet descriptorSetWrite{
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = globalDescriptorSet,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = static_cast<uint32_t>(imageDescriptors.size()),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = imageDescriptors.data()
	};

	vkUpdateDescriptorSets(device, 1, &descriptorSetWrite, 0, nullptr);
}

void Renderer::recreateFrameResourceDrawBuffers(FrameResources& resource, size_t size)
{
	ZoneScopedN("Recreate Frame Draw Buffers");

	// Unmap and destroy if buffers already exist
	if (resource.indirectDrawBuffer.buffer)
	{
		vmaUnmapMemory(vmaAllocator, resource.indirectDrawBuffer.allocation);
		vkDestroyBuffer(device, resource.indirectDrawBuffer.buffer, nullptr);
		vmaFreeMemory(vmaAllocator, resource.indirectDrawBuffer.allocation);
	}

	if (resource.renderItemBuffer.buffer)
	{
		vmaUnmapMemory(vmaAllocator, resource.renderItemBuffer.allocation);
		vkDestroyBuffer(device, resource.renderItemBuffer.buffer, nullptr);
		vmaFreeMemory(vmaAllocator, resource.renderItemBuffer.allocation);
	}

	// Set draw capacity
	resource.drawCapacity = size;

	// Create indirect draw buffer
	const size_t indirectBufferByteSize = size * sizeof(VkDrawIndexedIndirectCommand);
	resource.indirectDrawBuffer =
		createBuffer(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indirectBufferByteSize, true, VMA_MEMORY_USAGE_AUTO);

	// Map indirect draw buffer
	void* indirectBufferPointer = nullptr;
	if (vmaMapMemory(vmaAllocator, resource.indirectDrawBuffer.allocation, &indirectBufferPointer) != VK_SUCCESS)
	{
		throw RenderError("Failed to map indirect draw buffer.");
	}
	resource.indirectDrawPointer = reinterpret_cast<VkDrawIndexedIndirectCommand*>(indirectBufferPointer);

	// Create render item buffer (per-draw data)
	const size_t renderItemBufferByteSize = size * sizeof(RenderItem);
	resource.renderItemBuffer = createBuffer(
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		renderItemBufferByteSize,
		true,
		VMA_MEMORY_USAGE_AUTO
	);

	// Map render item buffer
	void* renderItemBufferPointer = nullptr;
	if (vmaMapMemory(vmaAllocator, resource.renderItemBuffer.allocation, &renderItemBufferPointer) != VK_SUCCESS)
	{
		throw RenderError("Failed to map render item buffer.");
	}
	resource.renderItemPointer = reinterpret_cast<RenderItem*>(renderItemBufferPointer);
}