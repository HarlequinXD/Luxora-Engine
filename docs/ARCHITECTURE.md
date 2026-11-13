# Luxora architecture and roadmap

This document explains how each system works today, its real limits, and what comes next. It is deliberately candid about where the current implementation stands against the long-term goals.

## Frame flow

```
platform pump → input update → scene simulate (behaviours, crowds) → audio update
→ renderer.begin_frame → render_scene (GI update, cull, shadows, PBR, tonemap) → UI → end_frame
```

Two frames are in flight. Per-frame resources (UBO slices, the instance stream, UI buffers and GI staging) are ring-buffered, so the CPU never writes data the GPU is still reading.

## Rendering (Vulkan 1.3)

- **Dynamic rendering** with no render-pass objects, and explicit image barriers.
- **Batching**: each visible mesh instance (entities and crowd agents) is culled against the frustum. Its LOD is picked from projected size, and it is appended to a batch keyed by (mesh, LOD, material). Each batch is a single `vkCmdDrawIndexed` with instancing.
- **Shadows**: up to four cascades, practical split scheme, bounding spheres, texel snapping to remove shimmer, 3×3 PCF on a `sampler2DArrayShadow`.
- **Shading**: GGX + Smith + Schlick Cook-Torrance. Normal maps use per-vertex tangents. Indirect diffuse comes from the GI volume; indirect specular is an analytic env-BRDF on the sky, attenuated by GI occlusion.
- **Shaders** are GLSL 450 compiled to SPIR-V by `tools/compile_shaders.py`. The result is committed, so builds need no shader compiler.

## Virtual photon global illumination

1. Static meshes are flattened into a triangle BVH (binned SAH, 4 triangles per leaf).
2. Each frame, `gi_photons_per_frame` photons are distributed over the lights and the sky in proportion to their power. Directional power is irradiance × projected scene area; point is 4πI; spot is 2π(1−cos θ)I; sky is πL×area.
3. Photons bounce diffusely with Russian roulette by albedo. Light photons deposit **after** their first bounce, because direct light is shaded analytically. Sky photons deposit on every hit, which yields sky occlusion.
4. Deposits are splatted trilinearly into an L1 SH irradiance volume, in parallel z-slabs so no locks are needed.
5. **Progressive accumulation**: while the lights and sky are unchanged, every frame gets equal weight (a running average over up to 1,024 frames), so Monte Carlo noise keeps falling roughly as 1/√frames. A hash of all emitters detects changes and shortens the history to `1 / gi_responsiveness` frames, so moving lights are followed quickly before the noise settles again.
6. A separable [1,2,1] filter on the output removes the remaining cell-sized blotches.

The tests check this: a red wall visibly tints the irradiance seen by surfaces facing it.

**Limits today:** the tracing runs on the CPU, the volume is a single uniform grid, and it covers static geometry only.
**Next:** GPU photon tracing with compute shaders and ray queries where available, cascaded or sparse volumes, specular bounce, and dynamic-object probes.

## Acoustics

- Every voice keeps a 1.36 s history ring. The direct path is read back with a delay of distance ÷ speed of sound, interpolated smoothly, which produces Doppler naturally.
- **Occlusion**: each blocking surface on the straight path multiplies by its material's `acoustic_transmission` and closes a low-pass filter.
- **Reflections**: Fibonacci-sphere rays bounce through the BVH, losing `acoustic_absorption` and air absorption at each hit. Wherever the listener is visible, a tap is added with the full path delay and direction. The strongest 24 taps feed a tapped delay line.
- **Reverb**: the ray energy histogram gives an RT60 (Schroeder-style fit), which drives a 4-line feedback delay network.

**Next:** diffraction around edges, frequency-dependent absorption, HRTF binaural output, and a convolution path for important sources.

## "Billions of polygons"

Here is how that goal maps onto the engine:

- **Today:** automatic LOD chains (vertex clustering, up to 4 levels), frustum culling, and instanced batching. One mesh drawn 100,000 times costs one draw call per LOD, and distant copies use reduced LODs. That lets scenes *represent* very high polygon counts while drawing far fewer.
- **Next (planned):** GPU-driven rendering.
  1. Split meshes into ~128-triangle meshlets with bounding cones and spheres.
  2. Cull meshlets in a compute shader (frustum + Hi-Z occlusion), writing `vkCmdDrawIndexedIndirectCount` arguments.
  3. Use a cluster LOD DAG for continuous, crack-free detail (virtualised geometry), with mesh shaders where supported (`VK_EXT_mesh_shader`).
  4. Stream geometry pages asynchronously.

No engine can rasterise billions of unique triangles per frame on a phone. Virtualised geometry works by drawing only what covers pixels, and that is where this roadmap leads.

## Editor and UI toolkit

The editor UI is Luxora's own retained-mode toolkit (`editor/ui.c`); no third-party UI library is used.

- Widgets are persistent nodes with flex-style layout, scroll and wrap containers, splitters, tabs, popups, modals, tooltips, toasts, drag-and-drop and text editing.
- Values bind by pointer, so panels always show live data.
- Panels rebuild only when their inputs change: hierarchy hash, selected entity, component mask, or scene storage.
- Undo and redo snapshot the whole scene in its text format. This is simple and always correct, and an edit gesture (such as a drag) is one undo step.
- Play mode snapshots the scene and restores it on Stop.

**Next:**
- Free docking with drag-to-split tabs and saved layouts
- Prefabs
- Material graph
- Hot-reloading scripts in the editor (scripts currently run in builds and through built-in behaviours in the editor)
- Animation and physics

## Platforms

| | Window | Graphics | Audio | Gamepad |
|---|---|---|---|---|
| Windows | Win32 | Vulkan (`vulkan-1.dll`) | WinMM | XInput |
| Linux | X11 | Vulkan (`libvulkan.so.1`) | ALSA (dlopen) | `/dev/input/js*` |
| Android | NativeActivity | Vulkan | AAudio | Android input events |

**Next:** Wayland, WASAPI, PipeWire, swapchain pre-rotation on Android, and ASTC texture compression.
