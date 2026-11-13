/* Luxora headless test suite: runs without a GPU or window. */
#include "lx_internal.h"
#include "png_fixtures.h"

static int g_fail, g_pass;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) g_pass++;                                           \
        else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)
#define NEAR(a, b, eps) (fabsf((a) - (b)) <= (eps))

static const char *tmpdir(void) {
    const char *t = getenv("TMPDIR");
    if (!t) t = getenv("TEMP");
    if (!t) t = getenv("TMP");
    return t ? t : "/tmp";
}

static void test_math(void) {
    printf("math\n");
    LxMat4 t = lx_mat4_trs(lx_v3(1, 2, 3), lx_quat_euler_deg(lx_v3(30, 45, 10)), lx_v3(2, 2, 2));
    LxMat4 i = lx_mat4_mul(t, lx_mat4_inverse(t));
    float err = 0;
    for (int k = 0; k < 16; k++) err += fabsf(i.m[k] - ((k % 5 == 0) ? 1.0f : 0.0f));
    CHECK(err < 1e-4f, "inverse error %g", err);
    LxVec3 r = lx_quat_rotate(lx_quat_axis_angle(lx_v3(0, 1, 0), LX_PI * 0.5f), lx_v3(1, 0, 0));
    CHECK(NEAR(r.z, -1.0f, 1e-5f), "rotate y 90: %g %g %g", r.x, r.y, r.z);
    LxMat4 vp = lx_mat4_mul(lx_mat4_perspective(1.0f, 1.0f, 0.1f, 100), lx_mat4_look_at(lx_v3(0, 0, 5), lx_v3(0, 0, 0), lx_v3(0, 1, 0)));
    LxFrustum f = lx_frustum_from_matrix(vp);
    CHECK(lx_frustum_test_sphere(&f, lx_v3(0, 0, 0), 0.5f), "origin visible");
    CHECK(!lx_frustum_test_sphere(&f, lx_v3(0, 0, 10), 0.5f), "behind camera culled");
    LxAabb box = {{-1, -1, -1}, {1, 1, 1}};
    CHECK(NEAR(lx_ray_aabb((LxRay){{0, 0, 5}, {0, 0, -1}}, box), 4.0f, 1e-4f), "ray-aabb");
}

static void sum_job(void *u, int b, int e, int t) {
    LX_UNUSED(t);
    long s = 0;
    for (int i = b; i < e; i++) s += i;
    lx_atomic_add((volatile long *)u, s);
}

static void test_jobs(void) {
    printf("jobs\n");
    volatile long total = 0;
    lx_parallel_for(100000, 777, sum_job, (void *)&total);
    CHECK(total == 4999950000L || sizeof(long) == 4, "parallel sum %ld", total);
    for (int rep = 0; rep < 200; rep++) { total = 0; lx_parallel_for(1000, 3, sum_job, (void *)&total); }
    CHECK(total == 499500, "repeated batches %ld", total);
}

