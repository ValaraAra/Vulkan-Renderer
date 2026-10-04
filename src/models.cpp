#include "models.h"

#include "tiny_gltf_v3.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <stb_image.h>
#include <tracy/Tracy.hpp>

Model ModelLoader::loadGLTF(const std::filesystem::path& filepath)
{
	ZoneScopedN("Load GLTF");

	if (!std::filesystem::exists(filepath)) { throw ModelError("GLTF file does not exists!"); }

	const std::string filepathString = filepath.string();
	std::cout << std::format("Loading GLTF: {}", filepathString) << std::endl;

	// Load and parse GLTF
	tg3_model model;
	tg3_parse_options modelOptions;
	tg3_error_stack modelErrors;

	tg3_parse_options_init(&modelOptions);
	tg3_error_stack_init(&modelErrors);
	tg3_error_code parseResult = tg3_parse_file(
		&model, &modelErrors, filepathString.c_str(), static_cast<uint32_t>(filepathString.size()), &modelOptions
	);

	// Handle parse errors
	if (parseResult != TG3_OK)
	{
		std::cerr << "GLTF parsing failed, errors found:" << std::endl;
		for (uint32_t i = 0; i < modelErrors.count; ++i)
		{
			std::cerr << modelErrors.entries[i].message << std::endl;
		}
		tg3_error_stack_free(&modelErrors);
		throw ModelError("GLTF parsing failed!");
	}
	tg3_error_stack_free(&modelErrors);

	// Loaded model data
	Model loadedModel;
	loadImages(model, filepath.parent_path(), loadedModel);
	loadSamplers(model, loadedModel);
	loadTextures(model, loadedModel);
	loadMaterials(model, loadedModel);
	loadMeshes(model, loadedModel);

	// Process nodes
	importNodes(model, loadedModel);

	// Cleanup
	tg3_model_free(&model);
	std::cout << "GLTF loaded successfully!" << std::endl;

	return loadedModel;
}

void ModelLoader::loadImages(const tg3_model& model, const std::filesystem::path& imageDir, Model& loadedModel)
{
	ZoneScopedN("Load Images");

	loadedModel.images.resize(model.images_count);

	for (uint32_t i = 0; i < model.images_count; ++i)
	{
		Image& image = loadedModel.images[i];
		std::filesystem::path imagePath = imageDir / model.images[i].uri.data;

		std::cout << std::format("Loading image {}/{}: {}", i + 1, model.images_count, model.images[i].uri.data)
				  << std::endl;

		image.data = stbi_load(imagePath.string().c_str(), &image.width, &image.height, &image.channels, 4);
		if (!image.data) { throw ModelError("Failed to load image: " + imagePath.string()); }
	}
}

void ModelLoader::loadSamplers(const tg3_model& model, Model& loadedModel)
{
	ZoneScopedN("Load Samplers");

	for (uint32_t i = 0; i < model.samplers_count; ++i)
	{
		const tg3_sampler& tg3Sampler = model.samplers[i];

		loadedModel.samplers.push_back({
			.magFilter = tg3Sampler.mag_filter,
			.minFilter = tg3Sampler.min_filter,
			.wrapU = tg3Sampler.wrap_s,
			.wrapV = tg3Sampler.wrap_t,
		});
	}

	std::cout << std::format("Loaded {} samplers.", loadedModel.samplers.size()) << std::endl;
}

void ModelLoader::loadTextures(const tg3_model& model, Model& loadedModel)
{
	ZoneScopedN("Load Textures");

	for (uint32_t i = 0; i < model.textures_count; ++i)
	{
		const tg3_texture& tg3Texture = model.textures[i];

		loadedModel.textures.push_back({
			.image = tg3Texture.source == -1 ? InvalidIndex : static_cast<uint32_t>(tg3Texture.source),
			.sampler = tg3Texture.sampler == -1 ? InvalidIndex : static_cast<uint32_t>(tg3Texture.sampler),
		});
	}

	std::cout << std::format("Loaded {} textures.", loadedModel.textures.size()) << std::endl;
}

void ModelLoader::loadMaterials(const tg3_model& model, Model& loadedModel)
{
	ZoneScopedN("Load Materials");

	for (uint32_t i = 0; i < model.materials_count; ++i)
	{
		const tg3_material& tg3Material = model.materials[i];
		loadedModel.materials.push_back({
			.baseColor = glm::vec4(
				tg3Material.pbr_metallic_roughness.base_color_factor[0],
				tg3Material.pbr_metallic_roughness.base_color_factor[1],
				tg3Material.pbr_metallic_roughness.base_color_factor[2],
				tg3Material.pbr_metallic_roughness.base_color_factor[3]
			),
			.textureID = tg3Material.pbr_metallic_roughness.base_color_texture.index == -1
							 ? InvalidIndex
							 : static_cast<uint32_t>(tg3Material.pbr_metallic_roughness.base_color_texture.index),
		});
	}

	std::cout << std::format("Loaded {} materials.", loadedModel.materials.size()) << std::endl;
}

