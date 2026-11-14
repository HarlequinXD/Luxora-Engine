/* Projects: folder layout, project.lxproj settings (graphics, sound, build), templates. */
#include "../lx_internal.h"

const char *lx_platform_name(LxPlatform p) {
    static const char *n[] = {"Windows", "Linux", "Android"};
    return (p >= 0 && p < LX_PLATFORM_COUNT) ? n[p] : "Unknown";
}

LxPlatform lx_platform_current(void) {
#if LX_OS_WINDOWS
    return LX_PLATFORM_WINDOWS;
#elif LX_OS_ANDROID
    return LX_PLATFORM_ANDROID;
#else
    return LX_PLATFORM_LINUX;
#endif
}

void lx_graphics_apply_preset(LxGraphicsSettings *g, int preset) {
    if (preset < 0) preset = 0;
    if (preset > 3) preset = 3;
    static const int shadow_res[] = {1024, 2048, 2048, 4096};
    static const int cascades[] = {1, 2, 3, 4};
    static const float shadow_dist[] = {40, 80, 120, 200};
    static const int photons[] = {8000, 24000, 60000, 150000};
    static const int bounces[] = {2, 3, 4, 6};
    static const int grid[] = {24, 32, 48, 64};
    static const float lod_bias[] = {0.6f, 0.8f, 1.0f, 1.4f};
    static const int probe_res[] = {64, 128, 256, 256};
    static const bool ssr[] = {false, true, true, true};
    static const int ssr_steps[] = {16, 24, 32, 48};
    g->quality_preset = preset;
    g->shadow_resolution = shadow_res[preset];
    g->shadow_cascades = cascades[preset];
    g->shadow_distance = shadow_dist[preset];
    g->gi_photons_per_frame = photons[preset];
    g->gi_max_bounces = bounces[preset];
    g->gi_grid_resolution = grid[preset];
    g->lod_bias = lod_bias[preset];
    g->reflection_resolution = probe_res[preset];
    g->ssr_enabled = ssr[preset];
    g->ssr_steps = ssr_steps[preset];
}

LxGraphicsSettings lx_graphics_defaults(int preset) {
    LxGraphicsSettings g;
    memset(&g, 0, sizeof g);
    g.render_scale = 1.0f;
    g.gi_enabled = true;
    g.gi_intensity = 1.0f;
    g.gi_responsiveness = 0.08f;
    g.lod_enabled = true;
    g.vsync = true;
    g.target_fps = 0;
    g.exposure = 1.0f;
    g.reflections_enabled = true;
    g.ssr_max_distance = 30.0f;
    g.ssr_thickness = 0.35f;
    g.ssr_max_roughness = 0.6f;
    lx_graphics_apply_preset(&g, preset);
    return g;
}

LxAudioSettings lx_audio_defaults(void) {
    LxAudioSettings a;
    memset(&a, 0, sizeof a);
    a.master_volume = 1.0f;
    for (int i = 0; i < LX_BUS_COUNT; i++) a.bus_volume[i] = 1.0f;
    a.sample_rate = 48000;
    a.speed_of_sound = 343.0f;
    a.propagation = true;
    a.rays_per_source = 256;
    a.max_bounces = 6;
    a.air_absorption = 0.002f;
    a.doppler = true;
    return a;
}

static void defaults(LxProject *p) {
    memset(p, 0, sizeof *p);
    lx_strcpy(p->name, sizeof p->name, "New Project");
    lx_strcpy(p->company, sizeof p->company, "My Studio");
    lx_strcpy(p->version, sizeof p->version, "0.1.0");
    lx_strcpy(p->startup_scene, sizeof p->startup_scene, "Scenes/Main.lxscene");
    p->graphics = lx_graphics_defaults(2);
    p->audio = lx_audio_defaults();
    p->physics = lx_physics_defaults();
    p->build.target = lx_platform_current();
    if (p->build.target == LX_PLATFORM_ANDROID) p->build.target = LX_PLATFORM_LINUX;
    p->build.window_width = 1280;
    p->build.window_height = 720;
    p->build.android_min_sdk = 28;
    p->build.android_orientation = 0;
    p->build.development_build = true;
    lx_strcpy(p->build.android_package, sizeof p->build.android_package, "com.mystudio.game");
    lx_strcpy(p->build.output_dir, sizeof p->build.output_dir, "Builds");
    lx_strcpy(p->build.scenes[0], sizeof p->build.scenes[0], "Scenes/Main.lxscene");
    p->build.scene_count = 1;
}

