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
    g->quality_preset = preset;
    g->shadow_resolution = shadow_res[preset];
    g->shadow_cascades = cascades[preset];
    g->shadow_distance = shadow_dist[preset];
    g->gi_photons_per_frame = photons[preset];
    g->gi_max_bounces = bounces[preset];
    g->gi_grid_resolution = grid[preset];
    g->lod_bias = lod_bias[preset];
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
    lx_sb_printf(&sb, "\n[audio]\nmaster = %g\nbus_sfx = %g\nbus_music = %g\nbus_voice = %g\nbus_ambient = %g\n", a->master_volume,
                 a->bus_volume[0], a->bus_volume[1], a->bus_volume[2], a->bus_volume[3]);
    lx_sb_printf(&sb, "sample_rate = %d\nspeed_of_sound = %g\npropagation = %d\nrays = %d\nbounces = %d\nair_absorption = %g\ndoppler = %d\n",
                 a->sample_rate, a->speed_of_sound, a->propagation, a->rays_per_source, a->max_bounces, a->air_absorption, a->doppler);
    lx_sb_printf(&sb, "\n[build]\ntarget = %s\ndevelopment = %d\nfullscreen = %d\nwidth = %d\nheight = %d\n", lx_platform_name(b->target),
                 b->development_build, b->fullscreen, b->window_width, b->window_height);
    lx_sb_printf(&sb, "android_package = %s\nandroid_min_sdk = %d\nandroid_orientation = %s\noutput_dir = %s\n", b->android_package,
                 b->android_min_sdk, b->android_orientation ? "portrait" : "landscape", b->output_dir);
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
    "/* Example game behaviour. Files in Assets/Scripts are compiled into your game build.\n"
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
    static const char *folders[] = {"Scenes", "Materials", "Meshes", "Textures", "Sounds", "Scripts", "Prefabs"};
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
        lx_strcpy(lx_get_mesh(s, sphere)->material, LX_REF_MAX, "builtin:metal");
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
        ground_mat.roughness = 0.85f;
        ground_mat.acoustic_absorption = 0.05f;
        char mp[LX_PATH_MAX];
        lx_path_join(mp, sizeof mp, p->assets, "Materials/Ground.lxmat");
        lx_material_save(&ground_mat, mp);
    }
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
