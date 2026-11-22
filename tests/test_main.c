/* Luxora headless test suite: runs without a GPU or window. */
#include <stdlib.h>
#include "lx_internal.h"
#include "png_fixtures.h"
#include "png_fixtures2.h"

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

static void test_reflection_probe(void) {
    printf("reflection probe\n");
    LxScene *s = lx_scene_create("Probes");
    LxEntity e = lx_entity_create(s, "Probe");
    LxReflectionProbe *p = lx_add_reflection_probe(s, e);
    CHECK(p && p->intensity > 0 && p->box_projection && p->size.x > 0, "probe defaults");
    p->size = lx_v3(4, 3, 2);
    p->intensity = 1.5f;
    p->blend_distance = 0.25f;
    p->realtime = true;
    char *text = lx_scene_serialize(s);
    LxScene *c = lx_scene_deserialize(text);
    LxReflectionProbe *q = lx_get_reflection_probe(c, lx_entity_find(c, "Probe"));
    CHECK(q && NEAR(q->size.x, 4, 1e-5f) && NEAR(q->size.z, 2, 1e-5f) && NEAR(q->intensity, 1.5f, 1e-5f) &&
          NEAR(q->blend_distance, 0.25f, 1e-5f) && q->realtime && q->box_projection, "probe round trip");
    lx_remove_component(c, lx_entity_find(c, "Probe"), LX_COMP_REFLECTION_PROBE);
    CHECK(!lx_get_reflection_probe(c, lx_entity_find(c, "Probe")), "probe removed");
    lx_free(text);
    lx_scene_destroy(s);
    lx_scene_destroy(c);
    LxGraphicsSettings g = lx_graphics_defaults(0), u = lx_graphics_defaults(3);
    CHECK(g.reflections_enabled && !g.ssr_enabled && u.ssr_enabled && u.reflection_resolution > g.reflection_resolution, "reflection presets");
}

static void test_icon_images(void) {
    printf("icon images\n");
    int w = 40, h = 24;
    uint8_t *px = lx_alloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) { px[i * 4] = (uint8_t)(i * 7); px[i * 4 + 1] = (uint8_t)(i >> 2); px[i * 4 + 2] = 200; px[i * 4 + 3] = (uint8_t)(i % 3 ? 255 : 128); }
    size_t n;
    uint8_t *png = lx_png_encode(px, w, h, &n);
    int dw, dh;
    uint8_t *back = lx_png_decode(png, n, &dw, &dh);
    CHECK(back && dw == w && dh == h && !memcmp(back, px, (size_t)w * h * 4), "png encode/decode round trip");
    uint8_t *sq = lx_image_square(px, w, h, 16);
    CHECK(sq != NULL, "square resize");
    const char *ico = "test_icon.ico";
    CHECK(lx_ico_write(ico, px, w, h), "ico written");
    size_t isz;
    uint8_t *f = (uint8_t *)lx_file_read(ico, &isz);
    CHECK(f && isz > 6 && f[2] == 1 && f[4] == 9, "ico header: %d images", f ? f[4] : -1);
    lx_free(f); lx_free(sq); lx_free(back); lx_free(png); lx_free(px);
#ifdef _WIN32
    /* the editor stamps icons and version details into the prebuilt player */
    const char *player = "build/Release/luxora_player.exe";
    if (lx_file_exists(player)) {
        size_t psz;
        char *bytes = lx_file_read(player, &psz);
        CHECK(bytes && lx_file_write("patched_player.exe", bytes, psz), "copy the player");
        lx_free(bytes);
        CHECK(lx_exe_set_version("patched_player.exe", "Patched Game", "Luxora Tests", "2.3.4"), "exe version details replaced");
        CHECK(lx_exe_set_icon("patched_player.exe", ico), "exe icon replaced");
        if (!getenv("LUXORA_KEEP_TEST_ICON")) remove("patched_player.exe");
    }
#endif
    if (!getenv("LUXORA_KEEP_TEST_ICON")) remove(ico);   /* CI builds a game with it */
}

/* ---------------- physics ---------------- */
static int g_enter, g_exit, g_trig_in, g_trig_out;
static float g_hit_impulse;
static void ev_enter(LxBehaviourCtx *c, const LxCollision *col) { LX_UNUSED(c); g_enter++; if (col->impulse > g_hit_impulse) g_hit_impulse = col->impulse; }
static void ev_exit(LxBehaviourCtx *c, LxEntity o) { LX_UNUSED(c); LX_UNUSED(o); g_exit++; }
static void ev_tin(LxBehaviourCtx *c, LxEntity o) { LX_UNUSED(c); LX_UNUSED(o); g_trig_in++; }
static void ev_tout(LxBehaviourCtx *c, LxEntity o) { LX_UNUSED(c); LX_UNUSED(o); g_trig_out++; }

static LxScene *phys_scene(void) {
    LxScene *s = lx_scene_create("Physics");
    LxEntity g = lx_entity_create(s, "Ground");
    lx_entity_set_position(s, g, lx_v3(0, -0.5f, 0));
    LxCollider *c = lx_add_collider(s, g, LX_COLLIDER_BOX);
    c->size = lx_v3(40, 1, 40);
    return s;
}
static void run(LxScene *s, float seconds) {
    for (int i = 0; i < (int)(seconds * 60); i++) lx_scene_simulate(s, 1.0f / 60.0f);
}
static LxEntity body(LxScene *s, const char *name, LxColliderShape shape, LxVec3 pos) {
    LxEntity e = lx_entity_create(s, name);
    lx_entity_set_position(s, e, pos);
    lx_add_collider(s, e, shape);
    lx_add_rigidbody(s, e);
    return e;
}

/* walking about on a thin mesh-collider floor, with a carried model that has its own collider and the odd long
 * frame (a hitch while falling), the character must never sink in (it did: 0.35 m and more) */
static void test_character_no_sinking(void) {
    printf("character never sinks into a thin floor\n");
    LxScene *s = lx_scene_create("Sink");
    LxEntity fl = lx_spawn_primitive(s, LX_PRIM_PLANE, "Floor", lx_v3(0, 0, 0));
    lx_entity_set_scale(s, fl, lx_v3(40, 1, 40));
    lx_remove_component(s, fl, LX_COMP_COLLIDER);
    lx_add_collider(s, fl, LX_COLLIDER_MESH);
    LxEntity p = lx_entity_create(s, "Player");
    lx_entity_set_position(s, p, lx_v3(0, 0.01f, 0));
    lx_add_character(s, p);
    LxEntity body = lx_spawn_primitive(s, LX_PRIM_CAPSULE, "Model", lx_v3(0, 0.9f, 0));   /* keeps its collider */
    lx_entity_set_parent(s, body, p);
    uint32_t rng = 7;
    float vy = 0, lowest = 1e9f;
    LxVec3 dir = lx_v3(1, 0, 0);
    for (int f = 0; f < 60 * 120; f++) {
        rng = rng * 1664525u + 1013904223u;
        float r = (float)(rng >> 8) / 16777216.0f;
        float dt = 1.0f / 60 * (0.3f + r * 1.4f);
        if (f % 97 == 13) dt = 0.1f + r * 0.3f;   /* a hitch */
        if (f % 90 == 0) dir = lx_v3(sinf(r * 6.283f), 0, cosf(r * 6.283f));
        LxVec3 pos = lx_entity_world_position(s, p);
        if (fabsf(pos.x) > 15) dir.x = pos.x > 0 ? -fabsf(dir.x) : fabsf(dir.x);
        if (fabsf(pos.z) > 15) dir.z = pos.z > 0 ? -fabsf(dir.z) : fabsf(dir.z);
        bool g = lx_character_grounded(s, p);
        vy = g ? (f % 61 == 0 ? 25.0f : -2.0f) : vy - 20 * dt;
        lx_character_move(s, p, lx_v3(dir.x * 6 * dt, vy * dt, dir.z * 6 * dt));
        lowest = lx_minf(lowest, lx_entity_world_position(s, p).y);
    }
    CHECK(lowest > -0.02f, "never below the floor (lowest feet %.3f)", lowest);
    lx_scene_destroy(s);
    /* walking into boxes: one lower than the step height is climbed, a 1 m one (box or mesh collider) never is, and
     * pushing into it must not bob the character up and down */
    static const float heights[3] = {0.25f, 1.0f, 1.0f};
    for (int t = 0; t < 3; t++) {
        LxScene *w = lx_scene_create("Steps");
        LxEntity g = lx_spawn_primitive(w, LX_PRIM_CUBE, "Ground", lx_v3(0, -0.5f, 0));
        lx_entity_set_scale(w, g, lx_v3(40, 1, 40));
        LxEntity bx = lx_spawn_primitive(w, LX_PRIM_CUBE, "Crate", lx_v3(0, heights[t] * 0.5f, 2));
        lx_entity_set_scale(w, bx, lx_v3(1, heights[t], 1));
        if (t == 2) { lx_remove_component(w, bx, LX_COMP_COLLIDER); lx_add_collider(w, bx, LX_COLLIDER_MESH); }
        LxEntity ch = lx_entity_create(w, "Player");
        lx_add_character(w, ch)->step_height = 0.35f;
        float highest = 0, prev = 0;
        int bobs = 0;
        for (int f = 0; f < 300; f++) {
            lx_character_move(w, ch, lx_v3(0, lx_character_grounded(w, ch) ? -0.03f : -0.15f, 0.1f));
            float y = lx_entity_world_position(w, ch).y;
            highest = lx_maxf(highest, y);
            if (f > 30 && fabsf(y - prev) > 0.05f && heights[t] > 0.5f) bobs++;
            prev = y;
        }
        if (t == 0) CHECK(highest > 0.24f, "climbs a 0.25 m step (highest %.2f)", highest);
        else CHECK(highest < 0.05f && bobs == 0, "a 1 m %s collider is a wall (highest %.2f, %d bobs)", t == 2 ? "mesh" : "box", highest, bobs);
        lx_scene_destroy(w);
    }
}

/* slopes: a walkable one is stood on without moving; walking into one too steep for it neither climbs it nor bobs
 * up and down against it (it used to be lifted along the slope's normal every frame, then fall back) */
static LxScene *ramp_scene(float deg, bool mesh, LxEntity *ch, LxVec3 start) {
    LxScene *s = lx_scene_create("Ramp");
    LxEntity g = lx_spawn_primitive(s, LX_PRIM_CUBE, "Ground", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, g, lx_v3(60, 1, 60));
    LxEntity r = lx_spawn_primitive(s, LX_PRIM_CUBE, "Ramp", lx_v3(0, 0, 0));
    float a = deg * LX_DEG2RAD, L = 20, T = 1;
    lx_entity_set_scale(s, r, lx_v3(10, T, L));
    lx_entity_set_rotation(s, r, lx_quat_euler_deg(lx_v3(-deg, 0, 0)));
    LxVec3 up = lx_v3(0, sinf(a), cosf(a)), nrm = lx_v3(0, cosf(a), -sinf(a));
    lx_entity_set_position(s, r, lx_add3(lx_v3(0, 0, 2), lx_sub3(lx_scale3(up, L / 2), lx_scale3(nrm, T / 2))));
    if (mesh) { lx_remove_component(s, r, LX_COMP_COLLIDER); lx_add_collider(s, r, LX_COLLIDER_MESH); }
    *ch = lx_entity_create(s, "Player");
    lx_entity_set_position(s, *ch, start);
    lx_add_character(s, *ch);   /* slope limit 50 */
    lx_scene_update_transforms(s);
    return s;
}
static void test_character_slopes(void) {
    printf("character on slopes\n");
    for (int mesh = 0; mesh < 2; mesh++) {
        for (int pass = 0; pass < 2; pass++) {
            float deg = pass ? 60.0f : 25.0f, walk = pass ? 3.0f : 0.0f;
            LxEntity ch;
            LxScene *s = ramp_scene(deg, mesh, &ch, pass ? lx_v3(0, 0.05f, 0) : lx_v3(0, 4 * tanf(25 * LX_DEG2RAD) + 0.3f, 6));
            float vy = 0, prev = 0, lo = 1e9f, hi = -1e9f;
            int bobs = 0;
            LxVec3 settled = lx_v3(0, 0, 0);
            for (int f = 0; f < 400; f++) {
                float dt = 1.0f / 60;
                if (lx_character_grounded(s, ch) && vy < 0) vy = -1;
                else vy -= 20 * dt;
                lx_character_move(s, ch, lx_v3(0, vy * dt, walk * dt));
                LxVec3 p = lx_entity_world_position(s, ch);
                if (f == 100) settled = p;
                if (f > 100) {
                    if (fabsf(p.y - prev) > 0.003f) bobs++;
                    lo = lx_minf(lo, p.y);
                    hi = lx_maxf(hi, p.y);
                }
                prev = p.y;
            }
            LxVec3 end = lx_entity_world_position(s, ch);
            if (pass == 0)
                CHECK(lx_len3(lx_sub3(end, settled)) < 1e-3f && lx_character_grounded(s, ch), "%s: stands still on a 25 degree slope (moved %.4f)",
                      mesh ? "mesh" : "box", lx_len3(lx_sub3(end, settled)));
            else
                CHECK(hi < 0.05f && bobs == 0, "%s: walking into a 60 degree slope neither climbs nor bobs (highest %.3f, %d bobs)",
                      mesh ? "mesh" : "box", hi, bobs);
            LX_UNUSED(lo);
            lx_scene_destroy(s);
        }
        /* walking down and up a 30 degree slope: its height changes smoothly, frame after frame (going down it used
         * to float a little above the slope one frame and drop onto it the next) */
        for (int down = 0; down < 2; down++) {
            float a = 30 * LX_DEG2RAD;
            LxEntity ch;
            LxScene *s = ramp_scene(30, mesh, &ch, down ? lx_v3(0, 12 * sinf(a) + 0.02f, 2 + 12 * cosf(a)) : lx_v3(0, 0.02f, 0));
            lx_character_move(s, ch, lx_v3(0, -0.05f, 0));
            float vy = 0, py = lx_entity_world_position(s, ch).y, pv = 0;
            int jolts = 0, airborne = 0, frames = 0;
            for (int f = 0; f < 150; f++) {
                float dt = 1.0f / 60;
                if (lx_character_grounded(s, ch) && vy < 0) vy = -1;
                else vy -= 20 * dt;
                int fl = lx_character_move(s, ch, lx_v3(0, vy * dt, (down ? -3.0f : 3.0f) * dt));
                LxVec3 p = lx_entity_world_position(s, ch);
                float v = (p.y - py) / dt;
                if (f > 3 && p.z > 3 && p.z < 2 + 19 * cosf(a) - 1) {
                    frames++;
                    if (fabsf(v - pv) > 0.5f) jolts++;
                    if (!(fl & LX_CHAR_GROUNDED)) airborne++;
                }
                pv = v;
                py = p.y;
            }
            CHECK(frames > 60 && jolts == 0 && airborne == 0, "%s: walking %s a 30 degree slope is smooth (%d jolts, %d frames off the ground of %d)",
                  mesh ? "mesh" : "box", down ? "down" : "up", jolts, airborne, frames);
            lx_scene_destroy(s);
        }
    }
}

/* Layers: a number for physics, a name for everybody else. */
static void test_layers(void) {
    printf("layers\n");
    LxLayers *l = lx_layers();
    memset(l, 0, sizeof *l);
    lx_strcpy(l->name[0], LX_LAYER_NAME_MAX, "Default");
    lx_strcpy(l->name[3], LX_LAYER_NAME_MAX, "Ground");
    lx_strcpy(l->name[7], LX_LAYER_NAME_MAX, "Props");

    CHECK(!strcmp(lx_layer_name(3), "Ground") && lx_layer_named(3), "a named layer gives its name");
    CHECK(!strcmp(lx_layer_name(9), "Layer 9") && !lx_layer_named(9), "an unnamed one still has something to call it (%s)",
          lx_layer_name(9));
    CHECK(lx_layer_find("Ground") == 3 && lx_layer_find("ground") == 3, "a layer is found by name, whatever its case");
    CHECK(lx_layer_find(" Props ") == 7, "with the spaces around it trimmed");
    CHECK(lx_layer_find("Nothing") < 0, "and not found when there is no such layer");
    CHECK(lx_layer_find("Layer 9") == 9, "an unnamed layer can still be named by number");

    CHECK(lx_layer_mask("") == LX_ALL_LAYERS && lx_layer_mask(NULL) == LX_ALL_LAYERS, "no layers named means every layer");
    CHECK(lx_layer_mask("Ground") == (1u << 3), "one name is one bit (%u)", lx_layer_mask("Ground"));
    CHECK(lx_layer_mask("Ground, Props") == ((1u << 3) | (1u << 7)), "a list is the bits of all of them");
    CHECK(lx_layer_mask("Ground;Props") == ((1u << 3) | (1u << 7)), "a semicolon separates them too");
    CHECK(lx_layer_mask("Ground, Nothing") == (1u << 3), "a name no layer has is passed over");
    /* a mask of nothing finds nothing, which looks exactly like a broken query: a list that named no
       layer at all is far more likely to be a typo than a request to search nowhere */
    CHECK(lx_layer_mask("Nothing, Nowhere") == LX_ALL_LAYERS, "a list of names that are all wrong searches everywhere");
    {
        char names[128];
        lx_layer_mask_names((1u << 3) | (1u << 7), names, sizeof names);
        CHECK(!strcmp(names, "Ground, Props"), "a mask reads back as its names (%s)", names);
        lx_layer_mask_names(LX_ALL_LAYERS, names, sizeof names);
        CHECK(!strcmp(names, "every layer"), "and all of them says so (%s)", names);
        lx_layer_mask_names(0, names, sizeof names);
        CHECK(!strcmp(names, "no layers"), "as does none of them (%s)", names);
    }

    /* an object carries its layer, and a scene remembers it */
    LxScene *s = lx_scene_create("Layers");
    LxEntity e = lx_entity_create(s, "Thing");
    CHECK(lx_entity_layer(s, e) == 0, "an object starts on the default layer");
    lx_entity_set_layer(s, e, 7);
    CHECK(lx_entity_layer(s, e) == 7, "and can be moved to another");
    lx_entity_set_layer(s, e, 99);
    CHECK(lx_entity_layer(s, e) == 7, "a layer that does not exist is refused (%d)", lx_entity_layer(s, e));
    char *text = lx_scene_serialize(s);
    LxScene *back = lx_scene_create("Back");
    lx_scene_deserialize_into(back, text, 0);
    LxEntity e2 = lx_entity_find(back, "Thing");
    CHECK(e2 && lx_entity_layer(back, e2) == 7, "the layer is saved with the scene (%d)",
          e2 ? lx_entity_layer(back, e2) : -1);
    lx_free(text);
    lx_scene_destroy(back);

    /* a scene from before the layer moved off the collider still means the object */
    {
        const char *old_scene =
            "[entity]\nname = Old\ncomponents = collider\ncollider.shape = box\ncollider.layer = 3\n";
        LxScene *o = lx_scene_create("Old");
        lx_scene_deserialize_into(o, old_scene, 0);
        LxEntity oe = lx_entity_find(o, "Old");
        CHECK(oe && lx_entity_layer(o, oe) == 3, "an older scene's collider layer becomes the object's (%d)",
              oe ? lx_entity_layer(o, oe) : -1);
        lx_scene_destroy(o);
    }

    /* and the thing it is all for: a query that only looks at some of them */
    {
        LxScene *q = lx_scene_create("Query");
        LxEntity ground = lx_spawn_primitive(q, LX_PRIM_CUBE, "Floor", lx_v3(0, 0, 0));
        lx_add_collider(q, ground, LX_COLLIDER_BOX);
        lx_entity_set_layer(q, ground, 3);
        LxEntity prop = lx_spawn_primitive(q, LX_PRIM_CUBE, "Crate", lx_v3(0, 3, 0));
        lx_add_collider(q, prop, LX_COLLIDER_BOX);
        lx_entity_set_layer(q, prop, 7);
        lx_scene_simulate(q, 1.0f / 60.0f);
        LxRaycastHit hit;
        bool any = lx_physics_raycast(q, lx_v3(0, 10, 0), lx_v3(0, -1, 0), 100, LX_ALL_LAYERS, &hit);
        CHECK(any && hit.entity == prop, "with every layer the ray meets the nearer one first");
        bool only_ground = lx_physics_raycast(q, lx_v3(0, 10, 0), lx_v3(0, -1, 0), 100, lx_layer_mask("Ground"), &hit);
        CHECK(only_ground && hit.entity == ground, "asking for Ground passes straight through the crate");
        bool none = lx_physics_raycast(q, lx_v3(0, 10, 0), lx_v3(0, -1, 0), 100, 1u << 12, &hit);
        CHECK(!none, "and a layer nothing is on finds nothing");
        /* A sweep that meets nothing has to march the whole way, which is where a distance it cannot
           compare against - NaN, or one too large to step through - used to leave it going for ever.
           Nothing hit this while every query looked at every layer and so returned on the first probe. */
        CHECK(!lx_physics_sphere_cast(q, lx_v3(0, 10, 0), 0.3f, lx_v3(0, -1, 0), (float)NAN, LX_ALL_LAYERS, &hit),
              "a sphere cast with no distance to go gives up rather than hanging");
        CHECK(!lx_physics_sphere_cast(q, lx_v3(0, 10, 0), 0.3f, lx_v3(0, -1, 0), -5, LX_ALL_LAYERS, &hit),
              "nor does it sweep backwards");
        CHECK(!lx_physics_sphere_cast(q, lx_v3(500, 10, 500), 0.01f, lx_v3(0, -1, 0), 1e9f, 1u << 12, &hit),
              "a sweep of a million metres past everything still ends");
        CHECK(lx_physics_sphere_cast(q, lx_v3(0, 10, 0), 0.3f, lx_v3(0, -1, 0), 100, lx_layer_mask("Ground"), &hit) &&
                  hit.entity == ground,
              "and one that is looking for the ground still finds it");
        lx_scene_destroy(q);
    }
    lx_scene_destroy(s);
    memset(lx_layers(), 0, sizeof(LxLayers));
    lx_strcpy(lx_layers()->name[0], LX_LAYER_NAME_MAX, "Default");
}

static void test_physics(void) {
    printf("physics\n");
    /* resting sphere and box */
    LxScene *s = phys_scene();
    LxEntity sp = body(s, "Sphere", LX_COLLIDER_SPHERE, lx_v3(-2, 3, 0));
    LxEntity bx = body(s, "Box", LX_COLLIDER_BOX, lx_v3(2, 3, 0));
    LxEntity cap = body(s, "Capsule", LX_COLLIDER_CAPSULE, lx_v3(5, 3, 0));
    lx_entity_set_euler(s, cap, lx_v3(0, 0, 90));
    run(s, 4);
    LxVec3 ps = lx_entity_world_position(s, sp), pb = lx_entity_world_position(s, bx), pc = lx_entity_world_position(s, cap);
    CHECK(NEAR(ps.y, 0.5f, 0.03f), "sphere rests on the ground (y=%g)", ps.y);
    CHECK(NEAR(pb.y, 0.5f, 0.03f), "box rests on the ground (y=%g)", pb.y);
    CHECK(NEAR(pc.y, 0.5f, 0.05f), "capsule lying down rests on its side (y=%g)", pc.y);
    LxVec3 up = lx_quat_rotate(lx_entity_world_rotation(s, bx), lx_v3(0, 1, 0));
    CHECK(up.y > 0.99f, "box stays upright (up.y=%g)", up.y);
    CHECK(lx_rigidbody_sleeping(s, bx) && lx_rigidbody_sleeping(s, sp), "resting bodies fall asleep");
    CHECK(!isnan(ps.x + pb.x + pc.x), "no NaNs");
    lx_scene_destroy(s);

    /* stacking */
    s = phys_scene();
    LxEntity st[4];
    for (int i = 0; i < 4; i++) st[i] = body(s, "Stack", LX_COLLIDER_BOX, lx_v3(0.02f * i, 0.5f + i * 1.01f, 0));
    run(s, 5);
    LxVec3 top = lx_entity_world_position(s, st[3]);
    CHECK(NEAR(top.y, 3.5f, 0.08f) && fabsf(top.x) < 0.25f, "4-box stack stands (top %g,%g)", top.x, top.y);
    lx_scene_destroy(s);

    /* bounciness and friction */
    s = phys_scene();
    LxEntity ball = body(s, "Ball", LX_COLLIDER_SPHERE, lx_v3(0, 4, 0));
    lx_get_collider(s, ball)->bounciness = 0.9f;
    lx_get_collider(s, lx_entity_find(s, "Ground"))->bounciness = 0.9f;
    float peak = 0;
    bool bounced = false;
    for (int i = 0; i < 240; i++) {
        lx_scene_simulate(s, 1.0f / 60.0f);
        LxVec3 v = lx_rigidbody_velocity(s, ball);
        if (v.y > 0.5f) bounced = true;
        if (bounced) peak = lx_maxf(peak, lx_entity_world_position(s, ball).y);
    }
    CHECK(peak > 2.0f, "bouncy ball bounces back up (peak %g)", peak);
    LxEntity slide = body(s, "Slider", LX_COLLIDER_BOX, lx_v3(-10, 0.5f, 5));
    LxEntity ice = body(s, "Ice", LX_COLLIDER_BOX, lx_v3(-10, 0.5f, 10));
    lx_get_collider(s, ice)->friction = 0;
    lx_get_collider(s, lx_entity_find(s, "Ground"))->friction = 0.6f;
    lx_rigidbody_set_velocity(s, slide, lx_v3(5, 0, 0));
    lx_rigidbody_set_velocity(s, ice, lx_v3(5, 0, 0));
    run(s, 1.5f);
    float vs = lx_len3(lx_rigidbody_velocity(s, slide)), vi = lx_len3(lx_rigidbody_velocity(s, ice));
    CHECK(vs < 0.5f && vi > 3.0f, "friction stops a box (v=%g), ice keeps sliding (v=%g)", vs, vi);
    lx_scene_destroy(s);

    /* events */
    LxBehaviourDesc ev = {0};
    ev.name = "PhysicsEvents";
    ev.on_collision_enter = ev_enter;
    ev.on_collision_exit = ev_exit;
    ev.on_trigger_enter = ev_tin;
    ev.on_trigger_exit = ev_tout;
    lx_register_behaviour(&ev);
    s = phys_scene();
    LxEntity faller = body(s, "Faller", LX_COLLIDER_SPHERE, lx_v3(0, 6, 0));
    lx_add_behaviour(s, faller, "PhysicsEvents");
    LxEntity zone = lx_entity_create(s, "Zone");
    lx_entity_set_position(s, zone, lx_v3(0, 3, 0));
    LxCollider *zc = lx_add_collider(s, zone, LX_COLLIDER_BOX);
    zc->size = lx_v3(2, 1, 2);
    zc->is_trigger = true;
    g_enter = g_exit = g_trig_in = g_trig_out = 0;
    g_hit_impulse = 0;
    run(s, 3);
    CHECK(g_trig_in == 1 && g_trig_out == 1, "trigger enter/exit once each (%d/%d)", g_trig_in, g_trig_out);
    CHECK(g_enter >= 1 && g_hit_impulse > 1.0f, "collision enter with an impact impulse (%d, %g)", g_enter, g_hit_impulse);
    lx_rigidbody_add_impulse(s, faller, lx_v3(0, 8, 0));
    run(s, 0.3f);
    CHECK(g_exit >= 1, "collision exit when it jumps off (%d)", g_exit);
    run(s, 3);   /* let it land again */
    /* queries */
    LxRaycastHit hit;
    CHECK(lx_physics_raycast(s, lx_v3(3, 10, 3), lx_v3(0, -1, 0), 100, LX_ALL_LAYERS, &hit) && NEAR(hit.distance, 10, 0.01f) &&
              hit.normal.y > 0.99f, "raycast hits the ground (d=%g)", hit.distance);
    CHECK(!lx_physics_raycast(s, lx_v3(-5, 3, 0.8f), lx_v3(1, 0, 0), 10, LX_ALL_LAYERS, NULL), "raycast ignores triggers");
    LxEntity found[4];
    int n = lx_physics_overlap_sphere(s, lx_v3(0, 0.5f, 0), 0.6f, LX_ALL_LAYERS, found, 4);
    CHECK(n == 2, "overlap sphere finds the ball and the ground (%d)", n);
    CHECK(lx_physics_sphere_cast(s, lx_v3(-5, 0.5f, 0), 0.4f, lx_v3(1, 0, 0), 20, LX_ALL_LAYERS, &hit) && hit.entity == faller &&
              NEAR(hit.distance, 4.1f, 0.15f), "sphere cast hits the ball (d=%g)", hit.distance);
    /* a camera's wall check starts inside its own player: told to ignore the player, it must skip the player and
       the model under it (whatever layer that model is on) and find the wall behind */
    {
        LxEntity pl = lx_entity_create(s, "Player");
        lx_entity_set_position(s, pl, lx_v3(10, 0, 10));
        lx_add_character(s, pl);
        LxEntity model = lx_spawn_primitive(s, LX_PRIM_CAPSULE, "Model", lx_v3(10, 0.9f, 10));
        lx_entity_set_parent(s, model, pl);
        LxEntity wall = lx_spawn_primitive(s, LX_PRIM_CUBE, "Wall", lx_v3(10, 1.5f, 7.5f));
        lx_entity_set_scale(s, wall, lx_v3(6, 3, 0.4f));
        LxVec3 pivot = lx_v3(10, 1.5f, 10);
        CHECK(lx_physics_sphere_cast(s, pivot, 0.25f, lx_v3(0, 0, -1), 4, LX_ALL_LAYERS, &hit) && hit.distance < 0.1f,
              "without ignoring it, the cast hits its own player (d=%g)", hit.distance);
        CHECK(lx_physics_sphere_cast_ignore(s, pivot, 0.25f, lx_v3(0, 0, -1), 4, LX_ALL_LAYERS, pl, &hit) && hit.entity == wall &&
                  NEAR(hit.distance, 2.05f, 0.05f), "ignoring the player, it finds the wall behind (d=%g)", hit.distance);
    }
    lx_scene_destroy(s);

    /* character controller: ground, walls, steps, mesh colliders */
    s = phys_scene();
    LxEntity wall = lx_entity_create(s, "Wall");
    lx_entity_set_position(s, wall, lx_v3(5, 1, 0));
    lx_add_collider(s, wall, LX_COLLIDER_BOX)->size = lx_v3(1, 2, 10);
    LxEntity stair = lx_entity_create(s, "Step");
    lx_entity_set_position(s, stair, lx_v3(-4, 0.1f, 0));
    lx_add_collider(s, stair, LX_COLLIDER_BOX)->size = lx_v3(2, 0.2f, 4);
    LxEntity ch = lx_entity_create(s, "Player");
    lx_entity_set_position(s, ch, lx_v3(0, 0.5f, 0));
    lx_add_character(s, ch);
    for (int i = 0; i < 30; i++) lx_character_move(s, ch, lx_v3(0, -0.2f, 0));
    CHECK(lx_character_grounded(s, ch) && NEAR(lx_entity_world_position(s, ch).y, 0, 0.03f), "character lands on the ground (y=%g)",
          lx_entity_world_position(s, ch).y);
    for (int i = 0; i < 120; i++) lx_character_move(s, ch, lx_v3(0.1f, -0.1f, 0));
    LxVec3 cp = lx_entity_world_position(s, ch);
    CHECK(cp.x < 4.5f - 0.3f && cp.x > 4.0f, "character stops at the wall (x=%g)", cp.x);
    for (int i = 0; i < 400 && lx_entity_world_position(s, ch).x > -4.0f; i++) lx_character_move(s, ch, lx_v3(-0.05f, -0.05f, 0));
    for (int i = 0; i < 5; i++) lx_character_move(s, ch, lx_v3(0, -0.05f, 0));
    cp = lx_entity_world_position(s, ch);
    CHECK(cp.x < -4 && NEAR(cp.y, 0.2f, 0.05f), "character climbs onto a 20 cm step (x=%g y=%g)", cp.x, cp.y);
    lx_scene_destroy(s);

    /* mesh collider + slope */
    s = lx_scene_create("Mesh");
    LxEntity plane = lx_spawn_primitive(s, LX_PRIM_PLANE, "Plane", lx_v3(0, 0, 0));
    lx_get_collider(s, plane)->shape = LX_COLLIDER_MESH;
    lx_entity_set_euler(s, plane, lx_v3(0, 0, -20));
    LxEntity roll = body(s, "Roller", LX_COLLIDER_SPHERE, lx_v3(0, 2, 0));
    run(s, 1.5f);
    LxVec3 rp = lx_entity_world_position(s, roll);
    CHECK(rp.x > 0.5f && rp.y > -3.0f, "sphere rolls down a tilted mesh collider (x=%g y=%g)", rp.x, rp.y);
    lx_scene_destroy(s);
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

static void test_png_formats(void) {
    printf("png formats\n");
    int w, h;
    uint8_t *p = lx_png_decode(PNG_RGB16, sizeof PNG_RGB16, &w, &h);
    CHECK(p && w == 3 && h == 2, "16-bit RGB decodes");
    if (p) CHECK(p[(1 * 3 + 2) * 4 + 0] == 200 && p[(1 * 3 + 2) * 4 + 1] == 200 && p[(1 * 3 + 2) * 4 + 2] == 77 && p[3] == 255, "16-bit values");
    lx_free(p);
    p = lx_png_decode(PNG_PAL4, sizeof PNG_PAL4, &w, &h);
    CHECK(p && w == 5 && h == 1, "4-bit palette decodes");
    if (p) CHECK(p[3 * 4] == 150 && p[3 * 4 + 1] == 10 && p[2 * 4 + 3] == 128 && p[4 * 4 + 3] == 255, "palette colours and tRNS alpha");
    lx_free(p);
    p = lx_png_decode(PNG_ADAM7, sizeof PNG_ADAM7, &w, &h);
    CHECK(p && w == 9 && h == 9, "interlaced decodes");
    bool good = p != NULL;
    for (int y = 0; p && y < 9; y++)
        for (int x = 0; x < 9; x++) good &= p[(y * 9 + x) * 4] == (uint8_t)(x * 20 + y);
    CHECK(good, "interlaced pixels in the right places");
    lx_free(p);
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

#define GI_EYE lx_v3(0, 1.5f, 0)
static void test_gi(void) {
    printf("live gi\n");
    LxScene *s = lx_scene_create("GI");
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(20, 1, 20));
    LxEntity wall = lx_spawn_primitive(s, LX_PRIM_CUBE, "Red Wall", lx_v3(0, 2, -2));
    lx_entity_set_scale(s, wall, lx_v3(6, 4, 0.5f));
    lx_strcpy(lx_get_mesh(s, wall)->material, LX_REF_MAX, "builtin:red");
    LxEntity sun = lx_entity_create(s, "Sun");
    lx_entity_set_euler(s, sun, lx_v3(60, 180, 0));
    lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL)->intensity = 3.0f;
    s->env.ambient_intensity = 0.0f; /* only the sun: every bit of GI must come from bounces */
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_rays_per_frame = 60000;
    double t0 = lx_time();
    for (int f = 0; f < 6; f++) lx_gi_update(s, &g, GI_EYE);
    double ms = (lx_time() - t0) * 1000.0 / 6.0;
    const LxGiVolume *v = lx_gi_volume();
    CHECK(v->valid && v->res[0] > 2 && v->casc[0].on, "gi volume valid");
    /* in front of the red wall, a surface facing it: the bounce light is red */
    LxVec3 e = lx_gi_irradiance(lx_v3(0, 1.5f, -1.45f), lx_v3(0, 0, -1), true);
    CHECK(e.x > 0, "irradiance present near wall (red %g)", e.x);
    CHECK(e.x > e.y * 1.3f && e.x > e.z * 1.3f, "colour bleeding towards the wall: r %g g %g b %g", e.x, e.y, e.z);
    printf("  %d rays/frame in %.1f ms\n", lx_gi_last_ray_count(), ms);
    lx_gi_shutdown();
    lx_scene_destroy(s);
}

