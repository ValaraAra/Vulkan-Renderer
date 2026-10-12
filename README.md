# Vulkan Renderer
Made to explore modern Vulkan 1.4 rendering techniques, coming from OpenGL. Loads and renders glTF scenes, focusing on dynamic rendering, timeline semaphores, and bindless resources.

The plan is to grow this into an optimized engine handling both voxel terrain and conventional meshes, picking up where my [OpenGL renderer](https://github.com/ValaraAra/Voxel-Engine) left off.

![Rendered Sponza Screenshot](screenshot.jpg "Rendered Sponza scene screenshot.")

## Features
- Resources
	- Bindless Textures (Descriptor Indexing)
	- Buffer Device Address
		- Vertices, Transforms, and Materials Fetched in the Vertex Shader
	- VMA Memory Allocation
- Rendering
	- Dynamic Rendering
	- Vertex Pulling
	- Multi-Draw-Indirect
		- Single Indirect Draw Call
- Synchronization
	- Timeline Semaphores for Frames-in-Flight
	- Synchronization2 Barriers
- Assets
	- glTF Loading (tinygltf)

## Controls
- General
	- Left Click: Capture mouse and enable input
	- Esc: Release mouse and disable input
	- V: Toggle camera mode (orbit/first person)
- Orbit Camera
	- WASD: Rotate around the target (yaw/pitch)
	- Mouse Scroll: Zoom toward/away from the target
- First Person Camera
	- WASD: Move relative to view direction
	- Space/LCtrl: Move along world up axis (up/down)
	- Mouse: Rotate view (yaw/pitch)

# Acknowledgements
Learned from the wonderful Vulkan YouTube series by [constref](https://www.youtube.com/@constref1983) and the well-known guide and samples by [Sascha Willems](https://github.com/SaschaWillems/Vulkan).