# vulkan-game

Minimal C++ Vulkan application using [vulkan-engine](https://github.com/xt9y/vulkan-engine)
through the C build system. No vendored engine sources, Vulkan shaders or game framework.

## Run

Install the [C build system](https://github.com/xt9y/build.c), a Vulkan loader/driver
and the GLFW build prerequisites for your platform. Then run:

```sh
c build run
```

The build downloads vulkan-engine, Vulkan-Headers and GLFW through the dependency
cache. A window opens and its background slowly changes color. Resize it or close it
with the window close button. Pressing Escape is not bound yet.

## Files

- `build.c` builds the executable and links the external VulkanEngine static library.
- `main.cpp` initializes GLFW and Vulkan, creates a surface and swapchain, submits
  one clear per frame, presents the image and releases resources on normal exit.

There is no ECS, model loader, lighting or renderer abstraction in this starter yet.
These can be added without moving VulkanEngine code into this repository.