void ModelLoader::loadMeshes(const tg3_model& model, Model& loadedModel)
{
	ZoneScopedN("Load Meshes");

	size_t vertexOffset = 0;
	size_t indexOffset = 0;

	// Attribute data copy lambda
	auto writeAttribute =
		[&model, &loadedModel, &vertexOffset]<typename T>(T Vertex::* member, const tg3_str_int_pair* attr) {
			const tg3_accessor* accessor = &model.accessors[attr->value];
			const tg3_buffer_view* bufferView = &model.buffer_views[accessor->buffer_view];
			const tg3_buffer* buffer = &model.buffers[bufferView->buffer];

			size_t componentCount;
			switch (accessor->type)
			{
				case TG3_TYPE_VEC2: componentCount = 2; break;
				case TG3_TYPE_VEC3: componentCount = 3; break;
				case TG3_TYPE_VEC4: componentCount = 4; break;
				default: throw ModelError("Unsupported mesh vertex attribute type!");
			}

			size_t componentSize;
			switch (accessor->component_type)
			{
				case TG3_COMPONENT_TYPE_FLOAT: componentSize = sizeof(float); break;
				default: throw ModelError("Unsupported mesh vertex attribute component type!");
			}

			const size_t bufferOffset = bufferView->byte_offset + accessor->byte_offset;
			const size_t elementSize = componentCount * componentSize;
			const size_t stride = bufferView->byte_stride != 0 ? bufferView->byte_stride : elementSize;

			for (uint64_t j = 0; j < accessor->count; ++j)
			{
				const size_t elementOffset = bufferOffset + j * stride;
				const float* data = reinterpret_cast<const float*>(buffer->data.data + elementOffset);

				if constexpr (std::is_same<T, glm::vec3>())
				{
					loadedModel.vertices[vertexOffset + j].*member = glm::vec3(data[0], data[1], data[2]);
				}
				else if constexpr (std::is_same<T, glm::vec2>())
				{
					loadedModel.vertices[vertexOffset + j].*member = glm::vec2(data[0], data[1]);
				}
			}
		};

	for (uint32_t i = 0; i < model.meshes_count; ++i)
	{
		Mesh mesh;
		const tg3_mesh* tg3Mesh = &model.meshes[i];

		// Copy name
		mesh.name = tg3Mesh->name.data != nullptr ? tg3Mesh->name.data : "No Name";

		// Copy vertex data
		mesh.subMeshes.resize(tg3Mesh->primitives_count);
		for (uint32_t j = 0; j < tg3Mesh->primitives_count; ++j)
		{
			const tg3_primitive* primitive = &tg3Mesh->primitives[j];
			mesh.subMeshes[j].materialID =
				primitive->material == -1 ? InvalidIndex : static_cast<uint32_t>(primitive->material);
			mesh.subMeshes[j].vertexStart = vertexOffset;

			const tg3_accessor* positionAccessor = nullptr;
			for (uint32_t k = 0; k < primitive->attributes_count; ++k)
			{
				if (strcmp(primitive->attributes[k].key.data, "POSITION") == 0)
				{
					positionAccessor = &model.accessors[primitive->attributes[k].value];
					break;
				}
			}
			if (!positionAccessor) { throw ModelError("Primitive missing POSITION attribute!"); }

			mesh.subMeshes[j].vertexCount = positionAccessor->count;
			loadedModel.vertices.resize(vertexOffset + positionAccessor->count);

			for (uint32_t k = 0; k < primitive->attributes_count; ++k)
			{
				const tg3_str_int_pair* attr = &primitive->attributes[k];
				if (strcmp(attr->key.data, "POSITION") == 0)
				{
					const tg3_accessor* accessor = &model.accessors[attr->value];
					assert(accessor->type == TG3_TYPE_VEC3 && accessor->component_type == TG3_COMPONENT_TYPE_FLOAT);
					writeAttribute(&Vertex::position, attr);
				}
				else if (strcmp(attr->key.data, "NORMAL") == 0)
				{
					const tg3_accessor* accessor = &model.accessors[attr->value];
					assert(accessor->type == TG3_TYPE_VEC3 && accessor->component_type == TG3_COMPONENT_TYPE_FLOAT);
					writeAttribute(&Vertex::normal, attr);
				}
				else if (strcmp(attr->key.data, "COLOR_0") == 0)
				{
					const tg3_accessor* accessor = &model.accessors[attr->value];
					assert(accessor->type == TG3_TYPE_VEC3 || accessor->type == TG3_TYPE_VEC4);
					assert(accessor->component_type == TG3_COMPONENT_TYPE_FLOAT);
					writeAttribute(&Vertex::color, attr);
				}
				else if (strcmp(attr->key.data, "TEXCOORD_0") == 0)
				{
					const tg3_accessor* accessor = &model.accessors[attr->value];
					assert(accessor->type == TG3_TYPE_VEC2 && accessor->component_type == TG3_COMPONENT_TYPE_FLOAT);
					writeAttribute(&Vertex::uv, attr);
				}
			}
			vertexOffset += mesh.subMeshes[j].vertexCount;

			// Copy index data
			if (primitive->indices != -1)
			{
				const tg3_accessor* accessor = &model.accessors[primitive->indices];
				const tg3_buffer_view* bufferView = &model.buffer_views[accessor->buffer_view];
				const tg3_buffer* buffer = &model.buffers[bufferView->buffer];

				mesh.subMeshes[j].indexStart = indexOffset;
				mesh.subMeshes[j].indexCount = accessor->count;
				loadedModel.indices.resize(indexOffset + accessor->count);

				if (accessor->component_type == TG3_COMPONENT_TYPE_UNSIGNED_INT)
				{
					const uint32_t* buffData = reinterpret_cast<const uint32_t*>(
						buffer->data.data + bufferView->byte_offset + accessor->byte_offset
					);
					memcpy(&loadedModel.indices[indexOffset], buffData, accessor->count * sizeof(uint32_t));
				}
				else if (accessor->component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT)
				{
					const uint16_t* buffData = reinterpret_cast<const uint16_t*>(
						buffer->data.data + bufferView->byte_offset + accessor->byte_offset
					);
					for (uint64_t k = 0; k < accessor->count; ++k)
					{
						loadedModel.indices[indexOffset + k] = static_cast<uint32_t>(buffData[k]);
					}
				}

				indexOffset += mesh.subMeshes[j].indexCount;
			}
		}

		loadedModel.meshes.push_back(std::move(mesh));
	}

	std::cout << std::format("Loaded {} meshes.", loadedModel.meshes.size()) << std::endl;
}

