/* Game templates for new projects: a First Person Shooter and a Third Person Action game, written in C
 * (Assets/Scripts) so every part of them can be read and changed. The scripts live in resources/templates and
 * are embedded into the engine by tools/embed_templates.cmake.
 *
 * Both share: the Game Manager (HUD, pause menu, winning and losing), Enemy AI (wander, spot, chase, melee or
 * ranged attacks, hit reactions), pickups (walk-over or Interact), doors, and game.h, the state they share. */
#include "../lx_internal.h"
#include <stdio.h>

typedef struct LxTemplateFile { const char *path; const unsigned char *data; unsigned size; } LxTemplateFile;
extern const LxTemplateFile lx_template_files[];
extern const int lx_template_file_count;

static bool put(const char *assets, const char *rel, const char *text) {
    char path[LX_PATH_MAX], dir[LX_PATH_MAX];
    lx_path_join(path, sizeof path, assets, rel);
    lx_path_parent(dir, sizeof dir, path);
    lx_mkdir_p(dir);
    return lx_file_write(path, text, strlen(text));
}

/* copy one template folder ("common", "fps", "tps") into the project's Assets */
static bool copy_template(const char *assets, const char *folder) {
    size_t n = strlen(folder);
    bool ok = true;
    for (int i = 0; i < lx_template_file_count; i++) {
        const LxTemplateFile *f = &lx_template_files[i];
        if (strncmp(f->path, folder, n) || f->path[n] != '/') continue;
        char path[LX_PATH_MAX], dir[LX_PATH_MAX];
        lx_path_join(path, sizeof path, assets, f->path + n + 1);
        lx_path_parent(dir, sizeof dir, path);
        lx_mkdir_p(dir);
        ok &= lx_file_write(path, f->data, f->size);
    }
    return ok;
}

/* global variables: the Game Manager keeps them up to date, so State Graphs can read the game's state */
static const char GLOBALS[] =
    "# Global variables: the Game Manager script copies the game's state here every frame, so State Graphs can use it\n"
    "[variable]\nname = Health\ntype = float\nvalue = 100\n"
    "[variable]\nname = Max Health\ntype = float\nvalue = 100\n"
    "[variable]\nname = Score\ntype = int\nvalue = 0\n"
    "[variable]\nname = Ammo\ntype = int\nvalue = 0\n"
    "[variable]\nname = Reserve Ammo\ntype = int\nvalue = 0\n"
    "[variable]\nname = Coins\ntype = int\nvalue = 0\n"
    "[variable]\nname = Game Phase\ntype = int\nvalue = 0\ntooltip = 0 playing, 1 paused, 2 won, 3 lost\n";

/* a behaviour with its parameters (the scripts aren't compiled yet when the project is made, so their
 * defaults aren't known here: every value is written out) */
static void behave(LxScene *s, LxEntity e, const char *name, const float *params, int n) {
    LxBehaviourRef *b = lx_add_behaviour(s, e, name);
    for (int i = 0; i < n && i < LX_BEHAVIOUR_PARAMS; i++) b->params[i] = params[i];
}

/* ======================================================================
 *  materials, prefabs, levels
 * ====================================================================== */
static void material(const char *assets, const char *name, LxVec3 albedo, float metallic, float roughness, LxVec3 emissive, float glow) {
    LxMaterial m = lx_material_default();
    lx_strcpy(m.name, sizeof m.name, name);
    m.albedo = lx_v4(albedo.x, albedo.y, albedo.z, 1);
    m.metallic = metallic;
    m.roughness = roughness;
    m.emissive = emissive;
    m.emissive_strength = glow;
    char rel[LX_REF_MAX], path[LX_PATH_MAX];
    snprintf(rel, sizeof rel, "Materials/%s.lxmat", name);
    lx_path_join(path, sizeof path, assets, rel);
    lx_material_save(&m, path);
}

