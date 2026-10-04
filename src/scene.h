#pragma once

#include "resources.h"

#include <cassert>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

class Node
{
  public:
	uint32_t meshID = InvalidIndex;
	uint32_t parentID = InvalidIndex;
	uint32_t nextSiblingID = InvalidIndex;
	uint32_t firstChildID = InvalidIndex;

	glm::vec3 getTranslation() const
	{ return translation; }
	void setTranslation(const glm::vec3& newTranslation)
	{
		translation = newTranslation;
		dirty = true;
	}

	glm::vec3 getScale() const
	{ return scale; }
	void setScale(const glm::vec3& newScale)
	{
		scale = newScale;
		dirty = true;
	}

	glm::quat getRotation() const
	{ return rotation; }
	void setRotation(const glm::quat& newRotation)
	{
		rotation = newRotation;
		dirty = true;
	}

	glm::mat4 getTransform()
	{
		// Recalculate local matrix transform if dirty
		if (dirty)
		{
			glm::mat4 matrixTranslate = glm::translate(glm::mat4(1.0f), translation);
			glm::mat4 matrixRotate = glm::mat4_cast(rotation);
			glm::mat4 matrixScale = glm::scale(glm::mat4(1.0f), scale);
			transform = matrixTranslate * matrixRotate * matrixScale;
			dirty = false;
		}

		return transform;
	}
	void setTransform(const glm::mat4& newTransform)
	{
		glm::vec3 skew;
		glm::vec4 perspective;
		glm::decompose(newTransform, scale, rotation, translation, skew, perspective);

		transform = newTransform;
		dirty = false;
	}

  private:
	glm::vec3 translation = glm::vec3(0.0f);
	glm::vec3 scale = glm::vec3(1.0f);
	glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::mat4 transform = glm::mat4(1.0f);

	bool dirty = true;
};

class Scene
{
  public:
	void initialize(const size_t initialNodes)
	{ nodes.reserve(initialNodes); }

	std::pair<Node&, uint32_t> createNode()
	{
		nodes.push_back(Node{});
		return {nodes.back(), static_cast<uint32_t>(nodes.size() - 1)};
	}

	Node& getNode(uint32_t nodeID)
	{
		assert(nodeID < nodes.size() && "Invalid node ID!");
		return nodes[nodeID];
	}

	size_t size() const
	{ return nodes.size(); }

  private:
	std::vector<Node> nodes;
};