static void test_scene(void) {
    printf("scene\n");
    LxScene *s = lx_scene_create_default("Test");
    CHECK(lx_scene_entity_count(s) == 3, "default scene has %d entities", lx_scene_entity_count(s));
    LxEntity cam = lx_entity_find(s, "Main Camera"), light = lx_entity_find(s, "Directional Light"), cube = lx_entity_find(s, "Cube");
    CHECK(cam && lx_get_camera(s, cam), "main camera");
    CHECK(light && lx_get_light(s, light) && lx_get_light(s, light)->type == LX_LIGHT_DIRECTIONAL, "light");
    CHECK(cube && lx_get_mesh(s, cube) && !strcmp(lx_get_mesh(s, cube)->mesh, "builtin:cube"), "cube");
    CHECK(lx_scene_main_camera(s) == cam, "main camera lookup");
    LxEntity child = lx_spawn_primitive(s, LX_PRIM_SPHERE, "Child", lx_v3(0, 2, 0));
    lx_entity_set_parent(s, child, cube);
    lx_entity_set_position(s, cube, lx_v3(5, 0, 0));
    LxVec3 wp = lx_entity_world_position(s, child);
    CHECK(NEAR(wp.x, 5, 1e-4f) && NEAR(wp.y, 2, 1e-4f), "hierarchy world pos %g %g", wp.x, wp.y);
    lx_entity_set_parent(s, cube, child);
    CHECK(lx_entity_parent(s, cube) == 0, "cycle prevented");
    LxEntity dup = lx_entity_duplicate(s, cube);
    CHECK(lx_entity_first_child(s, dup) != 0, "duplicate copies children");
    int roots = 0;
    for (LxEntity r = lx_scene_root_first(s); r; r = lx_entity_next_sibling(s, r)) roots++;
    CHECK(roots == 4, "root list has %d roots", roots);
    /* serialization round trip */
    char *text = lx_scene_serialize(s);
    LxScene *c = lx_scene_deserialize(text);
    CHECK(lx_scene_entity_count(c) == lx_scene_entity_count(s), "round trip count %d vs %d", lx_scene_entity_count(c), lx_scene_entity_count(s));
    LxEntity cc = lx_entity_find(c, "Child");
    CHECK(cc && lx_entity_parent(c, cc) && !strcmp(lx_entity_name(c, lx_entity_parent(c, cc)), "Cube"), "parent restored");
    char *text2 = lx_scene_serialize(c);
    CHECK(!strcmp(text, text2), "serialization is stable");
    lx_free(text); lx_free(text2);
    lx_entity_destroy(s, cube);
    CHECK(!lx_entity_valid(s, cube) && !lx_entity_valid(s, child), "destroy removes subtree");
    LxEntity reuse = lx_entity_create(s, "Reuse");
    CHECK(reuse != cube && lx_entity_valid(s, reuse) && !lx_entity_valid(s, cube), "generations protect stale handles");
    float dist;
    LxEntity hit = lx_scene_raycast(c, (LxRay){{5, 0.5f, 10}, {0, 0, -1}}, 100, &dist);
    CHECK(hit && !strcmp(lx_entity_name(c, hit), "Cube"), "raycast picks cube");
    lx_scene_destroy(s);
    lx_scene_destroy(c);
}

static void test_meshes(void) {
    printf("meshes\n");
    LxMesh *cube = lx_asset_mesh("builtin:cube");
    CHECK(lx_mesh_triangle_count(cube) == 12, "cube tris %d", lx_mesh_triangle_count(cube));
    LxAabb b = lx_mesh_bounds(cube);
    CHECK(NEAR(b.min.x, -0.5f, 1e-5f) && NEAR(b.max.y, 0.5f, 1e-5f), "cube bounds");
    /* winding: every face normal should point outward (CCW) */
    int bad = 0;
    for (int t = 0; t < 36; t += 3) {
        const LxVertex *a = &cube->lods[0].vertices[cube->lods[0].indices[t]], *bb = &cube->lods[0].vertices[cube->lods[0].indices[t + 1]],
                       *cc = &cube->lods[0].vertices[cube->lods[0].indices[t + 2]];
        LxVec3 n = lx_cross3(lx_v3(bb->px - a->px, bb->py - a->py, bb->pz - a->pz), lx_v3(cc->px - a->px, cc->py - a->py, cc->pz - a->pz));
        if (lx_dot3(n, lx_v3(a->nx, a->ny, a->nz)) <= 0) bad++;
    }
    CHECK(bad == 0, "cube winding: %d bad triangles", bad);
    LxMesh *sphere = lx_asset_mesh("builtin:sphere");
    CHECK(lx_mesh_lod_count(sphere) >= 2, "sphere has %d LODs", lx_mesh_lod_count(sphere));
    for (int l = 1; l < sphere->lod_count; l++)
        CHECK(sphere->lods[l].index_count < sphere->lods[l - 1].index_count, "LOD %d smaller", l);
    for (int p = 0; p < LX_PRIM_COUNT; p++) {
        char ref[32];
        snprintf(ref, sizeof ref, "builtin:%s", lx_primitive_name((LxPrimitive)p));
        CHECK(lx_mesh_triangle_count(lx_asset_mesh(ref)) > 0, "%s", ref);
    }
}

