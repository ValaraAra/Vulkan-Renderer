#include "application.h"

#include <filesystem>
#include <stb_image.h>
#include <stdexcept>
#include <tracy/Tracy.hpp>
#include <variant>

bool Application::initialize()
{
	// SDL initialization
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
	{
		showError("SDL initialization failed! " + std::string(SDL_GetError()));
		return false;
	}

	// SDL window creation
	window = SDL_CreateWindow("Vulkan Renderer", DefaultWidth, DefaultHeight, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	if (!window)
	{
		showError("SDL window creation failed! " + std::string(SDL_GetError()));
		return false;
	}

	// Renderer initialization
	try
	{
		renderer.initialize(window);
	}
	catch (const RenderError& error)
	{
		showError("Renderer initialization failed!\n\n" + std::string(error.what()));
		return false;
	}

	// Load scene
	try
	{
		const std::filesystem::path path = std::filesystem::path{ASSET_DIR} / "models/gltf/sponza-unity-remaster/scene.gltf";

		Model model = modelLoader.loadGLTF(path);
		renderer.loadModel(model);

		// Free model image mem (give image a destructor at some point, not really safe currently)
		for (Image& image : model.images)
		{
			stbi_image_free(image.data);
		}
	}
	catch (const std::runtime_error& error)
	{
		showError("Scene loading failed!\n\n" + std::string(error.what()));
		return false;
	}

	return true;
}

void Application::run()
{
	// Set start time
	uint64_t previousTime = SDL_GetTicksNS();

	running = true;
	while (running)
	{
		ZoneScopedN("Tick");

		// Update time
		uint64_t currentTime = SDL_GetTicksNS();
		const float deltaTime = std::min((currentTime - previousTime) / 1e9f, MaxDeltaTime);
		previousTime = currentTime;

		// Reset scroll delta
		scrollDelta = 0.0f;

		// Handle events
		SDL_Event event{0};
		{
			ZoneScopedN("Events");

			while (SDL_PollEvent(&event))
			{
				if (!handleEvent(event)) { break; }
			}

			if (!running) { break; }
		}

		// Camera input state
		glm::vec2 mouseDelta{0.0f};
		SDL_GetRelativeMouseState(&mouseDelta.x, &mouseDelta.y);

		InputState inputState{};
		if (SDL_GetWindowRelativeMouseMode(window))
		{
			const bool* keys = SDL_GetKeyboardState(nullptr);

			inputState.moveAxis.x = static_cast<float>(keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]);
			inputState.moveAxis.y = static_cast<float>(keys[SDL_SCANCODE_SPACE] - keys[SDL_SCANCODE_LCTRL]);
			inputState.moveAxis.z = static_cast<float>(keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]);

			inputState.lookDelta = mouseDelta;
			inputState.scrollDelta = scrollDelta;

			inputState.shift = keys[SDL_SCANCODE_LSHIFT];
		}

		// Get window size
		int windowWidth, windowHeight;
		SDL_GetWindowSizeInPixels(window, &windowWidth, &windowHeight);

		// Window size validity (minimized or resized to 0 width/height)
		const bool validSize = windowWidth > 0 && windowHeight > 0 && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED);

		// Skip rendering if the window doesn't have a valid size
		if (!validSize)
		{
			// Exclude skipped time from deltaTime
			previousTime = SDL_GetTicksNS();

			// Sleep for 100ms or until an event arrives
			SDL_WaitEventTimeout(nullptr, 100);

			continue;
		}

		// Update camera
		camera.setAspect(windowWidth / static_cast<float>(windowHeight));

		// Update controller
		std::visit([&](auto& controller) { updateController(controller, camera, inputState, deltaTime); }, cameraController);

		// Render
		try
		{
			renderer.render(camera.getViewProjection());
		}
		catch (const RenderError& error)
		{
			showError("Rendering failed!\n\n" + std::string(error.what()));

			running = false;
			break;
		}

		// Profile
		FrameMark;
	}
}

void Application::shutdown()
{
	renderer.shutdown();

	if (window) { SDL_DestroyWindow(window); }

	SDL_Quit();
}

// SDL error message box
void Application::showError(const std::string& errorMessage)
{ SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", errorMessage.c_str(), window); }

// Returns false if further event handling should stop early
bool Application::handleEvent(SDL_Event& event)
{
	if (event.type == SDL_EVENT_QUIT)
	{
		running = false;
		return false;
	}

	if (event.type == SDL_EVENT_WINDOW_RESIZED)
	{
		renderer.invalidateSwapchain();
		return true;
	}

	// Clicking into the window captures the mouse
	if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
	{
		SDL_SetWindowRelativeMouseMode(window, true);
		return true;
	}

	// Input disabled unless mouse is captured
	if (SDL_GetWindowRelativeMouseMode(window))
	{
		// Mouse scrolling delta
		if (event.type == SDL_EVENT_MOUSE_WHEEL)
		{
			scrollDelta += event.wheel.y;
			return true;
		}

		// Escape frees the mouse
		if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE)
		{
			SDL_SetWindowRelativeMouseMode(window, false);
			return true;
		}

		// V toggles between orbit and first person camera controls
		if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_V)
		{
			if (std::holds_alternative<OrbitController>(cameraController))
			{
				cameraController.emplace<FirstPersonController>(camera);
			}
			else
			{
				cameraController.emplace<OrbitController>(camera);
			}
			return true;
		}
	}

	return true;
}