#include "controllers.h"

#include "axes.h"

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
	controller.yaw += input.moveAxis.x * controller.rotationSpeed * deltaTime;

	// Pitch
	controller.pitch -= input.moveAxis.z * controller.rotationSpeed * deltaTime;
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
	controller.yaw -= input.lookDelta.x * controller.yawSensitivity;

	// Pitch
	controller.pitch -= input.lookDelta.y * controller.pitchSensitivity;
	controller.pitch = glm::clamp(controller.pitch, -PitchLimit, PitchLimit);

	// Get updated orientation
	const glm::quat orientation = quatFromYawPitch(controller.yaw, controller.pitch);

	// Movement
	const glm::vec3 forward = orientation * Direction::Forward;
	const glm::vec3 right = orientation * Direction::Right;

	const glm::vec3 movement = right * input.moveAxis.x + Direction::Up * input.moveAxis.y + forward * input.moveAxis.z;
	const float speed = controller.moveSpeed * (input.shift ? 3.0f : 1.0f);

	if (glm::dot(movement, movement) > 0.0f) { controller.position += glm::normalize(movement) * speed * deltaTime; }

	// Update Camera
	camera.setPose(controller.position, orientation);
}