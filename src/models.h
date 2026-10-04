#pragma once

#include "resources.h"

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <tiny_gltf_v3.h> // Replace with fastgltf eventually?
#include <vector>

static constexpr uint32_t InvalidIndex = UINT32_MAX;

struct ModelSampler
{
	int magFilter;
	int minFilter;
	int wrapU;
	int wrapV;
};

struct ModelTexture
{
	uint32_t image = 0;
	uint32_t sampler = 0;
};

struct ModelNode
{
	glm::mat4 transform = glm::mat4(1.0f);
	uint32_t mesh = InvalidIndex;
	uint32_t parent = InvalidIndex;
	uint32_t firstChild = InvalidIndex;
	uint32_t nextSibling = InvalidIndex;
};

struct Model
{
	std::vector<Image> images;
	std::vector<ModelSampler> samplers;
	std::vector<ModelTexture> textures;
	std::vector<Material> materials;
	std::vector<Mesh> meshes;
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<ModelNode> nodes;
	std::vector<uint32_t> rootNodes;
};

class ModelError : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

class ModelLoader
{
  public:
	Model loadGLTF(const std::filesystem::path& filepath);

  private:
	void loadImages(const tg3_model& model, const std::filesystem::path& imageDir, Model& loadedModel);
	void loadSamplers(const tg3_model& model, Model& loadedModel);
	void loadTextures(const tg3_model& model, Model& loadedModel);
	void loadMaterials(const tg3_model& model, Model& loadedModel);
	void loadMeshes(const tg3_model& model, Model& loadedModel);
	void importNodes(const tg3_model& model, Model& loadedModel);
};