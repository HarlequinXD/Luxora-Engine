/* =====================================================================
 *  Luxora Engine — public C API
 *  Everything a game needs: core, input, assets, scene, behaviours,
 *  rendering, global illumination, acoustics, crowds and projects.
 * ===================================================================== */
#ifndef LUXORA_H
#define LUXORA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lx_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LX_VERSION_MAJOR 0
#define LX_VERSION_MINOR 1
#define LX_VERSION_PATCH 0
#define LX_VERSION_STRING "0.1.0"

#define LX_NAME_MAX 64
#define LX_REF_MAX 160
#define LX_PATH_MAX 512

/* ===================================================================
 *  Core
 * =================================================================== */
typedef enum LxLogLevel { LX_LOG_INFO, LX_LOG_WARN, LX_LOG_ERROR } LxLogLevel;
typedef void (*LxLogSink)(LxLogLevel level, const char *msg, void *user);

void lx_log(LxLogLevel level, const char *fmt, ...);
void lx_log_add_sink(LxLogSink sink, void *user);
#define LX_INFO(...)  lx_log(LX_LOG_INFO, __VA_ARGS__)
#define LX_WARN(...)  lx_log(LX_LOG_WARN, __VA_ARGS__)
#define LX_ERROR(...) lx_log(LX_LOG_ERROR, __VA_ARGS__)

void *lx_alloc(size_t size);
void *lx_calloc(size_t count, size_t size);
void *lx_realloc(void *p, size_t size);
void lx_free(void *p);
char *lx_strdup(const char *s);
void lx_strcpy(char *dst, size_t cap, const char *src);

/* Files and directories (UTF-8 paths, '/' separators accepted everywhere) */
typedef struct LxDirEntry { char name[256]; bool is_dir; uint64_t size; } LxDirEntry;
char *lx_file_read(const char *path, size_t *out_size); /* NUL-terminated, free with lx_free */
bool lx_file_write(const char *path, const void *data, size_t size);
bool lx_file_exists(const char *path);
bool lx_dir_exists(const char *path);
bool lx_mkdir_p(const char *path);
bool lx_path_remove(const char *path); /* file or directory (recursive) */
bool lx_path_rename(const char *from, const char *to);
int lx_dir_list(const char *path, LxDirEntry **out_entries); /* sorted: dirs first; free with lx_free */
void lx_path_join(char *out, size_t cap, const char *a, const char *b);
const char *lx_path_filename(const char *path);
const char *lx_path_ext(const char *path); /* ".png" or "" */
void lx_path_parent(char *out, size_t cap, const char *path);
const char *lx_home_dir(void);
const char *lx_exe_dir(void);   /* folder of the running program ("" if unknown) */

double lx_time(void);  /* seconds, monotonic */
void lx_sleep_ms(int ms);

/* Job system: work is split into [begin,end) ranges and run on all cores. */
typedef void (*LxJobFn)(void *user, int begin, int end, int thread_index);
void lx_jobs_init(int threads); /* 0 = number of cores - 1 */
void lx_jobs_shutdown(void);
int lx_jobs_thread_count(void); /* worker threads + calling thread */
void lx_parallel_for(int count, int grain, LxJobFn fn, void *user);

/* ===================================================================
 *  Input
 * =================================================================== */
typedef enum LxKey {
    LX_KEY_UNKNOWN = 0,
    LX_KEY_A = 'A', LX_KEY_B, LX_KEY_C, LX_KEY_D, LX_KEY_E, LX_KEY_F, LX_KEY_G, LX_KEY_H, LX_KEY_I,
    LX_KEY_J, LX_KEY_K, LX_KEY_L, LX_KEY_M, LX_KEY_N, LX_KEY_O, LX_KEY_P, LX_KEY_Q, LX_KEY_R,
    LX_KEY_S, LX_KEY_T, LX_KEY_U, LX_KEY_V, LX_KEY_W, LX_KEY_X, LX_KEY_Y, LX_KEY_Z,
    LX_KEY_0 = '0', LX_KEY_1, LX_KEY_2, LX_KEY_3, LX_KEY_4, LX_KEY_5, LX_KEY_6, LX_KEY_7, LX_KEY_8, LX_KEY_9,
    LX_KEY_SPACE = ' ',
    LX_KEY_ESCAPE = 128, LX_KEY_ENTER, LX_KEY_TAB, LX_KEY_BACKSPACE, LX_KEY_DELETE, LX_KEY_INSERT,
    LX_KEY_LEFT, LX_KEY_RIGHT, LX_KEY_UP, LX_KEY_DOWN, LX_KEY_HOME, LX_KEY_END, LX_KEY_PAGE_UP, LX_KEY_PAGE_DOWN,
    LX_KEY_SHIFT, LX_KEY_CTRL, LX_KEY_ALT,
    LX_KEY_F1, LX_KEY_F2, LX_KEY_F3, LX_KEY_F4, LX_KEY_F5, LX_KEY_F6, LX_KEY_F7, LX_KEY_F8,
    LX_KEY_F9, LX_KEY_F10, LX_KEY_F11, LX_KEY_F12,
    LX_KEY_COUNT
} LxKey;

typedef enum LxMouseButton { LX_MOUSE_LEFT, LX_MOUSE_RIGHT, LX_MOUSE_MIDDLE, LX_MOUSE_COUNT } LxMouseButton;

bool lx_key_down(LxKey k);
bool lx_key_pressed(LxKey k);  /* went down this frame */
bool lx_key_released(LxKey k);
bool lx_mouse_down(LxMouseButton b);
bool lx_mouse_pressed(LxMouseButton b);
bool lx_mouse_released(LxMouseButton b);
LxVec2 lx_mouse_pos(void);
LxVec2 lx_mouse_delta(void);
float lx_mouse_wheel(void);
int lx_touch_count(void);
LxVec2 lx_touch_pos(int index);
const uint32_t *lx_text_input(int *count); /* unicode codepoints typed this frame */
void lx_window_size(int *w, int *h);
float lx_dpi_scale(void);

