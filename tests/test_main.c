/* Luxora headless test suite: runs without a GPU or window. */
#include <stdlib.h>
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
    "}\n";

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
    CHECK(st.registrations == 2, "two registrations found (%d)", st.registrations);
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
    test_reflection_probe();
    test_icon_images();
    test_physics();
    test_game_ui();
#if LUXORA_SCRIPT_COMPILER
    test_scripts();
#endif
    lx_assets_clear();
    lx_jobs_shutdown();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
