#pragma once

#include "models.h"
#include "resources.h"
#include "scene.h"

#include <SDL3/SDL_vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <shaderc/shaderc.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <vma/vk_mem_alloc.h> // Would be best to forward declare instead, but fine for now
#include <vulkan/vulkan.h>

struct GPUImage
{
	VkImage image = VK_NULL_HANDLE;
	VkImageView imageView = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
};

struct GPUBuffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	uint64_t deviceAddress = 0;
	VmaAllocation allocation = nullptr;
};

struct RenderItem
{
	glm::mat4 worldMatrix;
	glm::mat3 normalMatrix;
	uint32_t materialIndex = 0;
};

struct FrameResources
{
	VkCommandPool commandPool{VK_NULL_HANDLE};
	VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
	VkSemaphore imageAcquiredSemaphore{VK_NULL_HANDLE};
	VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
	GPUBuffer indirectDrawBuffer;
	GPUBuffer renderItemBuffer;
	VkDrawIndexedIndirectCommand* indirectDrawPointer = nullptr;
	RenderItem* renderItemPointer = nullptr;
	size_t drawCapacity = 0;
};

struct FrameConstants
{
	uint64_t vertexBufferAddress = 0;
	uint64_t materialBufferAddress = 0;
	uint64_t renderItemsBufferAddress = 0;
	glm::mat4 viewProjection;
};

class RenderError : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class Renderer
{
  public:
	void initialize(SDL_Window* window);
	void loadModel(const Model& model);
	void render(const glm::mat4& viewProjectionMatrix);
	void shutdown();

	void invalidateSwapchain();

  private:
	static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
		VkDebugUtilsMessageTypeFlagsEXT messageType,
		const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
		void* pUserData
	);

	void createVulkanInstance();
	void createSurface();
	VkPhysicalDevice selectPhysicalDevice();
	void selectGraphicsQueue();
	void createDevice();
	void initializeVMA();
	void createSwapchain();
	void destroySwapchain();
	VkShaderModule createShaderModule(const std::string& filename, shaderc_shader_kind kind) const;
	void createShaders();
	VkPipeline createGraphicsPipeline();
	void createSyncResources();
	void recreateImageAcquiredSemaphore(FrameResources& frameResource);
	void createCommandBuffers();
	VkCommandBuffer startTransientCommandBuffer();
	void submitTransientCommandBuffer(VkCommandBuffer commandBuffer);
	std::pair<uint32_t, GPUBuffer>
	createImage(VkCommandBuffer commandBuffer, unsigned char* imageData, uint32_t width, uint32_t height, int channels);
	GPUBuffer createBuffer(VkBufferUsageFlags usage, size_t byteSize, bool mappable, VmaMemoryUsage memoryUsage);
	void mapCopyBufferData(const GPUBuffer& buffer, size_t bufferOffset, const void* data, size_t byteSize);
	void createFallbackTexture();

	std::vector<uint32_t> uploadImages(const std::vector<Image>& cpuImages);
	std::vector<uint32_t> uploadSamplers(const std::vector<ModelSampler>& modelSamplers);

	uint32_t addBuffer(const GPUBuffer& buffer);

	void createDescriptorSets();
	void updateTextureDescriptors();

	void recreateFrameResourceDrawBuffers(FrameResources& resource, size_t size);

  private:
	constexpr static uint32_t VulkanAPIVersion{VK_API_VERSION_1_4};
	constexpr static uint32_t MaxFramesInFlight{2};
	constexpr static size_t MaxTextures{1024};
	constexpr static size_t InitialDrawBufferSize{1024};
	constexpr static VkFormat swapchainFormat{VK_FORMAT_B8G8R8A8_SRGB};
	constexpr static VkFormat depthFormat{VK_FORMAT_D32_SFLOAT};

	SDL_Window* window{nullptr};

	VkDebugUtilsMessengerEXT debugMessenger{VK_NULL_HANDLE};

	uint64_t frameIndex{0};
	uint64_t nextSignalValue{MaxFramesInFlight + 1};

	VkInstance vulkanInstance{VK_NULL_HANDLE};
	VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
	VkDevice device{VK_NULL_HANDLE};
	VkSurfaceKHR surface{VK_NULL_HANDLE};
	VmaAllocator vmaAllocator{nullptr};

	uint32_t graphicsQueueFamilyIndex{UINT32_MAX};
	VkQueue graphicsQueue{VK_NULL_HANDLE};

	VkSwapchainKHR swapchain{VK_NULL_HANDLE};
	std::vector<VkImage> swapchainImages;
	std::vector<VkImageView> swapchainImageViews;
	std::vector<VkSemaphore> renderCompleteSemaphores;
	bool requireSwapchainRecreation{false};
	uint32_t swapchainWidth{0};
	uint32_t swapchainHeight{0};

	VkImage depthImage{VK_NULL_HANDLE};
	VkImageView depthImageView{VK_NULL_HANDLE};
	VmaAllocation depthImageAllocation{nullptr};

	VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
	VkPipeline pipeline{VK_NULL_HANDLE};

	VkShaderModule vertShader{VK_NULL_HANDLE};
	VkShaderModule fragShader{VK_NULL_HANDLE};

	VkSemaphore timelineSemaphore{VK_NULL_HANDLE};
	std::array<FrameResources, MaxFramesInFlight> frameResources;

	VkCommandPool transientCommandPool{VK_NULL_HANDLE};

	// CPU resources
	std::vector<Mesh> meshes;

	// GPU resources
	uint32_t fallbackImageID = 0;
	uint32_t vertexBufferID = 0;
	uint32_t indexBufferID = 0;
	uint32_t materialBufferID = 0;
	std::vector<GPUImage> images;
	std::vector<VkSampler> samplers;
	std::vector<Texture> textures;
	std::vector<GPUBuffer> buffers;
	std::vector<Material> materials;

	// Descriptors
	VkDescriptorSetLayout globalDescriptorSetLayout{VK_NULL_HANDLE};
	VkDescriptorSet globalDescriptorSet{VK_NULL_HANDLE};
	VkDescriptorPool descriptorPool{VK_NULL_HANDLE};

	// Scene data
	Scene scene;
	uint32_t rootNodeID = 0;
	uint32_t lastRootNodeID = 0;
	std::vector<std::pair<Node*, glm::mat4>> nodeRenderStack;
	std::vector<VkDrawIndexedIndirectCommand> stagedDrawCommands;
	std::vector<RenderItem> stagedRenderItems;
};