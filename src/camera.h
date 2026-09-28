#pragma once

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera
{
  public:
	void resize(const float aspect);
	void update(const bool* keys, const float deltaTime);

	const glm::mat4& getView() const
	{ return view; }
	const glm::mat4& getProjection() const
	{ return projection; }
	glm::mat4 getViewProjection() const
	{ return projection * view; }

  private:
	static constexpr float epsilon = 0.01f;
	static constexpr float pitchLimit = glm::half_pi<float>() - epsilon;

	static constexpr float fov = 75.0f;
	static constexpr float near = 0.01f;
	static constexpr float far = 1000.0f;

	float yaw = glm::half_pi<float>();
	float pitch = 0.0f;
	float distance = 3.0f;

	float sensitivityX = 1.0f;
	float sensitivityY = 1.0f;

	glm::vec3 position{};
	glm::mat4 view{};
	glm::mat4 projection{};
};