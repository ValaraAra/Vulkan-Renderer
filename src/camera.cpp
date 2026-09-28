#include "camera.h"

#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_scancode.h>

void Camera::resize(const float aspect)
{
	SDL_assert(aspect > 0.0f);
	projection = glm::perspectiveRH(glm::radians(fov), aspect, near, far);
}

void Camera::update(const bool* keys, const float deltaTime)
{
	// Yaw
	if (keys[SDL_SCANCODE_A]) { yaw += sensitivityX * deltaTime; }
	if (keys[SDL_SCANCODE_D]) { yaw -= sensitivityX * deltaTime; }

	// Pitch
	if (keys[SDL_SCANCODE_W])
	{
		pitch += sensitivityY * deltaTime;
		pitch = glm::clamp(pitch, -pitchLimit, pitchLimit);
	}
	if (keys[SDL_SCANCODE_S])
	{
		pitch -= sensitivityY * deltaTime;
		pitch = glm::clamp(pitch, -pitchLimit, pitchLimit);
	}

	// Zoom Speed
	float zoomSpeed = 1.0f;
	if (keys[SDL_SCANCODE_LSHIFT]) { zoomSpeed *= 3.0f; }
	if (keys[SDL_SCANCODE_RSHIFT]) { zoomSpeed *= 10.0f; }

	// Zoom
	if (keys[SDL_SCANCODE_UP])
	{
		distance -= zoomSpeed * deltaTime;
		distance = glm::max(distance, epsilon);
	}
	if (keys[SDL_SCANCODE_DOWN]) { distance += zoomSpeed * deltaTime; }

	position = glm::vec3(glm::cos(yaw) * glm::cos(pitch), glm::sin(pitch), glm::sin(yaw) * glm::cos(pitch)) * distance;
	view = glm::lookAtRH(position, glm::vec3(0), glm::vec3(0, 1, 0));
}