/* ===================================================================
 *  Input manager
 *  Name what the player does (actions like "Jump", axes like "MoveX"), bind each name to
 *  any number of keys, mouse buttons/axes, gamepad buttons/sticks and touch controls, and
 *  read it in game code with one call:
 *
 *      if (lx_action_pressed("Jump")) jump();
 *      LxVec2 move = lx_axis2("MoveX", "MoveY");
 *
 *  Bindings live in the project (ProjectSettings/input.lxinput) and are edited in the
 *  editor's Input Manager window. New projects start with sensible defaults.
 * =================================================================== */
typedef enum LxGamepadButton {
    LX_PAD_A, LX_PAD_B, LX_PAD_X, LX_PAD_Y, LX_PAD_LB, LX_PAD_RB, LX_PAD_BACK, LX_PAD_START,
    LX_PAD_LSTICK, LX_PAD_RSTICK, LX_PAD_UP, LX_PAD_DOWN, LX_PAD_LEFT, LX_PAD_RIGHT, LX_PAD_GUIDE,
    LX_PAD_BUTTON_COUNT
} LxGamepadButton;

typedef enum LxGamepadAxis {
    LX_PAD_LEFT_X, LX_PAD_LEFT_Y, LX_PAD_RIGHT_X, LX_PAD_RIGHT_Y, LX_PAD_LT, LX_PAD_RT, LX_PAD_AXIS_COUNT
} LxGamepadAxis; /* sticks: -1..1 with +Y = up; triggers: 0..1 */

typedef enum LxMouseAxis { LX_MOUSE_AXIS_X, LX_MOUSE_AXIS_Y, LX_MOUSE_AXIS_WHEEL } LxMouseAxis;

typedef enum LxBindingType {
    LX_BIND_KEY,            /* code = LxKey */
    LX_BIND_KEY_AXIS,       /* code = positive key, code2 = negative key */
    LX_BIND_MOUSE_BUTTON,   /* code = LxMouseButton */
    LX_BIND_MOUSE_AXIS,     /* code = LxMouseAxis (per-frame delta) */
    LX_BIND_PAD_BUTTON,     /* code = LxGamepadButton */
    LX_BIND_PAD_AXIS,       /* code = LxGamepadAxis */
    LX_BIND_TOUCH_BUTTON,   /* code = touch control index */
    LX_BIND_TOUCH_STICK_X,  /* code = touch control index */
    LX_BIND_TOUCH_STICK_Y,
    LX_BIND_TYPE_COUNT
} LxBindingType;

typedef struct LxBinding {
    LxBindingType type;
    int code, code2;
    float scale;            /* multiplier, negative to invert */
} LxBinding;

#define LX_MAX_ACTIONS 64
#define LX_MAX_ACTION_BINDINGS 8
#define LX_MAX_TOUCH_CONTROLS 16

typedef struct LxInputAction {
    char name[32];
    bool is_axis;           /* false = button action, true = analog axis (-1..1) */
    LxBinding bindings[LX_MAX_ACTION_BINDINGS];
    int binding_count;
    float dead_zone;        /* axes: values below this read as 0 */
    float sensitivity;      /* axes: overall multiplier */
} LxInputAction;

typedef enum LxTouchControlType { LX_TOUCH_BUTTON, LX_TOUCH_STICK, LX_TOUCH_LOOK_AREA } LxTouchControlType;
typedef struct LxTouchControl {
    char name[32];
    LxTouchControlType type;
    float x, y;             /* centre, 0..1 of the screen (look areas: left/top corner) */
    float size;             /* radius as a fraction of the screen's short side (look areas: width) */
    float height;           /* look areas only: height fraction */
} LxTouchControl;

typedef struct LxInputMap {
    LxInputAction actions[LX_MAX_ACTIONS];
    int action_count;
    LxTouchControl touch[LX_MAX_TOUCH_CONTROLS];
    int touch_count;
    float trigger_threshold;    /* analog value that counts as "pressed" for button actions */
    int touch_controls_mode;    /* 0 auto (touch devices), 1 always, 2 never */
} LxInputMap;

typedef enum LxInputDevice { LX_DEVICE_KEYBOARD_MOUSE, LX_DEVICE_GAMEPAD, LX_DEVICE_TOUCH } LxInputDevice;

/* game API */
bool lx_action_down(const char *action);
bool lx_action_pressed(const char *action);   /* this frame */
bool lx_action_released(const char *action);
float lx_axis(const char *axis);                 /* -1..1 (mouse axes may exceed) */
LxVec2 lx_axis2(const char *x_axis, const char *y_axis);  /* with radial dead zone */
LxInputDevice lx_input_last_device(void);       /* for showing the right button prompts */

/* gamepads (up to 4) */
int lx_gamepad_count(void);
bool lx_gamepad_connected(int pad);
const char *lx_gamepad_name(int pad);
bool lx_gamepad_button(int pad, LxGamepadButton b);
float lx_gamepad_axis(int pad, LxGamepadAxis a);
void lx_gamepad_rumble(int pad, float low, float high, float seconds);

/* mapping */
LxInputMap *lx_input_map(void);
void lx_input_map_defaults(LxInputMap *map);
bool lx_input_map_save(const LxInputMap *map, const char *path);
bool lx_input_map_load(LxInputMap *map, const char *path);
LxInputAction *lx_input_find(LxInputMap *map, const char *name);
LxInputAction *lx_input_add_action(LxInputMap *map, const char *name, bool is_axis);
bool lx_input_add_binding(LxInputAction *a, LxBinding b);
void lx_input_update(float dt);                  /* called by the engine once per frame */
bool lx_input_touch_controls_visible(void);
void lx_input_draw_touch_controls(void);         /* runtime overlay on touch devices */
/* names for display and file storage */
const char *lx_key_name(LxKey k);
const char *lx_pad_button_name(LxGamepadButton b);
const char *lx_pad_axis_name(LxGamepadAxis a);
const char *lx_mouse_button_name(LxMouseButton b);
const char *lx_mouse_axis_name(LxMouseAxis a);
const char *lx_binding_type_name(LxBindingType t);
void lx_binding_describe(const LxInputMap *map, const LxBinding *b, char *out, size_t cap);
/* "press any input" capture for rebinding UIs: returns true and fills `out` when something is pressed */
bool lx_input_capture(LxBinding *out, bool want_axis);