bool lx_project_save(const LxProject *p) {
    LxStrBuf sb = {0};
    const LxGraphicsSettings *g = &p->graphics;
    const LxAudioSettings *a = &p->audio;
    const LxBuildSettings *b = &p->build;
    lx_sb_printf(&sb, "# Luxora project v1\n[project]\nname = %s\ncompany = %s\nversion = %s\nstartup_scene = %s\nengine = %s\n",
                 p->name, p->company, p->version, p->startup_scene, LX_VERSION_STRING);
    lx_sb_printf(&sb, "\n[graphics]\nquality = %d\nrender_scale = %g\nshadow_resolution = %d\nshadow_cascades = %d\nshadow_distance = %g\n",
                 g->quality_preset, g->render_scale, g->shadow_resolution, g->shadow_cascades, g->shadow_distance);
    lx_sb_printf(&sb, "gi = %d\ngi_photons = %d\ngi_bounces = %d\ngi_grid = %d\ngi_intensity = %g\ngi_responsiveness = %g\n",
                 g->gi_enabled, g->gi_photons_per_frame, g->gi_max_bounces, g->gi_grid_resolution, g->gi_intensity, g->gi_responsiveness);
    lx_sb_printf(&sb, "lod = %d\nlod_bias = %g\nvsync = %d\ntarget_fps = %d\nexposure = %g\n", g->lod_enabled, g->lod_bias,
                 g->vsync, g->target_fps, g->exposure);
    lx_sb_printf(&sb, "reflections = %d\nreflection_resolution = %d\nssr = %d\nssr_steps = %d\nssr_distance = %g\nssr_thickness = %g\nssr_roughness = %g\n",
                 g->reflections_enabled, g->reflection_resolution, g->ssr_enabled, g->ssr_steps, g->ssr_max_distance, g->ssr_thickness,
                 g->ssr_max_roughness);
    const LxPhysicsSettings *ph = &p->physics;
    lx_sb_printf(&sb, "\n[physics]\ngravity = %g %g %g\nfixed_timestep = %g\nsolver_iterations = %d\n", ph->gravity.x, ph->gravity.y,
                 ph->gravity.z, ph->fixed_timestep, ph->solver_iterations);
    lx_sb_printf(&sb, "\n[audio]\nmaster = %g\nbus_sfx = %g\nbus_music = %g\nbus_voice = %g\nbus_ambient = %g\n", a->master_volume,
                 a->bus_volume[0], a->bus_volume[1], a->bus_volume[2], a->bus_volume[3]);
    lx_sb_printf(&sb, "sample_rate = %d\nspeed_of_sound = %g\npropagation = %d\nrays = %d\nbounces = %d\nair_absorption = %g\ndoppler = %d\n",
                 a->sample_rate, a->speed_of_sound, a->propagation, a->rays_per_source, a->max_bounces, a->air_absorption, a->doppler);
    lx_sb_printf(&sb, "\n[build]\ntarget = %s\ndevelopment = %d\nfullscreen = %d\nwidth = %d\nheight = %d\n", lx_platform_name(b->target),
                 b->development_build, b->fullscreen, b->window_width, b->window_height);
    lx_sb_printf(&sb, "android_package = %s\nandroid_min_sdk = %d\nandroid_orientation = %s\noutput_dir = %s\n", b->android_package,
                 b->android_min_sdk, b->android_orientation ? "portrait" : "landscape", b->output_dir);
    lx_sb_printf(&sb, "icon = %s\n", b->icon);
    for (int i = 0; i < b->scene_count; i++) lx_sb_printf(&sb, "scene = %s\n", b->scenes[i]);
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, p->root, "project.lxproj");
    bool ok = lx_file_write(path, sb.data, sb.len);
    lx_free(sb.data);
    return ok;
}

