#pragma once
#include "camera.h"
#include "controllers.h"
#include "models.h"
#include "renderer.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

class Application
{
  public:
	bool initialize();
	void run();
	void shutdown();

  private:
	void showError(const std::string& errorMessage);
	bool handleEvent(SDL_Event& event);

  private:
	static constexpr int DefaultWidth = 1280;
	static constexpr int DefaultHeight = 720;

	// Fifth of a second max delta time (5 fps, 200ms)
	static constexpr float MaxDeltaTime = 0.2f;

	SDL_Window* window = nullptr;
	Renderer renderer;

	Camera camera;
	InputState inputState;
	CameraController cameraController;

	ModelLoader modelLoader;

	bool running = false;
};