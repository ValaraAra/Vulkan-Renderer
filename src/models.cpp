#include "models.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <numeric>
#include <span>
#include <stb_image.h>
#include <string_view>
#include <tracy/Tracy.hpp>

namespace
{
bool supportedAccessorType(std::string_view key, const tg3_accessor& accessor)
{
	const bool componentTypeValid = accessor.component_type == TG3_COMPONENT_TYPE_FLOAT;

	if (key == "POSITION" || key == "NORMAL") { return componentTypeValid && (accessor.type == TG3_TYPE_VEC3); }
	if (key == "COLOR_0")
	{
		return componentTypeValid && (accessor.type == TG3_TYPE_VEC3 || accessor.type == TG3_TYPE_VEC4);
	}
	if (key == "TEXCOORD_0") { return componentTypeValid && (accessor.type == TG3_TYPE_VEC2); }
	return true;
}

template <typename T>
void writeAttribute(const tg3_model& model, const tg3_accessor& accessor, Vertex* destination, T Vertex::* member)
{
	const tg3_buffer_view& bufferView = model.buffer_views[accessor.buffer_view];
	const tg3_buffer& buffer = model.buffers[bufferView.buffer];

	const uint8_t* source = buffer.data.data + bufferView.byte_offset + accessor.byte_offset;
	const size_t stride = static_cast<size_t>(tg3_accessor_byte_stride(&accessor, &bufferView));

	for (uint64_t i = 0; i < accessor.count; ++i)
	{
		std::memcpy(&(destination[i].*member), source + i * stride, sizeof(T));
	}
}

template <typename T>
void copyIndices(const uint8_t* source, uint32_t* destination, uint64_t count)
{
	for (uint64_t i = 0; i < count; ++i)
	{
		T index;
		std::memcpy(&index, source + i * sizeof(T), sizeof(T));
		destination[i] = index;
	}
}
} // namespace