static void test_png(void) {
    printf("png\n");
    int w, h;
    uint8_t *px = lx_png_decode(png_small, sizeof png_small, &w, &h);
    CHECK(px && w == 5 && h == 3, "small png %dx%d", w, h);
    if (px) {
        CHECK(px[0] == 255 && px[1] == 0 && px[3] == 255, "pixel 0");
        CHECK(px[(1 * 5 + 2) * 4 + 3] == 40, "alpha preserved");
        lx_free(px);
    }
    px = lx_png_decode(png_big, sizeof png_big, &w, &h);
    CHECK(px && w == 64 && h == 48, "big png");
    if (px) {
        long long chk = 0;
        for (int i = 0; i < w * h; i++) chk += (long long)(px[i * 4] * 3 + px[i * 4 + 1] * 5 + px[i * 4 + 2] * 7) * (i + 1);
        CHECK((long)(chk % 2147483648LL) == PNG_BIG_CHECKSUM, "big png pixels checksum %lld", chk % 2147483648LL);
        lx_free(px);
    }
}

static LxScene *make_room(bool with_wall) {
    /* 10 x 4 x 10 closed room built from cubes, optional dividing wall at z = 0 */
    LxScene *s = lx_scene_create("Room");
    struct { LxVec3 p, sc; } parts[] = {
        {{0, -0.1f, 0}, {10, 0.2f, 10}}, {{0, 4.1f, 0}, {10, 0.2f, 10}}, {{-5.1f, 2, 0}, {0.2f, 4, 10}},
        {{5.1f, 2, 0}, {0.2f, 4, 10}}, {{0, 2, -5.1f}, {10, 4, 0.2f}}, {{0, 2, 5.1f}, {10, 4, 0.2f}}};
    for (int i = 0; i < 6; i++) {
        LxEntity e = lx_spawn_primitive(s, LX_PRIM_CUBE, "Wall", parts[i].p);
        lx_entity_set_scale(s, e, parts[i].sc);
        lx_strcpy(lx_get_mesh(s, e)->material, LX_REF_MAX, "builtin:concrete");
    }
    if (with_wall) {
        LxEntity e = lx_spawn_primitive(s, LX_PRIM_CUBE, "Divider", lx_v3(0, 2, 0));
        lx_entity_set_scale(s, e, lx_v3(10, 4, 0.3f));
    }
    lx_scene_update_transforms(s);
    return s;
}