/* ===================================================================
 *  Assets
 *  References are strings: "builtin:cube", "Meshes/rock.obj",
 *  "Materials/stone.lxmat", "Textures/stone.png", "Sounds/step.wav".
 *  Paths are relative to the project's Assets folder.
 * =================================================================== */
typedef struct LxMesh LxMesh;
typedef struct LxTexture LxTexture;
typedef struct LxSound LxSound;

typedef enum LxPrimitive {
    LX_PRIM_CUBE, LX_PRIM_SPHERE, LX_PRIM_PLANE, LX_PRIM_CYLINDER, LX_PRIM_CAPSULE, LX_PRIM_COUNT
} LxPrimitive;

typedef struct LxMaterial {
    char name[LX_NAME_MAX];
    LxVec4 albedo;           /* linear RGB + alpha */
    float metallic;
    float roughness;
    LxVec3 emissive;
    float emissive_strength;
    char albedo_map[LX_REF_MAX];
    char normal_map[LX_REF_MAX];
    char roughness_map[LX_REF_MAX];  /* green channel (a grayscale map, or the G of an AO/rough/metal pack) x roughness */
    char metallic_map[LX_REF_MAX];   /* blue channel x metallic */
    char ao_map[LX_REF_MAX];         /* red channel: ambient occlusion */
    char height_map[LX_REF_MAX];     /* red channel, white = high: parallax occlusion mapping */
    float uv_scale;
    float normal_strength;           /* 1 = as authored */
    bool normal_flip_green;          /* DirectX-style normal maps (green points down) */
    bool roughness_is_smoothness;    /* the roughness map is a gloss/smoothness map */
    float ao_strength;               /* 0..1 */
    float height_scale;              /* parallax depth in UV units (0.02 - 0.1 typical) */
    bool parallax_shadows;           /* the height field shadows itself from the sun */
    /* acoustics: fraction of sound energy absorbed on reflection (0..1) and blocked on transmission */
    float acoustic_absorption;
    float acoustic_transmission;
} LxMaterial;

void lx_assets_set_root(const char *assets_dir);
const char *lx_assets_root(void);
void lx_asset_path(char *out, size_t cap, const char *ref);
LxMesh *lx_asset_mesh(const char *ref);
LxMaterial *lx_asset_material(const char *ref);
LxTexture *lx_asset_texture(const char *ref);
LxSound *lx_asset_sound(const char *ref);
void lx_assets_reload(const char *ref); /* drop cached copy so it reloads */
void lx_assets_clear(void);

LxMaterial lx_material_default(void);
bool lx_material_save(const LxMaterial *m, const char *path);
bool lx_material_load(LxMaterial *m, const char *path);

LxAabb lx_mesh_bounds(const LxMesh *m);
int lx_mesh_triangle_count(const LxMesh *m);
int lx_mesh_lod_count(const LxMesh *m);
const char *lx_primitive_name(LxPrimitive p);

/* ===================================================================
 *  Scene
 * =================================================================== */
typedef uint32_t LxEntity; /* 0 = none */
#define LX_NULL_ENTITY 0u

typedef enum LxComponentBits {
    LX_COMP_CAMERA = 1 << 0,
    LX_COMP_LIGHT = 1 << 1,
    LX_COMP_MESH = 1 << 2,
    LX_COMP_AUDIO_SOURCE = 1 << 3,
    LX_COMP_AUDIO_LISTENER = 1 << 4,
    LX_COMP_BEHAVIOUR = 1 << 5,
    LX_COMP_CROWD = 1 << 6,
    LX_COMP_REFLECTION_PROBE = 1 << 7,
    LX_COMP_COLLIDER = 1 << 8,
    LX_COMP_RIGIDBODY = 1 << 9,
    LX_COMP_CHARACTER = 1 << 10,
} LxComponentBits;

typedef struct LxCamera {
    float fov_deg, near_clip, far_clip;
    bool orthographic;
    float ortho_size;
    LxVec4 clear_color;
    int priority;          /* highest priority camera renders the game */
} LxCamera;

typedef enum LxLightType { LX_LIGHT_DIRECTIONAL, LX_LIGHT_POINT, LX_LIGHT_SPOT } LxLightType;
typedef struct LxLight {
    LxLightType type;
    LxVec3 color;
    float intensity;       /* directional: lux-like scale; point/spot: candela-like */
    float range;
    float spot_angle_deg;
    bool cast_shadows;
    bool contribute_gi;    /* emits virtual photons */
} LxLight;

typedef struct LxMeshRenderer {
    char mesh[LX_REF_MAX];
    char material[LX_REF_MAX];
    bool cast_shadows;
    bool static_geometry;  /* participates in GI photons + acoustics */
} LxMeshRenderer;

typedef enum LxAudioBus { LX_BUS_SFX, LX_BUS_MUSIC, LX_BUS_VOICE, LX_BUS_AMBIENT, LX_BUS_COUNT } LxAudioBus;
typedef struct LxAudioSource {
    char clip[LX_REF_MAX];
    float volume, pitch;
    bool loop, play_on_start, spatial;
    float min_distance, max_distance;
    LxAudioBus bus;
} LxAudioSource;

#define LX_BEHAVIOUR_PARAMS 8
typedef struct LxBehaviourRef {
    char name[LX_NAME_MAX];
    float params[LX_BEHAVIOUR_PARAMS];
} LxBehaviourRef;

typedef struct LxCrowdComponent {
    int agent_count;
    float area_radius;
    float agent_speed;
    char mesh[LX_REF_MAX];
    char material[LX_REF_MAX];
} LxCrowdComponent;

/* Captures the scene around it into a cubemap that nearby surfaces reflect.
 * The box is centred on the entity; inside it, reflections are parallax-corrected
 * (box projection) so floors and walls line up. Probes re-capture automatically when
 * the scene changes; realtime probes keep updating every few frames. */
typedef struct LxReflectionProbe {
    LxVec3 size;            /* influence box, world units */
    float intensity;
    float blend_distance;   /* fade-in distance at the box edges */
    bool box_projection;
    bool realtime;
} LxReflectionProbe;

/* ---- Physics components ----
 * Collider alone            = static scenery (never moves)
 * Collider + Rigidbody      = simulated body (or kinematic: moved by your code, pushes others)
 * Character Controller      = capsule moved with lx_character_move(), slides along walls, climbs steps
 * Sizes are local and scaled by the entity's scale. */