static LxEntity prim(LxScene *s, LxPrimitive p, const char *name, LxVec3 pos, LxVec3 scale, const char *mat, bool collider) {
    LxEntity e = lx_spawn_primitive(s, p, name, pos);
    lx_entity_set_scale(s, e, scale);
    LxMeshRenderer *m = lx_get_mesh(s, e);
    if (m && mat) lx_strcpy(m->material, LX_REF_MAX, mat);
    if (!collider) lx_remove_component(s, e, LX_COMP_COLLIDER);
    return e;
}

/* the level both games play in: a walled yard with cover, a raised walkway and ramps */
static void arena(LxScene *s, bool tps) {
    LxEnvironment *env = lx_scene_environment(s);
    env->sky_zenith = lx_v3(0.20f, 0.30f, 0.52f);
    env->sky_horizon = lx_v3(0.78f, 0.62f, 0.50f);
    env->fog_color = lx_v3(0.62f, 0.55f, 0.52f);
    env->fog_density = 0.008f;
    LxEntity sun = lx_entity_find(s, "Directional Light");
    if (sun) {
        lx_entity_set_euler(s, sun, lx_v3(-38, -140, 0));
        LxLight *l = lx_get_light(s, sun);
        l->color = lx_v3(1.0f, 0.88f, 0.74f);
        l->intensity = 3.2f;
    }
    LxEntity cube = lx_entity_find(s, "Cube");
    if (cube) lx_entity_destroy(s, cube);
    LxEntity ground = prim(s, LX_PRIM_PLANE, "Ground", lx_v3(0, 0, 0), lx_v3(5, 1, 5), "Materials/Floor.lxmat", true);
    lx_entity_set_tag(s, ground, "Ground");
    const float half = 24;
    prim(s, LX_PRIM_CUBE, "Wall North", lx_v3(0, 2, -half), lx_v3(2 * half, 4, 1), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Wall South", lx_v3(0, 2, half), lx_v3(2 * half, 4, 1), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Wall West", lx_v3(-half, 2, 0), lx_v3(1, 4, 2 * half), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Wall East", lx_v3(half, 2, 0), lx_v3(1, 4, 2 * half), "Materials/Wall.lxmat", true);
    /* cover: crates and pillars */
    static const float crates[][4] = {{-6, -4, 1.4f, 20}, {-5, -2.2f, 1.0f, -10}, {7, 5, 1.6f, 35}, {8.5f, 3.4f, 1.0f, 5}, {-12, 9, 1.5f, 0},
                                      {3, -12, 1.4f, 15}, {14, -9, 1.2f, -25}, {-15, -14, 1.6f, 40}, {0, 10, 1.2f, 10}};
    for (size_t i = 0; i < sizeof crates / sizeof crates[0]; i++) {
        char n[32];
        snprintf(n, sizeof n, "Crate %d", (int)i + 1);
        float sz = crates[i][2];
        LxEntity c = prim(s, LX_PRIM_CUBE, n, lx_v3(crates[i][0], sz * 0.5f, crates[i][1]), lx_v3(sz, sz, sz), "Materials/Crate.lxmat", true);
        lx_entity_set_euler(s, c, lx_v3(0, -crates[i][3], 0));
    }
    static const float pillars[][2] = {{-9, -9}, {9, -9}, {-9, 9}, {9, 9}};
    for (int i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof n, "Pillar %d", i + 1);
        prim(s, LX_PRIM_CYLINDER, n, lx_v3(pillars[i][0], 2.5f, pillars[i][1]), lx_v3(1.2f, 5, 1.2f), "Materials/Wall.lxmat", true);
    }
    /* a walkway along the north wall with a ramp up to it, and steps on the east side */
    prim(s, LX_PRIM_CUBE, "Walkway", lx_v3(-6, 1.4f, -20.5f), lx_v3(20, 0.4f, 5), "Materials/Metal.lxmat", true);
    LxEntity ramp = prim(s, LX_PRIM_CUBE, "Ramp", lx_v3(7.2f, 0.72f, -20.5f), lx_v3(7, 0.3f, 4), "Materials/Metal.lxmat", true);
    lx_entity_set_euler(s, ramp, lx_v3(0, 0, -12));
    for (int i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof n, "Step %d", i + 1);
        float h = 0.35f * (float)(i + 1);
        prim(s, LX_PRIM_CUBE, n, lx_v3(19, h * 0.5f, 6.0f - (float)i * 2.2f), lx_v3(4, h, 2.2f), "Materials/Crate.lxmat", true);
    }
    if (tps) {   /* platforms to jump across for coins */
        static const float plats[][4] = {{-14, 0, 1.0f, 3}, {-18, -4, 1.8f, 3}, {-14, -9, 2.6f, 3}, {-8, -13, 3.2f, 3.4f}, {12, 14, 1.2f, 3}, {16, 18, 2.2f, 3}};
        for (size_t i = 0; i < sizeof plats / sizeof plats[0]; i++) {
            char n[32];
            snprintf(n, sizeof n, "Platform %d", (int)i + 1);
            prim(s, LX_PRIM_CUBE, n, lx_v3(plats[i][0], plats[i][2] - 0.2f, plats[i][1]), lx_v3(plats[i][3], 0.4f, plats[i][3]), "Materials/Metal.lxmat", true);
        }
    }
    /* warm lamps */
    static const float lamps[][3] = {{-9, 3.5f, -9}, {9, 3.5f, 9}, {-12, 3.0f, 12}, {12, 3.0f, -12}};
    for (int i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof n, "Lamp %d", i + 1);
        LxEntity l = lx_entity_create(s, n);
        lx_entity_set_position(s, l, lx_v3(lamps[i][0], lamps[i][1], lamps[i][2]));
        LxLight *li = lx_add_light(s, l, LX_LIGHT_POINT);
        li->color = lx_v3(1.0f, 0.62f, 0.32f);
        li->intensity = 6;
        li->range = 12;
    }
    LxEntity probe = lx_entity_create(s, "Reflection Probe");
    lx_entity_set_position(s, probe, lx_v3(0, 3, 0));
    lx_add_reflection_probe(s, probe)->size = lx_v3(2 * half, 8, 2 * half);
}