bool lx_project_load(LxProject *p, const char *root) {
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, root, "project.lxproj");
    char *text = lx_file_read(path, NULL);
    if (!text) return false;
    defaults(p);
    lx_strcpy(p->root, sizeof p->root, root);
    lx_path_join(p->assets, sizeof p->assets, root, "Assets");
    p->build.scene_count = 0;
    const char *cur = text;
    char k[64], v[512], section[64] = "";
    int r;
    LxGraphicsSettings *g = &p->graphics;
    LxAudioSettings *a = &p->audio;
    LxBuildSettings *b = &p->build;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
        if (r == 2) { lx_strcpy(section, sizeof section, k); continue; }
        float f = (float)atof(v);
        int n = atoi(v);
        if (!strcmp(section, "project")) {
            if (!strcmp(k, "name")) lx_strcpy(p->name, sizeof p->name, v);
            else if (!strcmp(k, "company")) lx_strcpy(p->company, sizeof p->company, v);
            else if (!strcmp(k, "version")) lx_strcpy(p->version, sizeof p->version, v);
            else if (!strcmp(k, "startup_scene")) lx_strcpy(p->startup_scene, sizeof p->startup_scene, v);
        } else if (!strcmp(section, "graphics")) {
            if (!strcmp(k, "quality")) g->quality_preset = n;
            else if (!strcmp(k, "render_scale")) g->render_scale = f;
            else if (!strcmp(k, "shadow_resolution")) g->shadow_resolution = n;
            else if (!strcmp(k, "shadow_cascades")) g->shadow_cascades = n;
            else if (!strcmp(k, "shadow_distance")) g->shadow_distance = f;
            else if (!strcmp(k, "gi")) g->gi_enabled = lx_parse_bool(v);
            else if (!strcmp(k, "gi_photons")) g->gi_photons_per_frame = n;
            else if (!strcmp(k, "gi_bounces")) g->gi_max_bounces = n;
            else if (!strcmp(k, "gi_grid")) g->gi_grid_resolution = n;
            else if (!strcmp(k, "gi_intensity")) g->gi_intensity = f;
            else if (!strcmp(k, "gi_responsiveness")) g->gi_responsiveness = f;
            else if (!strcmp(k, "lod")) g->lod_enabled = lx_parse_bool(v);
            else if (!strcmp(k, "lod_bias")) g->lod_bias = f;
            else if (!strcmp(k, "vsync")) g->vsync = lx_parse_bool(v);
            else if (!strcmp(k, "target_fps")) g->target_fps = n;
            else if (!strcmp(k, "exposure")) g->exposure = f;
            else if (!strcmp(k, "reflections")) g->reflections_enabled = lx_parse_bool(v);
            else if (!strcmp(k, "reflection_resolution")) g->reflection_resolution = n;
            else if (!strcmp(k, "ssr")) g->ssr_enabled = lx_parse_bool(v);
            else if (!strcmp(k, "ssr_steps")) g->ssr_steps = n;
            else if (!strcmp(k, "ssr_distance")) g->ssr_max_distance = f;
            else if (!strcmp(k, "ssr_thickness")) g->ssr_thickness = f;
            else if (!strcmp(k, "ssr_roughness")) g->ssr_max_roughness = f;
        } else if (!strcmp(section, "physics")) {
            LxPhysicsSettings *ph = &p->physics;
            if (!strcmp(k, "gravity")) ph->gravity = lx_parse_vec3(v);
            else if (!strcmp(k, "fixed_timestep")) ph->fixed_timestep = f > 0.0005f ? f : 1.0f / 60.0f;
            else if (!strcmp(k, "solver_iterations")) ph->solver_iterations = n > 0 ? n : 10;
        } else if (!strcmp(section, "audio")) {
            if (!strcmp(k, "master")) a->master_volume = f;
            else if (!strcmp(k, "bus_sfx")) a->bus_volume[LX_BUS_SFX] = f;
            else if (!strcmp(k, "bus_music")) a->bus_volume[LX_BUS_MUSIC] = f;
            else if (!strcmp(k, "bus_voice")) a->bus_volume[LX_BUS_VOICE] = f;
            else if (!strcmp(k, "bus_ambient")) a->bus_volume[LX_BUS_AMBIENT] = f;
            else if (!strcmp(k, "sample_rate")) a->sample_rate = n;
            else if (!strcmp(k, "speed_of_sound")) a->speed_of_sound = f;
            else if (!strcmp(k, "propagation")) a->propagation = lx_parse_bool(v);
            else if (!strcmp(k, "rays")) a->rays_per_source = n;
            else if (!strcmp(k, "bounces")) a->max_bounces = n;
            else if (!strcmp(k, "air_absorption")) a->air_absorption = f;
            else if (!strcmp(k, "doppler")) a->doppler = lx_parse_bool(v);
        } else if (!strcmp(section, "build")) {
            if (!strcmp(k, "target")) {
                for (int i = 0; i < LX_PLATFORM_COUNT; i++)
                    if (!strcmp(v, lx_platform_name((LxPlatform)i))) b->target = (LxPlatform)i;
            } else if (!strcmp(k, "development")) b->development_build = lx_parse_bool(v);
            else if (!strcmp(k, "fullscreen")) b->fullscreen = lx_parse_bool(v);
            else if (!strcmp(k, "width")) b->window_width = n;
            else if (!strcmp(k, "height")) b->window_height = n;
            else if (!strcmp(k, "android_package")) lx_strcpy(b->android_package, sizeof b->android_package, v);
            else if (!strcmp(k, "android_min_sdk")) b->android_min_sdk = n;
            else if (!strcmp(k, "android_orientation")) b->android_orientation = !strcmp(v, "portrait");
            else if (!strcmp(k, "output_dir")) lx_strcpy(b->output_dir, sizeof b->output_dir, v);
            else if (!strcmp(k, "icon")) lx_strcpy(b->icon, sizeof b->icon, v);
            else if (!strcmp(k, "scene") && b->scene_count < LX_MAX_BUILD_SCENES)
                lx_strcpy(b->scenes[b->scene_count++], sizeof b->scenes[0], v);
        }
    }
    lx_free(text);
    return true;
}