static void test_bvh(void) {
    printf("bvh\n");
    LxScene *s = make_room(true);
    LxBvh b;
    memset(&b, 0, sizeof b);
    lx_bvh_build_from_scene(&b, s);
    CHECK(b.tri_count == 7 * 12, "bvh tris %d", b.tri_count);
    LxHit hit;
    bool h = lx_bvh_intersect(&b, (LxRay){{0, 2, 3}, {0, 0, -1}}, 100, &hit);
    CHECK(h && NEAR(hit.t, 2.85f, 1e-3f), "hits divider at %g", h ? hit.t : -1.0f);
    CHECK(lx_bvh_occluded(&b, (LxRay){{0, 2, 3}, {0, 0, -1}}, 5), "occlusion query");
    CHECK(!lx_bvh_occluded(&b, (LxRay){{0, 2, 3}, {0, 0, -1}}, 2.5f), "unoccluded short ray");
    /* brute force cross-check */
    LxRng rng = lx_rng_seed(9, 1);
    int mismatches = 0;
    for (int k = 0; k < 500; k++) {
        LxRay r = {lx_v3(lx_rng_f(&rng) * 8 - 4, lx_rng_f(&rng) * 3.5f + 0.2f, lx_rng_f(&rng) * 8 - 4), lx_rng_sphere(&rng)};
        float best = 1e30f;
        for (int t = 0; t < b.tri_count; t++) {
            float d = lx_ray_triangle(r, b.v0[t], lx_add3(b.v0[t], b.e1[t]), lx_add3(b.v0[t], b.e2[t]), NULL, NULL);
            if (d > 1e-4f && d < best) best = d;
        }
        LxHit hh;
        bool got = lx_bvh_intersect(&b, r, 1e30f, &hh);
        if ((best < 1e30f) != got || (got && fabsf(hh.t - best) > 1e-3f)) mismatches++;
    }
    CHECK(mismatches == 0, "bvh vs brute force mismatches %d", mismatches);
    lx_bvh_free(&b);
    lx_scene_destroy(s);
}

static void test_gi(void) {
    printf("photon gi\n");
    LxScene *s = lx_scene_create("GI");
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(20, 1, 20));
    LxEntity wall = lx_spawn_primitive(s, LX_PRIM_CUBE, "Red Wall", lx_v3(0, 2, -2));
    lx_entity_set_scale(s, wall, lx_v3(6, 4, 0.5f));
    lx_strcpy(lx_get_mesh(s, wall)->material, LX_REF_MAX, "builtin:red");
    LxEntity sun = lx_entity_create(s, "Sun");
    lx_entity_set_euler(s, sun, lx_v3(-60, 0, 0));
    lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL)->intensity = 3.0f;
    s->env.ambient_intensity = 0.0f; /* only the sun: every bit of GI must come from bounces */
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_photons_per_frame = 40000;
    double t0 = lx_time();
    for (int f = 0; f < 6; f++) lx_gi_update(s, &g);
    double ms = (lx_time() - t0) * 1000.0 / 6.0;
    const LxGiVolume *v = lx_gi_volume();
    CHECK(v->valid && v->res[0] > 2, "gi volume valid");
    /* sample in front of the red wall: indirect light should be red-dominant */
    LxVec3 p = lx_v3(0, 1.5f, -1.45f);
    int cx = (int)((p.x - v->origin.x) / v->cell_size.x), cy = (int)((p.y - v->origin.y) / v->cell_size.y),
        cz = (int)((p.z - v->origin.z) / v->cell_size.z);
    const float *c = lx_gi_cell(cx, cy, cz);
    CHECK(c && c[0] > 0.0f, "irradiance present near wall (L0 red %g)", c ? c[0] : -1.0f);
    if (c) {
        /* irradiance on a surface facing the red wall (normal -Z), as the PBR shader evaluates it */
        LxVec3 n = lx_v3(0, 0, -1);
        float e[3];
        for (int ch = 0; ch < 3; ch++) {
            const float *k = c + ch * 4;
            e[ch] = LX_PI * 0.282095f * k[0] + (2.0f * LX_PI / 3.0f) * 0.488603f * (k[1] * n.y + k[2] * n.z + k[3] * n.x);
        }
        CHECK(e[0] > e[1] * 1.3f && e[0] > e[2] * 1.3f, "colour bleeding towards the wall: r %g g %g b %g", e[0], e[1], e[2]);
    }
    printf("  %d photons/frame in %.1f ms\n", lx_gi_last_photon_count(), ms);
    lx_gi_shutdown();
    lx_scene_destroy(s);
}