/* a small storeroom in the south-west corner behind a sliding door, with something worth fetching inside */
static void storeroom(LxScene *s) {
    prim(s, LX_PRIM_CUBE, "Storeroom Wall", lx_v3(-16, 1.6f, 20), lx_v3(0.6f, 3.2f, 8), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Storeroom Front L", lx_v3(-22.75f, 1.6f, 16), lx_v3(2.5f, 3.2f, 0.6f), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Storeroom Front R", lx_v3(-17.25f, 1.6f, 16), lx_v3(2.5f, 3.2f, 0.6f), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Storeroom Lintel", lx_v3(-20, 2.9f, 16), lx_v3(3, 0.6f, 0.6f), "Materials/Wall.lxmat", true);
    prim(s, LX_PRIM_CUBE, "Storeroom Roof", lx_v3(-20, 3.35f, 20), lx_v3(8.6f, 0.3f, 8.6f), "Materials/Wall.lxmat", true);
    LxEntity door = prim(s, LX_PRIM_CUBE, "Door", lx_v3(-20, 1.3f, 16), lx_v3(3, 2.6f, 0.25f), "Materials/Door.lxmat", true);
    lx_entity_set_tag(s, door, "Interactable");
    const float dp[] = {2.8f, 0.7f, 6};
    behave(s, door, "Door", dp, 3);
    LxEntity lamp = lx_entity_create(s, "Storeroom Lamp");
    lx_entity_set_position(s, lamp, lx_v3(-20, 2.7f, 20));
    LxLight *l = lx_add_light(s, lamp, LX_LIGHT_POINT);
    l->color = lx_v3(0.6f, 0.8f, 1.0f);
    l->intensity = 4;
    l->range = 8;
}

static LxEntity enemy(LxScene *s, const char *name, LxVec3 at, bool ranged) {
    LxEntity e = lx_entity_create(s, name);
    lx_entity_set_position(s, e, at);
    lx_entity_set_tag(s, e, "Enemy");
    LxCharacterController *cc = lx_add_character(s, e);
    cc->radius = 0.45f;
    cc->height = 1.8f;
    LxCollider *col = lx_add_collider(s, e, LX_COLLIDER_CAPSULE);   /* what bullets and swords hit */
    col->center = lx_v3(0, 0.9f, 0);
    col->radius = 0.45f;
    col->height = 1.8f;
    LxEntity body = prim(s, LX_PRIM_CAPSULE, "Body", lx_v3(0, 0.9f, 0), lx_v3(0.9f, 0.9f, 0.9f), ranged ? "Materials/Enemy Ranged.lxmat" : "Materials/Enemy.lxmat", false);
    lx_entity_set_parent(s, body, e);
    LxEntity eye = prim(s, LX_PRIM_SPHERE, "Eye", lx_v3(0, 0.45f, 0.42f), lx_v3(0.35f, 0.18f, 0.12f), "Materials/Glow Red.lxmat", false);
    lx_entity_set_parent(s, eye, body);
    /* Health, Speed, Sight Range, Attack Reach, Attack Damage, Score, Ranged, Seconds Between Attacks */
    const float melee[] = {60, 3.2f, 14, 1.7f, 10, 100, 0, 1.1f};
    const float shooter[] = {45, 2.6f, 18, 1.7f, 7, 150, 1, 1.6f};
    behave(s, e, "Enemy", ranged ? shooter : melee, 8);
    return e;
}

/* Kind (0 health, 1 ammo, 2 coin), Amount, Walk Over, Radius */
static LxEntity pickup(LxScene *s, const char *name, int kind, float amount, const char *mat, LxVec3 at, LxPrimitive shape, LxVec3 scale) {
    LxEntity e = prim(s, shape, name, at, scale, mat, false);
    bool walk_over = kind == 2;
    lx_entity_set_tag(s, e, walk_over ? "Coin" : "Interactable");
    const float p[] = {(float)kind, amount, walk_over ? 1.0f : 0.0f, 1.2f};
    behave(s, e, "Pickup", p, 4);
    return e;
}

static void manager(LxScene *s, bool coins) {
    LxEntity gm = lx_entity_create(s, "Game Manager");
    /* Goal, Max Health, Clip Size, Start Reserve Ammo */
    const float p[] = {coins ? 1.0f : 0.0f, 100, 24, 72};
    behave(s, gm, "Game Manager", p, 4);
}

static void common_assets(const char *assets) {
    material(assets, "Floor", lx_v3(0.36f, 0.36f, 0.34f), 0, 0.55f, lx_v3(0, 0, 0), 0);
    material(assets, "Wall", lx_v3(0.62f, 0.56f, 0.50f), 0, 0.8f, lx_v3(0, 0, 0), 0);
    material(assets, "Crate", lx_v3(0.55f, 0.36f, 0.20f), 0, 0.7f, lx_v3(0, 0, 0), 0);
    material(assets, "Metal", lx_v3(0.55f, 0.57f, 0.60f), 1, 0.3f, lx_v3(0, 0, 0), 0);
    material(assets, "Door", lx_v3(0.25f, 0.32f, 0.38f), 0.7f, 0.35f, lx_v3(0.1f, 0.35f, 0.6f), 0.4f);
    material(assets, "Enemy", lx_v3(0.30f, 0.10f, 0.12f), 0.2f, 0.45f, lx_v3(0, 0, 0), 0);
    material(assets, "Enemy Ranged", lx_v3(0.12f, 0.12f, 0.28f), 0.3f, 0.4f, lx_v3(0, 0, 0), 0);
    material(assets, "Glow Red", lx_v3(1, 0.2f, 0.1f), 0, 0.4f, lx_v3(1, 0.15f, 0.05f), 6);
    material(assets, "Health", lx_v3(0.2f, 0.9f, 0.3f), 0, 0.3f, lx_v3(0.2f, 1, 0.3f), 2.5f);
    material(assets, "Player", lx_v3(0.18f, 0.32f, 0.62f), 0.1f, 0.4f, lx_v3(0, 0, 0), 0);
    put(assets, "Globals.lxglobals", GLOBALS);
    copy_template(assets, "common");
}

/* ---------------- First Person Shooter ---------------- */
static bool fps_template(const char *assets, LxScene *s) {
    common_assets(assets);
    copy_template(assets, "fps");
    material(assets, "Ammo", lx_v3(0.95f, 0.75f, 0.2f), 0.6f, 0.35f, lx_v3(1, 0.7f, 0.15f), 1.5f);
    material(assets, "Gun", lx_v3(0.08f, 0.08f, 0.09f), 0.9f, 0.35f, lx_v3(0, 0, 0), 0);
    material(assets, "Spark", lx_v3(1, 0.8f, 0.4f), 0, 0.5f, lx_v3(1, 0.7f, 0.3f), 8);
    {   /* the bullet impact prefab: a spark that shrinks away (Scripts/impact.c) */
        LxScene *ps = lx_scene_create("Impact");
        LxEntity sp = prim(ps, LX_PRIM_SPHERE, "Impact", lx_v3(0, 0, 0), lx_v3(0.16f, 0.16f, 0.16f), "Materials/Spark.lxmat", false);
        lx_add_behaviour(ps, sp, "Impact");
        char pp[LX_PATH_MAX];
        lx_path_join(pp, sizeof pp, assets, "Prefabs/Impact.lxprefab");
        lx_prefab_save(ps, sp, pp);
        lx_scene_destroy(ps);
    }
    arena(s, false);
    storeroom(s);
    /* the player: a Character Controller with the camera at eye height and a gun in view */
    LxEntity cam = lx_entity_find(s, "Main Camera");
    LxEntity p = lx_entity_create(s, "Player");
    lx_entity_set_position(s, p, lx_v3(0, 0, 16));
    lx_entity_set_euler(s, p, lx_v3(0, -180, 0));   /* facing into the yard (-Z) */
    lx_entity_set_tag(s, p, "Player");
    LxCharacterController *cc = lx_add_character(s, p);
    cc->radius = 0.4f;
    cc->height = 1.8f;
    /* Walk, Sprint, Crouch, Jump Height, Look Sensitivity, Gravity, Head Bob, Field Of View */
    const float pp[] = {4.6f, 7.4f, 2.2f, 1.2f, 1, 22, 1, 72};
    behave(s, p, "FPS Player", pp, 8);
    if (cam) {
        lx_entity_set_parent(s, cam, p);
        lx_entity_set_position(s, cam, lx_v3(0, 1.62f, 0));
        lx_entity_set_euler(s, cam, lx_v3(0, 0, 0));
        lx_get_camera(s, cam)->fov_deg = 72;
        lx_get_camera(s, cam)->near_clip = 0.05f;   /* the gun is close */
        LxEntity gun = prim(s, LX_PRIM_CUBE, "Gun", lx_v3(-0.24f, -0.22f, 0.46f), lx_v3(0.08f, 0.11f, 0.5f), "Materials/Gun.lxmat", false);
        lx_entity_set_parent(s, gun, cam);
        lx_entity_set_tag(s, gun, "Weapon");
        /* Damage, Shots Per Second, Reload Seconds, Range, Spread, Recoil, Automatic */
        const float wp[] = {25, 8, 1.3f, 120, 0.9f, 1.4f, 1};
        behave(s, gun, "FPS Weapon", wp, 7);
        LxEntity barrel = prim(s, LX_PRIM_CYLINDER, "Barrel", lx_v3(0, 0.02f, 0.62f), lx_v3(0.5f, 0.35f, 0.5f), "Materials/Metal.lxmat", false);
        lx_entity_set_euler(s, barrel, lx_v3(-90, 0, 0));
        lx_entity_set_parent(s, barrel, gun);
        LxEntity sight = prim(s, LX_PRIM_CUBE, "Sight", lx_v3(0, 0.62f, 0.1f), lx_v3(0.3f, 0.25f, 0.15f), "Materials/Metal.lxmat", false);
        lx_entity_set_parent(s, sight, gun);
        LxEntity mag = prim(s, LX_PRIM_CUBE, "Magazine", lx_v3(0, -0.75f, -0.1f), lx_v3(0.7f, 0.8f, 0.2f), "Materials/Gun.lxmat", false);
        lx_entity_set_parent(s, mag, gun);
        LxEntity ml = lx_entity_create(s, "Muzzle Light");
        lx_entity_set_parent(s, ml, cam);
        lx_entity_set_position(s, ml, lx_v3(-0.24f, -0.16f, 1.0f));
        LxLight *l = lx_add_light(s, ml, LX_LIGHT_POINT);
        l->color = lx_v3(1, 0.7f, 0.35f);
        l->intensity = 0;
        l->range = 6;
        l->cast_shadows = false;
        l->contribute_gi = false;   /* a flash for a frame: not worth re-solving the bounce light for */
    }
    static const float en[][3] = {{-8, -6, 0}, {6, -8, 1}, {12, 2, 0}, {-14, 4, 1}, {0, -16, 0}, {16, -16, 1}};
    for (int i = 0; i < 6; i++) {
        char n[32];
        snprintf(n, sizeof n, "Enemy %d", i + 1);
        enemy(s, n, lx_v3(en[i][0], 0, en[i][1]), en[i][2] > 0);
    }
    pickup(s, "Health Pack 1", 0, 35, "Materials/Health.lxmat", lx_v3(-18, 0.6f, -18), LX_PRIM_CUBE, lx_v3(0.5f, 0.5f, 0.5f));
    pickup(s, "Health Pack 2", 0, 35, "Materials/Health.lxmat", lx_v3(-21.5f, 0.6f, 21.5f), LX_PRIM_CUBE, lx_v3(0.5f, 0.5f, 0.5f));
    pickup(s, "Ammo Box 1", 1, 48, "Materials/Ammo.lxmat", lx_v3(-6, 2.2f, -20.5f), LX_PRIM_CUBE, lx_v3(0.6f, 0.35f, 0.4f));
    pickup(s, "Ammo Box 2", 1, 48, "Materials/Ammo.lxmat", lx_v3(-18.5f, 0.6f, 21.5f), LX_PRIM_CUBE, lx_v3(0.6f, 0.35f, 0.4f));
    pickup(s, "Ammo Box 3", 1, 48, "Materials/Ammo.lxmat", lx_v3(19, 1.9f, -0.6f), LX_PRIM_CUBE, lx_v3(0.6f, 0.35f, 0.4f));
    manager(s, false);
    return true;
}

/* ---------------- Third Person Action ---------------- */
static bool tps_template(const char *assets, LxScene *s) {
    common_assets(assets);
    copy_template(assets, "tps");
    material(assets, "Gold", lx_v3(1.0f, 0.78f, 0.3f), 1, 0.25f, lx_v3(0.6f, 0.4f, 0.05f), 0.8f);
    material(assets, "Blade", lx_v3(0.85f, 0.88f, 0.92f), 1, 0.15f, lx_v3(0, 0, 0), 0);
    arena(s, true);
    storeroom(s);
    LxEntity p = lx_entity_create(s, "Player");
    lx_entity_set_position(s, p, lx_v3(0, 0, 14));
    lx_entity_set_euler(s, p, lx_v3(0, -180, 0));   /* facing into the yard (-Z) */
    lx_entity_set_tag(s, p, "Player");
    LxCharacterController *cc = lx_add_character(s, p);
    cc->radius = 0.4f;
    cc->height = 1.8f;
    /* Run, Sprint, Jump Height, Turn Speed, Sword Damage, Sword Reach, Dodge Distance, Gravity */
    const float pp[] = {5.2f, 8.5f, 1.7f, 12, 24, 2.2f, 4, 24};
    behave(s, p, "TPS Player", pp, 8);
    LxEntity body = prim(s, LX_PRIM_CAPSULE, "Body", lx_v3(0, 0.9f, 0), lx_v3(0.8f, 0.9f, 0.8f), "Materials/Player.lxmat", false);
    lx_entity_set_parent(s, body, p);
    LxEntity visor = prim(s, LX_PRIM_CUBE, "Visor", lx_v3(0, 0.42f, 0.36f), lx_v3(0.55f, 0.16f, 0.2f), "Materials/Metal.lxmat", false);
    lx_entity_set_parent(s, visor, body);
    LxEntity sword = lx_entity_create(s, "Sword");   /* a pivot at the hand: the blade swings around it */
    lx_entity_set_parent(s, sword, p);
    lx_entity_set_position(s, sword, lx_v3(-0.45f, 1.0f, 0.1f));   /* the right hand: -X */
    LxEntity blade = prim(s, LX_PRIM_CUBE, "Blade", lx_v3(0, 0, 0.55f), lx_v3(0.06f, 0.12f, 1.1f), "Materials/Blade.lxmat", false);
    lx_entity_set_parent(s, blade, sword);
    LxEntity guard = prim(s, LX_PRIM_CUBE, "Guard", lx_v3(0, 0, -0.02f), lx_v3(0.32f, 0.06f, 0.06f), "Materials/Gold.lxmat", false);
    lx_entity_set_parent(s, guard, sword);
    LxEntity cam = lx_entity_find(s, "Main Camera");
    if (cam) {
        lx_entity_set_position(s, cam, lx_v3(0, 3.5f, 20));
        lx_get_camera(s, cam)->fov_deg = 60;
        /* Distance, Height, Sensitivity, Follow Smoothing, Shoulder Offset, Min Pitch, Max Pitch, Field Of View */
        const float cp[] = {5, 1.6f, 1, 14, 0.55f, -35, 70, 60};
        behave(s, cam, "TPS Camera", cp, 8);
    }
    static const float en[][2] = {{-8, -6}, {8, -8}, {12, 4}, {-12, 6}};
    for (int i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof n, "Enemy %d", i + 1);
        enemy(s, n, lx_v3(en[i][0], 0, en[i][1]), false);
    }
    static const float coins[][3] = {{0, 0.8f, 6}, {-5, 0.8f, 0}, {5, 0.8f, -3}, {-14, 1.8f, 0}, {-18, 2.6f, -4}, {-14, 3.4f, -9}, {-8, 4.0f, -13},
                                     {-6, 2.4f, -20.5f}, {12, 2.0f, 14}, {16, 3.0f, 18}, {19, 2.2f, -0.6f}, {-20, 0.8f, 20}};
    for (size_t i = 0; i < sizeof coins / sizeof coins[0]; i++) {
        char n[32];
        snprintf(n, sizeof n, "Coin %d", (int)i + 1);
        LxEntity c = pickup(s, n, 2, 50, "Materials/Gold.lxmat", lx_v3(coins[i][0], coins[i][1], coins[i][2]), LX_PRIM_CYLINDER, lx_v3(0.6f, 0.08f, 0.6f));
        lx_entity_set_euler(s, c, lx_v3(-90, 0, 0));
    }
    pickup(s, "Health Pack 1", 0, 35, "Materials/Health.lxmat", lx_v3(-21.5f, 0.6f, 21.5f), LX_PRIM_CUBE, lx_v3(0.5f, 0.5f, 0.5f));
    pickup(s, "Health Pack 2", 0, 35, "Materials/Health.lxmat", lx_v3(18, 0.6f, -14), LX_PRIM_CUBE, lx_v3(0.5f, 0.5f, 0.5f));
    manager(s, true);
    return true;
}

bool lx_project_template_build(int tpl, const char *assets, LxScene *s) {
    if (tpl == LX_TEMPLATE_FPS) return fps_template(assets, s);
    if (tpl == LX_TEMPLATE_THIRD_PERSON) return tps_template(assets, s);
    return false;
}