static void write_text(const char *dir, const char *rel, const char *text) {
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, dir, rel);
    lx_file_write(path, text, strlen(text));
}

static const char *SAMPLE_BEHAVIOUR =
    "/* Example game behaviour. Scripts are compiled by the editor's built-in compiler when you save them.\n"
    " * Register behaviours below; attach them to entities in the Inspector. */\n"
    "#include <luxora/luxora.h>\n\n"
    "typedef struct Spinner { float time; } Spinner;\n\n"
    "static void spinner_update(LxBehaviourCtx *ctx, float dt) {\n"
    "    Spinner *s = ctx->state;\n"
    "    s->time += dt;\n"
    "    LxVec3 e = lx_entity_euler(ctx->scene, ctx->self);\n"
    "    e.y += ctx->params[0] * dt;\n"
    "    lx_entity_set_euler(ctx->scene, ctx->self, e);\n"
    "    if (lx_key_pressed(LX_KEY_SPACE)) LX_INFO(\"Spinner alive for %.1f seconds\", s->time);\n"
    "}\n\n"
    "LX_SCRIPT_REGISTER(game) {\n"
    "    LxBehaviourDesc d = {0};\n"
    "    d.name = \"Spinner\";\n"
    "    d.state_size = sizeof(Spinner);\n"
    "    d.param_names[0] = \"Degrees/sec\";\n"
    "    d.param_defaults[0] = 90.0f;\n"
    "    d.on_update = spinner_update;\n"
    "    lx_register_behaviour(&d);\n"
    "}\n";

/* ---- the sample's cobblestone material: procedurally generated, tileable texture set ----
 * Height (for parallax occlusion mapping), normal, albedo, roughness and AO maps from one Voronoi
 * pattern, so the sample shows every material map working together. */
static bool write_png(const char *dir, const char *rel, const uint8_t *rgba, int w, int h) {
    size_t n;
    uint8_t *png = lx_png_encode(rgba, w, h, &n);
    if (!png) return false;
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, dir, rel);
    bool ok = lx_file_write(path, png, n);
    lx_free(png);
    return ok;
}

