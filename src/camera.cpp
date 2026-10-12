#include "camera.h"

#include <SDL3/SDL_assert.h>
#include <glm/gtc/matrix_transform.hpp>

void Camera::setPerspective(const float newFovDeg, const float newAspect, const float newNearPlane, const float newFarPlane)
{
	SDL_assert(newFovDeg > 0.0f && newFovDeg < 180.0f);
	SDL_assert(newAspect > 0.0f);
	SDL_assert(newNearPlane > 0.0f && newFarPlane > newNearPlane);

	fovDeg = newFovDeg;
	aspect = newAspect;
	nearPlane = newNearPlane;
	farPlane = newFarPlane;

	updateProjection();
}

void Camera::setAspect(const float newAspect)
{
	if (aspect == newAspect) { return; }

	SDL_assert(newAspect > 0.0f);
	aspect = newAspect;

	updateProjection();
}

void Camera::setPose(const glm::vec3& newPosition, const glm::quat& newOrientation)
{
	position = newPosition;
	orientation = glm::normalize(newOrientation);

	// Inverse of camera world transform
	view = glm::mat4_cast(glm::conjugate(orientation)) * glm::translate(glm::mat4(1.0f), -position);
}

void Camera::updateProjection()
{ projection = glm::perspectiveRH_ZO(glm::radians(fovDeg), aspect, nearPlane, farPlane); }