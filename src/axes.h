#pragma once

#include <glm/vec3.hpp>

namespace Axis
{
inline constexpr glm::vec3 X{glm::vec3(1.0f, 0.0f, 0.0f)};
inline constexpr glm::vec3 Y{glm::vec3(0.0f, 1.0f, 0.0f)};
inline constexpr glm::vec3 Z{glm::vec3(0.0f, 0.0f, 1.0f)};
} // namespace Axis

namespace Direction
{
inline constexpr glm::vec3 Right = Axis::X;
inline constexpr glm::vec3 Left = -Axis::X;
inline constexpr glm::vec3 Up = Axis::Y;
inline constexpr glm::vec3 Down = -Axis::Y;
inline constexpr glm::vec3 Forward = -Axis::Z;
inline constexpr glm::vec3 Backward = Axis::Z;
} // namespace Direction