typedef struct LxPhysicsSettings {
    LxVec3 gravity;             /* m/s^2, default (0, -9.81, 0) */
    float fixed_timestep;       /* seconds, default 1/60 */
    int solver_iterations;      /* default 10 */
    uint32_t layer_mask[32];    /* bit j of layer_mask[i]: layer i collides with layer j */
} LxPhysicsSettings;

typedef enum LxColliderShape { LX_COLLIDER_BOX, LX_COLLIDER_SPHERE, LX_COLLIDER_CAPSULE, LX_COLLIDER_MESH } LxColliderShape;
typedef struct LxCollider {
    LxColliderShape shape;
    LxVec3 center;          /* offset from the entity origin */
    LxVec3 size;            /* box: full size */
    float radius;           /* sphere, capsule */
    float height;           /* capsule: total height along local Y */
    bool is_trigger;        /* reports overlaps (on_trigger_enter/exit) instead of colliding */
    float friction;         /* 0 ice .. 1 rubber */
    float bounciness;       /* 0 dead .. 1 superball */
    int layer;              /* 0..31, see LxPhysicsSettings.layer_mask */
} LxCollider;

typedef struct LxRigidbody {
    float mass;             /* kg */
    float linear_damping, angular_damping;
    bool use_gravity;
    bool is_kinematic;      /* not simulated: moved by code, pushes dynamic bodies */
    bool freeze_rotation;   /* never tips over (characters, pickups) */
    LxVec3 velocity;        /* world space, live while playing (writeable) */
    LxVec3 angular_velocity;/* radians/second, live */
} LxRigidbody;

typedef enum LxCharacterFlags { LX_CHAR_GROUNDED = 1, LX_CHAR_SIDES = 2, LX_CHAR_ABOVE = 4 } LxCharacterFlags;
typedef struct LxCharacterController {
    float radius, height;   /* capsule; the entity origin is at the feet */
    float step_height;      /* climbs stairs/ledges up to this */
    float slope_limit_deg;  /* steeper surfaces are walls */
    float push_power;       /* how hard it shoves rigidbodies */
    bool grounded;          /* live: standing on walkable ground */
    LxVec3 velocity;        /* live: world velocity of the last move */
} LxCharacterController;

typedef struct LxEnvironment {
    LxVec3 sky_zenith, sky_horizon, ground_color;
    float ambient_intensity;
    float exposure;
    LxVec3 fog_color;
    float fog_density;
} LxEnvironment;

typedef struct LxScene LxScene;

LxScene *lx_scene_create(const char *name);
/* New scene with "Main Camera", "Directional Light" and a default "Cube". */
LxScene *lx_scene_create_default(const char *name);
void lx_scene_destroy(LxScene *s);
const char *lx_scene_name(const LxScene *s);
void lx_scene_set_name(LxScene *s, const char *name);
LxEnvironment *lx_scene_environment(LxScene *s);
LxScene *lx_scene_clone(const LxScene *s);

LxEntity lx_entity_create(LxScene *s, const char *name);
void lx_entity_destroy(LxScene *s, LxEntity e); /* destroys children too */
bool lx_entity_valid(const LxScene *s, LxEntity e);
LxEntity lx_entity_duplicate(LxScene *s, LxEntity e);
LxEntity lx_entity_find(const LxScene *s, const char *name);
const char *lx_entity_name(const LxScene *s, LxEntity e);
void lx_entity_set_name(LxScene *s, LxEntity e, const char *name);
bool lx_entity_active(const LxScene *s, LxEntity e);
void lx_entity_set_active(LxScene *s, LxEntity e, bool active);
uint32_t lx_entity_components(const LxScene *s, LxEntity e);

void lx_entity_set_parent(LxScene *s, LxEntity child, LxEntity parent);
LxEntity lx_entity_parent(const LxScene *s, LxEntity e);
LxEntity lx_entity_first_child(const LxScene *s, LxEntity e);
LxEntity lx_entity_next_sibling(const LxScene *s, LxEntity e);
int lx_scene_entity_count(const LxScene *s);
LxEntity lx_scene_entity_at(const LxScene *s, int index); /* stable creation order */
LxEntity lx_scene_root_first(const LxScene *s);           /* first root entity */

/* Transform (local space) */
LxVec3 lx_entity_position(const LxScene *s, LxEntity e);
void lx_entity_set_position(LxScene *s, LxEntity e, LxVec3 p);
LxVec3 lx_entity_euler(const LxScene *s, LxEntity e); /* degrees */
void lx_entity_set_euler(LxScene *s, LxEntity e, LxVec3 deg);
LxQuat lx_entity_rotation(const LxScene *s, LxEntity e);
LxVec3 lx_entity_scale(const LxScene *s, LxEntity e);
void lx_entity_set_scale(LxScene *s, LxEntity e, LxVec3 sc);
LxMat4 lx_entity_world_matrix(LxScene *s, LxEntity e);
LxVec3 lx_entity_world_position(LxScene *s, LxEntity e);
LxVec3 lx_entity_forward(LxScene *s, LxEntity e); /* -Z in world */
LxVec3 lx_entity_right(LxScene *s, LxEntity e);
void lx_scene_update_transforms(LxScene *s);
LxQuat lx_entity_world_rotation(LxScene *s, LxEntity e);
void lx_entity_set_rotation(LxScene *s, LxEntity e, LxQuat q);                      /* local */
void lx_entity_set_world_pose(LxScene *s, LxEntity e, LxVec3 pos, LxQuat rot);       /* handles parents */
void lx_entity_look_at(LxScene *s, LxEntity e, LxVec3 target, LxVec3 up);