/* noise = relative RMS change of the indirect light on the floor between two consecutive frames */
static void gi_floor_layer(float *out, int *n) {
    const LxGiVolume *v = lx_gi_volume();
    int y = (int)((0.6f - v->origin.y) / v->cell_size.y);
    *n = 0;
    for (int z = v->res[2] / 4; z < v->res[2] * 3 / 4; z++)
        for (int x = v->res[0] / 4; x < v->res[0] * 3 / 4; x++) out[(*n)++] = v->sh[((size_t)(z * v->res[1] + y) * v->res[0] + x) * 12];
}
static float gi_flicker(LxScene *s, const LxGraphicsSettings *g) {
    static float a[4096], b[4096];
    int n;
    gi_floor_layer(a, &n);
    lx_gi_update(s, g);
    gi_floor_layer(b, &n);
    double d = 0, m = 0;
    for (int i = 0; i < n; i++) { d += (double)(b[i] - a[i]) * (b[i] - a[i]); m += (double)b[i] * b[i]; }
    return m > 0 ? (float)sqrt(d / m) : 1.0f;
}

static void test_gi_convergence(void) {
    printf("photon gi convergence\n");
    LxScene *s = lx_scene_create("Noise");
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(20, 1, 20));
    LxEntity roof = lx_spawn_primitive(s, LX_PRIM_CUBE, "Ceiling", lx_v3(0, 6.5f, 0));
    lx_entity_set_scale(s, roof, lx_v3(20, 1, 20));
    LxEntity lamp = lx_entity_create(s, "Lamp");
    lx_entity_set_position(s, lamp, lx_v3(0, 4, 0));
    lx_add_light(s, lamp, LX_LIGHT_POINT)->intensity = 200;
    s->env.ambient_intensity = 0;
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_photons_per_frame = 20000;
    for (int f = 0; f < 8; f++) lx_gi_update(s, &g);
    float early = gi_flicker(s, &g);
    for (int f = 0; f < 300; f++) lx_gi_update(s, &g);
    float late = gi_flicker(s, &g);
    printf("  frame-to-frame noise after 8 frames %.4f, after 308 frames %.4f\n", early, late);
    CHECK(late < early * 0.25f, "static lighting converges (noise %.4f -> %.4f)", early, late);
    /* moving the light must restart accumulation quickly instead of smearing the old result */
    lx_entity_set_position(s, lamp, lx_v3(6, 4, 6));
    for (int f = 0; f < 30; f++) lx_gi_update(s, &g);
    const LxGiVolume *v = lx_gi_volume();
    int y = (int)((0.6f - v->origin.y) / v->cell_size.y);
    int cx = (int)((6 - v->origin.x) / v->cell_size.x), cz = (int)((6 - v->origin.z) / v->cell_size.z);
    int ox = (int)((-6 - v->origin.x) / v->cell_size.x), oz = (int)((-6 - v->origin.z) / v->cell_size.z);
    float near_l = v->sh[((size_t)(cz * v->res[1] + y) * v->res[0] + cx) * 12], far_l = v->sh[((size_t)(oz * v->res[1] + y) * v->res[0] + ox) * 12];
    CHECK(near_l > far_l * 2.0f, "GI follows a moved light within 30 frames (%g vs %g)", near_l, far_l);
    lx_gi_shutdown();
    lx_scene_destroy(s);
}

