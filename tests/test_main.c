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
    lx_game_ui_shutdown();
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
                       "[state]\nname = C\ntransition = AT_ZERO > R\n[action]\ntype = Countdown\nupdate = every_frame\np0 = $t\np1 = AT_ZERO\n"
                       "[state]\nname = R\ntransition = DONE > End\n[action]\ntype = Counter\np0 = $laps\np1 = 5\n"
                       "[action]\ntype = Repeat Event\nupdate = every_frame\np0 = 0.1\np1 = TICK\np2 = 3\np3 = DONE\n"
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
    CHECK(!strcmp(lx_fsm_current_state(s, f, 0), "End"), "Repeat Event stops after 3 and sends DONE (%s)", lx_fsm_current_state(s, f, 0));
    CHECK(lx_fsm_action_count() >= 150, "about 150 built-in actions (%d)", lx_fsm_action_count());

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

int main(void) {
    lx_jobs_init(0);
    lx_register_builtin_behaviours();
    test_math();
    test_jobs();
    test_scene();
    test_meshes();
    test_png();
    test_png_formats();
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
    test_bvh();
    test_gi();
    test_gi_convergence();
    test_acoustics();
    test_crowd();
    test_project();
    test_wav();
    test_input();
    test_reflection_probe();
    test_icon_images();
    test_physics();
    test_game_ui();
    test_gameplay();
    test_jpeg();
    test_models();
    test_state_graph();
    test_fsm_fuzz();
    test_fsm_semantics();
    test_templates();
    test_axis_into_character_move();
#if LUXORA_SCRIPT_COMPILER
    test_scripts();
#endif
    lx_assets_clear();
    lx_jobs_shutdown();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