/* Components */
LxCamera *lx_add_camera(LxScene *s, LxEntity e);
LxLight *lx_add_light(LxScene *s, LxEntity e, LxLightType type);
LxMeshRenderer *lx_add_mesh(LxScene *s, LxEntity e, const char *mesh_ref, const char *material_ref);
LxAudioSource *lx_add_audio_source(LxScene *s, LxEntity e, const char *clip_ref);
void lx_add_audio_listener(LxScene *s, LxEntity e);
LxBehaviourRef *lx_add_behaviour(LxScene *s, LxEntity e, const char *behaviour_name);
LxCrowdComponent *lx_add_crowd(LxScene *s, LxEntity e, int agents);
LxReflectionProbe *lx_add_reflection_probe(LxScene *s, LxEntity e);
LxCollider *lx_add_collider(LxScene *s, LxEntity e, LxColliderShape shape); /* sized to the mesh when there is one */
LxRigidbody *lx_add_rigidbody(LxScene *s, LxEntity e);
LxCharacterController *lx_add_character(LxScene *s, LxEntity e);
LxCamera *lx_get_camera(LxScene *s, LxEntity e);
LxLight *lx_get_light(LxScene *s, LxEntity e);
LxMeshRenderer *lx_get_mesh(LxScene *s, LxEntity e);
LxAudioSource *lx_get_audio_source(LxScene *s, LxEntity e);
LxBehaviourRef *lx_get_behaviour(LxScene *s, LxEntity e);
LxCrowdComponent *lx_get_crowd(LxScene *s, LxEntity e);
LxReflectionProbe *lx_get_reflection_probe(LxScene *s, LxEntity e);
LxCollider *lx_get_collider(LxScene *s, LxEntity e);
LxRigidbody *lx_get_rigidbody(LxScene *s, LxEntity e);
LxCharacterController *lx_get_character(LxScene *s, LxEntity e);
void lx_remove_component(LxScene *s, LxEntity e, LxComponentBits comp);

LxEntity lx_spawn_primitive(LxScene *s, LxPrimitive p, const char *name, LxVec3 pos);
LxEntity lx_scene_main_camera(const LxScene *s);
LxEntity lx_scene_listener(const LxScene *s);
/* Marks static geometry as changed so GI/acoustic structures rebuild. */
void lx_scene_mark_static_dirty(LxScene *s);
uint32_t lx_scene_static_version(const LxScene *s);

/* Ray picking against mesh renderers. Returns hit entity or 0. */
LxEntity lx_scene_raycast(LxScene *s, LxRay ray, float max_dist, float *out_dist);

/* Serialization (.lxscene text format) */
char *lx_scene_serialize(const LxScene *s); /* free with lx_free */
LxScene *lx_scene_deserialize(const char *text);
bool lx_scene_save(const LxScene *s, const char *path);
LxScene *lx_scene_load(const char *path);

/* ===================================================================
 *  Behaviours — the game coding API.
 *  Register a behaviour once at startup; attach it to entities by name.
 * =================================================================== */
typedef struct LxBehaviourCtx {
    LxScene *scene;
    LxEntity self;
    void *state;      /* zero-initialised block of state_size bytes */
    float *params;    /* editable in the inspector */
} LxBehaviourCtx;

/* What a behaviour learns about a collision. `normal` points away from `other`, towards you. */
typedef struct LxCollision {
    LxEntity other;
    LxVec3 point, normal;
    float impulse;              /* strength of the hit (N*s): use it for impact sounds/damage */
    LxVec3 relative_velocity;   /* other's velocity relative to you at the contact */
} LxCollision;

typedef struct LxBehaviourDesc {
    const char *name;
    size_t state_size;
    const char *param_names[LX_BEHAVIOUR_PARAMS];
    float param_defaults[LX_BEHAVIOUR_PARAMS];
    void (*on_start)(LxBehaviourCtx *ctx);
    void (*on_update)(LxBehaviourCtx *ctx, float dt);
    void (*on_destroy)(LxBehaviourCtx *ctx);
    /* optional: */
    void (*on_fixed_update)(LxBehaviourCtx *ctx, float fixed_dt);   /* before each physics step */
    void (*on_late_update)(LxBehaviourCtx *ctx, float dt);          /* after physics: cameras */
    void (*on_collision_enter)(LxBehaviourCtx *ctx, const LxCollision *c);
    void (*on_collision_exit)(LxBehaviourCtx *ctx, LxEntity other);
    void (*on_trigger_enter)(LxBehaviourCtx *ctx, LxEntity other);
    void (*on_trigger_exit)(LxBehaviourCtx *ctx, LxEntity other);
    /* messages sent with lx_send_message / lx_broadcast / lx_send_message_after / finished tweens */
    void (*on_message)(LxBehaviourCtx *ctx, const char *message, float value, LxEntity sender);
} LxBehaviourDesc;

void lx_register_behaviour(const LxBehaviourDesc *desc);
int lx_behaviour_count(void);
const LxBehaviourDesc *lx_behaviour_at(int index);
const LxBehaviourDesc *lx_behaviour_find(const char *name);
void lx_register_builtin_behaviours(void);

/* Game scripts: each file in Assets/Scripts registers its behaviours with
 *     LX_SCRIPT_REGISTER(filename_without_extension) { lx_register_behaviour(&desc); }
 * The build generates the call for every script file automatically. */
#define LX_SCRIPT_REGISTER(name) void lx_script_##name(void)

/* Runs behaviours (start on first tick), physics and crowds for a scene. */
void lx_scene_simulate(LxScene *s, float dt);
void lx_scene_stop_simulation(LxScene *s);

/* ===================================================================
 *  Gameplay: prefabs, tags, messages, timers, tweens, time, random, save data
 * =================================================================== */

/* Prefabs (.lxprefab): a saved object with its children and components, spawned any number of times.
 * The root is saved without its position; instances remember which prefab they came from. */
bool lx_prefab_save(const LxScene *s, LxEntity root, const char *path);
LxEntity lx_prefab_instantiate(LxScene *s, const char *prefab_ref, LxVec3 position, LxEntity parent);
LxEntity lx_prefab_instantiate_text(LxScene *s, const char *text, LxVec3 position, LxEntity parent);
const char *lx_entity_prefab(const LxScene *s, LxEntity e);           /* "" when not a prefab instance */
void lx_entity_set_prefab(LxScene *s, LxEntity e, const char *prefab_ref);
char *lx_entity_serialize(const LxScene *s, LxEntity root);           /* prefab text; free with lx_free */

LxEntity lx_entity_find_child(const LxScene *s, LxEntity parent, const char *name);   /* searches all descendants */

