#include "application.h"

#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <tracy/Tracy.hpp>

bool Application::initialize()
{
	// SDL initialization
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
	{
		showError("SDL initialization failed! " + std::string(SDL_GetError()));
		return false;
	}

	// SDL window creation
	window = SDL_CreateWindow("Vulkan Renderer", DEFAULT_WIDTH, DEFAULT_HEIGHT, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
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
		renderer.loadData(std::filesystem::path{ASSET_DIR} / "models/gltf/sponza-unity-remaster/scene.gltf");
	}
	catch (const RenderError& error)
	{
		showError("Scene loading failed!\n\n" + std::string(error.what()));
		return false;
	}

	return true;
}

void Application::run()
{
	// Get key state and start time
	const bool* keys = SDL_GetKeyboardState(nullptr);
	uint64_t previousTime = SDL_GetTicks();

	// Aspect ratio
	float prevAspect = 0.0f;

	// Game loop
	running = true;
	while (running)
	{
		ZoneScopedN("Tick");

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

		// Get window size
		int windowWidth, windowHeight;
		SDL_GetWindowSizeInPixels(window, &windowWidth, &windowHeight);

		// Window size validity (minimized or resized to 0 width/height)
		const bool validSize = windowWidth > 0 && windowHeight > 0 && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED);

		// Skip rendering if the window doesn't have a valid size
		if (!validSize)
		{
			if (SDL_WaitEventTimeout(&event, 100)) { handleEvent(event); }
			if (!running) { break; }

			continue;
		}

		// Resize camera if aspect changed
		const float aspect = windowWidth / static_cast<float>(windowHeight);
		if (prevAspect != aspect)
		{
			camera.resize(aspect);
			prevAspect = aspect;
		}

		// Update time
		uint64_t currentTime = SDL_GetTicks();
		const float deltaTime = (currentTime - previousTime) / 1000.0f;
		previousTime = currentTime;

		// Update camera
		camera.update(keys, deltaTime);

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

	return true;
}