Model ModelLoader::loadGLTF(const std::filesystem::path& filepath)
{
	ZoneScopedN("Load GLTF");

	if (!std::filesystem::exists(filepath)) { throw ModelError("GLTF file does not exists!"); }

	const std::string filepathString = filepath.string();
	std::cout << std::format("Loading GLTF: {}", filepathString) << std::endl;

	// Load and parse GLTF
	tinygltf3::Model gltfModel;
	tinygltf3::ErrorStack modelErrors;

	tg3_parse_options modelOptions;
	tg3_parse_options_init(&modelOptions);

	tg3_error_code parseResult = tg3_parse_file(
		gltfModel.get(),
		modelErrors.get(),
		filepathString.c_str(),
		static_cast<uint32_t>(filepathString.size()),
		&modelOptions
	);

	// Handle parse errors
	if (parseResult != TG3_OK)
	{
		std::cerr << "GLTF parsing failed, errors found:" << std::endl;
		for (uint32_t i = 0; i < modelErrors.count(); ++i)
		{
			std::cerr << modelErrors.entry(i)->message << std::endl;
		}
		throw ModelError("GLTF parsing failed!");
	}

	const tg3_model& model = *gltfModel.get();

	// Loaded model data
	Model loadedModel;
	loadImages(model, filepath.parent_path(), loadedModel);
	loadSamplers(model, loadedModel);
	loadTextures(model, loadedModel);
	loadMaterials(model, loadedModel);
	loadMeshes(model, loadedModel);

	// Process nodes
	importNodes(model, loadedModel);

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

	for (uint32_t i = 0; i < model.meshes_count; ++i)
	{
		Mesh mesh;
		const tg3_mesh& tg3Mesh = model.meshes[i];

		// Copy name
		mesh.name = tg3Mesh.name.len > 0 ? std::string(tg3Mesh.name.data, tg3Mesh.name.len) : "No Name";

		// Copy vertex data
		mesh.subMeshes.resize(tg3Mesh.primitives_count);
		for (uint32_t j = 0; j < tg3Mesh.primitives_count; ++j)
		{
			const tg3_primitive& primitive = tg3Mesh.primitives[j];
			if (primitive.mode != TG3_MODE_TRIANGLES) { throw ModelError("Unsupported primitive mode!"); }

			SubMesh& subMesh = mesh.subMeshes[j];
			subMesh.materialID = primitive.material == -1 ? InvalidIndex : static_cast<uint32_t>(primitive.material);
			subMesh.vertexStart = vertexOffset;

			// Grab the POSITION accessor for vertex count
			const tg3_accessor* positionAccessor = nullptr;
			for (uint32_t k = 0; k < primitive.attributes_count; ++k)
			{
				const tg3_str_int_pair& attr = primitive.attributes[k];
				if (std::string_view(attr.key.data, attr.key.len) == "POSITION")
				{
					positionAccessor = &model.accessors[attr.value];
					break;
				}
			}
			if (!positionAccessor) { throw ModelError("Primitive missing POSITION attribute!"); }

			const size_t vertexCount = positionAccessor->count;

			// Validate attributes
			for (uint32_t k = 0; k < primitive.attributes_count; ++k)
			{
				const tg3_str_int_pair& attr = primitive.attributes[k];
				const tg3_accessor& accessor = model.accessors[attr.value];

				if (accessor.count != vertexCount) { throw ModelError("Invalid accessor count!"); }
				if (accessor.buffer_view == -1) { throw ModelError("Invalid accessor buffer view!"); }
				if (accessor.sparse.is_sparse) { throw ModelError("Invalid accessor sparsity!"); }
				if (!supportedAccessorType(std::string_view(attr.key.data, attr.key.len), accessor))
				{
					throw ModelError("Unsupported accessor type!");
				}
			}

			// Write attributes
			subMesh.vertexCount = vertexCount;
			loadedModel.vertices.resize(vertexOffset + vertexCount);
			Vertex* vertDestination = loadedModel.vertices.data() + vertexOffset;

			for (uint32_t k = 0; k < primitive.attributes_count; ++k)
			{
				const tg3_str_int_pair& attr = primitive.attributes[k];
				const tg3_accessor& accessor = model.accessors[attr.value];

				const std::string_view key(attr.key.data, attr.key.len);

				if (key == "POSITION") { writeAttribute(model, accessor, vertDestination, &Vertex::position); }
				else if (key == "NORMAL") { writeAttribute(model, accessor, vertDestination, &Vertex::normal); }
				else if (key == "COLOR_0") { writeAttribute(model, accessor, vertDestination, &Vertex::color); }
				else if (key == "TEXCOORD_0") { writeAttribute(model, accessor, vertDestination, &Vertex::uv); }
			}
			vertexOffset += vertexCount;

			// Copy index data
			subMesh.indexStart = indexOffset;

			if (primitive.indices != -1)
			{
				const tg3_accessor& accessor = model.accessors[primitive.indices];
				if (accessor.count == 0 || accessor.type != TG3_TYPE_SCALAR || accessor.buffer_view == -1
					|| accessor.sparse.is_sparse)
				{
					throw ModelError("Invalid index accessor!");
				}

				const tg3_buffer_view& bufferView = model.buffer_views[accessor.buffer_view];
				const tg3_buffer& buffer = model.buffers[bufferView.buffer];

				subMesh.indexCount = accessor.count;
				loadedModel.indices.resize(indexOffset + accessor.count);
				uint32_t* indexDestination = loadedModel.indices.data() + indexOffset;

				const uint8_t* source = buffer.data.data + bufferView.byte_offset + accessor.byte_offset;

				switch (accessor.component_type)
				{
					case TG3_COMPONENT_TYPE_UNSIGNED_INT:
						copyIndices<uint32_t>(source, indexDestination, accessor.count);
						break;
					case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
						copyIndices<uint16_t>(source, indexDestination, accessor.count);
						break;
					case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
						copyIndices<uint8_t>(source, indexDestination, accessor.count);
						break;
					default: throw ModelError("Invalid component type!");
				}

				// Validate indices
				const uint32_t maxIndex = std::ranges::max(std::span(indexDestination, accessor.count));
				if (maxIndex >= vertexCount) { throw ModelError("Index out of range!"); }
			}
			else
			{
				subMesh.indexCount = vertexCount;
				loadedModel.indices.resize(indexOffset + vertexCount);
				uint32_t* indexDestination = loadedModel.indices.data() + indexOffset;

				std::iota(indexDestination, indexDestination + vertexCount, 0u);
			}

			indexOffset += subMesh.indexCount;
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