/* noise = relative RMS change of the indirect light on the floor (7 x 7 points, as the shader sees it) over 4 frames */
static void gi_floor_layer(float *out, int *n) {
    *n = 0;
    for (int z = 0; z < 7; z++)
        for (int x = 0; x < 7; x++) {
            LxVec3 e = lx_gi_irradiance(lx_v3(-6.0f + 2.0f * x, 0.02f, -6.0f + 2.0f * z), lx_v3(0, 1, 0), true);
            out[(*n)++] = e.x + e.y + e.z;
        }
}
static float gi_flicker(LxScene *s, const LxGraphicsSettings *g) {
    static float a[64], b[64];
    int n;
    gi_floor_layer(a, &n);
    for (int f = 0; f < 4; f++) lx_gi_update(s, g, GI_EYE);
    gi_floor_layer(b, &n);
    double d = 0, m = 0;
    for (int i = 0; i < n; i++) { d += (double)(b[i] - a[i]) * (b[i] - a[i]); m += (double)b[i] * b[i]; }
    return m > 0 ? (float)sqrt(d / m) : 1.0f;
}

static void test_gi_convergence(void) {
    printf("live gi convergence\n");
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
    g.gi_rays_per_frame = 60000;
    for (int f = 0; f < 2; f++) lx_gi_update(s, &g, GI_EYE);
    float early = gi_flicker(s, &g);
    for (int f = 0; f < 300; f++) lx_gi_update(s, &g, GI_EYE);
    float late = gi_flicker(s, &g);
    printf("  change over 4 frames: from frame 2 %.4f, after 300 more %.4f (%d rays then)\n", early, late, lx_gi_last_ray_count());
    CHECK(late < 0.005f && lx_gi_last_ray_count() == 0, "static lighting settles, and then nothing is traced (%.4f, %d rays)", late, lx_gi_last_ray_count());
    /* moving the light must restart accumulation quickly instead of smearing the old result */
    lx_entity_set_position(s, lamp, lx_v3(6, 4, 6));
    for (int f = 0; f < 30; f++) lx_gi_update(s, &g, GI_EYE);
    const LxGiVolume *v = lx_gi_volume();
    int y = (int)((0.6f - v->origin.y) / v->cell_size.y);
    int cx = (int)((6 - v->origin.x) / v->cell_size.x), cz = (int)((6 - v->origin.z) / v->cell_size.z);
    int ox = (int)((-6 - v->origin.x) / v->cell_size.x), oz = (int)((-6 - v->origin.z) / v->cell_size.z);
    float near_l = v->sh[((size_t)(cz * v->res[1] + y) * v->res[0] + cx) * 12], far_l = v->sh[((size_t)(oz * v->res[1] + y) * v->res[0] + ox) * 12];
    CHECK(near_l > far_l * 2.0f, "GI follows a moved light within 30 frames (%g vs %g)", near_l, far_l);
    /* a character walking about and a flickering lamp must not keep the whole scene noisy (the game templates did) */
    lx_entity_set_position(s, lamp, lx_v3(0, 4, 0));
    for (int f = 0; f < 300; f++) lx_gi_update(s, &g, GI_EYE);
    float still = gi_flicker(s, &g);
    LxEntity walker = lx_spawn_primitive(s, LX_PRIM_CAPSULE, "Walker", lx_v3(-5, 1, 0));
    lx_add_character(s, walker);
    lx_get_mesh(s, walker)->static_geometry = false;
    float noisy = 0;
    for (int f = 0; f < 400; f++) {
        lx_entity_set_position(s, walker, lx_v3(-5 + (float)(f % 200) * 0.05f, 1, 0));
        lx_get_light(s, lamp)->intensity = 200 * (0.995f + 0.005f * sinf((float)f));   /* a slight flicker: the change itself stays small */
        lx_gi_update(s, &g, GI_EYE);
        if (f == 399) noisy = gi_flicker(s, &g);
    }
    CHECK(noisy < 0.05f, "moving characters and flickering lights don't keep the GI noisy (%.4f; while it gathered: %.4f)", noisy, early);
    (void)still;
    lx_gi_shutdown();
    lx_scene_destroy(s);
}

/* moving the camera or pressing Play must never leave the GI dark while it gathers light again: probes that come
 * into a level start from the coarser level's light, and a copy of the scene (Play) keeps all of it */
static void test_gi_no_dark_sweep(void) {
    printf("live gi: no dark sweep when the camera moves or Play starts\n");
    LxScene *s = lx_scene_create("Sweep");
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(80, 1, 80));
    for (int i = 0; i < 12; i++) {
        LxEntity b = lx_spawn_primitive(s, LX_PRIM_CUBE, "Box", lx_v3((i % 4) * 8.0f - 12, 1, (i / 4) * 8.0f - 8));
        lx_entity_set_scale(s, b, lx_v3(2, 2, 2));
    }
    LxEntity sun = lx_entity_create(s, "Sun");
    lx_entity_set_euler(s, sun, lx_v3(50, 30, 0));
    lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL)->intensity = 3;
    s->env.ambient_intensity = 0.3f;
    LxGraphicsSettings g = lx_graphics_defaults(1);
    LxVec3 at = lx_v3(9.5f, 0.05f, 0.5f);   /* outside the finest level until the camera comes over */
    for (int f = 0; f < 400; f++) lx_gi_update(s, &g, lx_v3(0, 1.7f, 0));
    for (int f = 0; f < 400; f++) lx_gi_update(s, &g, lx_v3(9, 1.7f, 0));
    LxVec3 settled = lx_gi_irradiance(at, lx_v3(0, 1, 0), true);
    for (int f = 0; f < 400; f++) lx_gi_update(s, &g, lx_v3(0, 1.7f, 0));
    float worst = 1e9f;
    for (int step = 1; step <= 18; step++) {   /* walk over in half-metre steps, looking at each frame */
        lx_gi_update(s, &g, lx_v3(step * 0.5f, 1.7f, 0));
        LxVec3 e = lx_gi_irradiance(at, lx_v3(0, 1, 0), true);
        worst = lx_minf(worst, (e.x + e.y + e.z) / (settled.x + settled.y + settled.z));
    }
    CHECK(worst > 0.6f, "walking over, the light there never drops away (lowest %.2f of the settled light)", worst);
    /* Play: the scene is copied */
    for (int f = 0; f < 400; f++) lx_gi_update(s, &g, lx_v3(9, 1.7f, 0));
    LxVec3 before = lx_gi_irradiance(at, lx_v3(0, 1, 0), true);
    char *text = lx_scene_serialize(s);
    LxScene *copy = lx_scene_deserialize(text);
    lx_free(text);
    lx_gi_update(copy, &g, lx_v3(9, 1.7f, 0));
    LxVec3 after = lx_gi_irradiance(at, lx_v3(0, 1, 0), true);
    float ratio = (after.x + after.y + after.z) / (before.x + before.y + before.z);
    CHECK(ratio > 0.95f && ratio < 1.05f, "a copy of the scene (Play) keeps the light it had (%.3f)", ratio);
    lx_gi_shutdown();
    lx_scene_destroy(copy);
    lx_scene_destroy(s);
}

/* rough terrain: the GI measures its distance to the ground from the terrain's own heights, so the ground under
 * the camera isn't covered in dark blotches (it used a 2 m simplified mesh, which the real ground dips under) */
static void test_gi_terrain_ground(void) {
    printf("live gi: rough terrain without blotches\n");
    char dir[LX_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", "luxora_test_gi_terrain");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_assets_set_root(dir);
    LxScene *s = lx_scene_create("Rough");
    LxEntity te = lx_entity_create(s, "T");
    LxTerrainComponent *t = lx_add_terrain(s, te, "");
    t->grass_layer = -1;
    lx_entity_set_position(s, te, lx_v3(-32, 0, -32));
    lx_terrain_generate(s, te, LX_TERRAIN_HILLS, 30, 3);
    LxTerrainBrush nb = {LX_TERRAIN_NOISE, 30, 1, 0.3f, 0, 4, 0};
    lx_terrain_begin_stroke(s, te);
    for (int i = 0; i < 30; i++) lx_terrain_brush(s, te, lx_v3(0, 0, 0), &nb, 0.1f);
    lx_terrain_end_stroke(s, te);
    lx_terrain_update(s, lx_v3(0, 0, 0), true);
    LxEntity sun = lx_entity_create(s, "Sun");
    lx_entity_set_euler(s, sun, lx_v3(45, 30, 0));
    lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL)->intensity = 3;
    s->env.ambient_intensity = 0.5f;
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_rays_per_frame = 60000;
    float y0 = 0;
    lx_terrain_height(s, 0, 0, &y0);
    for (int f = 0; f < 300; f++) lx_gi_update(s, &g, lx_v3(0, y0 + 1.7f, 0));
    int dark = 0, n = 0;
    for (int z = 0; z < 40; z++)
        for (int x = 0; x < 40; x++) {
            float wx = -5 + x * 0.25f, wz = -5 + z * 0.25f, h;
            LxVec3 nn;
            if (!lx_terrain_sample(s, wx, wz, &h, &nn, NULL)) continue;
            n++;
            if (lx_gi_dfao(lx_v3(wx, h + 0.01f, wz), nn) < 0.7f) dark++;
        }
    CHECK(n > 1000 && dark < n / 10, "contact shadows darken under a tenth of rough ground (%d of %d points)", dark, n);
    lx_gi_shutdown();
    lx_scene_destroy(s);
    lx_terrain_shutdown();
    lx_path_remove(dir);
}

/* two rooms side by side, a wall much thinner than a GI cell between them, a lamp in only one:
 * the dark room must stay dark (before the fix, the cells straddling the wall carried light through it) */
static float leak_ratio(bool walls, float *lit_out) {
    lx_gi_set_walls(walls);
    LxScene *s = lx_scene_create("Rooms");
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(16, 1, 8));
    LxEntity roof = lx_spawn_primitive(s, LX_PRIM_CUBE, "Ceiling", lx_v3(0, 3.5f, 0));
    lx_entity_set_scale(s, roof, lx_v3(16, 1, 8));
    static const float walls_xz[4][4] = {{-8.05f, 0, 0.1f, 8}, {8.05f, 0, 0.1f, 8}, {0, -4.05f, 16, 0.1f}, {0, 4.05f, 16, 0.1f}};
    for (int i = 0; i < 4; i++) {
        LxEntity w = lx_spawn_primitive(s, LX_PRIM_CUBE, "Outer Wall", lx_v3(walls_xz[i][0], 1.5f, walls_xz[i][1]));
        lx_entity_set_scale(s, w, lx_v3(walls_xz[i][2], 3, walls_xz[i][3]));
    }
    LxEntity mid = lx_spawn_primitive(s, LX_PRIM_CUBE, "Thin Wall", lx_v3(0.13f, 1.5f, 0));   /* off the cell grid on purpose */
    lx_entity_set_scale(s, mid, lx_v3(0.08f, 3, 8));
    LxEntity lamp = lx_entity_create(s, "Lamp");
    lx_entity_set_position(s, lamp, lx_v3(-4, 2, 0));
    lx_add_light(s, lamp, LX_LIGHT_POINT)->intensity = 200;
    s->env.ambient_intensity = 0;
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_rays_per_frame = 60000;
    for (int f = 0; f < 40; f++) lx_gi_update(s, &g, GI_EYE);
    /* floor points just either side of the thin wall, and the middle of each room */
    float lit = 0, dark = 0;
    for (int k = 0; k < 5; k++) {
        float z = -2.0f + k;
        LxVec3 a = lx_gi_irradiance(lx_v3(-0.25f, 0.01f, z), lx_v3(0, 1, 0), walls), b = lx_gi_irradiance(lx_v3(0.45f, 0.01f, z), lx_v3(0, 1, 0), walls);
        lit += a.x + a.y + a.z;
        dark += b.x + b.y + b.z;
    }
    lx_gi_shutdown();
    lx_scene_destroy(s);
    lx_gi_set_walls(true);
    if (lit_out) *lit_out = lit;
    return lit > 0 ? dark / lit : 1.0f;
}
/* moving things can't be static geometry: GI and sound would keep a frozen copy where they started */
static void test_static_safeguard(void) {
    printf("static geometry safeguard\n");
    LxScene *s = lx_scene_create("Static");
    LxEntity wall = lx_spawn_primitive(s, LX_PRIM_CUBE, "Wall", lx_v3(0, 1, 0));
    LxEntity ball = lx_spawn_primitive(s, LX_PRIM_SPHERE, "Ball", lx_v3(3, 1, 0));
    lx_add_rigidbody(s, ball);
    LxEntity hero = lx_entity_create(s, "Hero");
    lx_add_character(s, hero);
    LxEntity sword = lx_spawn_primitive(s, LX_PRIM_CUBE, "Sword", lx_v3(0, 1, 3));
    lx_entity_set_parent(s, sword, hero);
    const char *why = NULL;
    CHECK(lx_entity_can_be_static(s, wall, &why) && !why, "a wall can be static");
    CHECK(!lx_entity_can_be_static(s, ball, &why) && why && strstr(why, "Rigidbody"), "a Rigidbody can't (%s)", why ? why : "");
    CHECK(!lx_entity_can_be_static(s, sword, &why) && why && strstr(why, "Character Controller"), "nor what a character carries (%s)", why ? why : "");
    /* even with the flag set (older scenes), GI and sound leave them out */
    lx_get_mesh(s, ball)->static_geometry = true;
    lx_get_mesh(s, sword)->static_geometry = true;
    LxBvh only_wall, all;
    memset(&only_wall, 0, sizeof only_wall);
    memset(&all, 0, sizeof all);
    lx_bvh_build_from_scene(&all, s);
    lx_entity_destroy(s, ball);
    lx_entity_destroy(s, hero);
    lx_scene_update_transforms(s);
    lx_bvh_build_from_scene(&only_wall, s);
    CHECK(all.tri_count == only_wall.tri_count && all.tri_count > 0, "GI and sound use only the wall (%d vs %d triangles)", all.tri_count, only_wall.tri_count);
    lx_bvh_free(&all);
    lx_bvh_free(&only_wall);
    lx_scene_destroy(s);
}

/* moving things bounce light with their real triangles: a red cube with a Rigidbody, lit by a lamp, tints the
 * surface facing it red; moved away, the tint goes with it */
static LxVec3 bleed_at(LxScene *s, const LxGraphicsSettings *g, int frames) {
    for (int f = 0; f < frames; f++) lx_gi_update(s, g, GI_EYE);
    LxVec3 e = lx_v3(0, 0, 0);
    for (int k = 0; k < 3; k++) {   /* surfaces facing the cube's lit side, just in front of it */
        LxVec3 v = lx_gi_irradiance(lx_v3(-0.9f, 0.3f + 0.4f * k, 0), lx_v3(-1, 0, 0), true);
        e = lx_add3(e, v);
    }
    return e;
}
static void test_gi_moving(void) {
    printf("live gi: moving objects leave no dark halos\n");
    LxScene *s = lx_scene_create("Moving");
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(20, 1, 20));
    LxEntity sun = lx_entity_create(s, "Sun");
    lx_entity_set_euler(s, sun, lx_v3(50, 30, 0));
    lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL)->intensity = 3;
    s->env.ambient_intensity = 0.5f;
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_rays_per_frame = 60000;
    LxVec3 clear = bleed_at(s, &g, 200);
    /* a character walks right next to where we look: the ground's bounce light stays as it was */
    LxEntity hero = lx_spawn_primitive(s, LX_PRIM_CAPSULE, "Hero", lx_v3(-1.6f, 1, 0));
    lx_add_character(s, hero);
    float worst = 1;
    for (int f = 0; f < 120; f++) {
        lx_entity_set_position(s, hero, lx_v3(-1.6f + 0.4f * sinf(f * 0.1f), 1, 0.3f * cosf(f * 0.1f)));
        LxVec3 e = bleed_at(s, &g, 1);
        worst = lx_minf(worst, (e.x + e.y + e.z) / (clear.x + clear.y + clear.z));
    }
    CHECK(lx_gi_moving_triangles() == 0 && worst > 0.95f, "a character walking by leaves no dark halo (lowest %.3f of the light)", worst);
    lx_gi_shutdown();
    lx_scene_destroy(s);
}

/* a moving object doesn't darken itself: its face gets bounce light much like the open air there
 * (the user's character had black, then patchy, faces with the box stand-ins) */
static float self_lit(bool with_object) {
    LxScene *s = lx_scene_create("Self");
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(12, 1, 12));
    LxEntity wall = lx_spawn_primitive(s, LX_PRIM_CUBE, "Wall", lx_v3(0, 1.5f, -3));
    lx_entity_set_scale(s, wall, lx_v3(12, 3, 0.3f));
    LxEntity lamp = lx_entity_create(s, "Lamp");
    lx_entity_set_position(s, lamp, lx_v3(0, 2.5f, -1.5f));
    lx_add_light(s, lamp, LX_LIGHT_POINT)->intensity = 150;
    s->env.ambient_intensity = 0;
    if (with_object) {   /* a person-sized moving box */
        LxEntity body = lx_spawn_primitive(s, LX_PRIM_CUBE, "Person", lx_v3(0, 0.9f, 0));
        lx_entity_set_scale(s, body, lx_v3(0.5f, 1.8f, 0.35f));
        lx_add_rigidbody(s, body);
    }
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_rays_per_frame = 60000;
    for (int f = 0; f < 40; f++) lx_gi_update(s, &g, GI_EYE);
    float sum = 0;
    for (int k = 0; k < 4; k++) {   /* the "face", towards the lamp-lit wall: what the bounce light comes from */
        LxVec3 e = lx_gi_irradiance(lx_v3(-0.15f + 0.1f * k, 1.2f + 0.1f * k, -0.18f), lx_v3(0, 0, -1), true);
        sum += e.x + e.y + e.z;
    }
    lx_gi_shutdown();
    lx_scene_destroy(s);
    return sum;
}
static void test_gi_self_shadow(void) {
    printf("live gi: moving objects don't darken themselves\n");
    float open_air = self_lit(false), face = self_lit(true);
    printf("  bounce light on a moving body's face: %.4g, in open air there: %.4g\n", face, open_air);
    CHECK(open_air > 0, "open air has bounce light (%.4g)", open_air);
    CHECK(face > open_air * 0.6f && face < open_air * 1.3f, "a moving body's face is lit like the air around it (%.4g vs %.4g)", face, open_air);
}

/* calibration: under an open sky of radiance L, the light arriving at an upward-facing surface is pi x L
 * (exactly what the shader uses as ambient light when GI is off). Floor reflects almost no blue, so the blue
 * channel is the sky alone: near the floor and in mid-air, the GI must give the L1 reconstruction of an open sky. */
static void test_gi_sky_calibration(void) {
    printf("live gi: sky calibration\n");
    LxScene *s = lx_scene_create("Sky");
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(40, 1, 40));
    lx_strcpy(lx_get_mesh(s, floor)->material, LX_REF_MAX, "builtin:red");
    LxEntity pole = lx_spawn_primitive(s, LX_PRIM_CUBE, "Pole", lx_v3(19.8f, 3, 19.8f));   /* makes the grid tall */
    lx_entity_set_scale(s, pole, lx_v3(0.2f, 6, 0.2f));
    s->env.sky_zenith = lx_v3(1, 1, 1);
    s->env.sky_horizon = lx_v3(1, 1, 1);
    s->env.ambient_intensity = 0.5f;
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_rays_per_frame = 60000;
    for (int f = 0; f < 60; f++) lx_gi_update(s, &g, GI_EYE);
    /* An open sky of radiance L over the floor: the probes see L in every direction above the horizon. What the
     * shader gives an upward surface from that is the L1 reconstruction of a lit upper hemisphere. */
    const float L = 0.5f;
    float lo = 0, hi = 0, want_lo = 0, want_hi = 0;
    for (int pass = 0; pass < 2; pass++) {
        float y = pass ? 1.6f : 0.02f;
        float c[4] = {L * 0.282095f * 2.0f * LX_PI, L * 0.488603f * LX_PI, 0, 0};
        float want = lx_gi_eval_l1(c, lx_v3(0, 1, 0));
        float got = 0;
        for (int k = 0; k < 9; k++) {
            float x = -4.0f + (k % 3) * 4.0f, z = -4.0f + (k / 3) * 4.0f;
            got += lx_gi_irradiance(lx_v3(x, y, z), lx_v3(0, 1, 0), true).z / 9.0f;
        }
        if (pass) { hi = got; want_hi = want; } else { lo = got; want_lo = want; }
    }
    printf("  sky light on an upward surface: %.3f at the floor (expected %.3f), %.3f in mid-air (expected %.3f)\n", lo, want_lo, hi, want_hi);
    CHECK(fabsf(lo / want_lo - 1.0f) < 0.08f, "at the floor it matches the true irradiance (%.3f vs %.3f)", lo, want_lo);
    CHECK(fabsf(hi / want_hi - 1.0f) < 0.08f, "and in mid-air (%.3f vs %.3f)", hi, want_hi);
    lx_gi_shutdown();
    lx_scene_destroy(s);
}

/* moving objects' BVH: refitting to new poses finds the same hits as building it again */
static void test_moving_refit(void) {
    printf("moving objects' BVH refit\n");
    LxScene *s = lx_scene_create("Refit");
    LxEntity balls[6];
    for (int i = 0; i < 6; i++) {
        balls[i] = lx_spawn_primitive(s, LX_PRIM_SPHERE, "Ball", lx_v3((float)i * 2.0f, 1, 0));
        lx_add_rigidbody(s, balls[i]);
    }
    LxBvh refit, fresh;
    memset(&refit, 0, sizeof refit);
    memset(&fresh, 0, sizeof fresh);
    lx_bvh_build_moving(&refit, s);
    for (int i = 0; i < 6; i++) lx_entity_set_position(s, balls[i], lx_v3((float)i * 2.0f + 0.7f, 1.0f + 0.3f * (float)i, (float)(i % 2)));
    CHECK(lx_bvh_refit_moving(&refit, s), "same objects: refits");
    lx_bvh_build_moving(&fresh, s);
    int agree = 0, total = 0;
    LxRng rng = lx_rng_seed(7, 1);
    for (int k = 0; k < 400; k++) {
        LxVec3 o = lx_v3(lx_rng_f(&rng) * 14 - 2, 6, lx_rng_f(&rng) * 4 - 2);
        LxRay r = {o, lx_norm3(lx_v3(lx_rng_f(&rng) - 0.5f, -1, lx_rng_f(&rng) - 0.5f))};
        LxHit a, b;
        bool ha = lx_bvh_intersect(&refit, r, 1e30f, &a), hb = lx_bvh_intersect(&fresh, r, 1e30f, &b);
        total++;
        if (ha == hb && (!ha || fabsf(a.t - b.t) < 1e-4f)) agree++;
    }
    CHECK(agree == total, "refitted and rebuilt trees find the same hits (%d / %d)", agree, total);
    lx_entity_destroy(s, balls[5]);
    lx_scene_update_transforms(s);
    CHECK(!lx_bvh_refit_moving(&refit, s), "an object removed: build again");
    lx_bvh_free(&refit);
    lx_bvh_free(&fresh);
    lx_scene_destroy(s);
}

/* the GI's light reconstruction never goes negative (the hard-edged dark shapes on a character's face were the
 * old one clamping at zero) and is exact for light from a single direction */
static void test_gi_reconstruction(void) {
    printf("live gi: light reconstruction\n");
    /* all light from straight above, irradiance w at normal incidence: radiance SH of a delta, w x basis */
    const float w = 2.0f;
    float c[4] = {w * 0.282095f, w * 0.488603f, 0, 0};   /* L0, y, z, x */
    CHECK(fabsf(lx_gi_eval_l1(c, lx_v3(0, 1, 0)) - w) < 1e-3f, "light from one direction: exact facing it (%g)", lx_gi_eval_l1(c, lx_v3(0, 1, 0)));
    float lowest = 1e9f, prev = lx_gi_eval_l1(c, lx_v3(0, 1, 0)), worst_jump = 0;
    for (int i = 1; i <= 180; i++) {   /* turning the surface all the way round: never below zero, no jumps */
        float a = (float)i * LX_PI / 180.0f;
        float e = lx_gi_eval_l1(c, lx_v3(sinf(a), cosf(a), 0));
        lowest = lx_minf(lowest, e);
        worst_jump = lx_maxf(worst_jump, fabsf(e - prev));
        prev = e;
    }
    float old_way = 0.282095f * LX_PI * c[0] + (2.0f * LX_PI / 3.0f) * 0.488603f * c[1] * -1.0f;   /* facing away, the old formula */
    CHECK(lowest >= 0 && old_way < 0, "facing away it never goes below zero (lowest %g; the old formula gave %g)", lowest, old_way);
    CHECK(worst_jump < w * 0.05f, "and changes smoothly as the surface turns (largest step %g per degree)", worst_jump);
    float even[4] = {1.0f, 0, 0, 0};
    CHECK(fabsf(lx_gi_eval_l1(even, lx_v3(0, 0, 1)) - LX_PI * 0.282095f) < 1e-4f, "even light stays even");
}

/* the user's scene: a flat ground, the sun and sky, a moving character. Before, the GI grid was sized from the
 * static geometry alone (a metre-tall slab over flat ground): the character's head was above it, unlit, and its
 * top edge cut across the face. Upper body, lower body, mid-jump and lifted high all get bounce light. */