static void test_acoustics(void) {
    printf("acoustics\n");
    LxAudioSettings *as = lx_audio_settings();
    as->rays_per_source = 512;
    int rate = 48000;
    float *ir = lx_calloc(48000, sizeof(float));
    LxScene *open_room = make_room(false);
    lx_acoustic_impulse(open_room, lx_v3(0, 2, -3), lx_v3(0, 2, 3), ir, 48000, rate);
    int first = -1;
    for (int i = 0; i < 48000; i++) if (ir[i] != 0) { first = i; break; }
    int expected = (int)(6.0f / 343.0f * rate);
    CHECK(abs(first - expected) <= 2, "direct sound arrives after %d samples (expected %d)", first, expected);
    float direct_open = ir[first];
    int reflections = 0;
    for (int i = first + 1; i < 48000; i++) if (ir[i] != 0) reflections++;
    CHECK(reflections >= 4, "room produces %d reflection taps", reflections);
    LxScene *walled = make_room(true);
    lx_acoustic_impulse(walled, lx_v3(0, 2, -3), lx_v3(0, 2, 3), ir, 48000, rate);
    CHECK(fabsf(ir[expected]) < direct_open * 0.6f, "wall attenuates direct path (%g vs %g)", ir[expected], direct_open);
    as->speed_of_sound = 1500.0f; /* water */
    lx_acoustic_impulse(open_room, lx_v3(0, 2, -3), lx_v3(0, 2, 3), ir, 48000, rate);
    first = -1;
    for (int i = 0; i < 48000; i++) if (ir[i] != 0) { first = i; break; }
    CHECK(abs(first - (int)(6.0f / 1500.0f * rate)) <= 2, "speed of sound is configurable (%d)", first);
    *as = lx_audio_defaults();
    lx_free(ir);
    lx_scene_destroy(open_room);
    lx_scene_destroy(walled);
}

static void test_crowd(void) {
    printf("crowd\n");
    int n = 20000;
    LxCrowd *c = lx_crowd_create(n);
    LxRng rng = lx_rng_seed(5, 5);
    for (int i = 0; i < n; i++) {
        int a = lx_crowd_add(c, lx_v3(lx_rng_f(&rng) * 100 - 50, 0, lx_rng_f(&rng) * 100 - 50), 0.3f, 1.5f);
        lx_crowd_set_target(c, a, lx_v3(0, 0, 0));
    }
    float d0 = 0;
    for (int i = 0; i < n; i++) d0 += lx_len3(lx_crowd_position(c, i));
    double t0 = lx_time();
    for (int f = 0; f < 60; f++) lx_crowd_update(c, 1.0f / 30.0f);
    double ms = (lx_time() - t0) * 1000.0 / 60.0;
    float d1 = 0;
    for (int i = 0; i < n; i++) d1 += lx_len3(lx_crowd_position(c, i));
    CHECK(d1 < d0 * 0.97f, "agents move towards goal (%g -> %g)", d0 / n, d1 / n);
    printf("  %d agents: %.2f ms per step\n", n, ms);
    lx_crowd_destroy(c);
}

