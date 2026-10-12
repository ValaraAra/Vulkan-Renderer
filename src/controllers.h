#pragma once

#include "camera.h"

#include <glm/glm.hpp>
#include <variant>

struct InputState
{
	glm::vec3 moveAxis{0.0f};
	glm::vec2 lookDelta{0.0f};
	float scrollDelta = 0.0f;
	bool shift = false;
};

// WASD orbits the target at specified distance, mouse scroll zooms
struct OrbitController
{
	glm::vec3 target{0.0f};

	float yaw = 0.0f;
	float pitch = 0.0f;
	float distance = 3.0f;

	float zoomRate = 0.1f;
	float rotationSpeed = 1.0f;

	OrbitController() = default;
	explicit OrbitController(const Camera& camera);
};

// WASD moves around, Space/LCtrl moves up/down, mouse looks around
struct FirstPersonController
{
	glm::vec3 position{0.0f};

	float yaw = 0.0f;
	float pitch = 0.0f;

	float moveSpeed = 2.0f;
	float yawSensitivity = 0.005f;
	float pitchSensitivity = 0.005f;

	FirstPersonController() = default;
	explicit FirstPersonController(const Camera& camera);
};

using CameraController = std::variant<OrbitController, FirstPersonController>;

void updateController(OrbitController& controller, Camera& camera, const InputState& input, float deltaTime);
void updateController(FirstPersonController& controller, Camera& camera, const InputState& input, float deltaTime);