/* ---------- lightmap UVs ---------- */
static int lm_find(int *p, int i) { while (p[i] != i) { p[i] = p[p[i]]; i = p[i]; } return i; }
static float lm_edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

/* checks one unwrapped mesh: inside the square, no two triangles on the same texel, padding between charts,
 * even texel density, geometry unchanged. Returns the number of problems. */
static int check_lightmap_uvs(const char *name, const LxMesh *m, const LxVec3 *orig_tris, int orig_tri_count) {
    int bad = 0;
    const LxMeshLod *l = &m->lods[0];
    int nt = l->index_count / 3, res = m->lm_pack_res;
    CHECK(m->has_uv2 && m->uv2_generated && res >= 64, "%s: unwrapped (res %d)", name, res);
    CHECK(nt == orig_tri_count, "%s: triangle count kept (%d vs %d)", name, nt, orig_tri_count);
    float geo_err = 0;
    for (int t = 0; t < nt && t < orig_tri_count; t++)
        for (int k = 0; k < 3; k++) {
            const LxVertex *v = &l->vertices[l->indices[t * 3 + k]];
            geo_err = lx_maxf(geo_err, lx_len3(lx_sub3(lx_v3(v->px, v->py, v->pz), orig_tris[t * 3 + k])));
        }
    CHECK(geo_err < 1e-6f, "%s: geometry unchanged (err %g)", name, geo_err);
    float lo = 1, hi = 0;
    for (int i = 0; i < l->vertex_count; i++) {
        lo = lx_minf(lo, lx_minf(l->vertices[i].u2, l->vertices[i].v2));
        hi = lx_maxf(hi, lx_maxf(l->vertices[i].u2, l->vertices[i].v2));
    }
    CHECK(lo > 0 && hi < 1, "%s: UVs inside the square (%g..%g)", name, lo, hi);
    /* charts = triangles joined by an identical lightmap UV (vertices are split on chart borders) */
    int *par = lx_alloc(sizeof(int) * (size_t)nt);
    for (int t = 0; t < nt; t++) par[t] = t;
    int *first = lx_alloc(sizeof(int) * (size_t)l->vertex_count);
    for (int i = 0; i < l->vertex_count; i++) first[i] = -1;
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) {
            uint32_t vi = l->indices[t * 3 + k];
            if (first[vi] < 0) first[vi] = t;
            else par[lm_find(par, t)] = lm_find(par, first[vi]);
        }
    lx_free(first);
    /* same position in the same chart can be two vertices (normal seams): join on equal u2,v2 too */
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++)
            for (int u = t + 1; u < nt && u < t + 64; u++)
                for (int j = 0; j < 3; j++) {
                    const LxVertex *a = &l->vertices[l->indices[t * 3 + k]], *b = &l->vertices[l->indices[u * 3 + j]];
                    if (a->u2 == b->u2 && a->v2 == b->v2) par[lm_find(par, u)] = lm_find(par, t);
                }
    /* rasterise texel centres */
    int *owner = lx_alloc(sizeof(int) * (size_t)res * (size_t)res), *chart = lx_alloc(sizeof(int) * (size_t)res * (size_t)res);
    for (int i = 0; i < res * res; i++) owner[i] = chart[i] = -1;
    int overlaps = 0;
    double dens_sum = 0, wsum = 0;
    float dmin = INFINITY, dmax = 0;
    float *dens = lx_alloc(sizeof(float) * (size_t)nt);
    for (int t = 0; t < nt; t++) {
        const LxVertex *a = &l->vertices[l->indices[t * 3]], *b = &l->vertices[l->indices[t * 3 + 1]], *c = &l->vertices[l->indices[t * 3 + 2]];
        float ax = a->u2 * res, ay = a->v2 * res, bx = b->u2 * res, by = b->v2 * res, cx = c->u2 * res, cy = c->v2 * res;
        float area = lm_edge(ax, ay, bx, by, cx, cy);
        float wa = lx_len3(lx_cross3(lx_v3(b->px - a->px, b->py - a->py, b->pz - a->pz), lx_v3(c->px - a->px, c->py - a->py, c->pz - a->pz)));
        dens[t] = wa > 1e-10f ? fabsf(area) / wa : -1;
        if (dens[t] > 0) { dens_sum += dens[t] * wa; wsum += wa; }
        if (fabsf(area) < 1e-9f) continue;
        float sg = area > 0 ? 1.0f : -1.0f;
        int x0 = (int)floorf(lx_minf(ax, lx_minf(bx, cx))), x1 = (int)ceilf(lx_maxf(ax, lx_maxf(bx, cx)));
        int y0 = (int)floorf(lx_minf(ay, lx_minf(by, cy))), y1 = (int)ceilf(lx_maxf(ay, lx_maxf(by, cy)));
        for (int y = lx_maxf(y0, 0); y <= y1 && y < res; y++)
            for (int x = lx_maxf(x0, 0); x <= x1 && x < res; x++) {
                float px = x + 0.5f, py = y + 0.5f, e = 1e-3f * fabsf(area);
                if (sg * lm_edge(ax, ay, bx, by, px, py) <= e || sg * lm_edge(bx, by, cx, cy, px, py) <= e || sg * lm_edge(cx, cy, ax, ay, px, py) <= e) continue;
                int i = y * res + x;
                if (owner[i] >= 0) overlaps++;
                owner[i] = t;
                chart[i] = lm_find(par, t);
            }
    }
    float mean = wsum > 0 ? (float)(dens_sum / wsum) : 0;
    double tiny = wsum / nt * 1e-4;   /* slivers (a sphere's pole) have no meaningful density */
    for (int t = 0; t < nt; t++) {
        const LxVertex *a = &l->vertices[l->indices[t * 3]], *b = &l->vertices[l->indices[t * 3 + 1]], *c = &l->vertices[l->indices[t * 3 + 2]];
        if (lx_len3(lx_cross3(lx_v3(b->px - a->px, b->py - a->py, b->pz - a->pz), lx_v3(c->px - a->px, c->py - a->py, c->pz - a->pz))) < tiny) dens[t] = -1;
    }
    for (int t = 0; t < nt; t++)
        if (dens[t] > 0) { dmin = lx_minf(dmin, dens[t] / mean); dmax = lx_maxf(dmax, dens[t] / mean); }
    lx_free(dens);
    int crowded = 0;
    for (int y = 0; y < res; y++)
        for (int x = 0; x < res; x++) {
            int c = chart[y * res + x];
            if (c < 0) continue;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= res || yy >= res) continue;
                    int o = chart[yy * res + xx];
                    if (o >= 0 && o != c) crowded++;
                }
        }
    CHECK(overlaps == 0, "%s: %d texels covered twice", name, overlaps);
    CHECK(crowded == 0, "%s: %d texels closer than the padding to another chart", name, crowded);
    CHECK(dmin > 0.45f && dmax < 2.2f, "%s: even texel density (%.2f..%.2f of the mean)", name, dmin, dmax);
    bad += overlaps + crowded;
    printf("  %-9s %5d tris  %4d charts  %4d px square\n", name, nt, m->lm_charts, res);
    lx_free(owner);
    lx_free(chart);
    lx_free(par);
    return bad;
}

static LxVec3 *lm_soup(const LxMesh *m) {
    const LxMeshLod *l = &m->lods[0];
    LxVec3 *p = lx_alloc(sizeof(LxVec3) * (size_t)l->index_count);
    for (int i = 0; i < l->index_count; i++) {
        const LxVertex *v = &l->vertices[l->indices[i]];
        p[i] = lx_v3(v->px, v->py, v->pz);
    }
    return p;
}

static void test_lightmap_uvs(void) {
    printf("lightmap UVs\n");
    /* primitives come unwrapped */
    for (int p = 0; p < LX_PRIM_COUNT; p++) {
        LxMesh *m = lx_mesh_build_primitive((LxPrimitive)p);
        /* unwrap again from scratch on a copy to compare geometry */
        LxMesh *c = lx_mesh_create(m->lods[0].vertices, m->lods[0].vertex_count, m->lods[0].indices, m->lods[0].index_count, "copy");
        LxVec3 *soup = lm_soup(c);
        int tris = c->lods[0].index_count / 3;
        CHECK(lx_mesh_unwrap_lightmap(c), "%s unwraps", lx_primitive_name((LxPrimitive)p));
        check_lightmap_uvs(lx_primitive_name((LxPrimitive)p), c, soup, tris);
        CHECK(m->has_uv2 && m->lm_world_size > 0, "%s built with lightmap UVs", lx_primitive_name((LxPrimitive)p));
        lx_free(soup);
        lx_mesh_destroy(c);
        lx_mesh_destroy(m);
    }
    /* a torus (saddles, curving both ways) and a lumpy rock (folds and slivers), smooth-shaded and shared vertices */
    for (int kind = 0; kind < 3; kind++) {
        LxVertex *v = NULL;
        uint32_t *idx = NULL;
        int vc = 0, vcap = 0, ic = 0, icap = 0;
        const int U = 48, V = 24;
        for (int j = 0; j <= V; j++)
            for (int i = 0; i <= U; i++) {
                float a = (float)i / U * 2 * LX_PI, b = (float)j / V * (kind == 0 ? 2 * LX_PI : LX_PI);
                LxVec3 p;
                if (kind == 0) p = lx_v3((1 + 0.35f * cosf(b)) * cosf(a), 0.35f * sinf(b), (1 + 0.35f * cosf(b)) * sinf(a));
                else {
                    LxVec3 d = lx_v3(sinf(b) * cosf(a), cosf(b), sinf(b) * sinf(a));
                    float r = 1 + 0.25f * sinf(3 * a + 2 * b) * sinf(5 * b) + 0.1f * cosf(7 * a);
                    if (kind == 2) r = 1 + 0.6f * sinf(4 * a) * sinf(3 * b);   /* deep folds */
                    p = lx_scale3(d, r);
                }
                LxVertex x;
                memset(&x, 0, sizeof x);
                x.px = p.x; x.py = p.y; x.pz = p.z;
                x.ny = 1;
                LX_VEC_PUSH(v, vc, vcap, x);
            }
        for (int j = 0; j < V; j++)
            for (int i = 0; i < U; i++) {
                uint32_t a = (uint32_t)(j * (U + 1) + i), b = a + 1, c = a + (uint32_t)(U + 1), d = c + 1;
                uint32_t q[6] = {a, c, b, b, c, d};
                for (int k = 0; k < 6; k++) LX_VEC_PUSH(idx, ic, icap, q[k]);
            }
        LxMesh *m = lx_mesh_create(v, vc, idx, ic, "shape");
        LxVec3 *soup = lm_soup(m);
        CHECK(lx_mesh_unwrap_lightmap(m), "shape unwraps");
        check_lightmap_uvs(kind == 0 ? "torus" : kind == 1 ? "rock" : "folds", m, soup, ic / 3);
        lx_free(soup);
        lx_mesh_destroy(m);
        lx_free(v);
        lx_free(idx);
    }
    /* deterministic: the same mesh gets the same UVs */
    LxMesh *a = lx_mesh_build_primitive(LX_PRIM_SPHERE), *b = lx_mesh_build_primitive(LX_PRIM_SPHERE);
    bool same = a->lods[0].vertex_count == b->lods[0].vertex_count;
    for (int i = 0; same && i < a->lods[0].vertex_count; i++) same = !memcmp(&a->lods[0].vertices[i], &b->lods[0].vertices[i], sizeof(LxVertex));
    CHECK(same, "unwrap is deterministic");
    /* LODs carry lightmap UVs too */
    bool lod_uv = a->lod_count > 1;
    for (int l = 1; l < a->lod_count; l++)
        for (int i = 0; i < a->lods[l].vertex_count; i++) lod_uv &= a->lods[l].vertices[i].u2 > 0 && a->lods[l].vertices[i].u2 < 1;
    CHECK(lod_uv, "LODs have lightmap UVs (%d LODs)", a->lod_count);
    /* a cube stretched into a floor gets its own unwrap: the big top takes a big share of the lightmap */
    LxMesh *cube = lx_mesh_build_primitive(LX_PRIM_CUBE);
    LxMesh *flat = lx_mesh_lightmap_variant(cube, lx_v3(16, 1, 16));
    CHECK(flat != cube && flat == lx_mesh_lightmap_variant(cube, lx_v3(32, 2, 32.0001f)), "stretched copies are cached by proportions");
    CHECK(lx_mesh_lightmap_variant(cube, lx_v3(2, 2.1f, 2)) == cube, "an evenly scaled object uses the mesh as it is");
    for (int pass = 0; pass < 2; pass++) {
        const LxMesh *m = pass ? flat : lx_mesh_build_primitive(LX_PRIM_CUBE);
        double top = 0, all = 0;
        const LxMeshLod *l = &m->lods[0];
        for (int t = 0; t + 2 < l->index_count; t += 3) {
            const LxVertex *p0 = &l->vertices[l->indices[t]], *p1 = &l->vertices[l->indices[t + 1]], *p2 = &l->vertices[l->indices[t + 2]];
            double ua = 0.5 * fabs((double)(p1->u2 - p0->u2) * (p2->v2 - p0->v2) - (double)(p2->u2 - p0->u2) * (p1->v2 - p0->v2));
            all += ua;
            if (p0->ny > 0.9f) top += ua;
        }
        if (pass) CHECK(top / all > 0.4, "the stretched floor's top gets %.0f%% of its lightmap UVs", 100 * top / all);
        else { CHECK(top / all < 0.2, "a plain cube's top gets a sixth (%.0f%%)", 100 * top / all); lx_mesh_destroy((LxMesh *)m); }
    }
    lx_mesh_destroy(cube);   /* takes its stretched copies with it */
    lx_mesh_destroy(a);
    lx_mesh_destroy(b);
}

/* ---------- baked lighting ---------- */
/* average baked irradiance (resolved colour) of an object's texels within r of p whose normal faces n */
static LxVec3 lm_probe(const LxLightmapBake *b, LxScene *s, LxEntity e, LxVec3 p, float r, LxVec3 n, int *count) {
    int tile = -1;
    for (int k = 0; k < b->tile_count; k++)
        if (b->tiles[k].entity == (int)(e & LX_INDEX_MASK)) tile = k;
    LX_UNUSED(s);
    LxVec3 sum = lx_v3(0, 0, 0);
    int c = 0;
    for (int i = 0; i < b->texel_count; i++) {
        if (b->owner[i] != tile) continue;
        const float *t = &b->texels[(size_t)i * 12];
        if (t[3] <= 0 || lx_dot3(lx_v3(t[8], t[9], t[10]), n) < 0.9f) continue;
        if (lx_len3(lx_sub3(lx_v3(t[0], t[1], t[2]), p)) > r) continue;
        sum = lx_add3(sum, lx_v3(b->color[i * 4], b->color[i * 4 + 1], b->color[i * 4 + 2]));
        c++;
    }
    if (count) *count = c;
    return c ? lx_scale3(sum, 1.0f / c) : sum;
}

static LxLightmapBake *lm_bake_cpu(LxScene *s) {
    LxLightmapBake *b = lx_lightmap_prepare(s);
    if (!b) return NULL;
    for (int k = 0; k < b->set.samples; k++) lx_lightmap_trace_cpu(b, 0, b->texel_count + b->voxel_count, (uint32_t)k);
    lx_lightmap_resolve(b);
    return b;
}


