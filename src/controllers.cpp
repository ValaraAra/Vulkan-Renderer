#include "controllers.h"

#include "axes.h"

#include <SDL3/SDL_scancode.h>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{
struct YawPitch
{
	float yaw = 0.0f;
	float pitch = 0.0f;
};

constexpr float PitchLimit = glm::half_pi<float>() - 0.01f;
constexpr float MinDistance = 0.5f;

glm::quat quatFromYawPitch(float yaw, float pitch)
{
	const glm::quat quatYaw = glm::angleAxis(yaw, Axis::Y);
	const glm::quat quatPitch = glm::angleAxis(pitch, Axis::X);

	return quatYaw * quatPitch;
}

YawPitch yawPitchFromQuat(const glm::quat& quat)
{
	const glm::vec3 forward = quat * Direction::Forward;

	return {
		.yaw = std::atan2(-forward.x, -forward.z),
		.pitch = glm::clamp(std::asin(glm::clamp(forward.y, -1.0f, 1.0f)), -PitchLimit, PitchLimit),
	};
}
} // namespace

// Orbit Controller
OrbitController::OrbitController(const Camera& camera)
{
	YawPitch yawPitch = yawPitchFromQuat(camera.getOrientation());
	yaw = yawPitch.yaw;
	pitch = yawPitch.pitch;

	target = camera.getPosition() + camera.getForward() * distance;
}

void updateController(OrbitController& controller, Camera& camera, const InputState& input, float deltaTime)
{
	// Yaw
	if (input.keys[SDL_SCANCODE_A]) { controller.yaw -= controller.roationSpeed * deltaTime; }
	if (input.keys[SDL_SCANCODE_D]) { controller.yaw += controller.roationSpeed * deltaTime; }

	// Pitch
	if (input.keys[SDL_SCANCODE_W]) { controller.pitch -= controller.roationSpeed * deltaTime; }
	if (input.keys[SDL_SCANCODE_S]) { controller.pitch += controller.roationSpeed * deltaTime; }

	controller.pitch = glm::clamp(controller.pitch, -PitchLimit, PitchLimit);

	// Zoom
	controller.distance *= std::exp(-input.scrollDelta * controller.zoomRate);
	controller.distance = glm::max(controller.distance, MinDistance);
	controller.distance = glm::min(controller.distance, camera.getFarPlane());

	// Update Camera
	const glm::quat orientation = quatFromYawPitch(controller.yaw, controller.pitch);
	const glm::vec3 forward = orientation * Direction::Forward;

	camera.setPose(controller.target - forward * controller.distance, orientation);
}

// First Person Controller
FirstPersonController::FirstPersonController(const Camera& camera)
{
	position = camera.getPosition();

	YawPitch yawPitch = yawPitchFromQuat(camera.getOrientation());
	yaw = yawPitch.yaw;
	pitch = yawPitch.pitch;
}

void updateController(FirstPersonController& controller, Camera& camera, const InputState& input, float deltaTime)
{
	// Yaw
	controller.yaw -= input.mouseDelta.x * controller.yawSensitivity;

	// Pitch
	controller.pitch -= input.mouseDelta.y * controller.pitchSensitivity;
	controller.pitch = glm::clamp(controller.pitch, -PitchLimit, PitchLimit);

	// Get updated orientation
	const glm::quat orientation = quatFromYawPitch(controller.yaw, controller.pitch);

	// Movement
	const glm::vec3 forward = orientation * Direction::Forward;
	const glm::vec3 right = orientation * Direction::Right;

	glm::vec3 movement{0.0f};

	if (input.keys[SDL_SCANCODE_D]) { movement += right; }
	if (input.keys[SDL_SCANCODE_A]) { movement -= right; }
	if (input.keys[SDL_SCANCODE_W]) { movement += forward; }
	if (input.keys[SDL_SCANCODE_S]) { movement -= forward; }
	if (input.keys[SDL_SCANCODE_SPACE]) { movement += Direction::Up; }
	if (input.keys[SDL_SCANCODE_LCTRL]) { movement -= Direction::Up; }

	if (glm::dot(movement, movement) > 0.0f)
	{
		controller.position += glm::normalize(movement) * controller.moveSpeed * deltaTime;
	}

	// Update Camera
	camera.setPose(controller.position, orientation);
}