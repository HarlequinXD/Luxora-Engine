# Luxora Engine

A real-time 3D game engine written in **C11**, rendering with **Vulkan 1.3**, with a professional editor built on its own retained-mode UI toolkit. Luxora builds games for **Windows, Linux and Android** from one codebase.

| | |
|---|---|
| **Rendering** | Physically based (GGX/Smith/Schlick), cascaded shadow maps with PCF, HDR + ACES tonemapping, procedural sky, fog |
| **Global illumination** | *Virtual photons* traced from every light and the sky through a SAH BVH on all CPU cores, accumulated into an L1 spherical-harmonics irradiance volume (colour bleeding, sky occlusion, progressive convergence) |
| **Sound** | Ray-traced acoustics: real speed of sound (delay lines, natural Doppler), occlusion and transmission through materials, early reflections, RT60-driven reverb. WinMM / ALSA / AAudio |
| **Scale** | Automatic LOD chains, frustum culling, instanced batching (one draw per mesh/LOD/material), data-oriented multithreaded crowds |
| **Input** | Input Manager: named actions/axes bound to keyboard, mouse, gamepads (XInput, Linux joystick, Android) and on-screen touch controls |
| **Editor** | Project Manager, dockable panels, hierarchy, inspector, content browser, console, audio mixer, scene manager, gizmos, undo/redo, play mode, project/build settings, one-click builds |

## Getting started

```sh
git clone https://github.com/HarlequinXD/Luxora-Engine
cd Luxora-Engine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/luxora_editor            # Windows: build\Release\LuxoraEditor.exe
```

Requirements: a C11 compiler (MSVC 2022, GCC, Clang), CMake 3.20+, and a GPU driver with Vulkan 1.3 (or 1.2 with `VK_KHR_dynamic_rendering`). **No Vulkan SDK is needed to build.** The headers are vendored, the loader is opened at runtime, and shaders ship precompiled as SPIR-V. On Linux you also need the X11 development headers (`libx11-dev`).

The editor opens on the **Project Manager**. Create a project from a template (**3D Empty**, **3D Sample** or **Mobile 3D**), and every new scene starts with a **Main Camera**, a **Directional Light** and a **Cube**.

## Writing game code

Gameplay is written in C as *behaviours*. These are small scripts with `start`/`update` callbacks that you attach to entities in the inspector. Put `.c` files in your project's `Assets/Scripts/` folder; the editor's **Create → Script** gives you a template. Every file registers its behaviours:

```c
#include <luxora/luxora.h>

typedef struct Player { float speed; } Player;

static void player_update(LxBehaviourCtx *ctx, float dt) {
    LxVec2 move = lx_axis2("MoveX", "MoveY");            /* keyboard, gamepad or touch */
    LxVec3 p = lx_entity_position(ctx->scene, ctx->self);
    p.x += move.x * ctx->params[0] * dt;
    p.z -= move.y * ctx->params[0] * dt;
    lx_entity_set_position(ctx->scene, ctx->self, p);

    if (lx_action_pressed("Jump")) lx_audio_play_oneshot("Sounds/jump.wav", p, 1.0f);
}

LX_SCRIPT_REGISTER(player) {
    LxBehaviourDesc d = {0};
    d.name = "Player";
    d.state_size = sizeof(Player);
    d.param_names[0] = "Speed";          /* editable in the inspector */
    d.param_defaults[0] = 5.0f;
    d.on_update = player_update;
    lx_register_behaviour(&d);
}
```

The full API is in [`include/luxora/luxora.h`](include/luxora/luxora.h). It covers scene and entities, components, assets, rendering, GI, audio, crowds, input, projects and the app runtime.

## Input Manager

Open it from **Window → Input Manager**. New projects come with ready-made bindings:

| Name | Keyboard & mouse | Gamepad | Touch |
|---|---|---|---|
| `MoveX` / `MoveY` (axes) | WASD, arrow keys | Left stick | Move Stick |
| `LookX` / `LookY` (axes) | Mouse | Right stick | Look Area (swipe) |
| `Jump` | Space | A | Jump Button |
| `Fire` / `Aim` | Left / right mouse | RT / LT | Fire Button |
| `Interact`, `Sprint`, `Pause` | E, Shift, Esc | X, L-stick, Start | — |

You can add actions and axes, rebind them by pressing the input you want, set dead zones and sensitivity, and drag the touch controls around a phone-shaped preview. Bindings are saved to `input.lxinput` in the project folder.

## Building games

Use **Build → Build Settings** and choose a platform, then click **Build** or **Build and Run**:

- **Scripts**: the editor has a C compiler built in ([TinyCC](https://github.com/TinyCC/tinycc), linked in as a library). Every `.c` file under `Assets` is compiled whenever you save it (usually in a few milliseconds); errors appear in the Console with file and line, and script behaviours show up in Add Component and run in Play mode. Scripts can use the whole `luxora.h` API plus a small C library (`string.h`, `stdio.h` - `printf` writes to the Console - `stdlib.h`, `math.h`, `stdint.h`, ...). Nothing needs to be installed, and the editor download contains no engine source.
- **Windows / Linux**: the editor packages the game player that ships with it (`Runtime/<Platform>/`), stamps your game's name, version and icon into it, and adds your data plus `Data/scripts.lxo` - your scripts, compiled. Script sources are not shipped. You get `Builds/<Platform>/<Game>/` with the executable and a `Data/` folder. Each editor download builds games for its own platform (Windows games from the Windows editor).
- **Android**: for now this still compiles the engine, so it needs a Luxora engine source checkout (Build Settings → Engine Source) plus `ANDROID_HOME` (SDK with the NDK and CMake), JDK 17 and Gradle 8.7+. Luxora produces an APK and can install it over `adb`. Vulkan and AAudio require API 26+.

## Repository layout

```
include/luxora/   public API (luxora.h) + math library
src/core/         memory, files, logging, job system
src/platform/     Win32, X11, Android windowing + Vulkan surfaces
src/gfx/          Vulkan renderer, meshes/LODs, images (PNG/TGA/BMP), assets
src/gi/           BVH + virtual photon GI
src/audio/        mixer, acoustics, device backends
src/input/        Input Manager + gamepads + touch controls
src/crowd/        crowd simulation
src/scene/        entities, serialization, behaviours
src/project/      projects and settings
editor/           editor + Luxora UI toolkit
player/           runtime that ships with games
shaders/          GLSL sources (compiled with tools/compile_shaders.py)
android/          Gradle project for the Android player
tests/            headless test suite + GPU-less editor build
```

## Testing

- `luxora_tests` runs 79 headless checks covering math, jobs, scenes and serialization, LODs, PNG decoding, the BVH (cross-checked against brute force), GI colour bleeding, acoustics (speed of sound, occlusion, reflections), crowds, projects, WAV and input.
- `luxora_editor_nogpu --new <dir> <name> --smoke` runs a scripted tour of every editor window and tool without a GPU.
- CI (`.github/workflows/build.yml`) builds on Linux, Windows and Android. It also runs the real Vulkan editor and a packaged game on Mesa's software renderer with validation layers enabled, and uploads screenshots.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the systems work and what's next.

## Licence notes

The UI uses the Inter font (SIL Open Font License 1.1) and DejaVu Sans Mono. Vulkan headers are © The Khronos Group (Apache 2.0 / MIT). Licence texts are in `third_party/`.