/* ---- Day-night sky and weather ---- */
static LxEntity first_dir_light(LxScene *s) {
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (d->alive && (d->comps & LX_COMP_LIGHT) && d->light.type == LX_LIGHT_DIRECTIONAL)
            return lx_entity_make((uint32_t)i, d->generation & ((1u << (32 - LX_INDEX_BITS)) - 1u));
    }
    return 0;
}
static float sky_lum(LxVec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
static void test_sky(void) {
    printf("day-night sky and weather\n");
    LxScene *s = lx_scene_create_default("Sky");
    LxEnvironment *e = lx_scene_environment(s);
    CHECK(e->sky_mode == LX_SKY_GRADIENT && e->weather == LX_WEATHER_CLEAR, "new scenes keep the gradient sky");
    LxEnvironment eff;
    lx_env_effective(s, &eff);
    CHECK(!memcmp(&eff.sky_zenith, &e->sky_zenith, sizeof eff.sky_zenith), "gradient: the effective sky is the scene's");
    e->sky_mode = LX_SKY_DAY_NIGHT;
    e->latitude = 35; e->day_of_year = 172; e->sun_heading = 0;

    /* where the sun is: east in the morning, high at noon, gone at midnight; lower in winter */
    lx_sky_set_time(s, 7);
    LxVec3 morning = lx_sky_sun_direction(s);
    CHECK(morning.x > 0.5f && morning.y > 0, "the morning sun is in the east, up (%.2f %.2f %.2f)", morning.x, morning.y, morning.z);
    lx_sky_set_time(s, 12);
    LxVec3 noon = lx_sky_sun_direction(s);
    CHECK(noon.y > 0.95f && lx_sky_is_day(s), "midsummer noon at 35 degrees: nearly overhead (%.3f)", noon.y);
    CHECK(noon.z > 0, "northern latitudes: the noon sun is to the south (+Z) (%.3f)", noon.z);
    lx_sky_set_time(s, 0);
    CHECK(lx_sky_sun_direction(s).y < -0.3f && !lx_sky_is_day(s), "midnight: the sun is below the horizon");
    e->day_of_year = 355;
    lx_sky_set_time(s, 12);
    CHECK(lx_sky_sun_direction(s).y < noon.y - 0.2f, "midwinter noon is lower (%.2f)", lx_sky_sun_direction(s).y);
    e->day_of_year = 172;
    lx_sky_set_time(s, 17);
    CHECK(lx_sky_sun_direction(s).x < -0.5f, "the afternoon sun is in the west");

    /* the light: bright white at noon, dimmer and redder low in the sky, the moon at night */
    lx_sky_set_time(s, 12);
    lx_sky_prepare(s);
    const LxSkyEval *v = lx_sky_eval(s);
    LxVec3 noon_light = v->sun_light, noon_zen = v->zenith;
    CHECK(sky_lum(noon_light) > 2.0f && noon_light.z / noon_light.x > 0.7f, "noon sunlight is bright and white (%.2f %.2f %.2f)",
          noon_light.x, noon_light.y, noon_light.z);
    CHECK(noon_zen.z > noon_zen.x * 1.5f, "the noon sky is blue (%.3f %.3f %.3f)", noon_zen.x, noon_zen.y, noon_zen.z);
    LxEntity sun = first_dir_light(s);
    lx_scene_update_transforms(s);
    LxVec3 fwd = lx_norm3(lx_mat4_dir(lx_entity_data(s, sun)->world, lx_v3(0, 0, 1)));
    CHECK(lx_dot3(fwd, lx_neg3(v->sun_dir)) > 0.999f, "the directional light shines from the sun");
    CHECK(lx_get_light(s, sun)->mode == LX_LIGHT_REALTIME, "a moving sun is real-time");
    lx_sky_set_time(s, 18.9f);
    lx_sky_prepare(s);
    LxVec3 low = v->sun_light;
    CHECK(sky_lum(low) < sky_lum(noon_light) * 0.8f && low.x / lx_maxf(low.z, 1e-4f) > noon_light.x / noon_light.z * 1.5f,
          "near sunset the light is dimmer and redder (%.2f %.2f %.2f)", low.x, low.y, low.z);
    lx_sky_set_time(s, 0.5f);
    e->moon_phase = 0.5f;
    lx_sky_prepare(s);
    CHECK(sky_lum(v->zenith) < sky_lum(noon_zen) * 0.15f && v->night > 0.9f, "night: the sky is dark (%.4f)", sky_lum(v->zenith));
    CHECK(sky_lum(v->moon_light) > 0.05f && v->moon_dir.y > 0.3f, "a full moon is up at midnight and gives light (%.3f)", sky_lum(v->moon_light));
    lx_scene_update_transforms(s);
    fwd = lx_norm3(lx_mat4_dir(lx_entity_data(s, sun)->world, lx_v3(0, 0, 1)));
    CHECK(lx_dot3(fwd, lx_neg3(v->moon_dir)) > 0.999f && lx_get_light(s, sun)->intensity > 0.05f, "at night the light is the moon");
    lx_env_effective(s, &eff);
    CHECK(!memcmp(&eff.sky_zenith, &v->zenith, sizeof eff.sky_zenith) && eff.ambient_intensity > 0, "Day-night: everything reads the sky's colours");

    /* clouds take sunlight; a preset sets everything */
    lx_sky_set_time(s, 12);
    lx_sky_set_weather(s, LX_WEATHER_STORM, 0);
    lx_sky_prepare(s);
    CHECK(e->weather == LX_WEATHER_STORM && e->rain > 0.9f && e->lightning > 0 && e->cloud_type == LX_CLOUDS_STORM, "the Storm preset");
    CHECK(sky_lum(v->sun_light) < sky_lum(noon_light) * 0.2f, "storm clouds take most of the sunlight (%.3f)", sky_lum(v->sun_light));
    CHECK(v->wetness > 0.9f && v->fog_density > e->fog_density, "rain: wet ground and mist straight away while editing");
    lx_sky_set_weather(s, LX_WEATHER_CLOUDY, 0);
    lx_sky_prepare(s);
    CHECK(sky_lum(v->sun_light) < sky_lum(noon_light) * 0.35f && e->cloud_coverage >= 0.85f,
          "an overcast sky hides the sun: little direct light gets through (%.3f)", sky_lum(v->sun_light));

    /* playing: the weather blends, the ground dries slowly, the clock runs */
    lx_sky_set_weather(s, LX_WEATHER_CLEAR, 0);
    s->simulating = true;
    s->sky.started = false;
    lx_sky_prepare(s);
    lx_sky_set_weather(s, LX_WEATHER_RAIN, 10);
    for (int i = 0; i < 50; i++) lx_sky_simulate(s, 0.1f);   /* 5 s of 10 */
    float half = s->sky.coverage;
    CHECK(half > 0.3f && half < 0.8f, "halfway through a 10 s change the clouds are halfway (%.2f)", half);
    for (int i = 0; i < 60; i++) lx_sky_simulate(s, 0.1f);
    CHECK(fabsf(s->sky.coverage - e->cloud_coverage) < 1e-4f && fabsf(s->sky.rain - e->rain) < 1e-4f, "after the blend time the weather has arrived");
    float wet_after = s->sky.wetness;
    CHECK(wet_after > 0.05f && wet_after < 0.9f, "the ground is getting wet, not soaked at once (%.2f)", wet_after);
    float t0 = e->time_of_day;
    e->day_length = 24;   /* 24 real minutes a day: an hour a minute */
    for (int i = 0; i < 60; i++) lx_sky_simulate(s, 1.0f);
    CHECK(fabsf(e->time_of_day - (t0 + 1.0f)) < 0.01f, "a minute of play moves the clock an hour (%.3f -> %.3f)", t0, e->time_of_day);
    lx_sky_strike_lightning(s);
    lx_sky_prepare(s);
    CHECK(v->flash > 0.5f && s->sky.thunder_in > 0, "lightning: a flash now, thunder later (%.2f s)", s->sky.thunder_in);
    s->simulating = false;

    /* saved and loaded */
    e->time_of_day = 19.25f; e->latitude = -33.9f; e->cloud_height = 2200; e->wind_direction = -45; e->moon_phase = 0.3f;
    e->weather = LX_WEATHER_CUSTOM; e->snow = 0.4f; e->weather_blend = 7;
    char *text = lx_scene_serialize(s);
    LxScene *c = lx_scene_deserialize(text);
    lx_free(text);
    const LxEnvironment *ce = lx_scene_environment(c);
    CHECK(ce->sky_mode == LX_SKY_DAY_NIGHT && NEAR(ce->time_of_day, 19.25f, 1e-4f) && NEAR(ce->latitude, -33.9f, 1e-4f) &&
          NEAR(ce->cloud_height, 2200, 1e-3f) && NEAR(ce->wind_direction, -45, 1e-4f) && NEAR(ce->moon_phase, 0.3f, 1e-4f) &&
          ce->weather == LX_WEATHER_CUSTOM && NEAR(ce->snow, 0.4f, 1e-4f) && NEAR(ce->weather_blend, 7, 1e-4f) &&
          ce->cloud_type == e->cloud_type && NEAR(ce->rain, e->rain, 1e-4f),
          "the sky and weather survive saving");
    lx_scene_destroy(c);
    CHECK(lx_sky_builtin_sound("thunder") && lx_sky_builtin_sound("thunder")->frame_count > 22050 * 4, "thunder is made in code");
    CHECK(lx_asset_sound("builtin:thunder") == lx_sky_builtin_sound("thunder"), "...and found as builtin:thunder");
    {   /* a sound that has played for hours still reads the right samples (float positions used to round after ~6 min) */
        uint32_t hours = 48000u * 3600u * 3u;   /* 3 hours at 48 kHz */
        float v = lx_audio_test_history(hours, 10.0f);
        float want = (float)((hours - 10u) % 1000u);
        CHECK(fabsf(v - want) < 1e-3f, "after 3 hours a voice still reads the right sample (%.2f, want %.0f)", v, want);
        v = lx_audio_test_history(hours, 10.5f);
        CHECK(fabsf(v - (want - 0.5f)) < 1e-2f || want < 1.0f, "...and between samples (%.3f)", v);
    }
    {   /* eight different thunder claps; rain loops that join without a click */
        int distinct = 0;
        for (int a = 0; a < 8; a++)
            for (int b = a + 1; b < 8; b++) {
                char na[16], nb[16];
                snprintf(na, sizeof na, "thunder%d", a);
                snprintf(nb, sizeof nb, "thunder%d", b);
                LxSound *x = lx_sky_builtin_sound(na);
                LxSound *y = lx_sky_builtin_sound(nb);
                distinct += x && y && x != y && (x->frame_count != y->frame_count || memcmp(x->samples, y->samples, 4000) != 0);
            }
        CHECK(distinct == 28, "eight different thunder claps (%d of 28 pairs differ)", distinct);
        for (int k = 0; k < 2; k++) {
            LxSound *r = lx_sky_builtin_sound(k ? "rain_heavy" : "rain_light");
            CHECK(r && r->frame_count >= 22050 * 30, "a long rain loop (%s)", k ? "heavy" : "light");
            if (!r) continue;
            float peak = 0, step = 0, jump = fabsf(r->samples[r->frame_count - 1] - r->samples[0]);
            for (int i = 1; i < r->frame_count; i++) { peak = lx_maxf(peak, fabsf(r->samples[i])); step = lx_maxf(step, fabsf(r->samples[i] - r->samples[i - 1])); }
            CHECK(peak > 0.1f && jump <= step, "the rain loop joins end to start like any other sample (%.3f, steps up to %.3f)", jump, step);
        }
    }
    lx_scene_destroy(s);

    /* lightmaps in Day-night: they keep how much sky each texel sees, for the sky of the moment */
    LxScene *b = lx_scene_create("Sky bake");
    b->bake.texels_per_unit = 2;
    b->bake.samples = 96;
    b->bake.bounces = 1;
    b->bake.ao_intensity = 0;
    b->env.sky_mode = LX_SKY_DAY_NIGHT;
    LxEntity floor = lx_spawn_primitive(b, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(b, floor, lx_v3(20, 1, 20));
    LxEntity roof = lx_spawn_primitive(b, LX_PRIM_CUBE, "Roof", lx_v3(0, 1.2f, 0));
    lx_entity_set_scale(b, roof, lx_v3(4, 0.2f, 4));
    LxLightmapBake *bk = lm_bake_cpu(b);
    CHECK(bk && bk->sky_separate, "a Day-night bake keeps the sky apart");
    if (bk) {
        float open = 0, under = 0, any_rgb = 0;
        int no = 0, nu = 0;
        for (int i = 0; i < bk->texel_count; i++) {
            const float *t = &bk->texels[(size_t)i * 12];
            if (t[3] <= 0 || t[9] < 0.9f || fabsf(t[1]) > 0.01f) continue;
            float a = bk->color[i * 4 + 3];
            any_rgb = lx_maxf(any_rgb, bk->color[i * 4 + 1]);
            if (fabsf(t[0]) > 6 && fabsf(t[2]) > 6) { open += -a; no++; }
            else if (fabsf(t[0]) < 0.5f && fabsf(t[2]) < 0.5f) { under += -a; nu++; }
        }
        open /= lx_maxf((float)no, 1); under /= lx_maxf((float)nu, 1);
        printf("  sky seen: open floor %.3f, under the roof %.3f\n", open, under);
        CHECK(no > 4 && nu > 0 && open > 0.85f && under < open * 0.6f, "open floor sees the sky, under the roof much less (%.2f, %.2f)", open, under);
        CHECK(any_rgb < 1e-4f, "with no baked lights the colour holds no sky of its own (%.5f)", any_rgb);
        lx_lightmap_bake_free(bk);
    }
    lx_scene_destroy(b);
}


/* ---- terrain ---- */
static int count_generated(LxScene *s, uint8_t kind) {
    int n = 0;
    for (int i = 1; i < s->count; i++) n += s->entities[i].alive && s->entities[i].generated == kind;
    return n;
}
static LxEntity chunk_entity(LxScene *s, int cx, int cz) {
    char name[48];
    snprintf(name, sizeof name, "Chunk %d,%d", cx, cz);
    for (int i = 1; i < s->count; i++)
        if (s->entities[i].alive && s->entities[i].generated == LX_GEN_TERRAIN_CHUNK && !strcmp(s->entities[i].name, name))
            return lx_entity_make((uint32_t)i, s->entities[i].generation & ((1u << (32 - LX_INDEX_BITS)) - 1u));
    return 0;
}
static void test_terrain(void) {
    printf("terrain\n");
    char dir[LX_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", "luxora_test_terrain");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_assets_set_root(dir);
    LxScene *s = lx_scene_create("Terrain test");
    LxEntity te = lx_entity_create(s, "Island");
    LxTerrainComponent *t = lx_add_terrain(s, te, "");
    CHECK(t && !strcmp(t->folder, "Terrains/Island") && lx_terrain_chunk_count(s, te) == 1, "a new terrain: one chunk, in Terrains/<name>");
    t->grass_layer = -1;   /* grass and scatter are checked later */
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    CHECK(count_generated(s, LX_GEN_TERRAIN_CHUNK) == 1, "...one chunk object");
    LxEntity c00 = chunk_entity(s, 0, 0);
    LxMesh *m = c00 ? lx_asset_mesh(lx_entity_data(s, c00)->mesh.mesh) : NULL;
    CHECK(m && m->lod_count == 4 && m->lods[0].vertex_count == 129 * 129 + 4 * 129 && !m->has_uv2,
          "its mesh: 129x129 points with skirts, four levels of detail, not lightmapped (%d LODs)", m ? m->lod_count : 0);
    CHECK(c00 && (lx_entity_data(s, c00)->comps & LX_COMP_COLLIDER) && lx_entity_data(s, c00)->collider.shape == LX_COLLIDER_MESH, "...and a mesh collider");
    float y = -1;
    CHECK(lx_terrain_height(s, 10, 10, &y) && fabsf(y) < 1e-5f, "flat at first (%g)", y);

    /* sculpt */
    LxTerrainBrush b = {LX_TERRAIN_RAISE, 10, 1, 0.5f, 0, 4, 0};
    uint32_t r0 = t->revision;
    lx_terrain_begin_stroke(s, te);
    for (int i = 0; i < 10; i++) lx_terrain_brush(s, te, lx_v3(32, 0, 32), &b, 0.05f);
    lx_terrain_end_stroke(s, te);
    float top = 0, far_y = 1;
    lx_terrain_height(s, 32, 32, &top);
    lx_terrain_height(s, 55, 55, &far_y);
    CHECK(top > 1.0f && fabsf(far_y) < 1e-5f && t->revision == r0 + 1, "Raise: up in the middle, untouched outside (%.2f)", top);

    /* grow: a neighbour joins without a step, and a brush across the border stays seamless */
    CHECK(lx_terrain_add_chunk(s, te, 1, 0) && lx_terrain_has_chunk(s, te, 1, 0) && !lx_terrain_add_chunk(s, te, 1, 0), "Add Chunk at the edge");
    CHECK(lx_terrain_test_seams(s, te) < 1e-5f, "the new chunk's edge matches its neighbour exactly");
    b.radius = 14;
    lx_terrain_begin_stroke(s, te);
    for (int i = 0; i < 10; i++) lx_terrain_brush(s, te, lx_v3(64, 0, 20), &b, 0.05f);
    b.tool = LX_TERRAIN_SMOOTH;
    for (int i = 0; i < 10; i++) lx_terrain_brush(s, te, lx_v3(62, 0, 22), &b, 0.05f);
    b.tool = LX_TERRAIN_ERODE;
    for (int i = 0; i < 10; i++) lx_terrain_brush(s, te, lx_v3(63, 0, 21), &b, 0.05f);
    lx_terrain_stamp(s, te, lx_v3(64, 0, 40), LX_STAMP_CRATER, 12, 6);
    lx_terrain_end_stroke(s, te);
    float l = 0, rr = 0;
    lx_terrain_height(s, 63.9f, 20, &l);
    lx_terrain_height(s, 64.1f, 20, &rr);
    CHECK(lx_terrain_test_seams(s, te) < 1e-5f && l > 0.5f && fabsf(l - rr) < 0.2f, "brushes across a chunk border leave no seam (%.3f | %.3f)", l, rr);
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    CHECK(count_generated(s, LX_GEN_TERRAIN_CHUNK) == 2, "two chunk objects now");

    /* undo and redo: the scene's revision decides */
    uint32_t r_after = t->revision;
    t->revision = r0;
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    lx_terrain_height(s, 32, 32, &top);
    CHECK(fabsf(top) < 1e-5f && !lx_terrain_has_chunk(s, te, 1, 0) && t->revision == r0, "undo walks back: flat again, the added chunk gone");
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    CHECK(count_generated(s, LX_GEN_TERRAIN_CHUNK) == 1, "...its object too");
    t->revision = r_after;
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    lx_terrain_height(s, 32, 32, &top);
    CHECK(top > 1.0f && lx_terrain_has_chunk(s, te, 1, 0) && t->revision == r_after, "redo brings it all back");

    /* relief (parallax) depth: on by default, saved with the scene, handed to the renderer */
    {
        CHECK(t->parallax_depth > 0.01f, "the ground has relief by default (%.2f m)", t->parallax_depth);
        t->parallax_depth = 0.12f;
        lx_terrain_update(s, lx_v3(32, 0, 32), true);
        LxEntityData *ed = lx_entity_data(s, te);
        char *text = lx_scene_serialize(s);
        CHECK(text && strstr(text, "terrain.parallax = 0.12"), "relief depth is saved");
        LxScene *c = lx_scene_deserialize(text);
        lx_free(text);
        LxEntity ce = c ? lx_entity_find(c, lx_entity_data(s, te)->name) : 0;
        LxTerrainComponent *ct = ce ? lx_get_terrain(c, ce) : NULL;
        CHECK(ct && NEAR(ct->parallax_depth, 0.12f, 1e-4f), "...and loaded");
        if (c) lx_scene_destroy(c);
        (void)ed;
    }

    /* paint, holes */
    LxTerrainBrush p = {LX_TERRAIN_PAINT, 6, 1, 0.3f, 0, 0, 2};
    for (int i = 0; i < 20; i++) lx_terrain_brush(s, te, lx_v3(100, 0, 10), &p, 0.05f);
    int layer = -1;
    lx_terrain_sample(s, 100, 10, NULL, NULL, &layer);
    CHECK(layer == 2, "Paint: layer 2 shows where it was painted (%d)", layer);
    int nlayers = t->layer_count;
    lx_terrain_remove_layer(s, te, 1);
    lx_terrain_sample(s, 100, 10, NULL, NULL, &layer);
    CHECK(t->layer_count == nlayers - 1 && layer == 1, "deleting a texture before it: the painted one moves up (%d)", layer);
    lx_terrain_remove_layer(s, te, 1);
    lx_terrain_sample(s, 100, 10, NULL, NULL, &layer);
    CHECK(t->layer_count == nlayers - 2 && layer == 0, "deleting the painted texture: the first one takes its place (%d)", layer);
    while (t->layer_count < nlayers) { t->layers[t->layer_count] = t->layers[0]; t->layers[t->layer_count].tiling = 4; t->layer_count++; }
    lx_strcpy(t->layers[0].texture, sizeof t->layers[0].texture, "Textures/my_grass.png");
    p.tool = LX_TERRAIN_HOLE;
    p.radius = 2;
    lx_terrain_brush(s, te, lx_v3(110, 0, 50), &p, 0.05f);
    CHECK(!lx_terrain_height(s, 110.2f, 50.2f, &y) && lx_terrain_height(s, 116, 50, &y), "Hole: no ground there, ground beside it");
    LxRay down = {{110.2f, 50, 50.2f}, {0, -1, 0}};
    LxVec3 hit;
    CHECK(!lx_terrain_raycast(s, te, down, 100, &hit), "a ray falls through the hole");
    down.origin = lx_v3(32, 50, 32);
    CHECK(lx_terrain_raycast(s, te, down, 100, &hit) && fabsf(hit.y - top) < 0.05f, "and hits the ground elsewhere (%.2f)", hit.y);

    /* edit layers: a road on its own layer, hidden, collapsed */
    int road = lx_terrain_add_edit_layer(s, te, "Road");
    CHECK(road == 1 && t->active_edit_layer == 1, "an edit layer, made active");
    LxTerrainBrush rb = {LX_TERRAIN_RAISE, 6, 1, 0.3f, 0, 0, 0};
    float before = 0, after = 0;
    lx_terrain_height(s, 20, 50, &before);
    lx_terrain_begin_stroke(s, te);
    for (int i = 0; i < 10; i++) lx_terrain_brush(s, te, lx_v3(20, 0, 50), &rb, 0.05f);
    lx_terrain_end_stroke(s, te);
    lx_terrain_height(s, 20, 50, &after);
    t->edit_layers[1].visible = false;
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    float hidden = 0;
    lx_terrain_height(s, 20, 50, &hidden);
    CHECK(after > before + 0.5f && fabsf(hidden - before) < 1e-4f, "hiding the layer hides its sculpting (%.2f -> %.2f -> %.2f)", before, after, hidden);
    t->edit_layers[1].visible = true;
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    lx_terrain_collapse_edit_layers(s, te);
    lx_terrain_height(s, 20, 50, &hidden);
    CHECK(t->edit_layer_count == 1 && fabsf(hidden - after) < 1e-4f, "collapsing keeps what shows in the Base");

    /* physics: a ball dropped on the terrain lands on it */
    LxEntity ball = lx_entity_create(s, "Ball");
    lx_entity_set_position(s, ball, lx_v3(40, 20, 40));
    lx_add_collider(s, ball, LX_COLLIDER_SPHERE)->radius = 0.5f;
    lx_add_rigidbody(s, ball);
    float ground = 0;
    lx_terrain_height(s, 40, 40, &ground);
    for (int i = 0; i < 300; i++) lx_scene_simulate(s, 1.0f / 60.0f);
    LxVec3 bp = lx_entity_world_position(s, ball);
    CHECK(fabsf(bp.y - 0.5f - ground) < 0.3f, "a ball lands on the terrain (%.2f on %.2f)", bp.y, ground);
    lx_scene_stop_simulation(s);

    /* saved: the scene keeps the settings, the folder the chunks; no chunk objects in the scene text */
    t->layers[1].tiling = 7.5f;
    CHECK(lx_terrain_save(s) && !lx_terrain_unsaved(s), "saved");
    char *text = lx_scene_serialize(s);
    CHECK(strstr(text, "terrain.folder = Terrains/Island") && !strstr(text, "Chunk 0,0") && strstr(text, "terrain.layer1 = 7.5"), "scene text: the settings, no chunk objects");
    char path[LX_PATH_MAX];
    snprintf(path, sizeof path, "%s/Terrains/Island/c_1_0.lxchunk", dir);
    CHECK(lx_file_exists(path), "a file per chunk");
    float h_saved = 0;
    lx_terrain_height(s, 32, 32, &h_saved);
    lx_scene_destroy(s);
    lx_terrain_shutdown();   /* forget everything in memory: what comes back is what was saved */
    s = lx_scene_deserialize(text);
    lx_free(text);
    te = lx_entity_find(s, "Island");
    t = lx_get_terrain(s, te);
    CHECK(t && t->layers[1].tiling == 7.5f && lx_terrain_chunk_count(s, te) == 2, "loaded back: settings and both chunks");
    CHECK(t && !strcmp(t->layers[0].texture, "Textures/my_grass.png"), "a ground texture's image is kept");
    t->grass_layer = -1;
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    lx_terrain_height(s, 32, 32, &top);
    CHECK(fabsf(top - h_saved) < 1e-4f && !lx_terrain_height(s, 110.2f, 50.2f, &y), "heights and holes came back from the files (%.3f)", top);

    /* streaming: far away, chunks drop to their simplified copy and lose collision; close, they come back */
    t->load_radius = 40;
    lx_terrain_update(s, lx_v3(2000, 0, 2000), true);
    LxEntity c10 = chunk_entity(s, 1, 0);
    CHECK(lx_terrain_loaded_count(s, te) == 0 && c10 && strstr(lx_entity_data(s, c10)->mesh.mesh, "/far") && !(lx_entity_data(s, c10)->comps & LX_COMP_COLLIDER),
          "far from the focus: simplified, no collision (%d loaded)", lx_terrain_loaded_count(s, te));
    float fy = 0;
    CHECK(lx_terrain_height(s, 32, 32, &fy) && fabsf(fy - top) < 0.6f, "the simplified copy keeps the shape (%.2f vs %.2f)", fy, top);
    lx_terrain_update(s, lx_v3(120, 0, 32), true);   /* chunk 1,0 holds it; chunk 0,0 is 56 m off */
    CHECK(lx_terrain_loaded_count(s, te) == 1 && !strstr(lx_entity_data(s, c10)->mesh.mesh, "/far") && (lx_entity_data(s, c10)->comps & LX_COMP_COLLIDER),
          "near: full detail and collision again, only for the chunk in range");

    /* grass and scatter */
    t->load_radius = 0;
    t->grass_layer = 0;
    t->grass_density = 2;
    t->scatter_count = 1;
    lx_strcpy(t->scatter[0].prefab, sizeof t->scatter[0].prefab, "builtin:cube");
    t->scatter[0].density = 4;
    t->scatter[0].layer = -1;
    t->scatter[0].scale_min = t->scatter[0].scale_max = 1;
    lx_terrain_update(s, lx_v3(32, 0, 32), true);
    int grass = count_generated(s, LX_GEN_GRASS), scat = count_generated(s, LX_GEN_SCATTER);
    CHECK(grass >= 1 && scat > 10, "grass near the camera, scattered objects (%d, %d)", grass, scat);
    LxEntity dup = lx_entity_duplicate(s, te);
    int kids = 0;
    for (LxEntity k = lx_entity_data(s, dup)->first_child; k; k = lx_entity_next_sibling(s, k)) kids++;
    CHECK(kids == 0, "duplicating a terrain copies none of its made objects (%d)", kids);
    lx_entity_destroy(s, dup);
    lx_scene_destroy(s);
    lx_terrain_shutdown();
    lx_path_remove(dir);
    lx_assets_set_root("");
}

static void test_lightmapper(void) {
    printf("baked lighting\n");
    LxScene *s = lx_scene_create("Bake");
    s->bake.texels_per_unit = 2;
    s->bake.samples = 64;
    s->bake.bounces = 0;
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(20, 1, 20));
    LxEntity box = lx_spawn_primitive(s, LX_PRIM_CUBE, "Box", lx_v3(0, 1, 0));
    lx_entity_set_scale(s, box, lx_v3(2, 2, 2));
    LxEntity sun = lx_entity_create(s, "Sun");
    LxLight *L = lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL);
    L->intensity = 3;
    L->color = lx_v3(1, 1, 1);
    L->mode = LX_LIGHT_BAKED;
    L->shadow_radius = 0;
    lx_entity_set_euler(s, sun, lx_v3(90, 180, 0));   /* pointing straight down */
    s->env.ambient_intensity = 0;
    LxVec3 up = lx_v3(0, 1, 0);

    /* a Baked sun straight down: 3 everywhere in the open, nothing under the box */
    LxLightmapBake *b = lm_bake_cpu(s);
    CHECK(b && b->tile_count == 2, "two objects to bake (%d)", b ? b->tile_count : 0);
    if (!b) { lx_scene_destroy(s); return; }
    int n_open = 0, n_under = 0;
    LxVec3 open = lm_probe(b, s, floor, lx_v3(6, 0, 6), 2.0f, up, &n_open);
    LxVec3 under = lm_probe(b, s, floor, lx_v3(0, 0, 0), 0.6f, up, &n_under);
    LxVec3 box_top = lm_probe(b, s, box, lx_v3(0, 2, 0), 0.8f, up, NULL);
    printf("  baked sun: open floor %.3f (%d texels), box top %.3f\n", open.y, n_open, box_top.y);
    LX_UNUSED(n_under);
    CHECK(n_open > 4 && fabsf(open.y - 3.0f) < 0.05f, "open floor gets the sun's 3 (%.3f)", open.y);
    /* under the box the texels look at the box's inside: thrown away (and filled from neighbours, never seen) */
    int inside = 0, under_n = 0;
    for (int i = 0; i < b->texel_count; i++) {
        const float *t = &b->texels[(size_t)i * 12], *a = &b->accum[(size_t)i * LX_LM_ACC];
        if (t[3] <= 0 || t[9] < 0.9f || fabsf(t[1]) > 0.01f || fabsf(t[0]) > 0.8f || fabsf(t[2]) > 0.8f) continue;
        under_n++;
        inside += a[3] > 0 && a[7] / a[3] > 0.5f;
    }
    LX_UNUSED(under);
    CHECK(under_n > 0 && inside == under_n, "texels under the box are found inside geometry (%d of %d)", inside, under_n);
    /* ...and stay dark: no light under the box, its shadow isn't cut off at its outline */
    float under_max = 0;
    for (int i = 0; i < b->texel_count; i++) {
        const float *t = &b->texels[(size_t)i * 12];
        if (t[3] <= 0 || t[9] < 0.9f || fabsf(t[1]) > 0.01f || fabsf(t[0]) > 0.8f || fabsf(t[2]) > 0.8f) continue;
        under_max = lx_maxf(under_max, b->color[i * 4 + 1]);
    }
    CHECK(under_max == 0, "no light under the box (brightest %.3f)", under_max);
    /* ambient occlusion leaves a Baked light's direct light alone: right beside the box the sun still gives 3 */
    LxVec3 beside = lm_probe(b, s, floor, lx_v3(1.4f, 0, 0), 0.36f, up, NULL);
    CHECK(fabsf(beside.y - 3.0f) < 0.05f, "the sun right beside the box isn't darkened by AO (%.3f)", beside.y);
    CHECK(fabsf(box_top.y - 3.0f) < 0.05f, "the box's top gets it too (%.3f)", box_top.y);
    /* direction map: straight up in the open */
    int dir_ok = 0, dir_n = 0;
    for (int i = 0; i < b->texel_count; i++) {
        const float *t = &b->texels[(size_t)i * 12];
        if (b->owner[i] < 0 || t[3] <= 0 || t[9] < 0.9f || lx_len3(lx_sub3(lx_v3(t[0], t[1], t[2]), lx_v3(6, 0, 6))) > 2) continue;
        dir_n++;
        dir_ok += fabsf(b->dir[i * 4 + 1] - 1.0f) < 0.02f && b->dir[i * 4 + 3] > 0.98f;
    }
    CHECK(dir_n > 0 && dir_ok == dir_n, "light direction points at the sun (%d of %d)", dir_ok, dir_n);
    lx_lightmap_bake_free(b);

    /* tilted 60 degrees: cos 60 = half */
    lx_entity_set_euler(s, sun, lx_v3(30, 180, 0));
    b = lm_bake_cpu(s);
    open = lm_probe(b, s, floor, lx_v3(6, 0, 6), 2.0f, up, NULL);
    CHECK(fabsf(open.y - 1.5f) < 0.05f, "a sun 30 degrees above the horizon gives half (%.3f)", open.y);
    lx_lightmap_bake_free(b);
    lx_entity_set_euler(s, sun, lx_v3(90, 180, 0));

    /* Mixed and Realtime suns put no direct light in the lightmap */
    L = lx_get_light(s, sun);
    L->mode = LX_LIGHT_MIXED;
    b = lm_bake_cpu(s);
    open = lm_probe(b, s, floor, lx_v3(6, 0, 6), 2.0f, up, NULL);
    CHECK(open.y < 1e-4f, "a Mixed sun's direct light isn't baked (%.4f)", open.y);
    lx_lightmap_bake_free(b);
    /* ...but its bounce is: the box's sides see the lit floor */
    s->bake.bounces = 1;
    b = lm_bake_cpu(s);
    LxVec3 side = lm_probe(b, s, box, lx_v3(1, 0.5f, 0), 0.5f, lx_v3(1, 0, 0), NULL);
    printf("  Mixed sun, one bounce: box side %.3f\n", side.y);
    CHECK(side.y > 0.1f, "a Mixed sun's bounce is baked (%.3f)", side.y);
    lx_lightmap_bake_free(b);
    L->mode = LX_LIGHT_REALTIME;
    b = lm_bake_cpu(s);
    side = lm_probe(b, s, box, lx_v3(1, 0.5f, 0), 0.5f, lx_v3(1, 0, 0), NULL);
    CHECK(side.y < 1e-4f, "a Realtime sun isn't baked at all (%.4f)", side.y);
    lx_lightmap_bake_free(b);

    /* sky: uniform radiance 0.5 over an open floor gives pi * 0.5 */
    s->bake.bounces = 0;
    s->bake.samples = 256;
    s->env.sky_zenith = lx_v3(1, 1, 1);
    s->env.sky_horizon = lx_v3(1, 1, 1);
    s->env.ambient_intensity = 0.5f;
    b = lm_bake_cpu(s);
    open = lm_probe(b, s, floor, lx_v3(8, 0, 8), 1.5f, up, NULL);
    LxVec3 near_box = lm_probe(b, s, floor, lx_v3(1.3f, 0, 0), 0.3f, up, NULL);
    printf("  sky: open floor %.3f (expected about %.3f), beside the box %.3f\n", open.y, LX_PI * 0.5f, near_box.y);
    CHECK(fabsf(open.y / (LX_PI * 0.5f) - 1.0f) < 0.06f, "sky irradiance in the open (%.3f)", open.y);
    CHECK(near_box.y < open.y * 0.9f, "the box shades the floor beside it (%.3f vs %.3f)", near_box.y, open.y);
    lx_lightmap_bake_free(b);
    /* ambient occlusion: the ground right at the box's foot darkens more than the sky alone does; the open floor doesn't */
    s->bake.ao_intensity = 0;
    b = lm_bake_cpu(s);
    LxVec3 foot_off = lm_probe(b, s, floor, lx_v3(1.4f, 0, 0), 0.36f, up, NULL), open_off = lm_probe(b, s, floor, lx_v3(8, 0, 8), 1.5f, up, NULL);
    lx_lightmap_bake_free(b);
    s->bake.ao_intensity = 1;
    s->bake.ao_radius = 1;
    b = lm_bake_cpu(s);
    LxVec3 foot_on = lm_probe(b, s, floor, lx_v3(1.4f, 0, 0), 0.36f, up, NULL), open_on = lm_probe(b, s, floor, lx_v3(8, 0, 8), 1.5f, up, NULL);
    printf("  ambient occlusion at the box's foot: %.3f without, %.3f with (open floor %.3f / %.3f)\n", foot_off.y, foot_on.y, open_off.y, open_on.y);
    CHECK(foot_on.y < foot_off.y * 0.9f, "AO darkens the ground at the box's foot (%.3f -> %.3f)", foot_off.y, foot_on.y);
    CHECK(fabsf(open_on.y - open_off.y) < open_off.y * 0.02f, "and not the open floor (%.3f / %.3f)", open_off.y, open_on.y);
    lx_lightmap_bake_free(b);

    /* files: written and read back */
    char path[LX_PATH_MAX];
    snprintf(path, sizeof path, "%s/lm_test.lxlm", tmpdir());
    float col[4 * 4], dir[4 * 4];
    for (int i = 0; i < 16; i++) { col[i] = (float)i * 0.37f; dir[i] = (float)(i % 4) * 0.25f; }
    CHECK(lx_lightmap_write(path, 2, col, dir), "lightmap written");
    int sz = 0;
    uint16_t *h = lx_lightmap_read(path, &sz);
    bool same = h && sz == 2;
    for (int i = 0; same && i < 16; i++) same = fabsf(lx_lightmap_half(h[i]) - col[i]) < 0.01f && fabsf(lx_lightmap_half(h[16 + i]) - dir[i]) < 0.001f;
    CHECK(same, "lightmap read back");
    lx_free(h);
    lx_path_remove(path);

    /* the scene keeps lightmap tiles, light modes and bake settings */
    lx_entity_data(s, floor)->lm_index = 0;
    lx_entity_data(s, floor)->lm_st[0] = 0.5f;
    lx_entity_data(s, floor)->lm_st[2] = 0.25f;
    lx_strcpy(s->lightmap_ref, sizeof s->lightmap_ref, "Lightmaps/Bake");
    s->lightmap_count = 1;
    L->mode = LX_LIGHT_BAKED;
    L->shadow_radius = 0.4f;
    char *text = lx_scene_serialize(s);
    LxScene *c = lx_scene_deserialize(text);
    lx_free(text);
    LxEntity cf = lx_entity_find(c, "Floor"), cs = lx_entity_find(c, "Sun"), cb = lx_entity_find(c, "Box");
    CHECK(lx_entity_data(c, cf)->lm_index == 0 && lx_entity_data(c, cf)->lm_st[0] == 0.5f && lx_entity_data(c, cf)->lm_st[2] == 0.25f,
          "lightmap tile saved");
    CHECK(lx_entity_data(c, cb)->lm_index == -1, "an object without one stays without");
    CHECK(c->lightmap_count == 1 && !strcmp(c->lightmap_ref, "Lightmaps/Bake"), "lightmap folder saved");
    CHECK(lx_get_light(c, cs)->mode == LX_LIGHT_BAKED && lx_get_light(c, cs)->shadow_radius == 0.4f, "light mode saved");
    CHECK(c->bake.samples == 256 && c->bake.texels_per_unit == 2 && c->bake.ao_intensity == 1 && c->bake.ao_radius == 1, "bake settings saved");
    lx_scene_destroy(c);
    lx_scene_destroy(s);
}

/* ---------- Baked Lighting Volumes ---------- */
/* the resolved voxel of a volume nearest to p */
static const float *vox_near(const LxLightmapBake *b, int vol, LxVec3 p) {
    const LxLmVolume *v = &b->vols[vol];
    float best = 1e30f;
    const float *out = NULL;
    for (int i = 0; i < v->n[0] * v->n[1] * v->n[2]; i++) {
        const float *q = &b->vox_pos[(size_t)(v->first + i) * 4];
        float d = lx_len3(lx_sub3(lx_v3(q[0], q[1], q[2]), p));
        if (d < best) { best = d; out = &b->vox_out[(size_t)(v->first + i) * 16]; }
    }
    return out;
}
static float vox_light(const float *vx, LxVec3 n) { return vx ? lx_gi_eval_l1(vx + 4, n) : -1; }   /* green channel */

static void test_bake_volumes(void) {
    printf("baked lighting volumes\n");
    LxScene *s = lx_scene_create("Volumes");
    s->bake.samples = 96;
    s->bake.bounces = 0;
    s->env.ambient_intensity = 0;
    LxEntity floor = lx_spawn_primitive(s, LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
    lx_entity_set_scale(s, floor, lx_v3(20, 1, 20));
    LxEntity roof = lx_spawn_primitive(s, LX_PRIM_CUBE, "Roof", lx_v3(-5, 4, 0));   /* covers x < 0 */
    lx_entity_set_scale(s, roof, lx_v3(10, 0.4f, 20));
    LxEntity sun = lx_entity_create(s, "Sun");
    LxLight *L = lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL);
    L->intensity = 3;
    L->color = lx_v3(1, 1, 1);
    L->mode = LX_LIGHT_BAKED;
    L->shadow_radius = 0;
    lx_entity_set_euler(s, sun, lx_v3(90, 180, 0));
    LxEntity ve = lx_entity_create(s, "Volume");
    LxBakeVolume *bv = lx_add_bake_volume(s, ve);
    bv->size = lx_v3(16, 3, 16);
    bv->density = 1;
    lx_entity_set_position(s, ve, lx_v3(0, 1.5f, 0));
    LxVec3 up = lx_v3(0, 1, 0), down = lx_v3(0, -1, 0);

    /* a Baked sun: a character in the open gets its 3 on its top, under the roof nothing */
    LxLightmapBake *b = lm_bake_cpu(s);
    CHECK(b && b->vol_count == 1 && b->voxel_count == 16 * 3 * 16, "one volume of 16 x 3 x 16 voxels (%d)", b ? b->voxel_count : 0);
    if (!b) { lx_scene_destroy(s); return; }
    float open_up = vox_light(vox_near(b, 0, lx_v3(5, 1, 0)), up), shade_up = vox_light(vox_near(b, 0, lx_v3(-5, 1, 0)), up);
    float open_down = vox_light(vox_near(b, 0, lx_v3(5, 1, 0)), down);
    printf("  baked sun: open %.3f (facing down %.3f), under the roof %.3f\n", open_up, open_down, shade_up);
    CHECK(fabsf(open_up - 3.0f) < 0.05f, "a character in the open gets the sun's 3 from above (%.3f)", open_up);
    CHECK(open_down < 0.05f, "and none on its underside (%.3f)", open_down);
    CHECK(shade_up < 0.05f, "under the roof: the roof's baked shadow (%.3f)", shade_up);
    lx_lightmap_bake_free(b);

    /* a Mixed sun: its light stays real time; the volume keeps where it's shadowed (shadowmask) */
    L->mode = LX_LIGHT_MIXED;
    b = lm_bake_cpu(s);
    const float *vo = vox_near(b, 0, lx_v3(5, 1, 0)), *vs = vox_near(b, 0, lx_v3(-5, 1, 0));
    CHECK(b->lights[0].mask_channel == 0, "the Mixed sun gets shadowmask channel 0");
    CHECK(vox_light(vo, up) < 1e-4f, "its direct light isn't baked (%.4f)", vox_light(vo, up));
    CHECK(vo && vs && vo[12] > 0.99f && vs[12] < 0.01f, "shadowmask: lit in the open (%.2f), shadowed under the roof (%.2f)", vo ? vo[12] : -1, vs ? vs[12] : -1);
    lx_lightmap_bake_free(b);

    /* sky only: brighter facing up than down, and the roof shades it */
    L->mode = LX_LIGHT_REALTIME;
    s->env.sky_zenith = lx_v3(1, 1, 1);
    s->env.sky_horizon = lx_v3(1, 1, 1);
    s->env.ambient_intensity = 0.5f;
    b = lm_bake_cpu(s);
    float sky_up = vox_light(vox_near(b, 0, lx_v3(6, 1, 6)), up), sky_down = vox_light(vox_near(b, 0, lx_v3(6, 1, 6)), down);
    float roof_up = vox_light(vox_near(b, 0, lx_v3(-6, 1, 0)), up);
    printf("  sky: open facing up %.3f (true %.3f), down %.3f; under the roof %.3f\n", sky_up, LX_PI * 0.5f, sky_down, roof_up);
    CHECK(sky_up > LX_PI * 0.5f * 0.75f && sky_up < LX_PI * 0.5f * 1.25f, "sky light from above, part of the sky behind the roof (%.3f)", sky_up);
    CHECK(sky_down < sky_up * 0.3f, "little from the floor below (%.3f)", sky_down);
    CHECK(roof_up < sky_up * 0.6f, "the roof shades the sky (%.3f)", roof_up);
    lx_lightmap_bake_free(b);

    /* files and saving */
    char path[LX_PATH_MAX];
    snprintf(path, sizeof path, "%s/vol_test.lxlv", tmpdir());
    int n[3] = {2, 3, 4}, rn[3];
    float vox[24 * 16];
    for (int i = 0; i < 24 * 16; i++) vox[i] = (float)(i % 37) * 0.1f;
    CHECK(lx_volume_write(path, n, vox), "volume written");
    uint16_t *h = lx_volume_read(path, rn);
    bool same = h && rn[0] == 2 && rn[1] == 3 && rn[2] == 4;
    for (int layer = 0; same && layer < 4; layer++)
        for (int v = 0; same && v < 24; v++)
            for (int k = 0; same && k < 4; k++) same = fabsf(lx_lightmap_half(h[((size_t)layer * 24 + v) * 4 + k]) - vox[v * 16 + layer * 4 + k]) < 0.01f;
    CHECK(same, "volume read back");
    lx_free(h);
    lx_path_remove(path);
    lx_entity_data(s, ve)->vol_index = 0;
    s->volume_count = 1;
    lx_strcpy(s->lightmap_ref, sizeof s->lightmap_ref, "Lightmaps/Volumes");
    L->mode = LX_LIGHT_MIXED;
    L->mask_channel = 2;
    char *text = lx_scene_serialize(s);
    LxScene *c = lx_scene_deserialize(text);
    lx_free(text);
    LxEntity cv = lx_entity_find(c, "Volume");
    LxBakeVolume *cb = lx_get_bake_volume(c, cv);
    CHECK(cb && cb->size.x == 16 && cb->size.y == 3 && cb->density == 1 && lx_entity_data(c, cv)->vol_index == 0, "volume saved with the scene");
    CHECK(c->volume_count == 1 && !strcmp(c->lightmap_ref, "Lightmaps/Volumes"), "baked volumes listed");
    CHECK(lx_get_light(c, lx_entity_find(c, "Sun"))->mask_channel == 2, "shadowmask channel saved");
    lx_scene_destroy(c);
    lx_scene_destroy(s);
}

static void test_gi_headroom(void) {
    printf("live gi: room above the ground\n");
    LxScene *s = lx_scene_create("Flat");
    LxEntity ground = lx_spawn_primitive(s, LX_PRIM_PLANE, "Ground", lx_v3(0, 0, 0));
    lx_entity_set_scale(s, ground, lx_v3(4, 1, 4));
    LxEntity sun = lx_entity_create(s, "Sun");
    lx_entity_set_euler(s, sun, lx_v3(50, 210, 0));
    lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL)->intensity = 3;
    LxEntity hero = lx_spawn_primitive(s, LX_PRIM_CAPSULE, "Hero", lx_v3(0, 0.9f, 0));
    lx_entity_set_scale(s, hero, lx_v3(0.5f, 0.9f, 0.5f));
    lx_add_character(s, hero);
    LxGraphicsSettings g = lx_graphics_defaults(1);
    g.gi_rays_per_frame = 60000;
    float level[3];
    float lifts[3] = {0, 2.0f, 9.0f};   /* standing, mid-jump, lifted high */
    for (int k = 0; k < 3; k++) {
        lx_entity_set_position(s, hero, lx_v3(0, 0.9f + lifts[k], 0));
        for (int f = 0; f < 40; f++) lx_gi_update(s, &g, GI_EYE);
        float y0 = lifts[k];
        LxVec3 low = lx_gi_irradiance(lx_v3(0.3f, y0 + 0.5f, 0), lx_v3(1, 0, 0), true);
        LxVec3 high = lx_gi_irradiance(lx_v3(0.3f, y0 + 1.65f, 0), lx_v3(1, 0, 0), true);
        float lo = low.x + low.y + low.z, hi = high.x + high.y + high.z;
        level[k] = hi;
        printf("  %s: bounce light %.3f at the waist, %.3f at the face\n", k == 0 ? "standing" : k == 1 ? "mid-jump" : "lifted 9 m", lo, hi);
        CHECK(hi > 0 && hi > lo * 0.5f, "%s: the face gets bounce light like the waist (%.3f vs %.3f)", k == 0 ? "standing" : k == 1 ? "mid-jump" : "lifted", hi, lo);
    }
    CHECK(level[1] > level[0] * 0.4f, "jumping doesn't take the bounce light away (%.3f vs %.3f)", level[1], level[0]);
    lx_gi_shutdown();
    lx_scene_destroy(s);
}