/* Tags: one short label per object ("Player", "Enemy", "Pickup") for finding things quickly. */
#define LX_TAG_MAX 32
void lx_entity_set_tag(LxScene *s, LxEntity e, const char *tag);
const char *lx_entity_tag(const LxScene *s, LxEntity e);
bool lx_entity_has_tag(const LxScene *s, LxEntity e, const char *tag);
LxEntity lx_entity_find_with_tag(const LxScene *s, const char *tag);     /* first one, or 0 */
int lx_entities_with_tag(const LxScene *s, const char *tag, LxEntity *out, int max);

/* Messages: call on_message on an object's behaviour. Returns true if a behaviour received it. */
bool lx_send_message(LxScene *s, LxEntity target, const char *message, float value, LxEntity sender);
int lx_broadcast(LxScene *s, const char *message, float value, LxEntity sender);   /* every object; returns receivers */
int lx_send_message_to_tag(LxScene *s, const char *tag, const char *message, float value, LxEntity sender);
/* Global events for systems that aren't objects (score, game state, UI). */
typedef void (*LxEventFn)(const char *event, float value, void *user);
int lx_event_listen(const char *event, LxEventFn fn, void *user);   /* returns an id for lx_event_unlisten */
void lx_event_unlisten(int id);
int lx_event_emit(const char *event, float value);                  /* returns listeners called */

/* Timers run on game time (paused, slowed or sped up with the time scale). A timer that belongs to an
 * object is cancelled when the object is destroyed. interval 0 = once, else repeats. */
typedef void (*LxTimerFn)(LxScene *s, LxEntity owner, void *user);
int lx_timer_start(LxScene *s, LxEntity owner, float delay, float interval, LxTimerFn fn, void *user);
int lx_send_message_after(LxScene *s, LxEntity target, const char *message, float value, float delay);
void lx_timer_cancel(LxScene *s, int timer);

/* Tweens: animate a value over time with easing. The object gets the message (if set) when it finishes. */
typedef enum LxEase {
    LX_EASE_LINEAR, LX_EASE_IN, LX_EASE_OUT, LX_EASE_IN_OUT, LX_EASE_BACK_OUT, LX_EASE_ELASTIC_OUT, LX_EASE_BOUNCE_OUT
} LxEase;
float lx_ease(LxEase ease, float t);
int lx_tween_position(LxScene *s, LxEntity e, LxVec3 to, float seconds, LxEase ease);
int lx_tween_scale(LxScene *s, LxEntity e, LxVec3 to, float seconds, LxEase ease);
int lx_tween_euler(LxScene *s, LxEntity e, LxVec3 to_degrees, float seconds, LxEase ease);
int lx_tween_float(LxScene *s, LxEntity owner, float *value, float to, float seconds, LxEase ease);
void lx_tween_set_done_message(LxScene *s, int tween, const char *message);
void lx_tween_set_loop(LxScene *s, int tween, bool ping_pong);      /* repeats forever (back and forth) */
void lx_tween_cancel(LxScene *s, int tween);
void lx_tweens_cancel_for(LxScene *s, LxEntity e);

/* Time */
void lx_time_set_scale(float scale);   /* 0 = paused, 0.5 = slow motion, 1 = normal */
float lx_time_scale(void);
float lx_game_time(const LxScene *s);  /* game-time seconds since play started */
float lx_delta_unscaled(const LxScene *s);

/* Random numbers (one shared generator; seed it for repeatable runs) */
void lx_random_seed(uint64_t seed);
float lx_random(void);                         /* 0 .. 1 */
float lx_random_range(float min, float max);
int lx_random_int(int min, int max);           /* min .. max inclusive */
LxVec3 lx_random_in_sphere(float radius);
bool lx_random_chance(float probability);

/* Save data: named values kept between runs of the game (written at the end of each frame they change). */
void lx_save_set_int(const char *key, int value);
void lx_save_set_float(const char *key, float value);
void lx_save_set_string(const char *key, const char *value);
int lx_save_get_int(const char *key, int fallback);
float lx_save_get_float(const char *key, float fallback);
const char *lx_save_get_string(const char *key, const char *fallback);
bool lx_save_has(const char *key);
void lx_save_delete(const char *key);
void lx_save_clear(void);
bool lx_save_write(void);                       /* write now (it also happens automatically) */
void lx_save_set_file(const char *path);        /* where save data lives (set up by the engine) */
const char *lx_save_file(void);

/* Another object's behaviour */
void *lx_behaviour_state(LxScene *s, LxEntity e);   /* its state block (NULL before it starts) */
float *lx_behaviour_params(LxScene *s, LxEntity e);
const char *lx_behaviour_name(const LxScene *s, LxEntity e);

/* ===================================================================
 *  Physics
 * =================================================================== */

LxPhysicsSettings *lx_physics_settings(void);
LxPhysicsSettings lx_physics_defaults(void);

typedef struct LxRaycastHit {
    LxEntity entity;
    LxVec3 point, normal;
    float distance;
} LxRaycastHit;
#define LX_ALL_LAYERS 0xffffffffu

/* Queries test colliders (and character controllers) as they are right now. Triggers are ignored. */
bool lx_physics_raycast(LxScene *s, LxVec3 origin, LxVec3 dir, float max_dist, uint32_t layer_mask, LxRaycastHit *hit);
bool lx_physics_sphere_cast(LxScene *s, LxVec3 origin, float radius, LxVec3 dir, float max_dist, uint32_t layer_mask,
                            LxRaycastHit *hit);
int lx_physics_overlap_sphere(LxScene *s, LxVec3 center, float radius, uint32_t layer_mask, LxEntity *out, int max_out);
int lx_physics_overlap_box(LxScene *s, LxVec3 center, LxVec3 half_extents, LxQuat rot, uint32_t layer_mask, LxEntity *out,
                           int max_out);

/* Rigidbodies (no-ops for entities without one) */
void lx_rigidbody_add_force(LxScene *s, LxEntity e, LxVec3 force);           /* N, applied over the next step */
void lx_rigidbody_add_impulse(LxScene *s, LxEntity e, LxVec3 impulse);       /* N*s, instant (jumps, explosions) */
void lx_rigidbody_add_impulse_at(LxScene *s, LxEntity e, LxVec3 impulse, LxVec3 world_point); /* also spins it */
void lx_rigidbody_add_torque(LxScene *s, LxEntity e, LxVec3 torque);
void lx_rigidbody_set_velocity(LxScene *s, LxEntity e, LxVec3 v);
LxVec3 lx_rigidbody_velocity(LxScene *s, LxEntity e);
void lx_rigidbody_wake(LxScene *s, LxEntity e);
bool lx_rigidbody_sleeping(LxScene *s, LxEntity e);
void lx_physics_explosion(LxScene *s, LxVec3 center, float radius, float impulse); /* pushes nearby bodies */

