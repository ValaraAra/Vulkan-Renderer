#pragma once

#include "camera.h"

#include <glm/glm.hpp>
#include <variant>

struct InputState
{
	const bool* keys = nullptr;
	glm::vec2 mouseDelta{0.0f};
	float scrollDelta = 0.0f;
};

// WASD orbits the target at specified distance, mouse scroll zooms
struct OrbitController
{
	glm::vec3 target{0.0f};

	float yaw = 0.0f;
	float pitch = 0.0f;
	float distance = 3.0f;

	float zoomRate = 0.1f;
	float roationSpeed = 1.0f;

	OrbitController() = default;
	explicit OrbitController(const Camera& camera);
};

// WASD moves around, Space/LCtrl moves up/down, mouse looks around
struct FirstPersonController
{
	glm::vec3 position{0.0f};

	float yaw = 0.0f;
	float pitch = 0.0f;

	float moveSpeed = 1.0f;
	float yawSensitivity = 0.01f;
	float pitchSensitivity = 0.01f;

	FirstPersonController() = default;
	explicit FirstPersonController(const Camera& camera);
};

using CameraController = std::variant<OrbitController, FirstPersonController>;

void updateController(OrbitController& controller, Camera& camera, const InputState& input, float deltaTime);
void updateController(FirstPersonController& controller, Camera& camera, const InputState& input, float deltaTime);