void ModelLoader::importNodes(const tg3_model& model, Model& loadedModel)
{
	loadedModel.nodes.resize(model.nodes_count);

	// Node meshes/transforms
	for (uint32_t i = 0; i < model.nodes_count; ++i)
	{
		const tg3_node& tg3Node = model.nodes[i];
		ModelNode& node = loadedModel.nodes[i];

		if (tg3Node.has_matrix)
		{
			glm::mat4 transform(1);
			float* transformPointer = glm::value_ptr(transform);

			for (int j = 0; j < 16; ++j)
			{
				transformPointer[j] = static_cast<float>(tg3Node.matrix[j]);
			}

			node.transform = transform;
		}
		else
		{
			glm::vec3 translation(
				static_cast<float>(tg3Node.translation[0]),
				static_cast<float>(tg3Node.translation[1]),
				static_cast<float>(tg3Node.translation[2])
			);
			glm::quat rotation(
				static_cast<float>(tg3Node.rotation[3]),
				static_cast<float>(tg3Node.rotation[0]),
				static_cast<float>(tg3Node.rotation[1]),
				static_cast<float>(tg3Node.rotation[2])
			);
			glm::vec3 scale(
				static_cast<float>(tg3Node.scale[0]),
				static_cast<float>(tg3Node.scale[1]),
				static_cast<float>(tg3Node.scale[2])
			);

			glm::mat4 translationMat = glm::translate(glm::mat4(1.0f), translation);
			glm::mat4 rotationMat = glm::mat4_cast(rotation);
			glm::mat4 scalingMat = glm::scale(glm::mat4(1.0f), scale);

			node.transform = translationMat * rotationMat * scalingMat;
		}

		node.mesh = tg3Node.mesh == -1 ? InvalidIndex : static_cast<uint32_t>(tg3Node.mesh);
	}

	// Node hierarchy
	std::vector<uint32_t> parentCount(model.nodes_count, 0);

	for (uint32_t i = 0; i < model.nodes_count; ++i)
	{
		const tg3_node& tg3Node = model.nodes[i];

		uint32_t previousChildIndex = InvalidIndex;
		for (uint32_t j = 0; j < tg3Node.children_count; ++j)
		{
			uint32_t childIndex = static_cast<uint32_t>(tg3Node.children[j]);

			if (++parentCount[childIndex] > 1) { throw ModelError("Multiple parents detected!"); }
			loadedModel.nodes[childIndex].parent = i;

			if (j == 0) { loadedModel.nodes[i].firstChild = childIndex; }
			else
			{
				loadedModel.nodes[previousChildIndex].nextSibling = childIndex;
			}

			previousChildIndex = childIndex;
		}
	}

	// Root nodes
	const tg3_scene& tg3Scene = model.scenes[model.default_scene == -1 ? 0 : model.default_scene];
	loadedModel.rootNodes.reserve(tg3Scene.nodes_count);

	for (uint32_t i = 0; i < tg3Scene.nodes_count; ++i)
	{
		loadedModel.rootNodes.push_back(static_cast<uint32_t>(tg3Scene.nodes[i]));
	}

	std::cout << std::format("Loaded {} nodes.", loadedModel.nodes.size()) << std::endl;
}