static bool write_cobblestone_textures(const char *assets) {
    enum { S = 256, G = 8 };   /* image size, stones per row */
    float cx[G][G], cy[G][G], tone[G][G];
    LxRng rng = lx_rng_seed(7, 3);
    for (int j = 0; j < G; j++)
        for (int i = 0; i < G; i++) {
            cx[j][i] = ((float)i + 0.2f + 0.6f * lx_rng_f(&rng)) * (float)S / G;
            cy[j][i] = ((float)j + 0.2f + 0.6f * lx_rng_f(&rng)) * (float)S / G;
            tone[j][i] = lx_rng_f(&rng);
        }
    float *height = lx_alloc(sizeof(float) * S * S);
    uint8_t *alb = lx_alloc(S * S * 4), *nrm = lx_alloc(S * S * 4), *rough = lx_alloc(S * S * 4), *ao = lx_alloc(S * S * 4),
            *hmap = lx_alloc(S * S * 4);
    float *mask = lx_alloc(sizeof(float) * S * S), *ctone = lx_alloc(sizeof(float) * S * S);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float f1 = 1e9f, f2 = 1e9f, t = 0;
            int gi = x * G / S, gj = y * G / S;
            for (int dj = -2; dj <= 2; dj++)
                for (int di = -2; di <= 2; di++) {
                    int i = gi + di, j = gj + dj;
                    float ox = 0, oy = 0;
                    int wi = (i % G + G) % G, wj = (j % G + G) % G;   /* wrap: the texture tiles */
                    ox = (float)(i - wi) * S / G; oy = (float)(j - wj) * S / G;
                    float dx = cx[wj][wi] + ox - ((float)x + 0.5f), dy = cy[wj][wi] + oy - ((float)y + 0.5f);
                    float d = sqrtf(dx * dx + dy * dy);
                    if (d < f1) { f2 = f1; f1 = d; t = tone[wj][wi]; }
                    else if (d < f2) f2 = d;
                }
            float edge = f2 - f1;
            float m = lx_clampf((edge - 2.0f) / 5.0f, 0, 1);
            m = m * m * (3 - 2 * m);
            float dome = sqrtf(lx_clampf((edge - 2.0f) / 14.0f, 0, 1));   /* rounded stone tops */
            float grain = 0.5f + 0.5f * sinf((float)x * 0.9f + t * 20.0f) * sinf((float)y * 0.7f + t * 9.0f);
            height[y * S + x] = 0.1f + m * (0.3f + 0.55f * dome + 0.02f * grain);
            mask[y * S + x] = m;
            ctone[y * S + x] = t;
        }
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int i = y * S + x;
            float h = height[i], m = mask[i], t = ctone[i];
            /* normal (OpenGL convention: green = +V, i.e. down the image) */
            float hl = height[y * S + (x + S - 1) % S], hr = height[y * S + (x + 1) % S];
            float hu = height[((y + S - 1) % S) * S + x], hd = height[((y + 1) % S) * S + x];
            LxVec3 n = lx_norm3(lx_v3(-(hr - hl) * 6.0f, -(hd - hu) * 6.0f, 1.0f));
            nrm[i * 4 + 0] = (uint8_t)(n.x * 127.5f + 127.5f);
            nrm[i * 4 + 1] = (uint8_t)(n.y * 127.5f + 127.5f);
            nrm[i * 4 + 2] = (uint8_t)(n.z * 127.5f + 127.5f);
            nrm[i * 4 + 3] = 255;
            float r = 0.46f + 0.16f * t, g = 0.43f + 0.12f * t, b = 0.39f + 0.08f * t;
            float shade = 0.75f + 0.25f * h;
            LxVec3 stone = lx_v3(r * shade, g * shade, b * shade), mortar = lx_v3(0.30f, 0.27f, 0.22f);
            LxVec3 c = lx_v3(mortar.x + (stone.x - mortar.x) * m, mortar.y + (stone.y - mortar.y) * m, mortar.z + (stone.z - mortar.z) * m);
            alb[i * 4 + 0] = (uint8_t)(lx_clampf(c.x, 0, 1) * 255.0f);
            alb[i * 4 + 1] = (uint8_t)(lx_clampf(c.y, 0, 1) * 255.0f);
            alb[i * 4 + 2] = (uint8_t)(lx_clampf(c.z, 0, 1) * 255.0f);
            alb[i * 4 + 3] = 255;
            uint8_t rv = (uint8_t)(255.0f * (0.95f - m * (0.3f + 0.15f * t)));
            rough[i * 4 + 0] = rough[i * 4 + 1] = rough[i * 4 + 2] = rv; rough[i * 4 + 3] = 255;
            uint8_t av = (uint8_t)(255.0f * lx_clampf(0.45f + 0.55f * (h - 0.12f) / 0.6f, 0, 1));
            ao[i * 4 + 0] = ao[i * 4 + 1] = ao[i * 4 + 2] = av; ao[i * 4 + 3] = 255;
            uint8_t hv = (uint8_t)(lx_clampf(h, 0, 1) * 255.0f);
            hmap[i * 4 + 0] = hmap[i * 4 + 1] = hmap[i * 4 + 2] = hv; hmap[i * 4 + 3] = 255;
        }
    bool ok = write_png(assets, "Textures/Cobblestone_Albedo.png", alb, S, S) &&
              write_png(assets, "Textures/Cobblestone_Normal.png", nrm, S, S) &&
              write_png(assets, "Textures/Cobblestone_Roughness.png", rough, S, S) &&
              write_png(assets, "Textures/Cobblestone_AO.png", ao, S, S) &&
              write_png(assets, "Textures/Cobblestone_Height.png", hmap, S, S);
    lx_free(height); lx_free(mask); lx_free(ctone);
    lx_free(alb); lx_free(nrm); lx_free(rough); lx_free(ao); lx_free(hmap);
    return ok;
}