/* Character controller: moves by `displacement` (already multiplied by dt), sliding along walls,
 * climbing steps and snapping to slopes. Add gravity yourself. Returns LxCharacterFlags. */
int lx_character_move(LxScene *s, LxEntity e, LxVec3 displacement);
bool lx_character_grounded(LxScene *s, LxEntity e);

/* ===================================================================
 *  Rendering + global illumination
 * =================================================================== */
typedef struct LxGraphicsSettings {
    float render_scale;         /* 0.5 .. 2.0 */
    int shadow_resolution;      /* per cascade */
    int shadow_cascades;        /* 1..4 */
    float shadow_distance;
    bool gi_enabled;            /* virtual photon global illumination */
    int gi_photons_per_frame;
    int gi_max_bounces;
    int gi_grid_resolution;     /* cells along the longest axis */
    float gi_intensity;
    float gi_responsiveness;    /* 0..1 temporal blend per frame */
    bool lod_enabled;
    float lod_bias;
    bool vsync;
    int target_fps;             /* 0 = unlimited */
    float exposure;
    int quality_preset;         /* 0 low, 1 medium, 2 high, 3 ultra */
    /* reflections: probes (cubemaps of the scene, plus an automatic environment probe)
       and screen-space reflections layered on top for exact on-screen detail */
    bool reflections_enabled;
    int reflection_resolution;  /* probe cube face size: 64 .. 512 */
    bool ssr_enabled;
    int ssr_steps;              /* ray-march steps (half resolution) */
    float ssr_max_distance;     /* world units */
    float ssr_thickness;        /* how thick on-screen surfaces are assumed to be */
    float ssr_max_roughness;    /* rougher surfaces use probes only */
} LxGraphicsSettings;

typedef struct LxRenderStats {
    int draw_calls, instances_drawn, instances_culled;
    int64_t triangles;
    int gi_photons_traced;
    float gi_ms, cull_ms, frame_ms;
    int probes_active, probe_faces_rendered;
} LxRenderStats;

typedef struct LxRenderView {
    LxMat4 view, proj;
    LxVec3 eye;
    int width, height;
    float near_clip, far_clip;
    bool draw_grid;
    bool draw_gizmos;            /* light/camera icons for the editor */
    LxEntity highlight;          /* selection outline */
    LxVec4 clear_color;
} LxRenderView;

typedef struct LxRenderTarget LxRenderTarget;

bool lx_renderer_init(void);
void lx_renderer_shutdown(void);
LxGraphicsSettings *lx_graphics_settings(void);
LxGraphicsSettings lx_graphics_defaults(int quality_preset);
void lx_graphics_apply_preset(LxGraphicsSettings *g, int preset);
/* Render a scene into a target (NULL = window backbuffer). */
void lx_render_scene(LxScene *s, const LxRenderView *view, LxRenderTarget *target);
bool lx_camera_view(LxScene *s, LxEntity camera, int w, int h, LxRenderView *out);
const LxRenderStats *lx_render_stats(void);
LxRenderTarget *lx_render_target_create(int w, int h);
void lx_render_target_resize(LxRenderTarget *rt, int w, int h);
void lx_render_target_destroy(LxRenderTarget *rt);
uint32_t lx_render_target_texture(const LxRenderTarget *rt); /* GL texture of the final LDR image */
void lx_gi_reset(void);  /* restart photon accumulation */
void lx_reflections_refresh(void);  /* re-capture every reflection probe */

/* ===================================================================
 *  Audio + acoustics
 * =================================================================== */
typedef struct LxAudioSettings {
    float master_volume;
    float bus_volume[LX_BUS_COUNT];
    int sample_rate;
    float speed_of_sound;       /* m/s; 343 in air */
    bool propagation;           /* ray-traced reflections + occlusion */
    int rays_per_source;
    int max_bounces;
    float air_absorption;       /* per metre */
    bool doppler;
} LxAudioSettings;

bool lx_audio_init(void);
void lx_audio_shutdown(void);
LxAudioSettings *lx_audio_settings(void);
LxAudioSettings lx_audio_defaults(void);
const char *lx_audio_backend_name(void);
/* Sync audio sources/listener/geometry from a scene. Call once per frame. */
void lx_audio_update(LxScene *s, float dt);
void lx_audio_play(LxScene *s, LxEntity source);
void lx_audio_stop(LxScene *s, LxEntity source);
void lx_audio_stop_all(void);
void lx_audio_play_oneshot(const char *clip_ref, LxVec3 pos, float volume);
/* Preview channel (editor tools, menus): one non-spatial sound at a time; a new preview replaces the old. */
void lx_audio_preview(const char *clip_ref, float volume, float pitch);
void lx_audio_preview_stop(void);
bool lx_audio_preview_playing(void);
int lx_audio_playing_count(void);   /* sounds currently audible (not fading out) */
float lx_sound_duration(const LxSound *snd);
/* Offline: compute the impulse response between two points (for tests/tools). */
int lx_acoustic_impulse(LxScene *s, LxVec3 src, LxVec3 lst, float *out_ir, int ir_len, int sample_rate);

/* ===================================================================
 *  Crowds — data-oriented, multithreaded agent simulation
 * =================================================================== */
typedef struct LxCrowd LxCrowd;
LxCrowd *lx_crowd_create(int max_agents);
void lx_crowd_destroy(LxCrowd *c);
int lx_crowd_add(LxCrowd *c, LxVec3 pos, float radius, float max_speed);
void lx_crowd_set_target(LxCrowd *c, int agent, LxVec3 target);
void lx_crowd_update(LxCrowd *c, float dt);
int lx_crowd_count(const LxCrowd *c);
LxVec3 lx_crowd_position(const LxCrowd *c, int agent);
LxVec3 lx_crowd_velocity(const LxCrowd *c, int agent);
void lx_crowd_add_obstacle(LxCrowd *c, LxVec3 center, float radius);