static void test_gi_leaks(void) {
    printf("live gi light leaks\n");
    float lit_old, lit_new;
    float before = leak_ratio(false, &lit_old), after = leak_ratio(true, &lit_new);
    printf("  dark room / lit room next to a thin wall: %.3f before, %.3f with walls\n", before, after);
    CHECK(lit_new > 0, "the lit side has bounce light (%g)", lit_new);
    CHECK(after < 0.05f, "no light leaks through a thin wall (%.3f, was %.3f)", after, before);
    CHECK(lit_new > lit_old * 0.6f, "the lit side keeps its light (%g vs %g)", lit_new, lit_old);
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
    q.graphics.gi_rays_per_frame = 12345;
    q.graphics.ao_radius = 1.25f; q.graphics.bloom_intensity = 0.6f; q.graphics.godrays_enabled = false; q.graphics.temperature = -0.4f;
    q.graphics.vignette = 0.5f; q.graphics.contrast = 1.2f;
    q.audio.speed_of_sound = 340.0f;
    q.build.target = LX_PLATFORM_ANDROID;
    lx_project_save(&q);
    LxProject r;
    lx_project_load(&r, p.root);
    CHECK(r.graphics.gi_rays_per_frame == 12345 && NEAR(r.audio.speed_of_sound, 340.0f, 1e-3f) && r.build.target == LX_PLATFORM_ANDROID,
          "settings round trip");
    CHECK(NEAR(r.graphics.ao_radius, 1.25f, 1e-4f) && NEAR(r.graphics.bloom_intensity, 0.6f, 1e-4f) && !r.graphics.godrays_enabled &&
          NEAR(r.graphics.temperature, -0.4f, 1e-4f) && NEAR(r.graphics.vignette, 0.5f, 1e-4f) && NEAR(r.graphics.contrast, 1.2f, 1e-4f),
          "camera and lens settings round trip");
    LxGraphicsSettings d = lx_graphics_defaults(2);
    CHECK(d.ao_enabled && d.bloom_enabled && d.godrays_enabled && d.contrast == 1.0f && d.saturation == 1.0f, "High: AO, bloom and sun shafts on");
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
    /* preview channel: pressing preview again replaces the sound instead of stacking it */
#ifdef _WIN32
    _putenv_s("LUXORA_NO_AUDIO", "1");
#else
    setenv("LUXORA_NO_AUDIO", "1", 1);
#endif
    lx_audio_init();
    char dir[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, path);
    lx_assets_set_root(dir);
    int before = lx_audio_playing_count();
    for (int i = 0; i < 5; i++) lx_audio_preview("luxora_test.wav", 1.0f, 1.0f);
    CHECK(lx_audio_preview_playing() && lx_audio_playing_count() == before + 1, "preview x5 plays one sound (%d)", lx_audio_playing_count() - before);
    lx_audio_preview_stop();
    CHECK(!lx_audio_preview_playing() && lx_audio_playing_count() == before, "preview stops");
    lx_audio_shutdown();
    lx_assets_clear();
    lx_path_remove(path);
    lx_free(w);
}

/* ---------------- game UI ---------------- */
static float gui_min_x(const LxGameUiGeometry *g) { float m = 1e9f; for (int i = 0; i < g->vertex_count; i++) m = lx_minf(m, g->vertices[i].x); return m; }
static float gui_max_x(const LxGameUiGeometry *g) { float m = -1e9f; for (int i = 0; i < g->vertex_count; i++) m = lx_maxf(m, g->vertices[i].x); return m; }

static void test_game_ui(void) {
    printf("game ui\n");
    lx_game_ui_use_atlas(true);
    memset(&g_lx_input, 0, sizeof g_lx_input);
    int touch_mode = lx_input_map()->touch_controls_mode;
    lx_input_map()->touch_controls_mode = 2;   /* no on-screen touch controls in the measured geometry */
    /* a 3840x2160 window: canvas stays 1080 tall, everything scales by 2 */
    lx_game_ui_begin_frame(0, 0, 3840, 2160, true);
    LxVec2 cs = lx_ui_canvas_size();
    CHECK(NEAR(cs.x, 1920, 0.5f) && NEAR(cs.y, 1080, 0.5f), "canvas is 1920x1080 at 16:9 (%gx%g)", cs.x, cs.y);
    lx_ui_anchor(LX_ANCHOR_TOP_RIGHT);
    lx_ui_rect(-20, 20, 100, 50, LX_COLOR_RED, 8);
    LxGameUiGeometry g;
    lx_game_ui_geometry(&g);
    CHECK(g.index_count > 0 && NEAR(gui_max_x(&g), (1920 - 20) * 2, 1) && NEAR(gui_min_x(&g), (1920 - 120) * 2, 1),
          "TOP_RIGHT anchor places the rect's right edge 20 units in (%g..%g)", gui_min_x(&g), gui_max_x(&g));
    lx_game_ui_begin_frame(0, 0, 1920, 1080, true);
    lx_ui_anchor(LX_ANCHOR_CENTER);
    float tw = lx_ui_text_width(48, "Game Over");
    lx_ui_text(0, 0, 48, LX_COLOR_WHITE, "Game Over");
    lx_game_ui_geometry(&g);
    CHECK(tw > 150 && tw < 400 && g.index_count >= 8 * 6, "48-unit text is %g wide and drawn (%d indices)", tw, g.index_count);
    CHECK(NEAR((gui_min_x(&g) + gui_max_x(&g)) * 0.5f, 960, 12), "CENTER anchor centres the text (%g)", (gui_min_x(&g) + gui_max_x(&g)) * 0.5f);
    /* a button clicked with the mouse: press over it, release over it */
    bool clicked = false;
    for (int frame = 0; frame < 3; frame++) {
        g_lx_input.mouse_pos = lx_v2(960, 540);
        g_lx_input.mouse[LX_MOUSE_LEFT] = frame == 1;
        lx_input_update(1.0f / 60.0f);
        lx_game_ui_begin_frame(0, 0, 1920, 1080, true);
        lx_ui_anchor(LX_ANCHOR_CENTER);
        clicked |= lx_ui_button("Play", 0, 0, 300, 80) && frame == 2;
    }
    CHECK(clicked && lx_ui_pointer_over(), "button reports a click and pointer-over");
    lx_game_ui_begin_frame(0, 0, 1920, 1080, true);
    lx_ui_bar(20, 20, 300, 24, 0.5f, LX_COLOR_GREEN, LX_RGBA(0, 0, 0, 128));
    lx_ui_circle(100, 100, 20, LX_COLOR_YELLOW);
    lx_ui_line(0, 0, 100, 100, 3, LX_COLOR_WHITE);
    lx_ui_rect_outline(10, 10, 50, 50, LX_COLOR_BLUE, 6, 2);
    lx_game_ui_geometry(&g);
    CHECK(g.index_count > 100 && g.cmd_count == 1, "shapes batch into one draw (%d cmds)", g.cmd_count);
    lx_input_map()->touch_controls_mode = touch_mode;

    /* the editor previewing Android: touch controls drawn in the game view, the mouse acts as a finger */
    LxInputMap saved = *lx_input_map();
    lx_input_map_defaults(lx_input_map());
    lx_input_map()->touch_controls_mode = 0;
    lx_input_set_platform_preview(LX_PLATFORM_ANDROID);
    g_lx_input.width = 1600; g_lx_input.height = 900;
    const LxTouchControl *jump = NULL;
    for (int i = 0; i < lx_input_map()->touch_count; i++) if (!strcmp(lx_input_map()->touch[i].name, "Jump Button")) jump = &lx_input_map()->touch[i];
    float vx = 100, vy = 50, vw = 1280, vh = 720;   /* a game view inside the editor window */
    lx_input_begin_frame();
    g_lx_input.mouse_pos = lx_v2(vx + jump->x * vw, vy + jump->y * vh);
    g_lx_input.mouse[LX_MOUSE_LEFT] = true;
    lx_game_ui_begin_frame(vx, vy, vw, vh, true);
    lx_input_update(1.0f / 60);
    lx_game_ui_geometry(&g);
    CHECK(lx_game_platform() == LX_PLATFORM_ANDROID && lx_input_touch_controls_visible() && g.index_count > 0, "Android preview: touch controls show in the game view");
    CHECK(lx_touch_count() == 1 && lx_action_down("Jump"), "Android preview: clicking the Jump button presses Jump");
    lx_input_begin_frame();
    g_lx_input.mouse_pos = lx_v2(5, 5);   /* outside the game view: not a finger */
    lx_game_ui_begin_frame(vx, vy, vw, vh, true);
    lx_input_update(1.0f / 60);
    bool dragged = lx_touch_count() == 1;   /* a held finger dragged out of the view stays down */
    lx_input_begin_frame();
    g_lx_input.mouse[LX_MOUSE_LEFT] = false;
    lx_input_update(1.0f / 60);
    lx_input_begin_frame();
    g_lx_input.mouse[LX_MOUSE_LEFT] = true;
    lx_input_update(1.0f / 60);
    CHECK(dragged && lx_touch_count() == 0, "Android preview: a click outside the game view isn't a touch");
    lx_input_set_platform_preview(-1);
    g_lx_input.mouse[LX_MOUSE_LEFT] = false;
    lx_input_begin_frame();
    lx_input_update(1.0f / 60);
    CHECK(lx_game_platform() == lx_platform_current() && lx_touch_count() == 0, "preview off: back to the PC");
    *lx_input_map() = saved;
    lx_game_ui_shutdown();
}

static void test_input(void) {
    printf("input\n");
    LxInputMap *m = lx_input_map();
    lx_input_map_defaults(m);
    g_lx_pad_polling_disabled = true; /* don't let real devices overwrite injected state */
    CHECK(lx_input_find(m, "Jump") && lx_input_find(m, "MoveX") && m->touch_count == 5 && lx_input_find(m, "Crouch") && lx_input_find(m, "Reload") && lx_input_find(m, "Lock On"), "default map");
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


/* ---- scripts: the built-in compiler and the module loader ---- */
#if LUXORA_SCRIPT_COMPILER
#include "compiler.h"
static int g_diag_errors;
static char g_diag_first[512];
static void test_diag(void *u, bool warning, const char *msg) {
    LX_UNUSED(u);
    if (!warning) { if (!g_diag_errors++) lx_strcpy(g_diag_first, sizeof g_diag_first, msg); }
    printf("    %s\n", msg);
}

static const char *TEST_SCRIPT =
    "#include <luxora/luxora.h>\n"
    "#include <stdio.h>\n#include <string.h>\n#include <math.h>\n#include <stdlib.h>\n#include <stdint.h>\n"
    "typedef struct Mover { int frames; float speed; } Mover;\n"
    "static int counter;\n"          /* bss */
    "static float table[3] = {1.5f, 2.5f, 3.5f};\n" /* data */
    "static int sum(int n, ...) { va_list ap; va_start(ap, n); int s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, int); va_end(ap); return s; }\n"
    "static void mover_start(LxBehaviourCtx *ctx) { Mover *m = ctx->state; m->speed = ctx->params[0]; }\n"
    "static void mover_update(LxBehaviourCtx *ctx, float dt) {\n"
    "    Mover *m = ctx->state; m->frames++; counter++;\n"
    "    LxVec3 p = lx_entity_position(ctx->scene, ctx->self);\n"   /* struct return */
    "    uint64_t big = 10000000000000ull; uint64_t q = big / (uint64_t)(counter + 1);\n" /* 64-bit division */
    "    char buf[64]; snprintf(buf, sizeof buf, \"%d-%s\", sum(3, 1, 2, 3), \"ok\");\n"
    "    char *heap = malloc(16); strcpy(heap, buf);\n"
    "    p.x += m->speed; p.y = sqrtf(16.0f) + table[1];\n"
    "    p.z = (q == 5000000000000ull && !strcmp(heap, \"6-ok\")) ? 7.0f : -1.0f;\n"
    "    free(heap);\n"
    "    lx_entity_set_position(ctx->scene, ctx->self, p);\n"
    "    if (counter == 1) printf(\"script says hello from %s\\n\", lx_entity_name(ctx->scene, ctx->self));\n"
    "}\n"
    "LX_SCRIPT_REGISTER(mover) {\n"
    "    LxBehaviourDesc d = {0};\n"
    "    d.name = \"TestMover\"; d.state_size = sizeof(Mover);\n"
    "    d.param_names[0] = \"Speed\"; d.param_defaults[0] = 2.0f;\n"
    "    d.on_start = mover_start; d.on_update = mover_update;\n"
    "    lx_register_behaviour(&d);\n"
    "}\n"
    "LX_SCRIPT_REGISTER(second) {\n"
    "    LxBehaviourDesc d = {0}; d.name = \"TestSecond\"; lx_register_behaviour(&d);\n"
    "}\n"
    /* a custom State Graph action */
    "static void double_it(LxFsmCtx *c) { lx_fsm_set_float(c, 1, lx_fsm_float(c, 0) * 2); lx_fsm_send(c, lx_fsm_event(c, 2)); }\n"
    "static const LxFsmActionDesc DOUBLE_IT = {\"Test Double\", \"Your Scripts\", \"Doubles a number.\", \"{1} = {0} x 2\",\n"
    "    {{\"Value\", LX_FSM_FLOAT, \"21\", 0, 0, 0}, {\"Store\", LX_FSM_FLOAT, \"\", LX_FSM_P_OUT, 0, 0}, {\"Done\", LX_FSM_EVENT, \"\", LX_FSM_P_OPTIONAL, 0, 0}},\n"
    "    0, 0, LX_FSM_ONCE, double_it, 0, 0};\n"
    "LX_SCRIPT_REGISTER(actions) { lx_fsm_register_action(&DOUBLE_IT); }\n";

static void test_scripts(void) {
    printf("scripts\n");
    char dir[LX_PATH_MAX], src[LX_PATH_MAX], bad[LX_PATH_MAX], sdk[LX_PATH_MAX], out[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), "luxora_test_scripts");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_path_join(src, sizeof src, dir, "mover.c");
    lx_path_join(bad, sizeof bad, dir, "broken.c");
    lx_path_join(sdk, sizeof sdk, dir, "sdk");
    lx_path_join(out, sizeof out, dir, "scripts.lxo");
    lx_file_write(src, TEST_SCRIPT, strlen(TEST_SCRIPT));
    const char *files[1] = {src};
    LxScriptCompileStats st;
    g_diag_errors = 0;
    bool ok = lx_script_compile(files, 1, sdk, out, test_diag, NULL, &st);
    CHECK(ok && st.errors == 0, "script compiles (%d errors)", st.errors);
    CHECK(st.registrations == 3, "three registrations found (%d)", st.registrations);
    CHECK(lx_file_exists(out), "module written");
    int before = lx_behaviour_count();
    CHECK(lx_scripts_load(out), "module loads");
    CHECK(lx_behaviour_find("TestMover") && lx_behaviour_find("TestSecond"), "script behaviours registered");
    CHECK(lx_behaviour_count() == before + 2, "count %d -> %d", before, lx_behaviour_count());
    LxScene *s = lx_scene_create("Scripts");
    LxEntity e = lx_entity_create(s, "Mover");
    LxBehaviourRef *ref = lx_add_behaviour(s, e, "TestMover");
    CHECK(ref && NEAR(ref->params[0], 2.0f, 1e-6f), "default param from script");
    lx_scene_simulate(s, 1.0f / 60.0f);
    LxVec3 p = lx_entity_position(s, e);
    CHECK(NEAR(p.x, 2.0f, 1e-5f) && NEAR(p.y, 6.5f, 1e-5f) && NEAR(p.z, 7.0f, 1e-5f), "script ran: %g %g %g", p.x, p.y, p.z);
    lx_scene_simulate(s, 1.0f / 60.0f);
    p = lx_entity_position(s, e);
    CHECK(NEAR(p.x, 4.0f, 1e-5f), "second frame %g", p.x);
    lx_scene_stop_simulation(s);
    lx_scene_destroy(s);
    {   /* the script's State Graph action runs in a graph */
        CHECK(lx_fsm_action_find("Test Double") != NULL, "script registers a State Graph action");
        const char *gt = "[graph]\nstart = A\n[variable]\nname = out\ntype = float\nvalue = 0\n"
                         "[state]\nname = A\ntransition = DONE > B\n[action]\ntype = Test Double\np0 = 21\np1 = $out\np2 = DONE\n[state]\nname = B\n";
        char gp[LX_PATH_MAX];
        lx_assets_set_root(dir);
        lx_path_join(gp, sizeof gp, dir, "T.lxgraph");
        lx_file_write(gp, gt, strlen(gt));
        LxScene *gs = lx_scene_create("ScriptGraph");
        LxEntity ge = lx_entity_create(gs, "G");
        lx_add_state_graph(gs, ge, "T.lxgraph");
        lx_scene_simulate(gs, 1.0f / 60);
        lx_scene_simulate(gs, 1.0f / 60);
        CHECK(NEAR(lx_fsm_get_var_float(gs, ge, 0, "out"), 42, 1e-4f) && !strcmp(lx_fsm_current_state(gs, ge, 0), "B"), "custom action ran (%g, %s)",
              lx_fsm_get_var_float(gs, ge, 0, "out"), lx_fsm_current_state(gs, ge, 0));
        lx_scripts_unload();   /* while the graph still runs: it must let go of the action */
        lx_scene_simulate(gs, 1.0f / 60);
        CHECK(!lx_fsm_action_find("Test Double"), "unloading forgets the script's action");
        lx_scene_stop_simulation(gs);
        lx_scene_destroy(gs);
        lx_assets_clear();
        lx_scripts_load(out);
    }
    lx_scripts_unload();
    CHECK(!lx_behaviour_find("TestMover") && lx_behaviour_count() == before, "unloading forgets the behaviours");
    CHECK(lx_behaviour_find("Rotator") != NULL, "built-ins stay");

    /* errors come back with file and line */
    const char *broken = "#include <luxora/luxora.h>\nvoid f(void) {\n    int x = undefined_thing;\n}\n";
    lx_file_write(bad, broken, strlen(broken));
    files[0] = bad;
    g_diag_errors = 0;
    g_diag_first[0] = 0;
    ok = lx_script_compile(files, 1, sdk, out, test_diag, NULL, &st);
    CHECK(!ok && g_diag_errors > 0, "compile error reported");
    CHECK(strstr(g_diag_first, "broken.c:3") != NULL, "error has file:line: %s", g_diag_first);

    /* calling something that isn't in the API fails at load, not at runtime */
    const char *unknown = "#include <luxora/luxora.h>\nvoid system(const char *);\nLX_SCRIPT_REGISTER(x) { system(\"x\"); }\n";
    lx_file_write(bad, unknown, strlen(unknown));
    ok = lx_script_compile(files, 1, sdk, out, test_diag, NULL, &st);
    char err[256];
    size_t size;
    void *data = lx_file_read(out, &size);
    CHECK(ok && data && !lx_scripts_load_memory(data, size, err, sizeof err) && strstr(err, "system"), "unknown function rejected: %s", err);
    lx_free(data);
    static const unsigned char junk[64] = {0x7f, 'E', 'L', 'F', 2, 1};
    CHECK(!lx_scripts_load_memory(junk, sizeof junk, err, sizeof err) && err[0], "damaged module rejected: %s", err);
    CHECK(!lx_scripts_loaded(), "nothing loaded after failures");
    lx_path_remove(dir);
}
#endif

/* ---- gameplay: prefabs, tags, messages, timers, tweens, time scale, deferred destroy, save data ---- */
typedef struct MsgState { int got; float last; char name[32]; int updates; bool destroy_self; } MsgState;
static void msg_on(LxBehaviourCtx *c, const char *m, float v, LxEntity sender) {
    MsgState *st = c->state; LX_UNUSED(sender);
    st->got++; st->last = v; lx_strcpy(st->name, sizeof st->name, m);
    if (!strcmp(m, "die")) lx_entity_destroy(c->scene, c->self);
}
static void msg_update(LxBehaviourCtx *c, float dt) { LX_UNUSED(dt); ((MsgState *)c->state)->updates++; }
static int g_timer_hits;
static void timer_hit(LxScene *s, LxEntity owner, void *user) { LX_UNUSED(s); LX_UNUSED(owner); g_timer_hits += *(int *)user; }
static float g_event_sum;
static void on_score(const char *e, float v, void *u) { LX_UNUSED(e); LX_UNUSED(u); g_event_sum += v; }

static void test_gameplay(void) {
    printf("gameplay\n");
    LxBehaviourDesc d = {0};
    d.name = "TestListener";
    d.state_size = sizeof(MsgState);
    d.on_update = msg_update;
    d.on_message = msg_on;
    lx_register_behaviour(&d);

    LxScene *s = lx_scene_create("Gameplay");
    /* prefab: a parent with a child, components and tags */
    LxEntity crate = lx_spawn_primitive(s, LX_PRIM_CUBE, "Crate", lx_v3(5, 1, 5));
    lx_entity_set_tag(s, crate, "Pickup");
    lx_add_rigidbody(s, crate)->mass = 3.5f;
    LxEntity lid = lx_entity_create(s, "Lid");
    lx_entity_set_parent(s, lid, crate);
    lx_entity_set_position(s, lid, lx_v3(0, 0.6f, 0));
    lx_add_behaviour(s, lid, "TestListener");
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, tmpdir(), "luxora_test.lxprefab");
    CHECK(lx_prefab_save(s, crate, path), "prefab saved");
    char *text = lx_file_read(path, NULL);
    LxEntity inst = text ? lx_prefab_instantiate_text(s, text, lx_v3(-2, 0, 3), 0) : 0;
    lx_free(text);
    CHECK(inst && inst != crate, "prefab instantiated");
    CHECK(NEAR(lx_entity_world_position(s, inst).x, -2, 1e-5f) && NEAR(lx_entity_world_position(s, inst).z, 3, 1e-5f), "placed where spawned");
    LxEntity inst_lid = lx_entity_find_child(s, inst, "Lid");
    CHECK(inst_lid && NEAR(lx_entity_world_position(s, inst_lid).y, 0.6f, 1e-4f), "children come along");
    CHECK(lx_get_rigidbody(s, inst) && NEAR(lx_get_rigidbody(s, inst)->mass, 3.5f, 1e-5f) && lx_get_mesh(s, inst), "components come along");
    CHECK(lx_entity_has_tag(s, inst, "Pickup"), "tag comes along");
    LxEntity found[4];
    CHECK(lx_entities_with_tag(s, "Pickup", found, 4) == 2 && lx_entity_find_with_tag(s, "Pickup"), "find by tag");
    lx_entity_set_prefab(s, inst, "Prefabs/Crate.lxprefab");
    char *scene_text = lx_scene_serialize(s);
    LxScene *s2 = lx_scene_deserialize(scene_text);
    lx_free(scene_text);
    LxEntity again = lx_entity_find_with_tag(s2, "Pickup");
    int prefabbed = 0;
    for (int i = 0; i < 2 && again; i++) { LxEntity all[4]; int n = lx_entities_with_tag(s2, "Pickup", all, 4); for (int k = 0; k < n; k++) prefabbed += !strcmp(lx_entity_prefab(s2, all[k]), "Prefabs/Crate.lxprefab"); break; }
    CHECK(prefabbed == 1, "scenes remember tags and prefab links");
    lx_scene_destroy(s2);

    /* messages */
    lx_scene_simulate(s, 1.0f / 60);
    MsgState *st = lx_behaviour_state(s, inst_lid);
    CHECK(st && st->updates == 1, "behaviour state reachable from outside");
    CHECK(lx_send_message(s, inst_lid, "hello", 4.0f, crate) && st->got == 1 && !strcmp(st->name, "hello") && NEAR(st->last, 4, 0), "send message");
    CHECK(lx_broadcast(s, "all", 1.0f, 0) == 2, "broadcast reaches both listeners");
    CHECK(!lx_send_message(s, crate, "x", 0, 0), "objects without on_message don't receive");
    int lid_id = lx_event_listen("score", on_score, NULL);
    CHECK(lx_event_emit("score", 10) == 1 && lx_event_emit("other", 5) == 0 && NEAR(g_event_sum, 10, 0), "global events");
    lx_event_unlisten(lid_id);
    CHECK(lx_event_emit("score", 10) == 0, "unlisten");

    /* timers (game time) */
    int one = 1, ten = 10;
    lx_timer_start(s, 0, 0.1f, 0, timer_hit, &one);
    lx_timer_start(s, 0, 0.05f, 0.05f, timer_hit, &ten);
    int owned = lx_timer_start(s, crate, 0.01f, 0, timer_hit, &ten);
    LX_UNUSED(owned);
    lx_send_message_after(s, inst_lid, "later", 7.0f, 0.12f);
    lx_entity_destroy(s, crate);   /* outside a frame: immediate, its timer is cancelled */
    for (int i = 0; i < 10; i++) lx_scene_simulate(s, 1.0f / 60);   /* 0.167 s */
    CHECK(g_timer_hits == 1 + 30, "timers fired once / repeated (%d)", g_timer_hits);
    CHECK(!strcmp(st->name, "later") && NEAR(st->last, 7, 0), "delayed message");

    /* time scale */
    lx_time_set_scale(0);
    float t0 = lx_game_time(s);
    lx_scene_simulate(s, 0.5f);
    CHECK(NEAR(lx_game_time(s), t0, 1e-6f), "time scale 0 pauses game time");
    lx_time_set_scale(1);

    /* tweens */
    int tw = lx_tween_position(s, inst, lx_v3(2, 0, 3), 0.5f, LX_EASE_IN_OUT);
    lx_tween_set_done_message(s, tw, "arrived");
    for (int i = 0; i < 15; i++) lx_scene_simulate(s, 1.0f / 60);
    float mid = lx_entity_position(s, inst).x;
    CHECK(mid > -2 && mid < 2, "tween moving (%g)", mid);
    for (int i = 0; i < 30; i++) lx_scene_simulate(s, 1.0f / 60);
    CHECK(NEAR(lx_entity_position(s, inst).x, 2, 1e-4f), "tween arrived");
    CHECK(NEAR(lx_ease(LX_EASE_BOUNCE_OUT, 1), 1, 1e-4f) && NEAR(lx_ease(LX_EASE_BACK_OUT, 0), 0, 1e-4f), "easing ends");
    float fv = 0;
    lx_tween_float(s, inst, &fv, 10, 0.1f, LX_EASE_LINEAR);
    for (int i = 0; i < 12; i++) lx_scene_simulate(s, 1.0f / 60);
    CHECK(NEAR(fv, 10, 1e-4f), "float tween");

    /* destroying during the frame waits until the frame ends */
    lx_send_message_after(s, inst_lid, "die", 0, 0);
    lx_scene_simulate(s, 1.0f / 60);
    CHECK(!lx_entity_valid(s, inst_lid), "destroyed at the end of the frame");
    CHECK(lx_entity_valid(s, inst), "parent survives");

    /* random */
    lx_random_seed(42);
    bool in_range = true;
    for (int i = 0; i < 1000; i++) { int r = lx_random_int(3, 6); float f = lx_random_range(-1, 1); in_range &= r >= 3 && r <= 6 && f >= -1 && f <= 1; }
    CHECK(in_range, "random ranges");

    /* save data */
    char save[LX_PATH_MAX];
    lx_path_join(save, sizeof save, tmpdir(), "luxora_test_save.txt");
    lx_path_remove(save);
    lx_save_set_file(save);
    lx_save_set_int("coins", 42);
    lx_save_set_float("volume", 0.25f);
    lx_save_set_string("name", "Grim");
    lx_scene_simulate(s, 1.0f / 60);   /* written at the end of the frame */
    lx_save_set_file("");
    lx_save_set_file(save);            /* read back from disk */
    CHECK(lx_save_get_int("coins", 0) == 42 && NEAR(lx_save_get_float("volume", 0), 0.25f, 1e-6f) &&
          !strcmp(lx_save_get_string("name", ""), "Grim") && !lx_save_has("nope") && lx_save_get_int("nope", 7) == 7, "save data survives");
    lx_save_delete("coins");
    CHECK(!lx_save_has("coins"), "delete key");
    lx_path_remove(save);
    lx_save_set_file("");
    lx_path_remove(path);
    lx_scene_stop_simulation(s);
    lx_scene_destroy(s);
}

