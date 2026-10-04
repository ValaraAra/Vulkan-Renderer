cmake_minimum_required(VERSION 3.28)
project(VulkanRenderer)

# Options
option(PROFILING "Enable Tracy profiling." OFF)
option(PROFILING_ON_DEMAND "Only start profiling when a Tracy connection is established." OFF)
option(COPY_RESOURCES "Copy shaders/assets for release builds." OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find packages
find_package(SDL3 REQUIRED)
find_package(Vulkan REQUIRED COMPONENTS volk shaderc_combined)
find_package(glm REQUIRED)

# Fetch content
include(FetchContent)

FetchContent_Declare(
	tracy
	GIT_REPOSITORY https://github.com/wolfpld/tracy.git
	GIT_TAG v0.14.1
	GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(tracy)

# Set resource path variables
if(COPY_RESOURCES)
	set(SHADER_DIR_VALUE "shaders")
	set(ASSET_DIR_VALUE "assets")
else()
	set(SHADER_DIR_VALUE "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
	set(ASSET_DIR_VALUE "${CMAKE_CURRENT_SOURCE_DIR}/assets")
endif()

# External
add_library(external STATIC "${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf.cpp")
target_include_directories(external PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external")

# Main app
add_executable(vulkanapp
	"${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/application.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/application.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/renderer.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/renderer.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/utility.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/utility.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/camera.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/camera.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/models.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/models.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/resources.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/scene.h"
)

target_compile_features(vulkanapp PRIVATE cxx_std_23)

if (MSVC)
	target_compile_options(vulkanapp PRIVATE /W4)
else()
	target_compile_options(vulkanapp PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(vulkanapp PRIVATE
	VK_NO_PROTOTYPES
	GLM_FORCE_DEPTH_ZERO_TO_ONE
	SHADER_DIR="${SHADER_DIR_VALUE}"
	ASSET_DIR="${ASSET_DIR_VALUE}"
)

if(PROFILING)
	target_compile_definitions(TracyClient PUBLIC TRACY_ENABLE)

	if(PROFILING)
		target_compile_definitions(TracyClient PUBLIC TRACY_ON_DEMAND)
	endif()
endif()

target_link_libraries(vulkanapp PRIVATE
	external
	Vulkan::volk
	Vulkan::shaderc_combined
	SDL3::SDL3
	glm::glm
	TracyClient
)

# Copy resources to build directory
if (COPY_RESOURCES)
	file(GLOB_RECURSE SHADERS_RESOURCE_FILES CONFIGURE_DEPENDS
		LIST_DIRECTORIES false
		"${CMAKE_CURRENT_SOURCE_DIR}/shaders/*"
	)
	
	file(GLOB_RECURSE ASSETS_RESOURCE_FILES CONFIGURE_DEPENDS
		LIST_DIRECTORIES false
		"${CMAKE_CURRENT_SOURCE_DIR}/assets/*"
	)
	
	set(SHADERS_RESOURCE_STAMP "${CMAKE_BINARY_DIR}/shaders_resources.stamp")
	set(ASSETS_RESOURCE_STAMP "${CMAKE_BINARY_DIR}/assets_resources.stamp")
	
	add_custom_command(OUTPUT "${SHADERS_RESOURCE_STAMP}"
		DEPENDS ${SHADERS_RESOURCE_FILES}
		COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:vulkanapp>/shaders"
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/shaders"
			"$<TARGET_FILE_DIR:vulkanapp>/shaders"
		COMMAND ${CMAKE_COMMAND} -E touch "${SHADERS_RESOURCE_STAMP}"
	)
	
	add_custom_command(OUTPUT "${ASSETS_RESOURCE_STAMP}"
		DEPENDS ${ASSETS_RESOURCE_FILES}
		COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:vulkanapp>/assets"
		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/assets"
			"$<TARGET_FILE_DIR:vulkanapp>/assets"
		COMMAND ${CMAKE_COMMAND} -E touch "${ASSETS_RESOURCE_STAMP}"
	)
	
	add_custom_target(vulkanapp_shaders DEPENDS "${SHADERS_RESOURCE_STAMP}")
	add_custom_target(vulkanapp_assets DEPENDS "${ASSETS_RESOURCE_STAMP}")
	
	add_dependencies(vulkanapp vulkanapp_shaders vulkanapp_assets)
endif()