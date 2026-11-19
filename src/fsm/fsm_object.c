/* Objects: the Game Object, Object, Component, Instantiate, Tags, Messages and Resources categories.
 *
 * "Object" in a graph is an entity in the scene. Unity's GameObject/Transform/Component split does not
 * exist here — an entity simply has components — so GameObjectGetTransform and friends have no
 * counterpart and are not needed. Components are named by Luxora's own kinds (Camera, Light, Mesh
 * Renderer, Collider, Rigidbody and so on) rather than by an arbitrary type string. */
#include "fsm_actions.h"

#define FSM_COMPONENTS "Camera|Light|Mesh Renderer|Audio Source|Audio Listener|Behaviour|Collider|Rigidbody|Character Controller|Animator|State Graph|Reflection Probe|Crowd|Bake Volume"

static LxComponentBits fsm_component_bit(int i) {
    static const LxComponentBits bits[] = {
        LX_COMP_CAMERA, LX_COMP_LIGHT, LX_COMP_MESH, LX_COMP_AUDIO_SOURCE, LX_COMP_AUDIO_LISTENER,
        LX_COMP_BEHAVIOUR, LX_COMP_COLLIDER, LX_COMP_RIGIDBODY, LX_COMP_CHARACTER, LX_COMP_ANIMATOR,
        LX_COMP_STATE_GRAPH, LX_COMP_REFLECTION_PROBE, LX_COMP_CROWD, LX_COMP_BAKE_VOLUME,
    };
    int n = (int)(sizeof bits / sizeof bits[0]);
    return bits[(i < 0 || i >= n) ? 0 : i];
}

/* ==========================================================================================
 *  finding objects
 * ========================================================================================== */