/* ---- models: FBX import, skinning, animation, avatars ---- */
static const char *TEST_FBX =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n    FBXVersion: 7400\n}\n"
    "GlobalSettings:  {\n    Properties70:  {\n        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n    }\n}\n"
    "Objects:  {\n"
    "    Geometry: 10, \"Geometry::Bar\", \"Mesh\" {\n"
    "        Vertices: *18 {\n            a: -0.1,0,0,0.1,0,0,-0.1,1,0,0.1,1,0,-0.1,2,0,0.1,2,0\n        }\n"
    "        PolygonVertexIndex: *8 {\n            a: 0,1,3,-3,2,3,5,-5\n        }\n"
    "        LayerElementUV: 0 {\n            MappingInformationType: \"ByPolygonVertex\"\n            ReferenceInformationType: \"Direct\"\n"
    "            UV: *16 {\n                a: 0,0,1,0,1,0.5,0,0.5,0,0.5,1,0.5,1,1,0,1\n            }\n        }\n"
    "    }\n"
    "    Model: 20, \"Model::Bar\", \"Mesh\" {\n    }\n"
    "    Model: 30, \"Model::Root\", \"LimbNode\" {\n    }\n"
    "    Model: 31, \"Model::Tip\", \"LimbNode\" {\n        Properties70:  {\n"
    "            P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,1,0\n        }\n    }\n"
    "    Deformer: 40, \"Deformer::Skin\", \"Skin\" {\n    }\n"
    "    Deformer: 41, \"SubDeformer::C0\", \"Cluster\" {\n        Indexes: *4 {\n            a: 0,1,2,3\n        }\n"
    "        Weights: *4 {\n            a: 1,1,1,1\n        }\n"
    "        Transform: *16 {\n            a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n        }\n"
    "        TransformLink: *16 {\n            a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n        }\n    }\n"
    "    Deformer: 42, \"SubDeformer::C1\", \"Cluster\" {\n        Indexes: *2 {\n            a: 4,5\n        }\n"
    "        Weights: *2 {\n            a: 1,1\n        }\n"
    "        Transform: *16 {\n            a: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n        }\n"
    "        TransformLink: *16 {\n            a: 1,0,0,0,0,1,0,0,0,0,1,0,0,1,0,1\n        }\n    }\n"
    "    Material: 50, \"Material::Red Paint\", \"\" {\n        Properties70:  {\n"
    "            P: \"DiffuseColor\", \"Color\", \"\", \"A\",1,0,0\n        }\n    }\n"
    "    AnimationStack: 60, \"AnimStack::Armature|Bend\", \"\" {\n    }\n"
    "    AnimationLayer: 61, \"AnimLayer::Base\", \"\" {\n    }\n"
    "    AnimationCurveNode: 62, \"AnimCurveNode::R\", \"\" {\n        Properties70:  {\n"
    "            P: \"d|X\", \"Number\", \"\", \"A\",0\n            P: \"d|Y\", \"Number\", \"\", \"A\",0\n"
    "            P: \"d|Z\", \"Number\", \"\", \"A\",0\n        }\n    }\n"
    "    AnimationCurve: 63, \"AnimCurve::\", \"\" {\n        KeyTime: *2 {\n            a: 0,46186158000\n        }\n"
    "        KeyValueFloat: *2 {\n            a: 0,90\n        }\n    }\n"
    "}\n"
    "Connections:  {\n"
    "    C: \"OO\",20,0\n    C: \"OO\",10,20\n    C: \"OO\",50,20\n    C: \"OO\",30,0\n    C: \"OO\",31,30\n"
    "    C: \"OO\",40,10\n    C: \"OO\",41,40\n    C: \"OO\",42,40\n    C: \"OO\",30,41\n    C: \"OO\",31,42\n"
    "    C: \"OO\",61,60\n    C: \"OO\",62,61\n    C: \"OP\",62,31, \"Lcl Rotation\"\n    C: \"OP\",63,62, \"d|Z\"\n"
    "}\n";

/* a rig with Unreal mannequin bone names, written as an FBX */
static char *unreal_rig_fbx(void) {
    static const char *bones[][2] = {{"root", ""}, {"pelvis", "root"}, {"spine_01", "pelvis"}, {"spine_02", "spine_01"}, {"spine_03", "spine_02"},
                                     {"neck_01", "spine_03"}, {"head", "neck_01"}, {"clavicle_l", "spine_03"}, {"upperarm_l", "clavicle_l"},
                                     {"lowerarm_l", "upperarm_l"}, {"hand_l", "lowerarm_l"}, {"thumb_01_l", "hand_l"}, {"index_01_l", "hand_l"},
                                     {"clavicle_r", "spine_03"}, {"upperarm_r", "clavicle_r"}, {"lowerarm_r", "upperarm_r"}, {"hand_r", "lowerarm_r"},
                                     {"thigh_l", "pelvis"}, {"calf_l", "thigh_l"}, {"foot_l", "calf_l"}, {"ball_l", "foot_l"},
                                     {"thigh_r", "pelvis"}, {"calf_r", "thigh_r"}, {"foot_r", "calf_r"}, {"ball_r", "foot_r"}};
    int n = (int)(sizeof bones / sizeof bones[0]);
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n    FBXVersion: 7400\n}\nObjects:  {\n");
    for (int i = 0; i < n; i++) lx_sb_printf(&sb, "    Model: %d, \"Model::%s\", \"LimbNode\" {\n    }\n", 100 + i, bones[i][0]);
    lx_sb_printf(&sb, "}\nConnections:  {\n");
    for (int i = 0; i < n; i++) {
        int p = 0;
        for (int k = 0; k < n; k++) if (!strcmp(bones[k][0], bones[i][1])) p = 100 + k;
        lx_sb_printf(&sb, "    C: \"OO\",%d,%d\n", 100 + i, p);
    }
    lx_sb_printf(&sb, "}\n");
    return sb.data;
}

#include "test_jpeg_data.h"
static void test_jpeg(void) {
    const unsigned char *files[2] = {JPG_BASE, JPG_PROG};
    size_t sizes[2] = {sizeof JPG_BASE, sizeof JPG_PROG};
    const int (*pix[2])[5] = {JPG_BASE_PIX, JPG_PROG_PIX};
    for (int f = 0; f < 2; f++) {
        int w = 0, h = 0;
        uint8_t *px = lx_jpeg_decode(files[f], sizes[f], &w, &h);
        CHECK(px && w == 16 && h == 12, "%s JPEG decodes (%dx%d)", f ? "progressive" : "baseline", w, h);
        int worst = 0;
        for (int i = 0; px && i < 5; i++) {
            const uint8_t *p = px + ((size_t)pix[f][i][1] * 16 + (size_t)pix[f][i][0]) * 4;
            for (int c = 0; c < 3; c++) { int d = abs(p[c] - pix[f][i][2 + c]); if (d > worst) worst = d; }
        }
        CHECK(px && worst <= 3, "%s JPEG matches the reference decoder (worst difference %d)", f ? "progressive" : "baseline", worst);
        lx_free(px);
    }
    /* broken data is refused, not crashed on */
    int w, h;
    uint8_t *junk = lx_jpeg_decode(JPG_BASE, 40, &w, &h);
    CHECK(junk == NULL, "truncated JPEG is refused");
    lx_free(junk);
}

/* Weighted pose blending: the foundation the animator's cross-fades, layers and blend trees all sit on. */
static void test_pose_blend(void) {
    printf("pose blending\n");
    LxPose a, b, out;
    CHECK(lx_pose_alloc(&a, 2) && lx_pose_alloc(&b, 2) && lx_pose_alloc(&out, 2), "poses allocate");

    a.pos[0] = lx_v3(0, 0, 0);
    a.scl[0] = lx_v3(1, 1, 1);
    a.rot[0] = lx_quat_euler_deg(lx_v3(0, 0, 0));
    b.pos[0] = lx_v3(10, 0, 0);
    b.scl[0] = lx_v3(3, 3, 3);
    b.rot[0] = lx_quat_euler_deg(lx_v3(0, 90, 0));
    for (int i = 1; i < 2; i++) { a.pos[i] = b.pos[i] = lx_v3(0, 0, 0); a.scl[i] = b.scl[i] = lx_v3(1, 1, 1);
                                  a.rot[i] = b.rot[i] = lx_quat_identity(); }

    /* half each: the midpoint */
    float total;
    lx_pose_blend_begin(&out, &total);
    lx_pose_blend_add(&out, &a, 0.5f, &total);
    lx_pose_blend_add(&out, &b, 0.5f, &total);
    lx_pose_blend_end(&out, total, &a);
    CHECK(NEAR(out.pos[0].x, 5, 1e-4f) && NEAR(out.scl[0].x, 2, 1e-4f), "an even blend is the midpoint (%g %g)",
          out.pos[0].x, out.scl[0].x);
    CHECK(NEAR(lx_quat_to_euler_deg(out.rot[0]).y, 45, 0.5f), "and halfway round the turn (%g deg)",
          lx_quat_to_euler_deg(out.rot[0]).y);

    /* weights need not sum to 1: 3 and 1 is three quarters of the way */
    lx_pose_blend_begin(&out, &total);
    lx_pose_blend_add(&out, &a, 3.0f, &total);
    lx_pose_blend_add(&out, &b, 1.0f, &total);
    lx_pose_blend_end(&out, total, &a);
    CHECK(NEAR(out.pos[0].x, 2.5f, 1e-4f), "raw weights are normalised by their total (%g)", out.pos[0].x);

    /* one contributor only: exactly that pose, whatever the weight */
    lx_pose_blend_begin(&out, &total);
    lx_pose_blend_add(&out, &b, 0.25f, &total);
    lx_pose_blend_end(&out, total, &a);
    CHECK(NEAR(out.pos[0].x, 10, 1e-4f) && NEAR(out.scl[0].x, 3, 1e-4f), "a single contributor comes through unchanged (%g)",
          out.pos[0].x);

    /* zero, negative and NaN weights are ignored, and no weight at all leaves the fallback */
    lx_pose_blend_begin(&out, &total);
    lx_pose_blend_add(&out, &b, 0.0f, &total);
    lx_pose_blend_add(&out, &b, -2.0f, &total);
    lx_pose_blend_add(&out, &b, (float)NAN, &total);
    lx_pose_blend_end(&out, total, &a);
    CHECK(NEAR(out.pos[0].x, 0, 1e-4f), "with nothing to blend the fallback pose is kept (%g)", out.pos[0].x);

    /* q and -q are the same rotation: blending must take the short way round, not through the far side */
    a.rot[0] = lx_quat_euler_deg(lx_v3(0, 170, 0));
    b.rot[0] = lx_quat_euler_deg(lx_v3(0, -170, 0));   /* 20 degrees apart across the wrap */
    a.pos[0] = b.pos[0] = lx_v3(0, 0, 0);
    a.scl[0] = b.scl[0] = lx_v3(1, 1, 1);
    lx_pose_blend_begin(&out, &total);
    lx_pose_blend_add(&out, &a, 0.5f, &total);
    lx_pose_blend_add(&out, &b, 0.5f, &total);
    lx_pose_blend_end(&out, total, &a);
    {
        float y = lx_quat_to_euler_deg(out.rot[0]).y;
        CHECK(fabsf(y) > 175.0f, "a blend across the 180 degree wrap goes the short way (%g deg, not ~0)", y);
        LxQuat q = out.rot[0];
        CHECK(NEAR(sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w), 1, 1e-4f), "the blended rotation is normalised");
    }

    lx_pose_free(&a);
    lx_pose_free(&b);
    lx_pose_free(&out);
    CHECK(out.pos == NULL && out.count == 0, "freeing a pose clears it, so freeing twice is safe");
    lx_pose_free(&out);
}

/* Blend tree weights: pure maths on the children's positions, so it can be checked exactly. */
static void test_blend_trees(void) {
    printf("blend trees\n");
    LxAnimTree t;
    float w[LX_ANIM_MAX_CHILDREN];
    float xy[2];

    /* 1D: idle at 0, walk at 2, run at 6 */
    memset(&t, 0, sizeof t);
    t.type = LX_AT_1D;
    t.child_count = 3;
    t.child[0].threshold = 0;
    t.child[1].threshold = 2;
    t.child[2].threshold = 6;
    xy[0] = 0;
    xy[1] = 0;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[0], 1, 1e-5f) && NEAR(w[1], 0, 1e-5f), "1D on a threshold is all that motion (%g %g)", w[0], w[1]);
    xy[0] = 1;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[0], 0.5f, 1e-5f) && NEAR(w[1], 0.5f, 1e-5f) && NEAR(w[2], 0, 1e-5f),
          "1D halfway between two is half each, and nothing of the third (%g %g %g)", w[0], w[1], w[2]);
    xy[0] = 4;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[1], 0.5f, 1e-5f) && NEAR(w[2], 0.5f, 1e-5f), "1D blends the pair it sits between (%g %g)", w[1], w[2]);
    xy[0] = 99;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[2], 1, 1e-5f), "1D past the end holds the last motion (%g)", w[2]);
    xy[0] = -5;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[0], 1, 1e-5f), "1D before the start holds the first (%g)", w[0]);
    xy[0] = (float)NAN;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[0] + w[1] + w[2], 1, 1e-4f), "a NaN parameter still produces usable weights");

    /* out of order thresholds are the same tree */
    memset(&t, 0, sizeof t);
    t.type = LX_AT_1D;
    t.child_count = 3;
    t.child[0].threshold = 6;
    t.child[1].threshold = 0;
    t.child[2].threshold = 2;
    xy[0] = 1;
    xy[1] = 0;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[1], 0.5f, 1e-5f) && NEAR(w[2], 0.5f, 1e-5f), "1D does not need its thresholds in order (%g %g)", w[1], w[2]);

    /* 2D cartesian: four motions around the origin */
    memset(&t, 0, sizeof t);
    t.type = LX_AT_2D_CARTESIAN;
    t.child_count = 4;
    t.child[0].px = -1; t.child[0].py = 0;
    t.child[1].px = 1;  t.child[1].py = 0;
    t.child[2].px = 0;  t.child[2].py = -1;
    t.child[3].px = 0;  t.child[3].py = 1;
    xy[0] = 1;
    xy[1] = 0;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[1], 1, 1e-4f), "2D on a motion's own position is all of it (%g)", w[1]);
    xy[0] = 0;
    xy[1] = 0;
    lx_anim_tree_weights(&t, xy, w);
    {
        float sum = w[0] + w[1] + w[2] + w[3];
        CHECK(NEAR(sum, 1, 1e-4f) && NEAR(w[0], w[1], 1e-4f) && NEAR(w[2], w[3], 1e-4f),
              "2D in the middle of a symmetric set is even (%g %g %g %g)", w[0], w[1], w[2], w[3]);
    }
    /* the weights move continuously, which is what stops the pose jumping */
    float prev[LX_ANIM_MAX_CHILDREN];
    xy[0] = 0.40f;
    lx_anim_tree_weights(&t, xy, prev);
    xy[0] = 0.41f;
    lx_anim_tree_weights(&t, xy, w);
    {
        float worst = 0;
        for (int i = 0; i < 4; i++) worst = lx_maxf(worst, fabsf(w[i] - prev[i]));
        CHECK(worst < 0.05f, "a small step in the parameter is a small step in the weights (%g)", worst);
    }

    /* 2D directional: direction matters more than speed */
    memset(&t, 0, sizeof t);
    t.type = LX_AT_2D_DIRECTIONAL;
    t.child_count = 3;
    t.child[0].px = 0;  t.child[0].py = 1;    /* forward */
    t.child[1].px = 0;  t.child[1].py = -1;   /* back */
    t.child[2].px = 1;  t.child[2].py = 0;    /* right */
    xy[0] = 0;
    xy[1] = 2;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(w[0] > 0.8f && w[1] < 0.05f, "directional: faster forwards is still forwards (%g forward, %g back)", w[0], w[1]);
    xy[0] = 0.7f;
    xy[1] = 0.7f;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(w[0] > 0.2f && w[2] > 0.2f && w[1] < 0.1f, "directional: forward-right blends those two (%g %g %g)", w[0], w[2], w[1]);

    /* Direct: a parameter per child, so the simple form shares the weight evenly */
    memset(&t, 0, sizeof t);
    t.type = LX_AT_DIRECT;
    t.child_count = 2;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[0], 0.5f, 1e-5f) && NEAR(w[1], 0.5f, 1e-5f), "a direct tree with no lookup shares evenly (%g %g)", w[0], w[1]);

    /* an empty tree asks for nothing, and one child takes everything */
    memset(&t, 0, sizeof t);
    t.type = LX_AT_1D;
    t.child_count = 0;
    lx_anim_tree_weights(&t, xy, w);
    t.child_count = 1;
    lx_anim_tree_weights(&t, xy, w);
    CHECK(NEAR(w[0], 1, 1e-5f), "one motion takes the whole blend (%g)", w[0]);
}

/* Two-bone IK: the closed-form answer for a limb reaching a point. */
static void test_ik(void) {
    printf("inverse kinematics\n");
    /* a straight-down leg: hip at the top, knee in the middle, ankle at the bottom */
    LxVec3 hip = lx_v3(0, 2, 0), knee = lx_v3(0, 1, 0), ankle = lx_v3(0, 0, 0);
    LxVec3 pole = lx_v3(0, 1, 2);   /* the knee points forward */
    LxIkResult r;

    /* a target the limb can reach: the ankle ends up there */
    LxVec3 target = lx_v3(0.6f, 0.4f, 0.3f);
    CHECK(lx_ik_two_bone(hip, knee, ankle, target, pole, lx_quat_identity(), lx_quat_identity(), 1, &r),
          "the solver answers for a reachable target");
    {
        /* follow the bones through the rotations it gave back */
        LxVec3 upper = lx_quat_rotate(r.root, lx_v3(0, -1, 0));
        LxVec3 new_knee = lx_add3(hip, upper);
        LxVec3 lower = lx_quat_rotate(r.mid, lx_v3(0, -1, 0));
        LxVec3 new_ankle = lx_add3(new_knee, lower);
        float err = lx_len3(lx_sub3(new_ankle, target));
        CHECK(err < 1e-3f, "and the ankle lands on it (off by %g)", err);
        float upper_len = lx_len3(lx_sub3(new_knee, hip)), lower_len = lx_len3(lx_sub3(new_ankle, new_knee));
        CHECK(NEAR(upper_len, 1, 1e-4f) && NEAR(lower_len, 1, 1e-4f), "without stretching either bone (%g %g)", upper_len, lower_len);
    }

    /* the knee goes the way the pole says: a leg bending straight down must bend forwards, towards it */
    for (int straight = 0; straight < 2; straight++) {
        LxVec3 k0 = straight ? knee : lx_v3(0, 1, -0.05f);   /* straight, or already a little backwards */
        CHECK(lx_ik_two_bone(hip, k0, ankle, lx_v3(0, 0.6f, 0), pole, lx_quat_identity(), lx_quat_identity(), 1, &r), "a bent leg answers");
        LxVec3 nk = lx_add3(hip, lx_quat_rotate(r.root, lx_sub3(k0, hip)));
        CHECK(nk.z > 0.3f, "the knee bends towards the pole, not away (knee z %.3f, %s)", nk.z, straight ? "from straight" : "from bent back");
    }

    /* a target further than the limb is long: it straightens towards it rather than tearing */
    LxVec3 far_target = lx_v3(0, -6, 0);
    CHECK(lx_ik_two_bone(hip, knee, ankle, far_target, pole, lx_quat_identity(), lx_quat_identity(), 1, &r), "an unreachable target still answers");
    {
        LxVec3 new_knee = lx_add3(hip, lx_quat_rotate(r.root, lx_v3(0, -1, 0)));
        LxVec3 new_ankle = lx_add3(new_knee, lx_quat_rotate(r.mid, lx_v3(0, -1, 0)));
        float reach = lx_len3(lx_sub3(new_ankle, hip));
        CHECK(reach > 1.9f && reach <= 2.0001f, "reaching as far as it can, no further (%g of 2)", reach);
        CHECK(!r.reached, "and it says so");
    }

    /* weight 0 leaves the limb alone; a part weight lands between */
    LxQuat was = lx_quat_euler_deg(lx_v3(0, 0, 0));
    CHECK(!lx_ik_two_bone(hip, knee, ankle, target, pole, was, was, 0, &r), "weight 0 does nothing");
    lx_ik_two_bone(hip, knee, ankle, target, pole, was, was, 1, &r);
    LxQuat full = r.root;
    lx_ik_two_bone(hip, knee, ankle, target, pole, was, was, 0.5f, &r);
    {
        float d_full = fabsf(r.root.x * full.x + r.root.y * full.y + r.root.z * full.z + r.root.w * full.w);
        float d_was = fabsf(r.root.x * was.x + r.root.y * was.y + r.root.z * was.z + r.root.w * was.w);
        CHECK(d_full < 0.9999f && d_was < 0.9999f, "half weight is part of the way (%g to the answer, %g to the pose)", d_full, d_was);
    }

    /* ---- foot grounding, as the two rules it is made of ----
     * Both of these came from a bug: the hips were being dropped by how far a foot was *above* the
     * ground, so a character sank into a crouch whenever it lifted a foot. */
    CHECK(NEAR(lx_ik_ground_weight(0, LX_FOOT_IK_BAND), 1, 1e-5f), "a foot on the ground is fully grounded");
    CHECK(NEAR(lx_ik_ground_weight(-0.3f, LX_FOOT_IK_BAND), 1, 1e-5f), "a foot through the ground is lifted out of it");
    CHECK(NEAR(lx_ik_ground_weight(LX_FOOT_IK_BAND * 0.5f, LX_FOOT_IK_BAND), 0.5f, 1e-5f),
          "a foot just off the ground is grounded in part (%g)", lx_ik_ground_weight(LX_FOOT_IK_BAND * 0.5f, LX_FOOT_IK_BAND));
    CHECK(lx_ik_ground_weight(LX_FOOT_IK_BAND, LX_FOOT_IK_BAND) == 0 && lx_ik_ground_weight(2.0f, LX_FOOT_IK_BAND) == 0,
          "a foot in the air is left where the animation put it");
    CHECK(NEAR(lx_ik_ground_weight((float)NAN, LX_FOOT_IK_BAND), 1, 1e-5f) && lx_ik_ground_weight(1, 0) == 0,
          "a NaN height or a band of nothing still gives a usable weight");
    {
        /* continuous: no step in the weight as a foot lifts, or the pose would jump */
        float worst = 0, prev = lx_ik_ground_weight(-0.05f, LX_FOOT_IK_BAND);
        for (int i = 1; i <= 100; i++) {
            float h = -0.05f + (float)i * 0.003f;
            float w = lx_ik_ground_weight(h, LX_FOOT_IK_BAND);
            worst = lx_maxf(worst, fabsf(w - prev));
            prev = w;
        }
        CHECK(worst < 0.1f, "the grounding fades in and out smoothly (worst step %g)", worst);
    }
    CHECK(lx_ik_hip_drop(0.5f, 1.0f) == 0, "a leg that can reach asks for no hip drop");
    CHECK(lx_ik_hip_drop(0.99f, 1.0f) == 0, "nor one reaching almost its whole length");
    CHECK(NEAR(lx_ik_hip_drop(1.2f, 1.0f), 0.21f, 1e-4f), "a leg too short asks for the difference (%g)", lx_ik_hip_drop(1.2f, 1.0f));
    CHECK(lx_ik_hip_drop(0.1f, 1.0f) == 0, "a foot raised high above the ground does not sink the hips");

    /* a limb with no length has nothing to solve */
    CHECK(!lx_ik_two_bone(hip, hip, hip, target, pole, was, was, 1, &r), "a limb with no length is refused");
    /* a target on top of the hip, and a pole in line with the limb: no NaN, no crash */
    CHECK(lx_ik_two_bone(hip, knee, ankle, hip, pole, was, was, 1, &r) || true, "a target at the root does not crash");
    lx_ik_two_bone(hip, knee, ankle, lx_v3(0, 1.2f, 0), lx_v3(0, 0, 0), was, was, 1, &r);
    CHECK(r.root.w == r.root.w && r.mid.w == r.mid.w, "a pole in line with the limb gives no NaN");
}

/* The animator graph asset: its text format, and the editing helpers that keep it consistent. */
static void test_anim_graph_asset(void) {
    printf("animator graph assets\n");
    LxAnimGraph *g = lx_anim_graph_new("Hero");
    CHECK(g && g->layer_count == 1 && g->layers[0].state_count == 1, "a new graph has a base layer with one state");
    CHECK(!strcmp(g->layers[0].start, g->layers[0].states[0].name), "and starts in it");

    lx_anim_add_param(g, "Speed", LX_AP_FLOAT);
    lx_anim_add_param(g, "Grounded", LX_AP_BOOL);
    lx_anim_add_param(g, "Attack", LX_AP_TRIGGER);
    g->params[0].f = 0.25f;
    LxAnimLayer *base = &g->layers[0];
    LxAnimNode *idle = &base->states[0];
    lx_strcpy(idle->name, sizeof idle->name, "Idle");
    lx_strcpy(idle->clip, sizeof idle->clip, "Models/Hero.fbx#Idle");
    lx_strcpy(base->start, sizeof base->start, "Idle");
    LxAnimNode *run = lx_anim_add_state(g, base, "Run", 400, 120);
    lx_strcpy(run->clip, sizeof run->clip, "Models/Hero.fbx#Run");
    run->foot_ik = true;
    lx_strcpy(run->tag, sizeof run->tag, "Locomotion");
    LxAnimTransition *t = lx_anim_add_transition(&base->states[0], "Run");
    t->has_exit_time = false;
    t->duration = 0.15f;
    lx_anim_add_cond(t, "Speed", LX_AC_GREATER, 0.1f);
    lx_anim_add_cond(t, "Grounded", LX_AC_IF, 0);
    LxAnimTransition *any = lx_anim_add_any_transition(base, "Idle");
    lx_anim_add_cond(any, "Attack", LX_AC_IF, 0);
    /* a second layer with a mask and a blend tree state */
    LxAnimLayer *upper = lx_anim_add_layer(g, "Upper Body");
    upper->weight = 0.5f;
    upper->blend = LX_ABM_ADDITIVE;
    upper->mask = 1ull << LX_HB_CHEST;
    LxAnimNode *aim = lx_anim_add_state(g, upper, "Aim", 200, 80);
    aim->motion = LX_AM_TREE;
    aim->tree = lx_anim_add_tree(g, LX_AT_2D_CARTESIAN);
    lx_strcpy(g->trees[aim->tree].param_x, LX_ANIM_NAME, "Speed");
    g->trees[aim->tree].child_count = 2;
    lx_strcpy(g->trees[aim->tree].child[0].clip, LX_REF_MAX, "Models/Hero.fbx#AimLeft");
    lx_strcpy(g->trees[aim->tree].child[1].clip, LX_REF_MAX, "Models/Hero.fbx#AimRight");
    g->trees[aim->tree].child[1].px = 1;
    lx_strcpy(upper->start, sizeof upper->start, "Aim");

    /* round trip: everything comes back */
    char *text = lx_anim_graph_to_text(g);
    LxAnimGraph *b = lx_anim_graph_from_text(text);
    CHECK(b != NULL, "the text parses back");
    if (b) {
        CHECK(!strcmp(b->name, "Hero") && b->param_count == 3 && b->layer_count == 2 && b->tree_count == 1,
              "name, parameters, layers and trees survive (%d %d %d)", b->param_count, b->layer_count, b->tree_count);
        CHECK(!strcmp(b->params[0].name, "Speed") && b->params[0].type == LX_AP_FLOAT && NEAR(b->params[0].f, 0.25f, 1e-5f) &&
              b->params[2].type == LX_AP_TRIGGER, "a parameter's name, type and starting value survive");
        const LxAnimLayer *l0 = &b->layers[0];
        CHECK(l0->state_count == 2 && !strcmp(l0->start, "Idle"), "the base layer's states and starting state survive");
        CHECK(!strcmp(l0->states[1].clip, "Models/Hero.fbx#Run") && l0->states[1].foot_ik &&
              !strcmp(l0->states[1].tag, "Locomotion"), "a state's clip, foot IK and tag survive");
        const LxAnimTransition *bt = l0->states[0].trans_count ? &l0->states[0].trans[0] : NULL;
        CHECK(bt && !strcmp(bt->to, "Run") && !bt->has_exit_time && NEAR(bt->duration, 0.15f, 1e-5f) && bt->cond_count == 2,
              "a transition's target, exit time, duration and conditions survive");
        CHECK(bt && !strcmp(bt->cond[0].param, "Speed") && bt->cond[0].mode == LX_AC_GREATER && NEAR(bt->cond[0].value, 0.1f, 1e-5f) &&
              bt->cond[1].mode == LX_AC_IF, "each condition's parameter, test and threshold survive");
        CHECK(l0->any_count == 1 && !strcmp(l0->any[0].to, "Idle") && l0->any[0].cond_count == 1, "Any State transitions survive");
        const LxAnimLayer *l1 = &b->layers[1];
        CHECK(!strcmp(l1->name, "Upper Body") && NEAR(l1->weight, 0.5f, 1e-5f) && l1->blend == LX_ABM_ADDITIVE &&
              l1->mask == (1ull << LX_HB_CHEST), "a layer's name, weight, blending and mask survive");
        CHECK(l1->state_count == 1 && l1->states[0].motion == LX_AM_TREE && l1->states[0].tree == 0,
              "a blend tree state keeps its tree");
        CHECK(b->trees[0].type == LX_AT_2D_CARTESIAN && b->trees[0].child_count == 2 &&
              !strcmp(b->trees[0].child[1].clip, "Models/Hero.fbx#AimRight") && NEAR(b->trees[0].child[1].px, 1, 1e-5f),
              "the tree's type, motions and positions survive");
        /* and writing it again gives the same text, so the format is stable */
        char *again = lx_anim_graph_to_text(b);
        CHECK(again && !strcmp(again, text), "writing it again gives identical text");
        lx_free(again);
        lx_anim_graph_free(b);
    }
    lx_free(text);

    /* a condition on a parameter whose name holds spaces still comes back whole */
    {
        LxAnimGraph *sp = lx_anim_graph_new("Spaces");
        lx_anim_add_param(sp, "Move Speed X", LX_AP_FLOAT);
        LxAnimTransition *st = lx_anim_add_transition(&sp->layers[0].states[0], sp->layers[0].states[0].name);
        st->to_self = true;
        lx_anim_add_cond(st, "Move Speed X", LX_AC_LESS, -2.5f);
        char *txt = lx_anim_graph_to_text(sp);
        LxAnimGraph *rb = lx_anim_graph_from_text(txt);
        CHECK(rb && rb->layers[0].states[0].trans_count == 1 &&
              !strcmp(rb->layers[0].states[0].trans[0].cond[0].param, "Move Speed X") &&
              NEAR(rb->layers[0].states[0].trans[0].cond[0].value, -2.5f, 1e-5f),
              "a parameter name with spaces survives a condition (%s)",
              rb && rb->layers[0].states[0].trans_count ? rb->layers[0].states[0].trans[0].cond[0].param : "gone");
        lx_free(txt);
        lx_anim_graph_free(rb);
        lx_anim_graph_free(sp);
    }

    /* editing helpers keep references right */
    base = &g->layers[0];
    CHECK(lx_anim_rename_state(base, 1, "Sprint"), "a state renames");
    CHECK(!strcmp(base->states[0].trans[0].to, "Sprint"), "and the transitions that pointed at it follow");
    CHECK(!lx_anim_rename_state(base, 1, "Idle"), "a name already taken is refused");
    CHECK(lx_anim_rename_param(g, 0, "Ground Speed"), "a parameter renames");
    CHECK(!strcmp(base->states[0].trans[0].cond[0].param, "Ground Speed"), "and its conditions follow");
    /* the condition on it and the blend tree that blends along it: both count */
    CHECK(lx_anim_param_uses(g, "Ground Speed") == 2, "uses of a parameter are counted (%d)", lx_anim_param_uses(g, "Ground Speed"));
    lx_anim_remove_state(base, 1);
    CHECK(base->state_count == 1 && base->states[0].trans[0].to[0] == 0,
          "deleting a state leaves the transitions that pointed at it unconnected");
    {
        char name[LX_ANIM_NAME];
        lx_anim_unique_name(g, base, "Idle", name, sizeof name, 0);
        CHECK(strcmp(name, "Idle") != 0, "a new state gets a name of its own (%s)", name);
    }
    /* problems are found */
    {
        char lines[8][160];
        int n = lx_anim_graph_problems(g, lines, 8);
        CHECK(n > 0, "the unconnected transition is reported (%d problems: %s)", n, n ? lines[0] : "");
    }
    CHECK(g->layer_count == 2, "two layers");
    lx_anim_remove_layer(g, 1);
    CHECK(g->layer_count == 1, "a layer is removed");
    lx_anim_remove_layer(g, 0);
    CHECK(g->layer_count == 1, "but the base layer stays: a graph always has one");
    lx_anim_graph_free(g);

    /* text that is not an animator graph at all */
    CHECK(lx_anim_graph_from_text("") == NULL, "empty text is not a graph");
    CHECK(lx_anim_graph_from_text("[nonsense]\nkey = value\n") == NULL, "text with no layers is not a graph");
    {
        /* a graph naming a tree that is not there, and a tree pointing at itself */
        LxAnimGraph *bad = lx_anim_graph_from_text("[animator]\nname = Bad\n[layer]\nname = L\n[state]\nname = S\nmotion = tree\ntree = 7\n");
        CHECK(bad && bad->layers[0].states[0].tree == -1, "a state naming a tree that is gone loses it rather than reading rubbish");
        lx_anim_graph_free(bad);
    }
}

