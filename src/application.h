#pragma once
#include "camera.h"
#include "models.h"
#include "renderer.h"

#include <SDL3/SDL.h>
#include <cstdint>
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
	static const uint32_t DEFAULT_WIDTH = 1280;
	static const uint32_t DEFAULT_HEIGHT = 720;

	SDL_Window* window = nullptr;
	Camera camera;
	Renderer renderer;

	ModelLoader modelLoader;

	bool running = false;
};