/* the sample project's State Graph: walks between two points, waits, hops when Space is pressed */
static const char SAMPLE_GRAPH[] =
    "[graph]\nname = Patroller\n"
    "description = Walks between two points and waits at each. Press Space while playing to make it hop.\n"
    "start = Wait at A\n"
    "[event]\nname = ARRIVED\n"
    "[event]\nname = HOP\n"
    "[variable]\nname = Speed\ntype = float\nvalue = 2\ninspector = 1\ntooltip = Walking speed in metres per second\n"
    "[variable]\nname = Point A\ntype = vector3\nvalue = -3 0.4 3\ninspector = 1\n"
    "[variable]\nname = Point B\ntype = vector3\nvalue = 3 0.4 3\ninspector = 1\n"
    "[variable]\nname = Laps\ntype = int\nvalue = 0\n"
    "[global]\nevent = HOP\nto = Hop\n"
    "[state]\nname = Wait at A\npos = 80 120\ndescription = Pause before walking to B\ntransition = FINISHED > Go to B\n"
    "[action]\ntype = Wait\nupdate = every_frame\np0 = 1\n"
    "[state]\nname = Go to B\npos = 360 120\ncolor = 2\ntransition = ARRIVED > Wait at B\n"
    "[action]\ntype = Look At\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = None\np2 = $Point B\np3 = true\n"
    "[action]\ntype = Move Towards\nupdate = every_frame\np0 = Owner\np1 = None\np2 = $Point B\np3 = $Speed\np4 = 0.05\np5 = ARRIVED\np6 = true\n"
    "[action]\ntype = Key Event\nupdate = every_frame\nfolded = 1\np0 = Space\np1 = Pressed\np2 = HOP\n"
    "[state]\nname = Wait at B\npos = 640 120\ntransition = FINISHED > Go to A\n"
    "[action]\ntype = Wait\nupdate = every_frame\np0 = 1\n"
    "[action]\ntype = Int Add\nupdate = once\np0 = $Laps\np1 = 1\n"
    "[state]\nname = Go to A\npos = 360 330\ncolor = 2\ntransition = ARRIVED > Wait at A\n"
    "[action]\ntype = Look At\nupdate = every_frame\nfolded = 1\np0 = Owner\np1 = None\np2 = $Point A\np3 = true\n"
    "[action]\ntype = Move Towards\nupdate = every_frame\np0 = Owner\np1 = None\np2 = $Point A\np3 = $Speed\np4 = 0.05\np5 = ARRIVED\np6 = true\n"
    "[action]\ntype = Key Event\nupdate = every_frame\nfolded = 1\np0 = Space\np1 = Pressed\np2 = HOP\n"
    "[state]\nname = Hop\npos = 640 330\ncolor = 3\nsequence = 1\ndescription = Up, then down with a bounce\ntransition = FINISHED > Go to A\n"
    "[action]\ntype = Move Object\nupdate = every_frame\np0 = Owner\np1 = 0 1.2 0\np2 = 0.25\np3 = Ease Out\np5 = true\n"
    "[action]\ntype = Move Object\nupdate = every_frame\np0 = Owner\np1 = 0 -1.2 0\np2 = 0.45\np3 = Bounce Out\np5 = true\n"
    "[comment]\nrect = 80 300 232 104\ncolor = 0\n"
    "text = HOP is a global transition: from any state that listens for Space, the graph jumps straight to Hop.\n";