/* The runtime: states play, parameters drive transitions, triggers are consumed, layers blend. */
static void test_animator_runtime(void) {
    printf("animator runtime\n");
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), "luxora_test_animator");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_assets_set_root(dir);
    lx_path_join(path, sizeof path, dir, "BarModel.fbx");
    lx_file_write(path, TEST_FBX, strlen(TEST_FBX));

    /* a graph: Idle, then Run when Speed > 1, back when it drops, and Hit from anywhere on a trigger */
    LxAnimGraph *g = lx_anim_graph_new("Bar");
    LxAnimLayer *base = &g->layers[0];
    lx_strcpy(base->states[0].name, LX_ANIM_NAME, "Idle");
    lx_strcpy(base->states[0].clip, LX_REF_MAX, "BarModel.fbx#Bend");
    lx_strcpy(base->start, sizeof base->start, "Idle");
    LxAnimNode *run = lx_anim_add_state(g, base, "Run", 300, 0);
    lx_strcpy(run->clip, sizeof run->clip, "BarModel.fbx#Bend");
    LxAnimNode *hit = lx_anim_add_state(g, base, "Hit", 300, 200);
    lx_strcpy(hit->clip, sizeof hit->clip, "BarModel.fbx#Bend");
    hit->loop = false;
    lx_anim_add_param(g, "Speed", LX_AP_FLOAT);
    lx_anim_add_param(g, "Hurt", LX_AP_TRIGGER);
    LxAnimTransition *to_run = lx_anim_add_transition(&base->states[0], "Run");
    to_run->has_exit_time = false;
    to_run->duration = 0;
    lx_anim_add_cond(to_run, "Speed", LX_AC_GREATER, 1.0f);
    LxAnimTransition *to_idle = lx_anim_add_transition(&base->states[1], "Idle");
    to_idle->has_exit_time = false;
    to_idle->duration = 0;
    lx_anim_add_cond(to_idle, "Speed", LX_AC_LESS, 1.0f);
    LxAnimTransition *to_hit = lx_anim_add_any_transition(base, "Hit");
    to_hit->duration = 0;
    lx_anim_add_cond(to_hit, "Hurt", LX_AC_IF, 0);
    lx_path_join(path, sizeof path, dir, "Bar.lxanim");
    CHECK(lx_anim_graph_save(g, path), "the graph saves");
    lx_anim_graph_free(g);

    LxScene *s = lx_scene_create("Animator");
    LxEntity root = lx_model_instantiate(s, "BarModel.fbx", lx_v3(0, 0, 0), 0);
    LxAnimator *an = lx_get_animator(s, root);
    CHECK(an != NULL, "the model has an Animator");
    if (!an) { lx_scene_destroy(s); return; }
    lx_strcpy(an->graph, sizeof an->graph, "Bar.lxanim");
    an->play_on_start = false;

    lx_animator_update(s, root, 0.016f);
    CHECK(lx_animator_layer_count(s, root) == 1, "the animator binds its graph (%d layers)", lx_animator_layer_count(s, root));
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Idle"), "it starts in the layer's starting state (%s)", lx_animator_state(s, root, 0));
    CHECK(lx_animator_has_param(s, root, "Speed") && !lx_animator_has_param(s, root, "Nothing"), "its parameters are there");

    /* normalised time runs forward, in cycles of the clip (1 second long) */
    for (int i = 0; i < 10; i++) lx_animator_update(s, root, 0.05f);
    CHECK(NEAR(lx_animator_normalized_time(s, root, 0), 0.5f + 0.016f, 0.02f),
          "progress is counted in cycles (%g)", lx_animator_normalized_time(s, root, 0));

    /* a parameter the transition is waiting on: it goes */
    lx_animator_set_float(s, root, "Speed", 4);
    CHECK(NEAR(lx_animator_get_float(s, root, "Speed"), 4, 1e-5f), "a float parameter reads back");
    lx_animator_update(s, root, 0.016f);
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Run"), "the condition fires the transition (%s)", lx_animator_state(s, root, 0));
    lx_animator_set_float(s, root, "Speed", 0);
    lx_animator_update(s, root, 0.016f);
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Idle"), "and back again when it no longer holds (%s)", lx_animator_state(s, root, 0));

    /* a trigger fires one transition and clears itself */
    lx_animator_set_trigger(s, root, "Hurt");
    CHECK(lx_animator_get_bool(s, root, "Hurt"), "a trigger is set");
    lx_animator_update(s, root, 0.016f);
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Hit"), "an Any State transition takes it (%s)", lx_animator_state(s, root, 0));
    CHECK(!lx_animator_get_bool(s, root, "Hurt"), "and the trigger is spent");
    for (int i = 0; i < 40; i++) lx_animator_update(s, root, 0.05f);
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Hit"), "with nothing to leave for, it stays (%s)", lx_animator_state(s, root, 0));
    CHECK(lx_animator_normalized_time(s, root, 0) <= 1.0001f, "a state that does not loop stops at the end (%g)",
          lx_animator_normalized_time(s, root, 0));

    /* going somewhere directly, whatever the transitions say */
    CHECK(lx_animator_play_state(s, root, 0, "Run", 0), "Play Animator State finds the state");
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Run"), "and goes there (%s)", lx_animator_state(s, root, 0));
    CHECK(!lx_animator_play_state(s, root, 0, "Nowhere", 0), "a state that is not there is refused");

    /* a cross-fade is reported while it runs */
    lx_animator_play_state(s, root, 0, "Idle", 0.5f);
    CHECK(lx_animator_in_transition(s, root, 0), "a fade is in progress");
    lx_animator_update(s, root, 0.6f);
    CHECK(!lx_animator_in_transition(s, root, 0), "and over when its time is up");

    /* the bones actually move: the animated bone is where the clip says at half way */
    LxEntity tip = lx_entity_find(s, "Tip");
    lx_animator_play_state(s, root, 0, "Idle", 0);
    for (int i = 0; i < 10; i++) lx_animator_update(s, root, 0.05f);
    CHECK(tip && NEAR(lx_entity_euler(s, tip).z, 45, 2.0f), "the graph poses the skeleton (%g deg)", tip ? lx_entity_euler(s, tip).z : 0);

    /* parameters a graph does not have are quietly ignored rather than fatal */
    lx_animator_set_float(s, root, "Nothing", 5);
    lx_animator_set_trigger(s, root, "Nothing");
    CHECK(NEAR(lx_animator_get_float(s, root, "Nothing"), 0, 1e-5f), "an unknown parameter reads 0");
    lx_animator_set_float(s, root, "Speed", (float)NAN);
    CHECK(lx_animator_get_float(s, root, "Speed") == lx_animator_get_float(s, root, "Speed"), "a NaN is not let in");

    /* layers */
    CHECK(NEAR(lx_animator_layer_weight(s, root, 0), 1, 1e-5f), "the base layer is always whole");
    lx_animator_set_layer_weight(s, root, 0, 0.2f);
    CHECK(NEAR(lx_animator_layer_weight(s, root, 0), 1, 1e-5f), "and cannot be turned down");
    lx_animator_set_layer_weight(s, root, 5, 0.5f);   /* a layer that is not there: nothing happens */

    /* IK goals and look-at are accepted even on a generic rig (there is simply nothing to solve) */
    lx_animator_set_ik_goal(s, root, LX_IK_LEFT_HAND, lx_v3(1, 1, 1), 1);
    lx_animator_set_look_at(s, root, lx_v3(0, 2, 5), 1);
    lx_animator_update(s, root, 0.016f);
    lx_animator_clear_ik_goal(s, root, LX_IK_LEFT_HAND);
    lx_animator_clear_look_at(s, root);
    an = lx_get_animator(s, root);
    an->foot_ik = true;
    an->foot_ik_weight = 1;
    lx_animator_update(s, root, 0.016f);
    CHECK(true, "foot IK on a rig with no avatar is a no-op, not a crash");

    /* editing the graph while it plays: the animator re-binds and keeps its state */
    LxAnimGraph *live = lx_asset_anim_graph("Bar.lxanim");
    CHECK(live != NULL, "the graph is the cached asset");
    if (live) {
        lx_animator_set_float(s, root, "Speed", 7);
        lx_animator_update(s, root, 0.016f);
        const char *before = lx_animator_state(s, root, 0);
        char kept[LX_ANIM_NAME];
        lx_strcpy(kept, sizeof kept, before);
        live->layers[0].states[1].speed = 2.0f;
        lx_anim_graph_touch(live);
        lx_animator_update(s, root, 0.016f);
        CHECK(!strcmp(lx_animator_state(s, root, 0), kept), "an edit mid-play keeps the state that was playing (%s)",
              lx_animator_state(s, root, 0));
        CHECK(NEAR(lx_animator_get_float(s, root, "Speed"), 7, 1e-5f), "and the parameter values");
    }

    /* no graph: the animator goes back to playing single clips */
    an = lx_get_animator(s, root);
    an->graph[0] = 0;
    lx_animator_update(s, root, 0.016f);
    CHECK(lx_animator_layer_count(s, root) == 0, "clearing the graph unbinds it");
    lx_animator_play(s, root, "BarModel.fbx#Bend", 0);
    lx_animator_update(s, root, 0.016f);
    CHECK(lx_animator_playing(s, root), "and the clip animator still works");

    /* a graph that is not there is a warning, not a crash */
    an = lx_get_animator(s, root);
    lx_strcpy(an->graph, sizeof an->graph, "Missing.lxanim");
    lx_animator_update(s, root, 0.016f);
    CHECK(lx_animator_layer_count(s, root) == 0, "a missing graph leaves the animator with nothing to play");

    lx_scene_destroy(s);
    lx_anim_graphs_clear();
    /* the model cache is keyed by name alone, and the next test writes its own BarModel.fbx */
    lx_model_reload("BarModel.fbx");
}

/* Exit time: a transition that waits for the clip to get where it should be before it goes. */
static void test_anim_exit_time(void) {
    printf("animator exit time\n");
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), "luxora_test_exit");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_assets_set_root(dir);
    lx_path_join(path, sizeof path, dir, "BarModel.fbx");
    lx_file_write(path, TEST_FBX, strlen(TEST_FBX));

    LxAnimGraph *g = lx_anim_graph_new("Exit");
    LxAnimLayer *base = &g->layers[0];
    lx_strcpy(base->states[0].name, LX_ANIM_NAME, "Swing");
    lx_strcpy(base->states[0].clip, LX_REF_MAX, "BarModel.fbx#Bend");
    base->states[0].loop = false;
    lx_strcpy(base->start, sizeof base->start, "Swing");
    LxAnimNode *rest = lx_anim_add_state(g, base, "Rest", 300, 0);
    lx_strcpy(rest->clip, sizeof rest->clip, "BarModel.fbx#Bend");
    /* no conditions at all: the exit time is the whole of it */
    LxAnimTransition *t = lx_anim_add_transition(&base->states[0], "Rest");
    t->has_exit_time = true;
    t->exit_time = 0.75f;
    t->duration = 0;
    lx_path_join(path, sizeof path, dir, "Exit.lxanim");
    lx_anim_graph_save(g, path);
    lx_anim_graph_free(g);

    LxScene *s = lx_scene_create("ExitTime");
    LxEntity root = lx_model_instantiate(s, "BarModel.fbx", lx_v3(0, 0, 0), 0);
    LxAnimator *an = lx_get_animator(s, root);
    if (!an) { lx_scene_destroy(s); return; }
    lx_strcpy(an->graph, sizeof an->graph, "Exit.lxanim");
    an->play_on_start = false;

    lx_animator_update(s, root, 0.0f);
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Swing"), "it starts in Swing (%s)", lx_animator_state(s, root, 0));
    /* the clip is one second long: after half of it, the exit time has not come */
    for (int i = 0; i < 10; i++) lx_animator_update(s, root, 0.05f);
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Swing"), "half way through it stays (%s at %g)",
          lx_animator_state(s, root, 0), lx_animator_normalized_time(s, root, 0));
    /* past three quarters, it goes */
    for (int i = 0; i < 8; i++) lx_animator_update(s, root, 0.05f);
    CHECK(!strcmp(lx_animator_state(s, root, 0), "Rest"), "past the exit time it goes (%s)", lx_animator_state(s, root, 0));

    lx_scene_destroy(s);
    lx_anim_graphs_clear();
    lx_model_reload("BarModel.fbx");
}

/* lx_model_import_load takes the model's path and adds the suffix itself */
static const char *imp_path_model(char *lximport_path) {
    static char model[LX_PATH_MAX];
    lx_strcpy(model, sizeof model, lximport_path);
    char *dot = strstr(model, ".lximport");
    if (dot) *dot = 0;
    return model;
}

/* Root motion: the clip's own travel, taken out of the pose and given to the object. */
static void test_root_motion(void) {
    printf("root motion\n");
    /* ---- the step rule, which is all of the timing ---- */
    LxVec3 a = lx_v3(0, 0, 1), b = lx_v3(0, 0, 1.25f), last = lx_v3(0, 0, 0.25f);
    LxVec3 s1 = lx_root_motion_step(a, b, false, last);
    CHECK(NEAR(s1.z, 0.25f, 1e-5f), "a step is where the root went (%g)", s1.z);
    LxVec3 s2 = lx_root_motion_step(lx_v3(0, 0, 4), lx_v3(0, 0, 0), true, last);
    CHECK(NEAR(s2.z, 0.25f, 1e-5f), "at the loop point the jump back is not a step: it keeps going (%g)", s2.z);
    CHECK(NEAR(lx_root_motion_turn(0.1f, 0.3f, false, 0), 0.2f, 1e-5f), "a turn is the angle it turned");
    {
        const float pi = 3.14159265358979f;
        float t = lx_root_motion_turn(pi - 0.1f, -pi + 0.1f, false, 0);
        CHECK(NEAR(t, 0.2f, 1e-4f), "a turn across the wrap goes the short way (%g, not ~-6.1)", t);
    }
    CHECK(NEAR(lx_root_motion_turn(3.0f, 0.0f, true, 0.05f), 0.05f, 1e-5f), "and the loop point keeps turning too");

    /* ---- the twist it is measured with ---- */
    {
        LxQuat yaw = lx_quat_euler_deg(lx_v3(0, 40, 0));
        CHECK(NEAR(lx_quat_twist_y_angle(yaw) * LX_RAD2DEG, 40, 0.01f), "a turn about Y reads back (%g deg)",
              lx_quat_twist_y_angle(yaw) * LX_RAD2DEG);
        LxQuat lean = lx_quat_euler_deg(lx_v3(25, 0, 0));
        CHECK(fabsf(lx_quat_twist_y_angle(lean)) < 0.01f, "leaning forward is not turning (%g)", lx_quat_twist_y_angle(lean));
        LxQuat both = lx_quat_mul(lx_quat_euler_deg(lx_v3(0, 40, 0)), lx_quat_euler_deg(lx_v3(25, 0, 0)));
        CHECK(NEAR(lx_quat_twist_y_angle(both) * LX_RAD2DEG, 40, 0.5f), "a lean on top of a turn still reads the turn (%g deg)",
              lx_quat_twist_y_angle(both) * LX_RAD2DEG);
        /* taking the twist out leaves the lean behind */
        LxQuat tw = lx_quat_twist_y(both);
        LxQuat rest = lx_quat_mul(lx_quat_conj(tw), both);
        CHECK(fabsf(lx_quat_twist_y_angle(rest)) < 0.5f * LX_DEG2RAD, "and taking it out leaves no turn behind");
    }

    /* ---- on a real animator ---- */
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), "luxora_test_rootmotion");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_assets_set_root(dir);
    lx_path_join(path, sizeof path, dir, "BarModel.fbx");
    lx_file_write(path, TEST_FBX, strlen(TEST_FBX));

    /* ---- a settings file written before root motion had settings ----
     * It says nothing about what the root gives away, and what it does not say must not read as
     * "hand the bob and the turning to the object": that lifts a running character off the ground
     * every stride, which is exactly what it did. */
    {
        char imp_path[LX_PATH_MAX];
        lx_path_join(imp_path, sizeof imp_path, dir, "BarModel.fbx.lximport");
        const char *old_format =
            "scale = 1\nimport_animations = 1\nrig = generic\n"
            "clip.0.name = Bend\nclip.0.loop = 1\n";
        lx_file_write(imp_path, old_format, strlen(old_format));
        LxModelImport imp;
        CHECK(lx_model_import_load(imp_path_model(imp_path), &imp), "an old settings file loads");
        CHECK(imp.clip_count == 1 && !strcmp(imp.clips[0].name, "Bend") && imp.clips[0].loop,
              "its name and loop come through (%d clips)", imp.clip_count);
        CHECK(imp.clips[0].up_down == LX_ROOT_IN_POSE && imp.clips[0].turn == LX_ROOT_IN_POSE && imp.clips[0].travel == LX_ROOT_TO_OBJECT,
              "and the root motion it never mentioned keeps its defaults (xz %d, y %d, rot %d)",
              (int)imp.clips[0].travel, (int)imp.clips[0].up_down, (int)imp.clips[0].turn);
        /* and a clip that does say so is believed */
        /* a file of this version is taken at its word; one without a version is not, because the
           build that wrote those fields without ever meaning them left no other way to tell */
        const char *says =
            "version = 2\nclip.0.name = Bend\nclip.0.loop = 1\nclip.0.bake_xz = 1\nclip.0.bake_y = 0\nclip.0.bake_rot = 0\n";
        lx_file_write(imp_path, says, strlen(says));
        CHECK(lx_model_import_load(imp_path_model(imp_path), &imp) && imp.clips[0].travel == LX_ROOT_IN_POSE &&
              imp.clips[0].up_down == LX_ROOT_TO_OBJECT && imp.clips[0].turn == LX_ROOT_TO_OBJECT,
              "a file that does say what the root gives away is taken at its word");
        const char *unversioned =
            "clip.0.name = Bend\nclip.0.loop = 1\nclip.0.bake_xz = 1\nclip.0.bake_y = 0\nclip.0.bake_rot = 0\n";
        lx_file_write(imp_path, unversioned, strlen(unversioned));
        CHECK(lx_model_import_load(imp_path_model(imp_path), &imp) && imp.clips[0].travel == LX_ROOT_TO_OBJECT &&
              imp.clips[0].up_down == LX_ROOT_IN_POSE && imp.clips[0].turn == LX_ROOT_IN_POSE,
              "the same settings without a version go back to the defaults");
        lx_path_remove(imp_path);
    }

    LxScene *s = lx_scene_create("RootMotion");
    LxEntity root = lx_model_instantiate(s, "BarModel.fbx", lx_v3(0, 0, 0), 0);
    LxAnimator *an = lx_get_animator(s, root);
    CHECK(an != NULL, "the model has an Animator");
    if (!an) { lx_scene_destroy(s); return; }
    an->play_on_start = false;
    an->root_motion = true;
    lx_animator_play(s, root, "BarModel.fbx#Bend", 0);
    LxVec3 before = lx_entity_position(s, root);
    for (int i = 0; i < 40; i++) lx_animator_update(s, root, 0.05f);
    LxVec3 after = lx_entity_position(s, root);
    /* the clip bends a bar: it does not travel, so neither does the object - no drift either way */
    CHECK(lx_len3(lx_sub3(after, before)) < 1e-3f, "a clip that does not travel moves nothing (%g %g %g)",
          after.x, after.y, after.z);

    /* the root stays where the model has it, rather than drifting off with the animation */
    LxEntity bar = lx_entity_find(s, "Bar");
    if (bar) {
        LxVec3 p = lx_entity_position(s, bar);
        CHECK(fabsf(p.x) < 1e-3f && fabsf(p.z) < 1e-3f, "the root bone stays over its object (%g %g)", p.x, p.z);
    }

    /* root motion off: still nothing moves, and the pose is still pinned */
    an = lx_get_animator(s, root);
    an->root_motion = false;
    before = lx_entity_position(s, root);
    for (int i = 0; i < 20; i++) lx_animator_update(s, root, 0.05f);
    after = lx_entity_position(s, root);
    CHECK(lx_len3(lx_sub3(after, before)) < 1e-3f, "with root motion off the object stays put");

    lx_scene_destroy(s);
    lx_model_reload("BarModel.fbx");

    /* ---- a clip that walks: 1 m forward each one-second cycle ----
     * Every time it comes round, the object must keep going forwards. Sampling the cycle's end as its
     * start once read each loop as a step of a whole cycle backwards: the character jolted back a metre,
     * then carried on. Checked playing the clip directly and through a graph. */
    {
        const char *mark = "    AnimationCurve: 63,";
        const char *at = strstr(TEST_FBX, mark);
        LxStrBuf sb = {0};
        lx_sb_printf(&sb, "%.*s", (int)(at - TEST_FBX), TEST_FBX);
        lx_sb_printf(&sb, "    AnimationCurveNode: 64, \"AnimCurveNode::T\", \"\" {\n        Properties70:  {\n"
                          "            P: \"d|X\", \"Number\", \"\", \"A\",0\n            P: \"d|Y\", \"Number\", \"\", \"A\",0\n"
                          "            P: \"d|Z\", \"Number\", \"\", \"A\",0\n        }\n    }\n"
                          "    AnimationCurve: 65, \"AnimCurve::\", \"\" {\n        KeyTime: *2 {\n            a: 0,46186158000\n        }\n"
                          "        KeyValueFloat: *2 {\n            a: 0,1\n        }\n    }\n");
        const char *conn = strstr(at, "    C: \"OP\",63,62");
        lx_sb_printf(&sb, "%.*s", (int)(conn - at), at);
        lx_sb_printf(&sb, "    C: \"OO\",64,61\n    C: \"OP\",64,30, \"Lcl Translation\"\n    C: \"OP\",65,64, \"d|Z\"\n%s", conn);
        lx_path_join(path, sizeof path, dir, "Walker.fbx");
        lx_file_write(path, sb.data, strlen(sb.data));
        lx_free(sb.data);
        LxAnimGraph *g = lx_anim_graph_new("Walk");
        lx_strcpy(g->layers[0].states[0].name, LX_ANIM_NAME, "Walk");
        lx_strcpy(g->layers[0].states[0].clip, LX_REF_MAX, "Walker.fbx#Bend");
        g->layers[0].states[0].loop = true;
        lx_strcpy(g->layers[0].start, sizeof g->layers[0].start, "Walk");
        lx_path_join(path, sizeof path, dir, "Walk.lxanim");
        lx_anim_graph_save(g, path);
        lx_anim_graph_free(g);
        for (int use_graph = 0; use_graph < 2; use_graph++) {
            LxScene *ws = lx_scene_create("Walking");
            LxEntity w = lx_model_instantiate(ws, "Walker.fbx", lx_v3(0, 0, 0), 0);
            LxAnimator *wa = lx_get_animator(ws, w);
            CHECK(wa != NULL, "the walker has an Animator");
            if (!wa) { lx_scene_destroy(ws); continue; }
            wa->play_on_start = false;
            wa->root_motion = true;
            if (use_graph) lx_strcpy(wa->graph, sizeof wa->graph, "Walk.lxanim");
            else lx_animator_play(ws, w, "Walker.fbx#Bend", 0);
            float back = 0, z0 = lx_entity_position(ws, w).z, zlast = z0;
            for (int f = 0; f < 70; f++) {   /* 3.5 cycles */
                lx_animator_update(ws, w, 0.05f);
                float z = lx_entity_position(ws, w).z;
                if (z - zlast < back) back = z - zlast;
                zlast = z;
            }
            float went = zlast - z0;
            CHECK(back > -1e-3f && went > 3.0f && went < 3.6f, "%s: a looping walk only goes forwards (worst step %.3f, %.2f m in 3.5 cycles)",
                  use_graph ? "through a graph" : "playing the clip", back, went);
            lx_scene_destroy(ws);
        }
        lx_anim_graphs_clear();
        lx_model_reload("Walker.fbx");

        /* ---- a blend tree must not jump when its parameter does ----
         * Lifted.fbx's clip holds the root 0.5 m higher. A 1D tree between the two, with Speed going
         * 0 -> 1 in one frame (a key pressed), once moved the root the whole 0.5 m in that frame: the hips
         * popping up and down whenever the player started or stopped. */
        {
            LxStrBuf lb = {0};
            const char *lat = strstr(TEST_FBX, mark);
            lx_sb_printf(&lb, "%.*s", (int)(lat - TEST_FBX), TEST_FBX);
            lx_sb_printf(&lb, "    AnimationCurveNode: 64, \"AnimCurveNode::T\", \"\" {\n        Properties70:  {\n"
                              "            P: \"d|X\", \"Number\", \"\", \"A\",0\n            P: \"d|Y\", \"Number\", \"\", \"A\",0.5\n"
                              "            P: \"d|Z\", \"Number\", \"\", \"A\",0\n        }\n    }\n"
                              "    AnimationCurve: 65, \"AnimCurve::\", \"\" {\n        KeyTime: *2 {\n            a: 0,46186158000\n        }\n"
                              "        KeyValueFloat: *2 {\n            a: 0.5,0.5\n        }\n    }\n");
            const char *lconn = strstr(lat, "    C: \"OP\",63,62");
            lx_sb_printf(&lb, "%.*s", (int)(lconn - lat), lat);
            lx_sb_printf(&lb, "    C: \"OO\",64,61\n    C: \"OP\",64,30, \"Lcl Translation\"\n    C: \"OP\",65,64, \"d|Y\"\n%s", lconn);
            lx_path_join(path, sizeof path, dir, "Lifted.fbx");
            lx_file_write(path, lb.data, strlen(lb.data));
            lx_free(lb.data);
            const char *gt = "[animator]\nname = Ease\n[parameter]\nname = Speed\ntype = float\nvalue = 0 0 0\n"
                             "[tree]\nindex = 0\ntype = 1d\nparam_x = Speed\n"
                             "[child]\nclip = BarModel.fbx#Bend\ntree = -1\nthreshold = 0\nspeed = 1\n"
                             "[child]\nclip = Lifted.fbx#Bend\ntree = -1\nthreshold = 1\nspeed = 1\n"
                             "[layer]\nname = Base\nstart = Move\nweight = 1\nblend = override\nmask = 0\n"
                             "[state]\nname = Move\nmotion = tree\ntree = 0\nspeed = 1\nloop = 1\n";
            lx_path_join(path, sizeof path, dir, "Ease.lxanim");
            lx_file_write(path, gt, strlen(gt));
            LxScene *es = lx_scene_create("Ease");
            LxEntity em = lx_model_instantiate(es, "BarModel.fbx", lx_v3(0, 0, 0), 0);
            LxAnimator *ea = lx_get_animator(es, em);
            CHECK(ea != NULL, "the bar has an Animator");
            if (ea) {
                ea->play_on_start = false;
                lx_strcpy(ea->graph, sizeof ea->graph, "Ease.lxanim");
                LxEntity rootb = lx_entity_find(es, "Root");
                float prev = 0, biggest = 0, ylast = 0;
                for (int f = 0; f < 60; f++) {
                    lx_animator_set_float(es, em, "Speed", f < 20 ? 0.0f : 1.0f);
                    lx_animator_update(es, em, 1.0f / 60);
                    float y = lx_entity_world_position(es, rootb).y;
                    if (f > 0) biggest = lx_maxf(biggest, fabsf(y - prev));
                    prev = ylast = y;
                }
                CHECK(biggest < 0.1f && ylast > 0.45f, "a blend tree eases to a new Speed instead of jumping (biggest frame %.3f m, ends at %.3f)", biggest, ylast);
            }
            lx_scene_destroy(es);
            lx_anim_graphs_clear();
            lx_model_reload("BarModel.fbx");
            lx_model_reload("Lifted.fbx");
        }
    }
}