/* ===================================================================
 *  Projects
 * =================================================================== */
typedef enum LxPlatform { LX_PLATFORM_WINDOWS, LX_PLATFORM_LINUX, LX_PLATFORM_ANDROID, LX_PLATFORM_COUNT } LxPlatform;
#define LX_MAX_BUILD_SCENES 32



typedef struct LxBuildSettings {
    LxPlatform target;
    char scenes[LX_MAX_BUILD_SCENES][LX_REF_MAX];
    int scene_count;
    bool development_build;
    bool fullscreen;
    int window_width, window_height;
    char android_package[128];
    int android_min_sdk;
    int android_orientation;    /* 0 landscape, 1 portrait */
    char output_dir[LX_PATH_MAX];
    char icon[LX_REF_MAX];      /* game icon image in Assets (PNG/TGA/BMP); empty = default game-controller icon */
} LxBuildSettings;

typedef struct LxProject {
    char name[LX_NAME_MAX];
    char company[LX_NAME_MAX];
    char version[32];
    char root[LX_PATH_MAX];       /* project folder */
    char assets[LX_PATH_MAX];     /* <root>/Assets */
    char startup_scene[LX_REF_MAX];
    LxGraphicsSettings graphics;
    LxAudioSettings audio;
    LxPhysicsSettings physics;
    LxBuildSettings build;
} LxProject;

typedef enum LxProjectTemplate { LX_TEMPLATE_EMPTY_3D, LX_TEMPLATE_SAMPLE_3D, LX_TEMPLATE_MOBILE_3D } LxProjectTemplate;

bool lx_project_create(LxProject *p, const char *parent_dir, const char *name, LxProjectTemplate tpl);
bool lx_project_load(LxProject *p, const char *root_dir);
bool lx_project_save(const LxProject *p);
const char *lx_platform_name(LxPlatform p);
LxPlatform lx_platform_current(void);

/* ===================================================================
 *  Game UI: HUDs, menus, health bars, buttons (immediate mode)
 *
 *  Call these every frame from a behaviour (on_update or on_late_update); what you draw is shown
 *  for that frame, over the game. Units are "canvas" units: the screen scaled to 1080 units tall
 *  (see lx_ui_set_reference_height), x to the right, y down - so a layout designed at 1080p fits
 *  any resolution and aspect. lx_ui_anchor() chooses which screen point x/y are measured from;
 *  the element is aligned to that point too (TOP_RIGHT puts its top-right corner there).
 *  Text uses the Inter font (regular/bold) or DejaVu Sans Mono.
 * =================================================================== */
#define LX_RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define LX_RGB(r, g, b) LX_RGBA(r, g, b, 255)
#define LX_COLOR_WHITE LX_RGB(255, 255, 255)
#define LX_COLOR_BLACK LX_RGB(0, 0, 0)
#define LX_COLOR_RED LX_RGB(229, 72, 77)
#define LX_COLOR_GREEN LX_RGB(70, 190, 100)
#define LX_COLOR_BLUE LX_RGB(66, 135, 245)
#define LX_COLOR_YELLOW LX_RGB(245, 200, 66)

typedef enum LxUiAnchor {
    LX_ANCHOR_TOP_LEFT, LX_ANCHOR_TOP, LX_ANCHOR_TOP_RIGHT,
    LX_ANCHOR_LEFT, LX_ANCHOR_CENTER, LX_ANCHOR_RIGHT,
    LX_ANCHOR_BOTTOM_LEFT, LX_ANCHOR_BOTTOM, LX_ANCHOR_BOTTOM_RIGHT
} LxUiAnchor;
typedef enum LxUiFont { LX_FONT_REGULAR, LX_FONT_BOLD, LX_FONT_MONO } LxUiFont;

LxVec2 lx_ui_canvas_size(void);                 /* e.g. 1920 x 1080 on a 16:9 screen */
void lx_ui_set_reference_height(float units);   /* default 1080 */
void lx_ui_anchor(LxUiAnchor anchor);           /* for the following calls; resets to TOP_LEFT each frame */
void lx_ui_font(LxUiFont font);                 /* resets to REGULAR each frame */
void lx_ui_rect(float x, float y, float w, float h, uint32_t color, float corner_radius);
void lx_ui_rect_outline(float x, float y, float w, float h, uint32_t color, float corner_radius, float thickness);
void lx_ui_circle(float x, float y, float radius, uint32_t color);           /* x, y is the centre */
void lx_ui_line(float x0, float y0, float x1, float y1, float thickness, uint32_t color);
void lx_ui_text(float x, float y, float size, uint32_t color, const char *fmt, ...);
float lx_ui_text_width(float size, const char *text);
void lx_ui_image(const char *texture_ref, float x, float y, float w, float h, uint32_t tint);
void lx_ui_bar(float x, float y, float w, float h, float value01, uint32_t fill, uint32_t background); /* health, loading */
bool lx_ui_button(const char *label, float x, float y, float w, float h);   /* true on the frame it's clicked or tapped */
LxVec2 lx_ui_pointer(void);                     /* mouse / first touch, in canvas units */
bool lx_ui_pointer_over(void);                  /* over a button: don't treat the click as gameplay */
/* project a world position (e.g. above an enemy's head) to canvas units with the scene's main camera */
bool lx_ui_world_to_canvas(LxScene *s, LxVec3 world, LxVec2 *out);

/* ===================================================================
 *  Application runtime
 * =================================================================== */
typedef struct LxAppDesc {
    const char *title;
    int width, height;
    const char *project_dir;   /* folder containing project.lxproj (NULL = no project) */
    const char *start_scene;   /* overrides project startup scene */
    void (*on_init)(void);     /* register behaviours here */
    void (*on_start)(LxScene *scene);
    void (*on_update)(LxScene *scene, float dt);
    void (*on_shutdown)(void);
} LxAppDesc;

int lx_run(const LxAppDesc *desc);
LxScene *lx_active_scene(void);
bool lx_load_scene(const char *ref); /* switches scene at end of frame */
void lx_quit(void);
const LxProject *lx_active_project(void);

#ifdef __cplusplus
}
#endif
#endif
