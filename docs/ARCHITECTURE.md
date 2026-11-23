# Luxora architecture and roadmap

This document explains how each system works today, its real limits, and what comes next. It is deliberately candid about where the current implementation stands against the long-term goals.

## Frame flow

```
platform pump → input update → scene simulate (behaviours, crowds) → audio update
→ renderer.begin_frame → render_scene (GI update, cull, shadows, PBR, AO, bloom, sun shafts, final image) → UI → end_frame
```

Two frames are in flight. Per-frame resources (UBO slices, the instance stream, UI buffers and GI staging) are ring-buffered, so the CPU never writes data the GPU is still reading.

## Rendering (Vulkan 1.3)

- **Dynamic rendering** with no render-pass objects, and explicit image barriers.
- **Batching**: each visible mesh instance (entities and crowd agents) is culled against the frustum. Its LOD is picked from projected size, and it is appended to a batch keyed by (mesh, LOD, material). Each batch is a single `vkCmdDrawIndexed` with instancing.
- **Lightmap UVs** (`src/gfx/lightmap_uv.c`): every static mesh carries a second UV set (`u2, v2`) for baked lighting. Models use their own second UV set when they have one (import setting *Lightmap UVs*: Auto / Generate / Model's Own); otherwise the mesh is unwrapped automatically. Charts are grown across shared edges within 45° of their first triangle, small charts are merged into neighbours, and any chart that folds over itself when flattened is grown again with a tighter angle. Charts are turned to their tightest box and shelf-packed with 2 texels of padding at one scale for the whole mesh, so texel density is even. Vertices are split on chart borders. The unwrap is deterministic; 1M triangles take about 1.6 s.
- **Lightmapper** (`src/gi/lightmapper.c`, `shaders/lm_bake.comp`): each Static object with lightmap UVs gets a square tile in a lightmap atlas, sized by its surface area and the scene's texels per unit. Every texel is placed on its mesh (closest point of the nearest triangle, so edge texels are covered) and path traced through the static-geometry BVH: Baked lights straight onto the texel (with soft shadows from the light's radius or the sun's size), then a path collecting sky light, glow and the light of Mixed and Baked lights at every bounce. Texels that mostly see the back of geometry are inside something; they are thrown away and filled from their neighbours. Results are irradiance plus the main light direction (a Unity-style directional map); a joint bilateral filter guided by normals, positions and luminance denoises. Atlases are saved as half-float `.lxlm` files under `Assets/Lightmaps/<scene>/`, and each object stores its tile. One sample per texel per pass, on the GPU (compute, one thread per texel) or on all CPU cores with the same code; the editor bakes a slice per frame. In shading, lightmapped surfaces take their diffuse indirect (and all of a Baked light's light) from the lightmap; Mixed lights still light them in real time.
- **Shadows**: up to four cascades, practical split scheme, bounding spheres, texel snapping to remove shimmer, 3×3 PCF on a `sampler2DArrayShadow`.
- **Shading**: GGX + Smith + Schlick Cook-Torrance. Normal maps use per-vertex tangents. Indirect diffuse comes from the GI volume; indirect specular is an analytic env-BRDF on the sky, attenuated by GI occlusion.
- **Transparency**: each material has a Transparency mode (Opaque, Cutout, Fade, Glass, Additive) and Double Sided. Alpha is the colour's alpha × the colour map's alpha (× a crowd tint's). Cutout draws with the solid world in a variant shader that discards pixels under the cutoff (alpha is lifted on smaller mips so thin cut-outs don't vanish in the distance). Fade, Glass and Additive are see-through: collected in their own list one object per draw, sorted furthest first, and drawn in the overlay pass over the finished solid world (after SSR and AO), depth-tested but not written, with premultiplied blending: Fade scales everything by alpha, Glass scales only the body and keeps its reflections (hiding more of the view at grazing angles), Additive writes alpha 0 so it only adds light. Shadows: Cutout casts the cut shape, Fade and Glass a dithered shadow that the shadow filter softens, Additive none (`shadow_cut.vert/frag`). Reflection probes skip see-through objects; Live GI and the lightmapper let light through them, sound still bounces off them. Imported models pick the mode from the colour map's alpha (a mask → Cutout, soft alpha → Fade) or an opacity below 1 (Glass), and the material editor does the same when a picture with transparency is put on an Opaque material.
- **Ambient occlusion** (`shaders/ao.frag`, `ao_apply.frag`): when on, the main pass also writes the ambient light (diffuse bounce and sky light plus half of the probe reflections) to a fourth target. A half-resolution pass samples 12 points in the hemisphere above each pixel against the depth buffer; a full-resolution pass blurs it with depth-aware weights and subtracts `ambient × occlusion` from the picture, so direct light from the sun and lamps is never darkened.
- **Bloom** (`bloom_down.frag`, `bloom_up.frag`): up to six half-size levels made with a 13-tap filter (the first averages by brightness so lone hot pixels don't flicker), then added back up level by level with a 3×3 tent. The final image mixes in the average of the levels, with no threshold, as a lens would.
- **Sun shafts** (`godrays.frag`): the sun's position on screen is worked out on the CPU, faded as it leaves the screen, turns away or sets. At half resolution each pixel marches 40 steps towards it and gathers the open sky near the sun (depth at the far plane), so leaves, clouds and window frames leave streaks.
- **Final image** (`tonemap.frag`): shafts and bloom, exposure, white balance, vignette and colour fringes, ACES, contrast and saturation, then film grain. Asset thumbnails skip the lens effects. All of these are project graphics settings (Camera & Lens).
- **The sun's disc** is drawn four times its real angular size (0.0372 rad radius), at a brightness that blooms without swamping the picture.
- **Shaders** are GLSL 450 compiled to SPIR-V by `tools/compile_shaders.py`. The result is committed, so builds need no shader compiler.

## Day-night sky and weather

`src/scene/sky.c` with its shader half in `shaders/scene_common.glsl` and `shaders/sky_body.glsl`. A scene uses it when its Sky Mode is Day-night.

- **The clock** places the sun and moon from the hour, latitude, day of the year and a heading (east is +X at heading 0). The moon trails the sun by its phase, so a full moon is up at midnight. The scene's first directional light is driven: the sun by day, the moon by night, at zero brightness in the dusk gap between them so the switch is never seen. It is forced to Realtime because a moving sun can't be baked.
- **The atmosphere** is single scattering ray-marched through a spherical shell: Rayleigh (blue sky, red sunsets), Mie haze (the glow around the sun, scaled by Haze) and ozone (blue twilight). The light's path out of the air uses Schüler's approximation of the Chapman function, so there is no inner march. Multiple scattering is folded into the phase functions, and a 1.8× gain brings the sky up to what a camera sees. The same model runs per pixel in the sky shader (sun and moon in one march) and on the CPU, which gives the sun light's colour and the sky's colour overhead, at the horizon and from the ground (`LxSkyEval`). Everything that read the Gradient sky reads those colours through `lx_env_effective`: ambient light, reflections, fog, GI and lighting volumes.
- **Drawing the sky**: it is drawn last, at the far plane with a depth test, so only uncovered pixels pay for it. On top of the air come stars (one per cell of a cube around the sky, turning about the celestial pole), the moon's disc lit from the sun's side (so it shows its phase), the sun's disc with limb darkening, and the clouds.
- **Clouds**: a slab between Height and Height + Thickness, ray-marched per pixel (10 to 44 steps by quality, jittered) with a short march towards the light for self-shadowing. Shapes come from a 2D coverage map (fbm, drifting with the wind) times a height profile per type (cumulus, stratus, storm), eroded by 3D noise. Lighting is dual-lobe Henyey-Greenstein with a multiple-scattering floor and sky ambient; far clouds fade into the horizon haze. The same coverage map projected along the sun gives moving cloud shadows on the ground, normalised by the average shade already taken off the sun light.
- **Weather**: the scene's settings are targets. While playing, the live weather moves to them linearly over the Blend Time; wetness follows rain (about 30 s to wet, 2 min to dry) and snow cover follows snow. Presets set coverage, density, type, rain, snow, lightning, mist, wind and haze together.
- **Rain sound** (scene setting *Rain Sound*): Procedural, Your Own Sounds, or Off, with a volume. Two ambient loops, light and heavy, are mixed by how hard it rains. The procedural loops (`make_rain` in `src/scene/sky.c`, 30 s, made once) are noise only, with no pitched resonators that read as dripping or trickling water: a band-limited hiss that swells slowly, a patter of thousands of millisecond noise bursts a second (high-passed), and in heavy rain a soft low wash; levelled by loudness. With your own sounds, an empty heavy (or light) slot uses the other file, played as one loop that gets louder.
- **Thunder** (scene settings *Thunder Sound*, *Thunder Volume*, *Closest / Farthest Strike*): each strike lands at a random bearing between the two distances and its thunder plays as late as sound would (343 m/s). The storm strength (`lx_sky_storm_strength`: the live Lightning, made stronger by heavy rain) draws strikes closer, makes them louder and more frequent (mean gap 2.5 s in a full storm to about 50 s in a light one). Close strikes (under 2.5 km) crack, far ones rumble: eight procedural variants, or up to four Close and four Far audio files of your own (one group is enough), never the same twice running.
- **Rain and snow** are instanced quads drawn in the post pass: streaks and flakes placed by hash in a box that travels with the camera but is anchored in the world. A *precipitation map* (one extra layer of the sun's shadow-map array, rendered from straight above over 80 m around the camera) keeps them off anything with a roof. Surfaces use the same map: open ones get wet (darker, glossier, puddles in noise hollows with raindrop rings), and snow settles on what faces up.
- **Lightning** strikes at random under thick cloud: a flash (sky, clouds lit from inside towards the bolt, ambient), a bolt drawn under the cloud base, and thunder (made in code, `builtin:thunder`) played from the bolt's direction after distance ÷ 343 seconds.
- **Lightmaps** in Day-night keep the sky apart. A bake records how much sky each texel sees (alpha below zero; old bakes keep alpha 1), and the shader lights it with the sky of the moment. So baked scenes still go dark at night. Lighting volumes take the sky as it was when baked.
- **GI and probes**: the sun creeping across the sky counts as a small lighting change (like a flickering light), so GI stays smooth. Only a jump of the clock restarts it. Reflection probes recapture after the sun moves a couple of degrees or the sky's colour changes by a few percent.
- **State graphs** get SUNRISE, SUNSET, NOON, MIDNIGHT and LIGHTNING events. The *Sky* actions are Set/Get Time of Day, At Time of Day, Set Weather, Set Clouds, Set Rain and Snow, and Strike Lightning.

## Terrain

`src/terrain/terrain.c`, with its shading in `terrain_surface()` (`shaders/pbr_body.glsl`) and the editor half in `editor/terrain_tools.c`. A Terrain component is one object made of square chunks on a grid. The editor shows it as a workshop with five modes (Shape, Paint, Plants, Expand, Settings), each with picture tiles for its tools.

- **Data** lives per folder (`Assets/Terrains/<name>/`), shared by every scene that shows it: `terrain.lxterrain` lists the chunks with a 17×17 simplified copy of each one's heights and paint, and each chunk has its own `c_<x>_<z>.lxchunk` (heights per edit layer, paint weights, holes). Chunks share their edge points, so a brush that crosses a border writes both sides and seams can't open. A new chunk's heights are blended in from the edges of its neighbours.
- **Objects**: each chunk is drawn by a child object the terrain makes itself (`terrain:<id>/<x>_<z>` mesh, a mesh collider, static). These are never saved, don't show in the Hierarchy, and clicking one selects the terrain.
- **Streaming and LOD**: chunks within the load radius of the listener (or the camera; the editor camera while editing) are loaded in full detail with collision, a few per frame while playing. Further chunks are drawn from the simplified copy, and the mesh LOD chain does the rest; skirts hide the cracks between levels. There is no geomorphing, so a chunk switches detail in one step.
- **Edit layers** ("Sculpt Layers" in the editor): each chunk keeps one height field per layer on top of the Base; what shows is the Base plus each visible layer times its strength. Brushes write to the active layer; Collapse merges them.
- **Undo**: a stroke (begin … end) stores the chunks it touched. The component's `revision` is saved with the scene, so the editor's text-snapshot undo restores it, and the terrain seeks its history to match.
- **Paint**: six layers, each from a plain image (plus an optional normal map) or a material. Weights ride in the vertex (uv: 0-1, uv2: 2-3, tangent.w: 4; layer 5 is the remainder) and slope or height rules are applied as meshes are built. The shader (`MF_TERRAIN`) samples each layer's maps from texture arrays (set bindings 6 and 7) triplanar and blends by height.
- **Relief (parallax)**: seen from above, the ground is ray-marched through the height of the layers that show at that point (each layer's height map, or its brightness when it has none), up to the terrain's relief depth in metres, then marched towards the sun for its own small shadows. It fades out past 25-45 m and on steep slopes.
- **Grass and scatter** are made per loaded chunk from the paint: grass as instanced tufts that sway (instance `lm.w = -2`), scatter as prefab instances. Both are generated objects like the chunks.
- Live GI builds its distance fields from LOD2 of each chunk.

## Live GI (global illumination)

`src/gi/gi.c`, read by `gi_gather()` / `gi_dfao()` in `shaders/pbr_body.glsl`. Fully dynamic, nothing baked, no ray-tracing hardware.

1. **Levels.** Three nested clipmap levels follow the camera: the finest covers about 12 m, the next twice that, the far one four times the spacing and 1.5× wider. Each moves in steps of two probes; a move copies what is still inside and only builds the slab that came into view.
2. **Distance fields.** Each level has a signed distance field at half the probe spacing, truncated three voxels from surfaces, built from the static-geometry BVH (closest triangle); terrain is measured from its own height data (exact). Moving objects (characters, Rigidbodies) are lit by the GI but don't block or bounce it: at this probe spacing they only made dark halos around themselves; their shadows come from the shadow maps. 4³-voxel empty-space bricks store their Chebyshev distance to the nearest surface brick, so rays cross open space in long strides.
3. **Surface cache.** The voxels the surfaces pass through, with the surface's albedo and glow. They are lit by every GI-casting light (the shader's falloff and spot cone) with soft shadows traced through the field, plus the bounce light the probes see in front of them: each update adds another bounce (`gi_max_bounces` 1 turns that off).
4. **Probes.** Each level has an irradiance probe per cell. A probe update traces `gi_rays_per_probe` rays (a spherical Fibonacci set, randomly rotated) through the levels, finest first; a hit returns the cache's light there, a miss the sky gradient (ground colour below the horizon). The result is L1 spherical harmonics, blended over updates (a running average, never lighter than `2 × gi_responsiveness` per update). Probes are classified as buried (skipped; filled from their neighbours), near a surface (8 updates after a change) or in the open (3 updates, half the rays).
5. **Budget.** `gi_rays_per_frame` rays a frame, 45/30/25 % over the levels, go to the probes that still want updates, never-traced ones first. A light that moves, appears or changes a lot resets the probes' history; a drift (the Day-night sun, a flickering lamp, dusk colours) only asks for a couple more updates. A geometry change re-gathers the probes around it. Once nothing wants updates, nothing is traced.
6. **Shading.** The renderer uploads only levels that changed: the far level into the scene GI textures, the two camera levels stacked in the cascade textures, with the walls between probes (distance to the first surface along ±x ±y ±z) so light doesn't leak through thin walls, and the camera levels' distance fields. The shader blends the levels as before and multiplies the bounce light by distance-field occlusion: four taps out along the normal darken contact areas the probes are too coarse to see (the foot of a wall, under a character).

Tests: colour bleeding from a red wall, convergence and following a moved light, no leaks through a thin wall, moving objects' bounce light following them, sky calibration against the L1 reconstruction of an open hemisphere.

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

## Animation

Animation is posed in one direction: something produces a pose, poses are mixed, and the result is written
to the bones. An `LxPose` is one local transform per node of a skeleton, and everything that animates -
a clip, a blend tree, a layer - produces one, so anything can be mixed with anything by the same call.
Positions and scales are a weighted mean; rotations accumulate with their signs aligned and are
normalised at the end, which is what keeps a blend continuous as its weights move and what makes a blend
across the 180 degree wrap take the short way round.

- **Clips** come from the model file. A generic rig plays clips whose bone names match; a humanoid rig
  retargets any humanoid clip through its avatar (source pose -> canonical deltas from its T-pose ->
  applied to the target's T-pose, with the hips scaled by leg length).
- **Animator graphs** (`.lxanim`) are a state machine over animation rather than over game logic. Their
  clock is the cycle, not the second: a state's progress runs 0..1 over its motion whatever that motion's
  length is, which is what lets a blend tree hold a walk and a run of different lengths and still keep
  their feet together, and what makes an exit time of 0.75 mean the same thing everywhere.
- **Transitions** are conditions on typed parameters (float, int, bool, and triggers that one transition
  consumes), optionally waiting for an exit time, and they cross-fade over a duration given in seconds or
  in cycles. Any State transitions are taken from whichever state is playing; interruption rules say what
  may cut a running transition short, and a transition that interrupts a cross-fade fades from what is
  actually on screen rather than from a state.
- **Blend trees** mix several motions by one parameter (1D thresholds), by two (2D cartesian, or
  directional for a locomotion rose where direction matters more than speed), or by a parameter each
  (direct). The 2D kinds use gradient bands: every other child carves away part of a child's influence,
  so the weights are local to where the input is and move continuously. A tree's length is its children's
  lengths weighted by how much of each shows, which is how the children stay in step.
- **Layers** each run their own state machine and blend over the base by weight, through a humanoid bone
  mask (a masked bone brings its descendants with it) and either overriding what is underneath or adding
  the layer's departure from the rest pose to it.
- **IK** is closed form: a limb is two bones and a triangle, so the law of cosines fixes both angles and
  the pole vector decides which way the knee points - no iteration, nothing to tune. Foot grounding
  raycasts under each foot, aligns it to the slope, and eases the hips down far enough that neither leg
  has to stretch.

A State Graph drives all of this by setting parameters (**Animator Graph** actions) and leaves the
choosing and the blending to the animator. The editor's **Animator** tab edits the graph as a graph, and
every edit is saved and picked up by running animators on the next frame, so a transition's duration can
be tuned while the game plays.

**Next:** root motion from the graph rather than from the clip, animation events on a state's timeline,
and sub-state machines.

## State Graph (visual scripting)

A graph (`.lxgraph`, `src/fsm/graph.c`) is a flat list of states. Nesting is a property of each state:
its `parent` (the Sub State it is inside, empty at the top) and its `region` within that parent. Exits
are states of kind `exit`; region names and start states live in `[region]` entries. Any number of
objects can run one graph; variables ticked *Inspector* are its inputs, overridden per object.

At run time (`src/fsm/fsm.c`) each object holds one root `LxFsmRun` per graph slot. Every region that is
playing - each region of an active Sub State, and the graph's own second region onwards - is a nested
run under the run that started it. Nested runs share the root's variables and bound actions (an action
is bound once per state, and a state only ever plays in one region) and keep only their place. Events
go to the innermost runs first, then their Sub States, then the graph's global transitions; a global
transition into a nested state walks down the playing Sub States and enters the rest of the path.
Transitions asked for while an action is running are queued on the root and taken when it returns, and
runs that leave are moved to the root's graveyard and freed once nothing of theirs is on the stack.

The editor (`editor/graph_editor.c`) shows one level at a time (`GE.scope`), with breadcrumbs; the
inspector, Action Browser and Finder are `graph_inspector.c` and `graph_finder.c`.

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