bool lx_project_create(LxProject *p, const char *parent, const char *name, LxProjectTemplate tpl) {
    defaults(p);
    lx_strcpy(p->name, sizeof p->name, name);
    lx_path_join(p->root, sizeof p->root, parent, name);
    lx_path_join(p->assets, sizeof p->assets, p->root, "Assets");
    char existing[LX_PATH_MAX];
    lx_path_join(existing, sizeof existing, p->root, "project.lxproj");
    if (lx_file_exists(p->root) || lx_file_exists(existing)) {
        LX_ERROR("A project already exists at %s", p->root);
        return false;
    }
    static const char *folders[] = {"Scenes", "Materials", "Meshes", "Textures", "Sounds", "Scripts", "Prefabs", "Graphs"};
    for (int i = 0; i < LX_ARRAY_COUNT(folders); i++) {
        char d[LX_PATH_MAX];
        lx_path_join(d, sizeof d, p->assets, folders[i]);
        if (!lx_mkdir_p(d)) { LX_ERROR("Cannot create %s", d); return false; }
    }
    if (tpl == LX_TEMPLATE_MOBILE_3D) {
        p->graphics = lx_graphics_defaults(1);
        p->build.target = LX_PLATFORM_ANDROID;
        p->build.android_orientation = 0;
    }
    char pkg[128];
    snprintf(pkg, sizeof pkg, "com.mystudio.");
    size_t n = strlen(pkg);
    for (const char *c = name; *c && n < sizeof pkg - 1; c++)
        if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) pkg[n++] = *c;
        else if (*c >= 'A' && *c <= 'Z') pkg[n++] = (char)(*c - 'A' + 'a');
    pkg[n] = 0;
    if (n == strlen("com.mystudio.")) lx_strcpy(pkg, sizeof pkg, "com.mystudio.game");
    lx_strcpy(p->build.android_package, sizeof p->build.android_package, pkg);

    LxScene *s = lx_scene_create_default("Main");
    if (tpl == LX_TEMPLATE_SAMPLE_3D) {
        LxEntity ground = lx_spawn_primitive(s, LX_PRIM_PLANE, "Ground", lx_v3(0, 0, 0));
        lx_entity_set_scale(s, ground, lx_v3(3, 1, 3));
        lx_strcpy(lx_get_mesh(s, ground)->material, LX_REF_MAX, "Materials/Ground.lxmat");
        LxEntity sphere = lx_spawn_primitive(s, LX_PRIM_SPHERE, "Metal Sphere", lx_v3(-2, 0.75f, 0));
        lx_entity_set_scale(s, sphere, lx_v3(1.5f, 1.5f, 1.5f));
        lx_strcpy(lx_get_mesh(s, sphere)->material, LX_REF_MAX, "Materials/Chrome.lxmat");
        LxEntity red = lx_spawn_primitive(s, LX_PRIM_CUBE, "Red Wall", lx_v3(3, 1.5f, -1));
        lx_entity_set_scale(s, red, lx_v3(0.3f, 3, 4));
        lx_strcpy(lx_get_mesh(s, red)->material, LX_REF_MAX, "builtin:red");
        LxEntity cube = lx_entity_find(s, "Cube");
        lx_add_behaviour(s, cube, "Rotator");
        LxEntity lamp = lx_entity_create(s, "Point Light");
        lx_entity_set_position(s, lamp, lx_v3(1, 2.5f, 2));
        LxLight *l = lx_add_light(s, lamp, LX_LIGHT_POINT);
        l->color = lx_v3(1.0f, 0.6f, 0.3f);
        LxEntity crowd = lx_entity_create(s, "Crowd");
        lx_entity_set_position(s, crowd, lx_v3(0, 0.5f, -25));
        lx_add_crowd(s, crowd, 2000)->area_radius = 18.0f;
        LxMaterial ground_mat = lx_material_default();
        lx_strcpy(ground_mat.name, sizeof ground_mat.name, "Ground");
        ground_mat.albedo = lx_v4(0.42f, 0.44f, 0.4f, 1);
        ground_mat.roughness = 0.45f;   /* polished concrete: soft screen-space + probe reflections */
        ground_mat.acoustic_absorption = 0.05f;
        char mp[LX_PATH_MAX];
        lx_path_join(mp, sizeof mp, p->assets, "Materials/Ground.lxmat");
        lx_material_save(&ground_mat, mp);
        LxMaterial chrome = lx_material_default();
        lx_strcpy(chrome.name, sizeof chrome.name, "Chrome");
        chrome.albedo = lx_v4(0.95f, 0.94f, 0.92f, 1);
        chrome.metallic = 1.0f;
        chrome.roughness = 0.06f;
        lx_path_join(mp, sizeof mp, p->assets, "Materials/Chrome.lxmat");
        lx_material_save(&chrome, mp);
        /* a cobblestone patch: albedo, normal, roughness, AO and height (parallax occlusion) maps */
        if (write_cobblestone_textures(p->assets)) {
            LxMaterial cob = lx_material_default();
            lx_strcpy(cob.name, sizeof cob.name, "Cobblestone");
            lx_strcpy(cob.albedo_map, sizeof cob.albedo_map, "Textures/Cobblestone_Albedo.png");
            lx_strcpy(cob.normal_map, sizeof cob.normal_map, "Textures/Cobblestone_Normal.png");
            lx_strcpy(cob.roughness_map, sizeof cob.roughness_map, "Textures/Cobblestone_Roughness.png");
            lx_strcpy(cob.ao_map, sizeof cob.ao_map, "Textures/Cobblestone_AO.png");
            lx_strcpy(cob.height_map, sizeof cob.height_map, "Textures/Cobblestone_Height.png");
            cob.roughness = 1.0f;
            cob.albedo = lx_v4(1, 1, 1, 1);
            cob.uv_scale = 0.25f;
            cob.height_scale = 0.05f;
            cob.acoustic_absorption = 0.08f;
            lx_path_join(mp, sizeof mp, p->assets, "Materials/Cobblestone.lxmat");
            lx_material_save(&cob, mp);
            LxEntity path = lx_spawn_primitive(s, LX_PRIM_PLANE, "Cobblestone Path", lx_v3(1.0f, 0.01f, 2.5f));
            lx_entity_set_scale(s, path, lx_v3(0.45f, 1, 0.45f));
            lx_strcpy(lx_get_mesh(s, path)->material, LX_REF_MAX, "Materials/Cobblestone.lxmat");
        }
        /* a State Graph: the Patroller walks between two points (Assets/Graphs/Patroller.lxgraph) */
        write_text(p->assets, "Graphs/Patroller.lxgraph", SAMPLE_GRAPH);
        LxEntity pat = lx_spawn_primitive(s, LX_PRIM_CUBE, "Patroller", lx_v3(-3, 0.4f, 3));
        lx_entity_set_scale(s, pat, lx_v3(0.8f, 0.8f, 0.8f));
        lx_strcpy(lx_get_mesh(s, pat)->material, LX_REF_MAX, "builtin:blue");
        lx_add_state_graph(s, pat, "Graphs/Patroller.lxgraph");
        /* a reflection probe over the play area: box-projected reflections of the walls and floor */
        LxEntity probe = lx_entity_create(s, "Reflection Probe");
        lx_entity_set_position(s, probe, lx_v3(0, 1.5f, 0));
        lx_add_reflection_probe(s, probe)->size = lx_v3(16, 8, 16);
    }
    if (tpl == LX_TEMPLATE_FPS || tpl == LX_TEMPLATE_THIRD_PERSON) lx_project_template_build(tpl, p->assets, s);
    char sp[LX_PATH_MAX];
    lx_path_join(sp, sizeof sp, p->assets, "Scenes/Main.lxscene");
    bool ok = lx_scene_save(s, sp);
    lx_scene_destroy(s);
    write_text(p->assets, "Scripts/game.c", SAMPLE_BEHAVIOUR);
    LxMaterial def = lx_material_default();
    lx_strcpy(def.name, sizeof def.name, "Default");
    char dp[LX_PATH_MAX];
    lx_path_join(dp, sizeof dp, p->assets, "Materials/Default.lxmat");
    lx_material_save(&def, dp);
    write_text(p->root, ".gitignore", "Builds/\nLibrary/\n");
    LxInputMap *im = lx_alloc(sizeof *im);
    lx_input_map_defaults(im);
    char ip[LX_PATH_MAX];
    lx_path_join(ip, sizeof ip, p->root, "input.lxinput");
    lx_input_map_save(im, ip);
    lx_free(im);
    return ok && lx_project_save(p);
}