static void o_get_owner(LxFsmCtx *c) { lx_fsm_set_object(c, 0, c->self); }
static const LxFsmActionDesc A_O_OWNER = {"Get Owner", "Game Object", "The object this graph is running on.", "owner -> {0}",
    {POUT("Store Object", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, o_get_owner, o_get_owner, NULL};

static void o_find(LxFsmCtx *c) {
    char name[LX_NAME_MAX], tag[LX_NAME_MAX];
    lx_strcpy(name, sizeof name, lx_fsm_string(c, 0));
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 1));
    LxEntity found = 0;
    if (name[0]) found = lx_entity_find(c->scene, name);
    if (!found && tag[0]) found = lx_entity_find_with_tag(c->scene, tag);
    lx_fsm_set_object(c, 2, found);
    lx_fsm_set_bool(c, 3, found != 0);
    fsm_send(c, found ? 4 : 5);
}
static const LxFsmActionDesc A_O_FIND = {"Find Object", "Game Object", "Looks for an object by name, by tag, or by both.", "find \"{0}\" / \"{1}\" -> {2}",
    {PT("Name", LX_FSM_STRING, "", "leave empty to search by tag only"), PT("Tag", LX_FSM_STRING, "", "used when Name is empty or finds nothing"),
     POUT("Store Object", LX_FSM_OBJECT), POPT("Store Found", LX_FSM_BOOL), PEV("Found Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, o_find, o_find, NULL};

static void o_find_with_tag(LxFsmCtx *c) {
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 0));
    LxArray a = lx_fsm_array(c, 1);
    LxEntity found[512];
    int n = lx_entities_with_tag(c->scene, tag, found, 512);
    if (lx_array_valid(a)) {
        lx_array_clear(a);
        for (int i = 0; i < n; i++) lx_array_add_object(a, found[i]);
    }
    lx_fsm_set_int(c, 2, n);
    lx_fsm_set_object(c, 3, n > 0 ? found[0] : 0);
    fsm_send(c, n > 0 ? 4 : 5);
}
static const LxFsmActionDesc A_O_FIND_ALL_TAG = {"Find All With Tag", "Game Object", "Collects every object carrying a tag into a list.",
    "all \"{0}\" -> {1}",
    {P("Tag", LX_FSM_STRING, "Enemy"), PARRT("Store List", "an Object array"), POPT("Store Count", LX_FSM_INT),
     POPT("Store First", LX_FSM_OBJECT), PEV("Found Event"), PEV("None Found Event")},
    0, 0, LX_FSM_ONCE, o_find_with_tag, o_find_with_tag, NULL};

static void o_find_closest(LxFsmCtx *c) {
    LxEntity from = fsm_obj(c, 0);
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 1));
    LxVec3 at = from ? lx_entity_world_position(c->scene, from) : lx_fsm_vec3(c, 2);
    float dist = 0;
    float max = lx_fsm_float(c, 3);
    LxEntity found = lx_find_closest_with_tag(c->scene, at, tag, max > 0 ? max : 1e9f, &dist);
    lx_fsm_set_object(c, 4, found);
    lx_fsm_set_float(c, 5, found ? dist : 0);
    fsm_send(c, found ? 6 : 7);
}
static const LxFsmActionDesc A_O_FIND_CLOSEST = {"Find Closest With Tag", "Game Object", "The nearest object with a tag: the closest enemy, the nearest pickup.",
    "closest \"{1}\" to {0} -> {4}",
    {POBJ("From Object"), P("Tag", LX_FSM_STRING, "Enemy"), PT("From Position", LX_FSM_VEC3, "0 0 0", "used when From Object is None"),
     PT("Max Distance", LX_FSM_FLOAT, "0", "0 = no limit"), POUT("Store Object", LX_FSM_OBJECT),
     POPT("Store Distance", LX_FSM_FLOAT), PEV("Found Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, o_find_closest, o_find_closest, NULL};

static void o_random_tag(LxFsmCtx *c) {
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 0));
    LxEntity e = lx_random_with_tag(c->scene, tag);
    lx_fsm_set_object(c, 1, e);
    fsm_send(c, e ? 2 : 3);
}
static const LxFsmActionDesc A_O_RANDOM_TAG = {"Find Random With Tag", "Game Object", "Picks one tagged object at random: a random spawn point, a random target.",
    "random \"{0}\" -> {1}",
    {P("Tag", LX_FSM_STRING, "SpawnPoint"), POUT("Store Object", LX_FSM_OBJECT), PEV("Found Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, o_random_tag, o_random_tag, NULL};

static void o_count_tag(LxFsmCtx *c) {
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 0));
    int n = lx_count_with_tag(c->scene, tag);
    lx_fsm_set_int(c, 1, n);
    fsm_send(c, n == 0 ? 2 : 3);
}
static const LxFsmActionDesc A_O_COUNT_TAG = {"Count With Tag", "Game Object", "How many objects carry a tag: are all the enemies dead yet?",
    "count \"{0}\" -> {1}",
    {P("Tag", LX_FSM_STRING, "Enemy"), POUT("Store Count", LX_FSM_INT), PEV("None Left Event"), PEV("Some Left Event")},
    0, 0, LX_FSM_EVERY_FRAME, o_count_tag, o_count_tag, NULL};

/* ==========================================================================================
 *  hierarchy
 * ========================================================================================== */
static void o_get_parent(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxEntity p = e ? lx_entity_parent(c->scene, e) : 0;
    lx_fsm_set_object(c, 1, p);
    fsm_send(c, p ? 2 : 3);
}
static const LxFsmActionDesc A_O_GET_PARENT = {"Get Parent", "Game Object", "The object this one is attached to.", "parent of {0} -> {1}",
    {POBJ("Object"), POUT("Store Parent", LX_FSM_OBJECT), PEV("Has Parent Event"), PEV("No Parent Event")},
    0, 0, LX_FSM_ONCE, o_get_parent, o_get_parent, NULL};

static void o_set_parent(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 world = lx_entity_world_position(c->scene, e);
    LxQuat rot = lx_entity_world_rotation(c->scene, e);
    lx_entity_set_parent(c->scene, e, fsm_obj(c, 1));
    if (lx_fsm_bool(c, 2)) lx_entity_set_world_pose(c->scene, e, world, rot);
}
static const LxFsmActionDesc A_O_SET_PARENT = {"Set Parent", "Game Object", "Attaches an object to another (or to nothing). Keep World Position stops it jumping.",
    "{0} -> child of {1}",
    {POBJ("Object"), POBJNT("New Parent", "None detaches it"), PT("Keep World Position", LX_FSM_BOOL, "true", "stay where it is on screen")},
    0, 0, LX_FSM_ONCE, o_set_parent, o_set_parent, NULL};

static void o_get_child(LxFsmCtx *c) {
    LxEntity parent = fsm_obj(c, 0);
    if (!parent) { fsm_send(c, 5); return; }
    const char *name = lx_fsm_string(c, 1);
    LxEntity found = 0;
    int count = 0;
    if (name && name[0]) {
        found = lx_entity_find_child(c->scene, parent, name);
        for (LxEntity k = lx_entity_first_child(c->scene, parent); k; k = lx_entity_next_sibling(c->scene, k)) count++;
    } else {
        int want = lx_fsm_int(c, 2);
        for (LxEntity k = lx_entity_first_child(c->scene, parent); k; k = lx_entity_next_sibling(c->scene, k)) {
            if (count == want) found = k;
            count++;
        }
        if (want < 0 && count > 0) {   /* -1 is the last child */
            int target = count + want;
            int i = 0;
            for (LxEntity k = lx_entity_first_child(c->scene, parent); k; k = lx_entity_next_sibling(c->scene, k), i++)
                if (i == target) found = k;
        }
    }
    lx_fsm_set_object(c, 3, found);
    lx_fsm_set_int(c, 4, count);
    fsm_send(c, found ? 6 : 5);
}
static const LxFsmActionDesc A_O_GET_CHILD = {"Get Child", "Game Object", "A child by name, or by position in the list. Also reports how many children there are.",
    "child of {0} -> {3}",
    {POBJ("Object"), PT("Child Name", LX_FSM_STRING, "", "leave empty to use the index instead"),
     PT("Child Index", LX_FSM_INT, "0", "0 is the first; -1 is the last"), POUT("Store Child", LX_FSM_OBJECT),
     POPT("Store Child Count", LX_FSM_INT), PEV("Not Found Event"), PEV("Found Event")},
    0, 0, LX_FSM_ONCE, o_get_child, o_get_child, NULL};

static void o_get_children(LxFsmCtx *c) {
    LxEntity parent = fsm_obj(c, 0);
    LxArray a = lx_fsm_array(c, 1);
    int n = 0;
    if (lx_array_valid(a)) lx_array_clear(a);
    for (LxEntity k = parent ? lx_entity_first_child(c->scene, parent) : 0; k; k = lx_entity_next_sibling(c->scene, k)) {
        if (lx_array_valid(a)) lx_array_add_object(a, k);
        n++;
    }
    lx_fsm_set_int(c, 2, n);
}
static const LxFsmActionDesc A_O_GET_CHILDREN = {"Get Children", "Game Object", "Collects an object's children into a list.", "children of {0} -> {1}",
    {POBJ("Object"), PARRT("Store List", "an Object array"), POPT("Store Count", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, o_get_children, o_get_children, NULL};

static void o_destroy_children(LxFsmCtx *c) {
    LxEntity parent = fsm_obj(c, 0);
    if (!parent) return;
    LxEntity kids[256];
    int n = 0;
    for (LxEntity k = lx_entity_first_child(c->scene, parent); k && n < 256; k = lx_entity_next_sibling(c->scene, k)) kids[n++] = k;
    for (int i = 0; i < n; i++) if (lx_entity_valid(c->scene, kids[i])) lx_entity_destroy(c->scene, kids[i]);
    lx_fsm_set_int(c, 1, n);
}
static const LxFsmActionDesc A_O_DESTROY_CHILDREN = {"Destroy Children", "Game Object", "Removes every child of an object: clearing a list, emptying a container.",
    "destroy children of {0}",
    {POBJ("Object"), POPT("Store Count", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, o_destroy_children, o_destroy_children, NULL};

/* ==========================================================================================
 *  name, tag, active
 * ========================================================================================== */
static void o_get_name(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    lx_fsm_set_string(c, 1, e ? lx_entity_name(c->scene, e) : "");
    lx_fsm_set_string(c, 2, e ? lx_entity_tag(c->scene, e) : "");
    lx_fsm_set_bool(c, 3, e && lx_entity_active(c->scene, e));
}
static const LxFsmActionDesc A_O_GET_NAME = {"Get Object Info", "Game Object", "An object's name, its tag and whether it is switched on.", "info of {0}",
    {POBJ("Object"), POPT("Store Name", LX_FSM_STRING), POPT("Store Tag", LX_FSM_STRING), POPT("Store Is Active", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, o_get_name, o_get_name, NULL};

static void o_set_name(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (e) lx_entity_set_name(c->scene, e, lx_fsm_string(c, 1));
}
static const LxFsmActionDesc A_O_SET_NAME = {"Set Object Name", "Game Object", "Renames an object.", "{0}.name = \"{1}\"",
    {POBJ("Object"), P("Name", LX_FSM_STRING, "")}, 0, 0, LX_FSM_ONCE, o_set_name, o_set_name, NULL};

static void o_set_tag(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (e) lx_entity_set_tag(c->scene, e, lx_fsm_string(c, 1));
}
static const LxFsmActionDesc A_O_SET_TAG = {"Set Tag", "Game Object", "Gives an object a tag.", "{0}.tag = \"{1}\"",
    {POBJ("Object"), P("Tag", LX_FSM_STRING, "")}, 0, 0, LX_FSM_ONCE, o_set_tag, o_set_tag, NULL};

static void o_has_tag(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 1));
    bool has = e && lx_entity_has_tag(c->scene, e, tag);
    lx_fsm_set_bool(c, 2, has);
    fsm_send(c, has ? 3 : 4);
}
static const LxFsmActionDesc A_O_HAS_TAG = {"Compare Tag", "Game Object", "True when an object carries a tag.", "{0} tagged \"{1}\"?",
    {POBJ("Object"), P("Tag", LX_FSM_STRING, "Player"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, o_has_tag, o_has_tag, NULL};

static void o_set_active(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    int mode = lx_fsm_int(c, 1);
    bool on = mode == 0 ? true : (mode == 1 ? false : !lx_entity_active(c->scene, e));
    lx_entity_set_active(c->scene, e, on);
    if (lx_fsm_bool(c, 2))
        for (LxEntity k = lx_entity_first_child(c->scene, e); k; k = lx_entity_next_sibling(c->scene, k))
            lx_entity_set_active(c->scene, k, on);
}
static const LxFsmActionDesc A_O_SET_ACTIVE = {"Set Object Active", "Game Object", "Switches an object on or off, or flips it.", "{0} active: {1}",
    {POBJ("Object"), PENUM("Set", "Activate|Deactivate|Toggle", "0"),
     PT("Include Children", LX_FSM_BOOL, "false", "do the same to its direct children")},
    0, 0, LX_FSM_ONCE, o_set_active, o_set_active, NULL};

static void o_is_active(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    bool on = e && lx_entity_active(c->scene, e);
    lx_fsm_set_bool(c, 1, on);
    fsm_send(c, on ? 2 : 3);
}
static const LxFsmActionDesc A_O_IS_ACTIVE = {"Is Object Active", "Game Object", "True when an object is switched on.", "{0} active?",
    {POBJ("Object"), POPT("Store Result", LX_FSM_BOOL), PEV("Active Event"), PEV("Inactive Event")},
    0, 0, LX_FSM_ONCE, o_is_active, o_is_active, NULL};

static void o_exists(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    bool ok = e && lx_entity_valid(c->scene, e);
    lx_fsm_set_bool(c, 1, ok);
    fsm_send(c, ok ? 2 : 3);
}
static const LxFsmActionDesc A_O_EXISTS = {"Object Exists", "Object", "True when an Object variable points at something that is still in the scene.",
    "{0} exists?",
    {POBJN("Object"), POPT("Store Result", LX_FSM_BOOL), PEV("Exists Event"), PEV("Is None Event")},
    0, 0, LX_FSM_ONCE, o_exists, o_exists, NULL};

static void o_equals(LxFsmCtx *c) {
    bool eq = fsm_obj(c, 0) == fsm_obj(c, 1);
    lx_fsm_set_bool(c, 2, eq);
    fsm_send(c, eq ? 3 : 4);
}
static const LxFsmActionDesc A_O_EQUALS = {"Object Equals", "Object", "True when two Object variables point at the same thing.", "{0} == {1}?",
    {POBJN("Object 1"), POBJN("Object 2"), POPT("Store Result", LX_FSM_BOOL), PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, o_equals, o_equals, NULL};

static void o_select(LxFsmCtx *c) { lx_fsm_set_object(c, 3, lx_fsm_bool(c, 0) ? fsm_obj(c, 1) : fsm_obj(c, 2)); }
static const LxFsmActionDesc A_O_SELECT = {"Object Select Value", "Object", "Picks one of two objects depending on a bool.", "{3} = {0} ? {1} : {2}",
    {P("Condition", LX_FSM_BOOL, "true"), POBJN("If True"), POBJN("If False"), POUT("Store Result", LX_FSM_OBJECT)},
    0, 0, LX_FSM_ONCE, o_select, o_select, NULL};

/* ==========================================================================================
 *  components
 * ========================================================================================== */
static void o_has_component(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    bool has = e && (lx_entity_components(c->scene, e) & (uint32_t)fsm_component_bit(lx_fsm_int(c, 1))) != 0;
    lx_fsm_set_bool(c, 2, has);
    fsm_send(c, has ? 3 : 4);
}
static const LxFsmActionDesc A_O_HAS_COMPONENT = {"Has Component", "Component", "True when an object has a given kind of component.", "{0} has {1}?",
    {POBJ("Object"), PENUM("Component", FSM_COMPONENTS, "0"), POPT("Store Result", LX_FSM_BOOL),
     PEV("Has Event"), PEV("Missing Event")},
    0, 0, LX_FSM_ONCE, o_has_component, o_has_component, NULL};

static void o_add_component(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    switch (lx_fsm_int(c, 1)) {
    case 0: lx_add_camera(c->scene, e); break;
    case 1: lx_add_light(c->scene, e, LX_LIGHT_POINT); break;
    case 2: lx_add_mesh(c->scene, e, "builtin:cube", ""); break;
    case 3: lx_add_audio_source(c->scene, e, ""); break;
    case 4: lx_add_audio_listener(c->scene, e); break;
    case 6: lx_add_collider(c->scene, e, LX_COLLIDER_BOX); break;
    case 7: lx_add_rigidbody(c->scene, e); break;
    case 8: lx_add_character(c->scene, e); break;
    case 11: lx_add_reflection_probe(c->scene, e); break;
    case 13: lx_add_bake_volume(c->scene, e); break;
    default: break;   /* Behaviour, Animator, State Graph and Crowd need a name or asset to attach */
    }
}
static const LxFsmActionDesc A_O_ADD_COMPONENT = {"Add Component", "Component", "Adds a component to an object at run time, with its default settings.",
    "add {1} to {0}",
    {POBJ("Object"), PENUMT("Component", FSM_COMPONENTS, "6", "Behaviour, Animator, State Graph and Crowd need an asset, so add those in the inspector")},
    0, 0, LX_FSM_ONCE, o_add_component, o_add_component, NULL};

static void o_remove_component(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (e) lx_remove_component(c->scene, e, fsm_component_bit(lx_fsm_int(c, 1)));
}
static const LxFsmActionDesc A_O_REMOVE_COMPONENT = {"Remove Component", "Component", "Takes a component off an object.", "remove {1} from {0}",
    {POBJ("Object"), PENUM("Component", FSM_COMPONENTS, "6")}, 0, 0, LX_FSM_ONCE, o_remove_component, o_remove_component, NULL};

/* ==========================================================================================
 *  creating and destroying
 * ========================================================================================== */
static void o_spawn(LxFsmCtx *c) {
    if (!lx_fsm_spawn_allowed(c->scene)) return;      /* per-frame budget: a runaway graph cannot flood the scene */
    const char *prefab = lx_fsm_asset(c, 0);
    if (!prefab || !prefab[0]) { fsm_send(c, 7); return; }
    LxEntity at = fsm_obj(c, 1);
    LxVec3 pos = at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 2);
    if (at && !lx_fsm_is_none(c, 2)) pos = lx_add3(pos, lx_fsm_vec3(c, 2));
    LxEntity e = lx_prefab_instantiate(c->scene, prefab, pos, fsm_obj(c, 3));
    if (!e) { fsm_send(c, 7); return; }
    if (!lx_fsm_is_none(c, 4)) lx_entity_set_rotation(c->scene, e, lx_fsm_quat(c, 4));
    else if (at) lx_entity_set_rotation(c->scene, e, lx_entity_world_rotation(c->scene, at));
    lx_fsm_set_object(c, 5, e);
    fsm_send(c, 6);
}
static const LxFsmActionDesc A_O_SPAWN = {"Create Object", "Game Object", "Spawns a copy of a prefab: bullets, enemies, pickups, effects.",
    "spawn {0} -> {5}",
    {PASSET("Prefab", "prefab"), POBJNT("At Object", "spawn where this object is"),
     PT("Position", LX_FSM_VEC3, "0 0 0", "the world position, or an offset when At Object is set"),
     POBJNT("Parent", "attach the new object to this one"), PIN_OPT("Rotation", LX_FSM_QUAT),
     POPT("Store New Object", LX_FSM_OBJECT), PEV("Spawned Event"), PEV("Failed Event")},
    0, 0, LX_FSM_ONCE, o_spawn, o_spawn, NULL};

static void o_create_primitive(LxFsmCtx *c) {
    if (!lx_fsm_spawn_allowed(c->scene)) return;
    LxPrimitive p = (LxPrimitive)lx_fsm_int(c, 0);
    const char *name = lx_fsm_string(c, 1);
    LxEntity e = lx_spawn_primitive(c->scene, p, (name && name[0]) ? name : "Object", lx_fsm_vec3(c, 2));
    if (!e) return;
    if (!lx_fsm_is_none(c, 3)) lx_entity_set_scale(c->scene, e, lx_fsm_vec3(c, 3));
    if (!lx_fsm_is_none(c, 4)) lx_entity_set_parent(c->scene, e, fsm_obj(c, 4));
    lx_fsm_set_object(c, 5, e);
}
static const LxFsmActionDesc A_O_CREATE_PRIMITIVE = {"Create Primitive", "Game Object", "Makes a cube, sphere, plane, cylinder or capsule from nothing: blockouts and debug shapes.",
    "create {0} -> {5}",
    {PENUM("Shape", "Cube|Sphere|Plane|Cylinder|Capsule", "0"), P("Name", LX_FSM_STRING, "Object"),
     P("Position", LX_FSM_VEC3, "0 0 0"), PIN_OPT("Scale", LX_FSM_VEC3), POBJN("Parent"), POPT("Store New Object", LX_FSM_OBJECT)},
    0, 0, LX_FSM_ONCE, o_create_primitive, o_create_primitive, NULL};

static void o_duplicate(LxFsmCtx *c) {
    if (!lx_fsm_spawn_allowed(c->scene)) return;
    LxEntity src = fsm_obj(c, 0);
    if (!src) return;
    LxEntity e = lx_entity_duplicate(c->scene, src);
    if (!e) return;
    if (!lx_fsm_is_none(c, 1)) lx_entity_set_position(c->scene, e, lx_fsm_vec3(c, 1));
    if (!lx_fsm_is_none(c, 2)) lx_entity_set_parent(c->scene, e, fsm_obj(c, 2));
    lx_fsm_set_object(c, 3, e);
}
static const LxFsmActionDesc A_O_DUPLICATE = {"Duplicate Object", "Game Object", "Copies an object that is already in the scene, children and all.",
    "duplicate {0} -> {3}",
    {POBJ("Object"), PIN_OPT("Position", LX_FSM_VEC3), POBJN("Parent"), POPT("Store New Object", LX_FSM_OBJECT)},
    0, 0, LX_FSM_ONCE, o_duplicate, o_duplicate, NULL};

typedef struct { float t; bool armed; } FsmDestroy;
static void o_destroy_enter(LxFsmCtx *c) {
    FsmDestroy *d = c->state;
    d->armed = true;
    if (lx_fsm_float(c, 1) <= 0) {
        LxEntity e = fsm_obj(c, 0);
        if (e) lx_entity_destroy(c->scene, e);
        d->armed = false;
        lx_fsm_finish(c);
    }
}
static void o_destroy_update(LxFsmCtx *c) {
    FsmDestroy *d = c->state;
    if (!d->armed) { lx_fsm_finish(c); return; }
    d->t += c->dt;
    if (d->t >= lx_fsm_float(c, 1)) {
        LxEntity e = fsm_obj(c, 0);
        if (e) lx_entity_destroy(c->scene, e);
        d->armed = false;
        lx_fsm_finish(c);
    }
}
static const LxFsmActionDesc A_O_DESTROY = {"Destroy Object", "Game Object", "Removes an object from the scene, with its children. A delay lets effects finish first.",
    "destroy {0} after {1}s",
    {POBJ("Object"), PT("Delay", LX_FSM_FLOAT, "0", "seconds; leaving the state first cancels it")},
    sizeof(FsmDestroy), LX_FSM_A_CONTINUOUS, LX_FSM_ONCE, o_destroy_enter, o_destroy_update, NULL};

/* ==========================================================================================
 *  messages
 * ========================================================================================== */
static void o_send_message(LxFsmCtx *c) {
    char msg[LX_NAME_MAX];
    lx_strcpy(msg, sizeof msg, lx_fsm_string(c, 1));
    if (!msg[0]) return;
    float v = lx_fsm_float(c, 2);
    float delay = lx_fsm_float(c, 3);
    LxEntity t = fsm_obj(c, 0);
    if (delay > 0) { if (t) lx_send_message_after(c->scene, t, msg, v, delay); return; }
    bool got = t && lx_send_message(c->scene, t, msg, v, c->self);
    lx_fsm_set_bool(c, 4, got);
    fsm_send(c, got ? 5 : 6);
}
static const LxFsmActionDesc A_O_SEND_MESSAGE = {"Send Message", "Game Object", "Calls a behaviour's on_message on another object: how a graph talks to C code.",
    "{0} <- \"{1}\"",
    {POBJ("Object"), P("Message", LX_FSM_STRING, "Damage"), P("Value", LX_FSM_FLOAT, "0"),
     PT("Delay", LX_FSM_FLOAT, "0", "seconds; 0 = now"), POPT("Store Received", LX_FSM_BOOL),
     PEV("Received Event"), PEV("No Receiver Event")},
    0, 0, LX_FSM_ONCE, o_send_message, o_send_message, NULL};

static void o_broadcast(LxFsmCtx *c) {
    char msg[LX_NAME_MAX], tag[LX_NAME_MAX];
    lx_strcpy(msg, sizeof msg, lx_fsm_string(c, 0));
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 1));
    if (!msg[0]) return;
    float v = lx_fsm_float(c, 2);
    int n = tag[0] ? lx_send_message_to_tag(c->scene, tag, msg, v, c->self) : lx_broadcast(c->scene, msg, v, c->self);
    lx_fsm_set_int(c, 3, n);
}
static const LxFsmActionDesc A_O_BROADCAST = {"Broadcast Message", "Game Object", "Sends a message to every object, or to everything carrying a tag.",
    "broadcast \"{0}\"",
    {P("Message", LX_FSM_STRING, ""), PT("Tag", LX_FSM_STRING, "", "empty = every object in the scene"),
     P("Value", LX_FSM_FLOAT, "0"), POPT("Store Receivers", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, o_broadcast, o_broadcast, NULL};

/* ==========================================================================================
 *  assets
 * ========================================================================================== */
static void o_asset_exists(LxFsmCtx *c) {
    const char *ref = lx_fsm_string(c, 0);
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref ? ref : "");
    bool ok = lx_file_exists(path);
    lx_fsm_set_bool(c, 1, ok);
    fsm_send(c, ok ? 2 : 3);
}
static const LxFsmActionDesc A_O_ASSET_EXISTS = {"Asset Exists", "Resources", "True when an asset is really there: check before loading a level or a model.",
    "\"{0}\" exists?",
    {P("Asset Path", LX_FSM_STRING, ""), POPT("Store Result", LX_FSM_BOOL), PEV("Exists Event"), PEV("Missing Event")},
    0, 0, LX_FSM_ONCE, o_asset_exists, o_asset_exists, NULL};


/* An object's layer, by name. Numbers are what physics works in, but a graph should not have to know
 * that layer 6 is the one the ground is on - and it would be wrong the moment someone renumbered it. */
static void o_set_layer(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    const char *name = lx_fsm_string(c, 1);
    int layer = lx_layer_find(name);
    if (layer < 0) { fsm_send(c, 3); return; }
    lx_entity_set_layer(c->scene, e, layer);
    if (lx_fsm_bool(c, 2))
        for (LxEntity ch = lx_entity_first_child(c->scene, e); ch; ch = lx_entity_next_sibling(c->scene, ch))
            lx_entity_set_layer(c->scene, ch, layer);
}
static const LxFsmActionDesc A_O_SET_LAYER = {"Set Layer", "Game Object",
    "Moves an object onto a layer, which decides what it collides with and which searches find it.",
    "{0} -> layer {1}",
    {POBJ("Object"), PT("Layer", LX_FSM_STRING, "Default", "the layer's name, from Project Settings"),
     PT("Include Children", LX_FSM_BOOL, "false", "move everything directly under it as well"),
     PEV("No Such Layer Event")},
    0, 0, LX_FSM_ONCE, o_set_layer, o_set_layer, NULL};

static void o_get_layer(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    int layer = e ? lx_entity_layer(c->scene, e) : 0;
    lx_fsm_set_string(c, 1, e ? lx_layer_name(layer) : "");
    lx_fsm_set_int(c, 2, layer);
    const char *want = lx_fsm_string(c, 3);
    if (want && want[0]) fsm_send(c, lx_layer_find(want) == layer && e ? 4 : 5);
}
static const LxFsmActionDesc A_O_GET_LAYER = {"Get Layer", "Game Object",
    "Which layer an object is on, by name and by number, and whether it is the one you were asking about.",
    "layer of {0}",
    {POBJ("Object"), POPT("Store Layer Name", LX_FSM_STRING), POPT("Store Layer Number", LX_FSM_INT),
     PT("Is It", LX_FSM_STRING, "", "a layer name to compare against; empty sends no event"),
     PEV("Yes Event"), PEV("No Event")},
    0, 0, LX_FSM_EVERY_FRAME, o_get_layer, o_get_layer, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_object(void) {
    static const LxFsmActionDesc *all[] = {
        &A_O_OWNER, &A_O_FIND, &A_O_SET_LAYER, &A_O_GET_LAYER, &A_O_FIND_ALL_TAG, &A_O_FIND_CLOSEST, &A_O_RANDOM_TAG, &A_O_COUNT_TAG,
        &A_O_GET_PARENT, &A_O_SET_PARENT, &A_O_GET_CHILD, &A_O_GET_CHILDREN, &A_O_DESTROY_CHILDREN,
        &A_O_GET_NAME, &A_O_SET_NAME, &A_O_SET_TAG, &A_O_HAS_TAG, &A_O_SET_ACTIVE, &A_O_IS_ACTIVE,
        &A_O_EXISTS, &A_O_EQUALS, &A_O_SELECT,
        &A_O_HAS_COMPONENT, &A_O_ADD_COMPONENT, &A_O_REMOVE_COMPONENT,
        &A_O_SPAWN, &A_O_CREATE_PRIMITIVE, &A_O_DUPLICATE, &A_O_DESTROY,
        &A_O_SEND_MESSAGE, &A_O_BROADCAST, &A_O_ASSET_EXISTS,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