static void test_models(void) {
    printf("models\n");
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), "luxora_test_models");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_assets_set_root(dir);
    lx_path_join(path, sizeof path, dir, "BarModel.fbx");
    lx_file_write(path, TEST_FBX, strlen(TEST_FBX));
    LxModel *m = lx_asset_model("BarModel.fbx");
    CHECK(m != NULL, "ASCII FBX imports (%s)", lx_model_error("BarModel.fbx"));
    if (!m) return;
    CHECK(lx_model_node_count(m) == 3 && lx_model_bone_count(m) == 2 && lx_model_mesh_count(m) == 1, "nodes %d bones %d meshes %d",
          lx_model_node_count(m), lx_model_bone_count(m), lx_model_mesh_count(m));
    CHECK(lx_model_clip_count(m) == 1 && !strcmp(lx_model_clip_name(m, 0), "Bend") && NEAR(lx_model_clip_length(m, 0), 1.0f, 0.01f), "clip 'Bend' (%s)",
          lx_model_clip_name(m, 0));
    char mat[LX_PATH_MAX];
    lx_path_join(mat, sizeof mat, dir, "BarModel Materials/Red Paint.lxmat");
    LxMaterial lm;
    CHECK(lx_material_load(&lm, mat) && NEAR(lm.albedo.x, 1, 1e-4f) && NEAR(lm.albedo.y, 0, 1e-4f), "material written from the FBX");
    LxMesh *mesh = lx_asset_mesh("BarModel.fbx#mesh0");
    CHECK(mesh && mesh->skin && mesh->skin->bone_count == 2 && mesh->lods[0].index_count == 12, "skinned mesh part");

    LxScene *s = lx_scene_create("Models");
    LxEntity root = lx_model_instantiate(s, "BarModel.fbx", lx_v3(0, 0, 0), 0);
    LxEntity tip = lx_entity_find(s, "Tip");
    CHECK(root && tip && lx_get_animator(s, root) && lx_get_mesh(s, lx_entity_find(s, "Bar")), "placed with bones, renderer and animator");
    CHECK(NEAR(lx_entity_world_position(s, tip).y, 1, 1e-5f), "bone rest pose");
    lx_animator_play(s, root, "BarModel.fbx#Bend", 0);
    lx_animator_set_time(s, root, 0.5f);
    lx_animator_update(s, root, 0);
    lx_scene_update_skins(s);
    LxMesh *def = lx_skin_mesh_for(s, lx_entity_find(s, "Bar"));
    bool found = false;
    for (int i = 0; def && i < def->lods[0].vertex_count; i++) {
        LxVertex *v = &def->lods[0].vertices[i];
        if (NEAR(v->px, -0.7778f, 2e-3f) && NEAR(v->py, 1.6364f, 2e-3f)) found = true;
    }
    CHECK(def && found, "skinned vertex follows the animated bone (45 degrees)");
    LxVec3 e = lx_entity_euler(s, tip);
    CHECK(NEAR(e.z, 45, 0.1f), "bone rotated %g", e.z);
    /* a clip by its name alone plays the character's own clip ("Bend", "bend", "#Bend") */
    const char *bare[] = {"Bend", "bend", "#Bend"};
    for (int k = 0; k < 3; k++) {
        lx_animator_reset_pose(s, root);
        lx_animator_play(s, root, bare[k], 0);
        lx_animator_set_time(s, root, 0.5f);
        lx_animator_update(s, root, 0);
        CHECK(lx_animator_playing(s, root) && NEAR(lx_entity_euler(s, tip).z, 45, 0.1f), "clip named \"%s\" plays", bare[k]);
    }
    /* the Play Animation action finds the Animator on the model inside a player object */
    {
        const char *g = "[graph]\nname = Anim\nstart = A\n[state]\nname = A\n[action]\ntype = Play Animation\np0 = Owner\np1 = BarModel.fbx#Bend\np2 = 0\n";
        char gp[LX_PATH_MAX];
        lx_path_join(gp, sizeof gp, dir, "Anim.lxgraph");
        lx_file_write(gp, g, strlen(g));
        LxScene *ps = lx_scene_create("Player");
        LxEntity player = lx_entity_create(ps, "Player");
        LxEntity model = lx_model_instantiate(ps, "BarModel.fbx", lx_v3(0, 0, 0), player);
        lx_get_animator(ps, model)->play_on_start = false;
        lx_add_state_graph(ps, player, "Anim.lxgraph");
        CHECK(lx_find_animator(ps, player) == model, "the player's Animator is found on its model");
        lx_scene_simulate(ps, 0.1f);
        lx_scene_simulate(ps, 0.1f);
        CHECK(lx_animator_playing(ps, model) && !strcmp(lx_animator_current_clip(ps, model), "BarModel.fbx#Bend"),
              "Play Animation on the player object plays its model's clip (%s)", lx_animator_current_clip(ps, model));
        lx_scene_stop_simulation(ps);
        lx_scene_destroy(ps);
    }
    /* scenes remember animators */
    char *text = lx_scene_serialize(s);
    LxScene *s2 = lx_scene_deserialize(text);
    lx_free(text);
    LxEntity r2 = lx_entity_find(s2, "Bar");
    LxEntity top2 = r2 ? lx_entity_parent(s2, r2) : 0;
    CHECK(top2 && lx_get_animator(s2, top2) && !strcmp(lx_get_animator(s2, top2)->model, "BarModel.fbx"), "animator saved with the scene");
    lx_scene_simulate(s2, 0.25f);   /* play on start */
    CHECK(lx_animator_playing(s2, top2) && NEAR(lx_animator_time(s2, top2), 0.25f, 1e-4f), "plays on start");
    lx_scene_stop_simulation(s2);
    lx_scene_destroy(s2);
    lx_scene_destroy(s);

    /* automapping: Unreal names */
    char *ue = unreal_rig_fbx();
    lx_path_join(path, sizeof path, dir, "UE.fbx");
    lx_file_write(path, ue, strlen(ue));
    lx_free(ue);
    LxModel *um = lx_asset_model("UE.fbx");
    char av[LX_HB_COUNT][64];
    int mapped = um ? lx_avatar_automap(um, av) : 0;
    char miss[256];
    CHECK(um && lx_avatar_is_valid(av, miss, sizeof miss), "Unreal skeleton maps to a valid avatar (missing: %s)", miss);
    CHECK(!strcmp(av[LX_HB_HIPS], "pelvis") && !strcmp(av[LX_HB_LEFT_UPPER_ARM], "upperarm_l") && !strcmp(av[LX_HB_RIGHT_LOWER_LEG], "calf_r") &&
          !strcmp(av[LX_HB_CHEST], "spine_02") && !strcmp(av[LX_HB_UPPER_CHEST], "spine_03") && !strcmp(av[LX_HB_LEFT_TOES], "ball_l") &&
          !strcmp(av[LX_HB_LEFT_THUMB_PROXIMAL], "thumb_01_l"), "Unreal names (%d mapped)", mapped);

    /* real files when available (CI downloads Mixamo samples): import, automap, retarget */
    const char *fbx_dir = getenv("LUXORA_TEST_FBX_DIR");
    if (fbx_dir && fbx_dir[0]) {
        lx_assets_set_root(fbx_dir);
        LxModel *samba = lx_asset_model("Samba Dancing.fbx"), *beta = lx_asset_model("mixamo.fbx");
        CHECK(samba && beta, "Mixamo FBX files import");
        if (samba && beta) {
            CHECK(lx_model_bone_count(samba) > 50 && lx_model_clip_count(samba) == 1 && lx_model_mesh_count(samba) == 2, "Samba Dancing contents");
            {   /* at rest, skinning leaves every vertex where the file has it */
                LxScene *ks = lx_scene_create("Skin");
                lx_model_instantiate(ks, "mixamo.fbx", lx_v3(0, 0, 0), 0);
                lx_scene_update_skins(ks);
                double err = 0;
                int cnt = 0;
                for (int i = 0; i < lx_scene_entity_count(ks); i++) {
                    LxEntity x = lx_scene_entity_at(ks, i);
                    LxMesh *sm = lx_skin_mesh_for(ks, x);
                    LxMeshRenderer *mr = lx_get_mesh(ks, x);
                    if (!sm || !mr) continue;
                    LxMesh *src = lx_asset_mesh(mr->mesh);
                    for (int k = 0; k < src->lods[0].vertex_count; k += 13, cnt++) {
                        const LxVertex *p = &sm->lods[0].vertices[k], *q = &src->lods[0].vertices[k];
                        err += fabs(p->px - q->px) + fabs(p->py - q->py) + fabs(p->pz - q->pz);
                    }
                }
                CHECK(cnt > 1000 && err / cnt < 1e-4, "rest pose skins to the bind pose (mean error %g over %d)", cnt ? err / cnt : 0, cnt);
                lx_scene_destroy(ks);
            }
            LxAabb b = lx_model_bounds(samba);
            CHECK(b.max.y > 1.6f && b.max.y < 2.0f, "converted to metres (height %.2f)", b.max.y);
            char a1[LX_HB_COUNT][64], a2[LX_HB_COUNT][64];
            CHECK(lx_avatar_automap(samba, a1) >= 50 && lx_avatar_is_valid(a1, NULL, 0), "Mixamo automap");
            lx_avatar_automap(beta, a2);
            /* make both humanoid, then retarget Samba's dance onto the other character */
            LxModelImport i1 = *lx_model_settings(samba), i2 = *lx_model_settings(beta);
            i1.rig = i2.rig = LX_RIG_HUMANOID;
            memcpy(i1.avatar, a1, sizeof a1);
            memcpy(i2.avatar, a2, sizeof a2);
            char p1[LX_PATH_MAX], p2[LX_PATH_MAX];
            lx_path_join(p1, sizeof p1, fbx_dir, "Samba Dancing.fbx");
            lx_path_join(p2, sizeof p2, fbx_dir, "mixamo.fbx");
            lx_model_import_save(p1, &i1);
            lx_model_import_save(p2, &i2);
            lx_model_reload("Samba Dancing.fbx");
            lx_model_reload("mixamo.fbx");
            LxScene *rs = lx_scene_create("Retarget");
            LxEntity dancer = lx_model_instantiate(rs, "mixamo.fbx", lx_v3(0, 0, 0), 0);
            lx_animator_play(rs, dancer, "Samba Dancing.fbx#0", 0);
            LxEntity hips = lx_entity_find(rs, "mixamorig:Hips");
            LxVec3 h0 = lx_entity_world_position(rs, hips);
            for (int f = 0; f < 60; f++) lx_scene_simulate(rs, 1.0f / 30);
            LxVec3 h1 = lx_entity_world_position(rs, hips);
            LxQuat q = lx_entity_world_rotation(rs, lx_entity_find(rs, "mixamorig:LeftForeArm"));
            CHECK(!isnan(h1.x) && !isnan(q.w) && lx_len3(lx_sub3(h1, h0)) > 0.01f, "retargeted dance moves the hips (%.3f)", lx_len3(lx_sub3(h1, h0)));
            /* foot IK on flat ground must leave the body alone: the dance goes up on its toes, and that is
               the animation, not a dip in the floor - lowering the hips for it bobbed the character */
            {
                LxScene *fs[2];
                LxEntity fr[2], fh[2];
                for (int k = 0; k < 2; k++) {
                    fs[k] = lx_scene_create("FootIK");
                    LxEntity fl = lx_spawn_primitive(fs[k], LX_PRIM_CUBE, "Floor", lx_v3(0, -0.5f, 0));
                    lx_entity_set_scale(fs[k], fl, lx_v3(20, 1, 20));
                    fr[k] = lx_model_instantiate(fs[k], "Samba Dancing.fbx", lx_v3(0, 0, 0), 0);
                    LxAnimator *fa = lx_get_animator(fs[k], fr[k]);
                    fa->play_on_start = false;
                    fa->foot_ik = k == 1;
                    fa->foot_ik_weight = 1;
                    lx_animator_play(fs[k], fr[k], "Samba Dancing.fbx#0", 0);
                    fh[k] = lx_entity_find(fs[k], "mixamorig:Hips");
                }
                float gap = 0;
                for (int f = 0; f < 600; f++) {
                    for (int k = 0; k < 2; k++) lx_animator_update(fs[k], fr[k], 1.0f / 60);
                    gap = lx_maxf(gap, fabsf(lx_entity_world_position(fs[0], fh[0]).y - lx_entity_world_position(fs[1], fh[1]).y));
                }
                CHECK(gap < 0.005f, "foot IK on flat ground doesn't move the hips (%.3f m)", gap);
                for (int k = 0; k < 2; k++) lx_scene_destroy(fs[k]);
            }
            CHECK(h1.y > 0.6f && h1.y < 1.3f, "hips stay at a sensible height (%.2f)", h1.y);
            lx_scene_stop_simulation(rs);
            lx_scene_destroy(rs);
            lx_path_remove(strcat(p1, ".lximport"));
            lx_path_remove(strcat(p2, ".lximport"));
        }
    }
    lx_models_clear();
    lx_assets_set_root("");
    lx_path_remove(dir);
}


static const char TEST_GRAPH[] =
    "[graph]\nname = Door\nstart = Closed\n"
    "[event]\nname = OPEN\n"
    "[event]\nname = RESET\nglobal = 1\n"
    "[variable]\nname = count\ntype = int\nvalue = 0\ninspector = 1\n"
    "[variable]\nname = speed\ntype = vector3\nvalue = 2 0 0\ninspector = 1\n"
    "[global]\nevent = RESET\nto = Closed\n"
    "[state]\nname = Closed\npos = 0 0 0\ntransition = OPEN > Opening\n"
    "[action]\ntype = Int Add\nupdate = once\np0 = $count\np1 = 1\n"
    "[state]\nname = Opening\npos = 200 0 0\ntransition = FINISHED > Open\n"
    "[action]\ntype = Wait\nupdate = every_frame\np0 = 0.5\n"
    "[action]\ntype = Set Position\nupdate = once\np0 = Owner\np1 = 0 3 0\np2 = World\n"
    "[state]\nname = Open\npos = 400 0 0\ntransition = TICK > Open\n"
    "[action]\ntype = Translate\nupdate = every_frame\np0 = Owner\np1 = $speed\np2 = World\np3 = true\n";

static void test_state_graph(void) {
    printf("state graph\n");
    CHECK(lx_fsm_action_count() > 80, "built-in actions registered (%d)", lx_fsm_action_count());
    CHECK(lx_fsm_action_find("Send Event") && lx_fsm_action_find("Move Towards") && lx_fsm_action_find("Raycast"), "action lookup");

    LxGraph *g = lx_graph_from_text(TEST_GRAPH);
    CHECK(g && g->state_count == 3 && g->var_count == 2 && g->global_count == 1, "graph parses");
    if (!g) return;
    char *t1 = lx_graph_to_text(g);
    LxGraph *g2 = lx_graph_from_text(t1);
    char *t2 = g2 ? lx_graph_to_text(g2) : NULL;
    CHECK(t2 && !strcmp(t1, t2), "graph text round-trips");
    lx_free(t1); lx_free(t2);
    lx_graph_free(g2);
    /* awkward names and texts come back exactly */
    lx_graph_rename_state(g, 0, " spaced  > out ");
    lx_strcpy(g->description, sizeof g->description, "  line one\nline two \\ end ");
    lx_strcpy(g->states[1].trans[0].event, sizeof g->states[1].trans[0].event, "A > B ");
    lx_strcpy(g->states[1].trans[0].to, sizeof g->states[1].trans[0].to, " spaced  > out ");
    t1 = lx_graph_to_text(g);
    g2 = lx_graph_from_text(t1);
    CHECK(g2 && !strcmp(g2->states[0].name, " spaced  > out ") && !strcmp(g2->description, "  line one\nline two \\ end ") &&
              !strcmp(g2->states[1].trans[0].event, "A > B ") && !strcmp(g2->states[1].trans[0].to, " spaced  > out ") && !strcmp(g2->start, " spaced  > out "),
          "names with spaces, > and newlines survive saving");
    lx_free(t1);
    lx_graph_free(g2);
    lx_graph_free(g);

    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), "luxora_test_graph");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_assets_set_root(dir);
    lx_path_join(path, sizeof path, dir, "Door.lxgraph");
    lx_file_write(path, TEST_GRAPH, strlen(TEST_GRAPH));

    LxScene *s = lx_scene_create("Graphs");
    LxEntity e = lx_entity_create(s, "Door");
    lx_add_state_graph(s, e, "Door.lxgraph");
    LxStateGraph *sg = lx_get_state_graph(s, e);
    lx_strcpy(sg->slots[0].overrides[0].var, sizeof sg->slots[0].overrides[0].var, "speed");
    lx_strcpy(sg->slots[0].overrides[0].value, sizeof sg->slots[0].overrides[0].value, "4 0 0");
    sg->slots[0].override_count = 1;

    lx_scene_simulate(s, 0.1f);
    CHECK(!strcmp(lx_fsm_current_state(s, e, 0), "Closed"), "starts in the START state (%s)", lx_fsm_current_state(s, e, 0));
    CHECK(NEAR(lx_fsm_get_var_float(s, e, 0, "count"), 1, 1e-5f), "entering ran the action (count %g)", lx_fsm_get_var_float(s, e, 0, "count"));
    lx_fsm_send_event(s, e, "OPEN", 0, 0);
    lx_scene_simulate(s, 0.1f);
    CHECK(!strcmp(lx_fsm_current_state(s, e, 0), "Opening"), "event follows the transition (%s)", lx_fsm_current_state(s, e, 0));
    for (int i = 0; i < 6; i++) lx_scene_simulate(s, 0.1f);
    CHECK(!strcmp(lx_fsm_current_state(s, e, 0), "Open"), "FINISHED once Wait is done (%s)", lx_fsm_current_state(s, e, 0));
    LxVec3 p0 = lx_entity_world_position(s, e);
    CHECK(NEAR(p0.y, 3, 1e-4f), "Set Position ran (y %g)", p0.y);
    lx_scene_simulate(s, 0.5f);
    LxVec3 p1 = lx_entity_world_position(s, e);
    CHECK(NEAR(p1.x - p0.x, 2, 0.05f), "Translate every frame with the overridden variable (moved %g)", p1.x - p0.x);
    lx_fsm_send_event(s, e, "TICK", 0, 0);   /* self transition re-enters */
    lx_scene_simulate(s, 0.1f);
    CHECK(!strcmp(lx_fsm_current_state(s, e, 0), "Open"), "self transition");
    lx_fsm_send_event(s, e, "RESET", 0, 0);
    lx_scene_simulate(s, 0.1f);
    CHECK(!strcmp(lx_fsm_current_state(s, e, 0), "Closed") && NEAR(lx_fsm_get_var_float(s, e, 0, "count"), 2, 1e-5f), "global transition (%s)", lx_fsm_current_state(s, e, 0));
    lx_send_message(s, e, "OPEN", 0, 0);
    lx_scene_simulate(s, 0.1f);
    CHECK(!strcmp(lx_fsm_current_state(s, e, 0), "Opening"), "messages reach graphs");

    /* graph edited while running: reload keeps the state */
    LxGraph *live = lx_asset_graph("Door.lxgraph");
    lx_graph_touch(live);
    lx_scene_simulate(s, 0.1f);
    CHECK(!strcmp(lx_fsm_current_state(s, e, 0), "Opening"), "re-bind keeps the current state");

    /* serialization keeps the slot and overrides */
    char *text = lx_scene_serialize(s);
    lx_scene_stop_simulation(s);
    LxScene *s2 = lx_scene_deserialize(text);
    lx_free(text);
    LxEntity e2 = lx_entity_find(s2, "Door");
    LxStateGraph *sg2 = e2 ? lx_get_state_graph(s2, e2) : NULL;
    CHECK(sg2 && sg2->count == 1 && !strcmp(sg2->slots[0].graph, "Door.lxgraph") && sg2->slots[0].override_count == 1 &&
              !strcmp(sg2->slots[0].overrides[0].value, "4 0 0"), "state graph saved with the scene");
    lx_scene_destroy(s2);

    /* switches, countdowns and repeating events */
    const char *flow = "[graph]\nstart = S\n[variable]\nname = n\ntype = int\nvalue = 2\n[variable]\nname = t\ntype = float\nvalue = 0.5\n"
                       "[variable]\nname = laps\ntype = int\nvalue = 0\n"
                       "[state]\nname = S\ntransition = TWO > C\ntransition = OTHER > Bad\n"
                       "[action]\ntype = Int Switch\np0 = $n\np1 = 0\np2 = ZERO\np3 = 1\np4 = ONE\np5 = 2\np6 = TWO\np7 = 3\np8 = THREE\np9 = OTHER\n"
                       "[state]\nname = C\ntransition = AT_ZERO > R\n[action]\ntype = Countdown Timer\nupdate = every_frame\np0 = $t\np4 = AT_ZERO\n"
                       "[state]\nname = R\ntransition = DONE > End\n[action]\ntype = Int Add\np0 = $laps\np1 = 1\n"
                       "[action]\ntype = Wait\nupdate = every_frame\np0 = 0.3\np1 = DONE\n"
                       "[state]\nname = Bad\n[state]\nname = End\n";
    lx_path_join(path, sizeof path, dir, "Flow.lxgraph");
    lx_file_write(path, flow, strlen(flow));
    LxEntity f = lx_entity_create(s, "Flow");
    lx_add_state_graph(s, f, "Flow.lxgraph");
    lx_scene_simulate(s, 0.05f);
    CHECK(!strcmp(lx_fsm_current_state(s, f, 0), "C"), "Int Switch picks the matching case (%s)", lx_fsm_current_state(s, f, 0));
    for (int i = 0; i < 11; i++) lx_scene_simulate(s, 0.05f);
    CHECK(!strcmp(lx_fsm_current_state(s, f, 0), "R") && NEAR(lx_fsm_get_var_float(s, f, 0, "laps"), 1, 1e-4f), "Countdown reaches zero; Counter counts (%s)",
          lx_fsm_current_state(s, f, 0));
    for (int i = 0; i < 8; i++) lx_scene_simulate(s, 0.05f);
    CHECK(!strcmp(lx_fsm_current_state(s, f, 0), "End"), "the Wait in R finishes and sends DONE (%s)", lx_fsm_current_state(s, f, 0));
    /* a name collision between two built-in actions would silently drop one, so the count would fall */
    CHECK(lx_fsm_action_count() >= 500, "the whole built-in action library is registered (%d)", lx_fsm_action_count());

    /* a runaway loop is stopped, not hung */
    const char *loop = "[graph]\nstart = A\n[state]\nname = A\ntransition = FINISHED > B\n[state]\nname = B\ntransition = FINISHED > A\n";
    lx_path_join(path, sizeof path, dir, "Loop.lxgraph");
    lx_file_write(path, loop, strlen(loop));
    LxEntity l = lx_entity_create(s, "Loop");
    lx_add_state_graph(s, l, "Loop.lxgraph");
    lx_scene_simulate(s, 0.1f);
    CHECK(lx_fsm_current_state(s, l, 0)[0] != 0 && lx_fsm_run_of(s, l, 0)->error, "infinite FINISHED loop is caught");
    lx_scene_stop_simulation(s);
    lx_scene_destroy(s);
    lx_assets_clear();

    /* the sample project's Patroller walks from A to B, waits, and counts laps */
    char pdir[LX_PATH_MAX];
    lx_path_join(pdir, sizeof pdir, tmpdir(), "luxora_test_sgproj");
    lx_path_remove(pdir);
    LxProject proj;
    CHECK(lx_project_create(&proj, pdir, "Demo", LX_TEMPLATE_SAMPLE_3D), "sample project with a State Graph");
    lx_assets_set_root(proj.assets);
    char scene_path[LX_PATH_MAX];
    lx_path_join(scene_path, sizeof scene_path, proj.assets, "Scenes/Main.lxscene");
    LxScene *ps = lx_scene_load(scene_path);
    LxEntity pat = ps ? lx_entity_find(ps, "Patroller") : 0;
    CHECK(pat && lx_get_state_graph(ps, pat), "Patroller has its graph");
    if (pat) {
        LxGraph *pg = lx_asset_graph("Graphs/Patroller.lxgraph");
        char probs[8][160];
        CHECK(pg && lx_graph_problems(pg, probs, 8) == 0, "sample graph has no problems (%s)", pg && lx_graph_problems(pg, probs, 8) ? probs[0] : "");
        for (int i = 0; i < 66; i++) lx_scene_simulate(ps, 1.0f / 60);   /* waits a second at A */
        CHECK(!strcmp(lx_fsm_current_state(ps, pat, 0), "Go to B"), "after waiting it walks to B (%s)", lx_fsm_current_state(ps, pat, 0));
        for (int i = 0; i < 60 * 3 + 12; i++) lx_scene_simulate(ps, 1.0f / 60);   /* 6 m at 2 m/s */
        LxVec3 at = lx_entity_world_position(ps, pat);
        CHECK(NEAR(at.x, 3, 0.1f) && !strcmp(lx_fsm_current_state(ps, pat, 0), "Wait at B"), "arrives at B (x %.2f, %s)", at.x, lx_fsm_current_state(ps, pat, 0));
        CHECK(NEAR(lx_fsm_get_var_float(ps, pat, 0, "Laps"), 1, 1e-4f), "counts the lap");
        lx_fsm_send_event(ps, pat, "HOP", 0, 0);
        lx_scene_simulate(ps, 1.0f / 60);
        CHECK(!strcmp(lx_fsm_current_state(ps, pat, 0), "Hop"), "HOP is global: works from Wait at B");
        for (int i = 0; i < 60; i++) lx_scene_simulate(ps, 1.0f / 60);
        at = lx_entity_world_position(ps, pat);
        CHECK(!strcmp(lx_fsm_current_state(ps, pat, 0), "Go to A") && NEAR(at.y, 0.4f, 0.05f), "hops up and lands (y %.2f, %s)", at.y, lx_fsm_current_state(ps, pat, 0));
        lx_scene_stop_simulation(ps);
    }
    if (ps) lx_scene_destroy(ps);
    lx_assets_clear();
}

#include "test_fsm.c"

/* LUXORA_TEST=<part of a name> runs only the tests whose names contain it */
#define RUN(fn) do { const char *only_ = getenv("LUXORA_TEST"); if (!only_ || strstr(#fn, only_)) fn(); } while (0)
int main(void) {
    lx_jobs_init(0);
    lx_register_builtin_behaviours();
    RUN(test_math);
    RUN(test_jobs);
    RUN(test_scene);
    RUN(test_meshes);
    RUN(test_png);
    RUN(test_png_formats);
    {
        printf("material maps\n");
        LxMaterial m = lx_material_default(), r;
        lx_strcpy(m.height_map, sizeof m.height_map, "Textures/H.png");
        lx_strcpy(m.ao_map, sizeof m.ao_map, "Textures/AO.png");
        lx_strcpy(m.roughness_map, sizeof m.roughness_map, "Textures/R.png");
        lx_strcpy(m.metallic_map, sizeof m.metallic_map, "Textures/M.png");
        m.normal_strength = 1.5f; m.normal_flip_green = true; m.roughness_is_smoothness = true;
        m.height_scale = 0.07f; m.parallax_shadows = false; m.ao_strength = 0.5f;
        char path[LX_PATH_MAX];
        lx_path_join(path, sizeof path, tmpdir(), "luxora_mat_test.lxmat");
        CHECK(lx_material_save(&m, path) && lx_material_load(&r, path), "material saves");
        CHECK(!strcmp(r.height_map, m.height_map) && !strcmp(r.ao_map, m.ao_map) && !strcmp(r.roughness_map, m.roughness_map) &&
              !strcmp(r.metallic_map, m.metallic_map), "map references survive");
        CHECK(NEAR(r.normal_strength, 1.5f, 1e-5f) && r.normal_flip_green && r.roughness_is_smoothness && NEAR(r.height_scale, 0.07f, 1e-5f) &&
              !r.parallax_shadows && NEAR(r.ao_strength, 0.5f, 1e-5f), "map settings survive");
        lx_path_remove(path);
    }
    RUN(test_bvh);
    RUN(test_gi);
    RUN(test_gi_convergence);
    RUN(test_gi_no_dark_sweep);
    RUN(test_gi_terrain_ground);
    RUN(test_gi_leaks);
    RUN(test_static_safeguard);
    RUN(test_gi_moving);
    RUN(test_gi_self_shadow);
    RUN(test_moving_refit);
    RUN(test_gi_reconstruction);
    RUN(test_gi_headroom);
    RUN(test_lightmap_uvs);
    RUN(test_lightmapper);
    RUN(test_sky);
    RUN(test_terrain);
    RUN(test_bake_volumes);
    RUN(test_gi_sky_calibration);
    RUN(test_acoustics);
    RUN(test_crowd);
    RUN(test_project);
    RUN(test_wav);
    RUN(test_input);
    RUN(test_reflection_probe);
    RUN(test_icon_images);
    RUN(test_layers);
    RUN(test_physics);
    RUN(test_character_no_sinking);
    RUN(test_character_slopes);
    RUN(test_game_ui);
    RUN(test_gameplay);
    RUN(test_jpeg);
    RUN(test_pose_blend);
    RUN(test_blend_trees);
    RUN(test_ik);
    RUN(test_anim_graph_asset);
    RUN(test_animator_runtime);
    RUN(test_anim_exit_time);
    RUN(test_root_motion);
    RUN(test_models);
    RUN(test_state_graph);
    RUN(test_fsm_fuzz);
    RUN(test_fsm_semantics);
    RUN(test_fsm_nesting);
    RUN(test_fsm_action_library);
    RUN(test_templates);
    RUN(test_axis_into_character_move);
#if LUXORA_SCRIPT_COMPILER
    RUN(test_scripts);
#endif
    lx_assets_clear();
    lx_jobs_shutdown();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