static void test_project(void) {
    printf("project\n");
    char dir[LX_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/luxora_test_%d", tmpdir(), (int)(lx_time() * 1000) % 100000);
    lx_mkdir_p(dir);
    LxProject p;
    CHECK(lx_project_create(&p, dir, "Sample Game", LX_TEMPLATE_SAMPLE_3D), "create project");
    LxProject q;
    CHECK(lx_project_load(&q, p.root), "load project");
    CHECK(!strcmp(q.name, "Sample Game") && q.build.scene_count == 1, "project fields");
    CHECK(!strcmp(q.build.android_package, "com.mystudio.samplegame"), "package id %s", q.build.android_package);
    q.graphics.gi_photons_per_frame = 12345;
    q.audio.speed_of_sound = 340.0f;
    q.build.target = LX_PLATFORM_ANDROID;
    lx_project_save(&q);
    LxProject r;
    lx_project_load(&r, p.root);
    CHECK(r.graphics.gi_photons_per_frame == 12345 && NEAR(r.audio.speed_of_sound, 340.0f, 1e-3f) && r.build.target == LX_PLATFORM_ANDROID,
          "settings round trip");
    char scene_path[LX_PATH_MAX];
    lx_path_join(scene_path, sizeof scene_path, p.assets, "Scenes/Main.lxscene");
    LxScene *s = lx_scene_load(scene_path);
    CHECK(s && lx_entity_find(s, "Main Camera") && lx_entity_find(s, "Directional Light") && lx_entity_find(s, "Cube"),
          "startup scene has camera, light and cube");
    CHECK(s && lx_get_crowd(s, lx_entity_find(s, "Crowd")), "sample has a crowd");
    lx_scene_destroy(s);
    LxDirEntry *ents;
    int n = lx_dir_list(p.assets, &ents);
    CHECK(n >= 6 && ents[0].is_dir, "assets folders listed (%d)", n);
    lx_free(ents);
    lx_path_remove(dir);
    CHECK(!lx_dir_exists(dir), "cleanup");
}

static void test_wav(void) {
    printf("wav\n");
    /* build a 16-bit stereo wav in memory and load it */
    int frames = 480;
    size_t data_bytes = (size_t)frames * 4;
    uint8_t *w = lx_calloc(44 + data_bytes, 1);
    memcpy(w, "RIFF", 4);
    uint32_t v = (uint32_t)(36 + data_bytes);
    memcpy(w + 4, &v, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    v = 16; memcpy(w + 16, &v, 4);
    uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
    uint32_t rate = 48000, bps = 48000 * 4;
    memcpy(w + 20, &fmt, 2); memcpy(w + 22, &ch, 2); memcpy(w + 24, &rate, 4); memcpy(w + 28, &bps, 4);
    memcpy(w + 32, &align, 2); memcpy(w + 34, &bits, 2);
    memcpy(w + 36, "data", 4);
    v = (uint32_t)data_bytes; memcpy(w + 40, &v, 4);
    int16_t *pcm = (int16_t *)(w + 44);
    for (int i = 0; i < frames; i++) { pcm[i * 2] = 16384; pcm[i * 2 + 1] = 0; }
    char path[LX_PATH_MAX];
    snprintf(path, sizeof path, "%s/luxora_test.wav", tmpdir());
    lx_file_write(path, w, 44 + data_bytes);
    LxSound *s = lx_sound_load_wav(path);
    CHECK(s && s->frame_count == frames && s->sample_rate == 48000, "wav header");
    if (s) CHECK(NEAR(s->samples[10], 0.25f, 1e-3f), "stereo downmix %g", s->samples[10]);
    if (s) { lx_free(s->samples); lx_free(s); }
    lx_path_remove(path);
    lx_free(w);
}

static void test_input(void) {
    printf("input\n");
    LxInputMap *m = lx_input_map();
    lx_input_map_defaults(m);
    g_lx_pad_polling_disabled = true; /* don't let real devices overwrite injected state */
    CHECK(lx_input_find(m, "Jump") && lx_input_find(m, "MoveX") && m->touch_count == 4, "default map");
    /* keyboard */
    memset(&g_lx_input, 0, sizeof g_lx_input);
    g_lx_input.width = 1280; g_lx_input.height = 720;
    lx_input_update(0.016f);
    CHECK(!lx_action_down("Jump"), "jump idle");
    g_lx_input.keys[LX_KEY_SPACE] = true;
    g_lx_input.keys[LX_KEY_D] = true;
    lx_input_update(0.016f);
    CHECK(lx_action_pressed("Jump") && lx_action_down("Jump"), "jump pressed");
    CHECK(NEAR(lx_axis("MoveX"), 1.0f, 1e-4f), "MoveX from D = %g", lx_axis("MoveX"));
    lx_input_update(0.016f);
    CHECK(lx_action_down("Jump") && !lx_action_pressed("Jump"), "pressed only one frame");
    g_lx_input.keys[LX_KEY_SPACE] = false;
    g_lx_input.keys[LX_KEY_D] = false;
    lx_input_update(0.016f);
    CHECK(lx_action_released("Jump"), "released");
    /* gamepad with dead zone + trigger as button */
    g_lx_pads[0].connected = true;
    g_lx_pads[0].axes[LX_PAD_LEFT_Y] = 0.1f;
    lx_input_update(0.016f);
    CHECK(lx_axis("MoveY") == 0.0f, "inside dead zone");
    g_lx_pads[0].axes[LX_PAD_LEFT_Y] = 1.0f;
    g_lx_pads[0].axes[LX_PAD_LEFT_X] = 1.0f;
    g_lx_pads[0].axes[LX_PAD_RT] = 0.9f;
    lx_input_update(0.016f);
    CHECK(NEAR(lx_axis("MoveY"), 1.0f, 1e-4f), "full stick %g", lx_axis("MoveY"));
    LxVec2 mv = lx_axis2("MoveX", "MoveY");
    CHECK(NEAR(sqrtf(mv.x * mv.x + mv.y * mv.y), 1.0f, 1e-3f), "diagonal normalised");
    CHECK(lx_action_down("Fire") && lx_input_last_device() == LX_DEVICE_GAMEPAD, "trigger fires");
    memset(&g_lx_pads[0], 0, sizeof g_lx_pads[0]);
    /* touch: stick + button */
    LxTouchControl *stick = &m->touch[0], *jump = &m->touch[2];
    float r = stick->size * 720.0f;
    g_lx_input.touch_count = 2;
    g_lx_input.touches[0] = lx_v2(stick->x * 1280 + r, stick->y * 720);
    g_lx_input.touches[1] = lx_v2(jump->x * 1280, jump->y * 720);
    lx_input_update(0.016f);
    CHECK(NEAR(lx_axis("MoveX"), 1.0f, 1e-3f) && lx_action_down("Jump") && lx_input_last_device() == LX_DEVICE_TOUCH, "touch stick %g + button",
          lx_axis("MoveX"));
    CHECK(!lx_action_down("Fire"), "touch does not leak into mouse bindings");
    g_lx_input.touch_count = 0;
    /* save / load round trip */
    char path[LX_PATH_MAX];
    snprintf(path, sizeof path, "%s/luxora_input_test.lxinput", tmpdir());
    LxInputAction *custom = lx_input_add_action(m, "Throw Grenade", false);
    lx_input_add_binding(custom, (LxBinding){LX_BIND_KEY, LX_KEY_G, 0, 1});
    lx_input_add_binding(custom, (LxBinding){LX_BIND_PAD_BUTTON, LX_PAD_RB, 0, 1});
    CHECK(lx_input_map_save(m, path), "save input");
    LxInputMap *copy = lx_alloc(sizeof *copy);
    CHECK(lx_input_map_load(copy, path), "load input");
    CHECK(copy->action_count == m->action_count && copy->touch_count == m->touch_count, "counts %d/%d", copy->action_count, m->action_count);
    int diff = 0;
    for (int i = 0; i < m->action_count; i++) {
        LxInputAction *a = &m->actions[i], *b = lx_input_find(copy, a->name);
        if (!b || b->binding_count != a->binding_count || b->is_axis != a->is_axis) { diff++; continue; }
        for (int k = 0; k < a->binding_count; k++)
            if (a->bindings[k].type != b->bindings[k].type || a->bindings[k].code != b->bindings[k].code ||
                a->bindings[k].code2 != b->bindings[k].code2 || fabsf(a->bindings[k].scale - b->bindings[k].scale) > 1e-4f) diff++;
    }
    CHECK(diff == 0, "bindings round trip (%d differences)", diff);
    char desc[64];
    lx_binding_describe(m, &custom->bindings[1], desc, sizeof desc);
    CHECK(!strcmp(desc, "Gamepad RB"), "describe: %s", desc);
    lx_free(copy);
    lx_path_remove(path);
    memset(&g_lx_input, 0, sizeof g_lx_input);
}

int main(void) {
    lx_jobs_init(0);
    lx_register_builtin_behaviours();
    test_math();
    test_jobs();
    test_scene();
    test_meshes();
    test_png();
    test_bvh();
    test_gi();
    test_gi_convergence();
    test_acoustics();
    test_crowd();
    test_project();
    test_wav();
    test_input();
    lx_assets_clear();
    lx_jobs_shutdown();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
