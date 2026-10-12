#pragma once

#include "axes.h"

#include <glm/gtc/quaternion.hpp>

class Camera
{
  public:
	Camera()
	{
		updateProjection();
		setPose(position, orientation);
	}

	void setPerspective(const float newFovDeg, const float newAspect, const float newNearPlane, const float newFarPlane);
	void setAspect(const float newAspect);
	void setPose(const glm::vec3& newPosition, const glm::quat& newOrientation);

	const glm::vec3& getPosition() const
	{ return position; }
	const glm::quat& getOrientation() const
	{ return orientation; }
	glm::vec3 getForward() const
	{ return orientation * Direction::Forward; }

	float getNearPlane() const
	{ return nearPlane; }
	float getFarPlane() const
	{ return farPlane; }

	const glm::mat4& getView() const
	{ return view; }
	const glm::mat4& getProjection() const
	{ return projection; }
	glm::mat4 getViewProjection() const
	{ return projection * view; }

  private:
	void updateProjection();

	float fovDeg = 75.0f;
	float aspect = 16.0f / 9.0f;
	float nearPlane = 0.01f;
	float farPlane = 1000.0f;

	glm::quat orientation = glm::identity<glm::quat>();
	glm::vec3 position{};
	glm::mat4 view{};
	glm::mat4 projection{};
};