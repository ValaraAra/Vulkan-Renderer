#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

inline constexpr uint32_t FallbackIndex = 0;
inline constexpr uint32_t InvalidIndex = UINT32_MAX;

struct Image
{
	int width;
	int height;
	int channels;
	unsigned char* data;
};

struct Texture
{
	uint32_t imageID = FallbackIndex;
	uint32_t samplerID = FallbackIndex;
};

struct Material
{
	glm::vec4 baseColor = glm::vec4(1.0f);
	uint32_t textureID = FallbackIndex;
};

struct Vertex
{
	glm::vec3 position = glm::vec3(0.0f);
	glm::vec3 color = glm::vec3(1.0f);
	glm::vec3 normal = glm::vec3(0.0f);
	glm::vec2 uv = glm::vec2(0.0f);
};

struct SubMesh
{
	size_t vertexStart = 0;
	size_t vertexCount = 0;
	size_t indexStart = 0;
	size_t indexCount = 0;
	uint32_t materialID = FallbackIndex;
};

struct Mesh
{
	std::string name;
	std::vector<SubMesh> subMeshes;
};
