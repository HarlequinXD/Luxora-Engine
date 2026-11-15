/* The game around the graph: the Scene Manager, Application, Save System and Player Prefs categories,
 * plus checkpoints and the inventory.
 *
 * Luxora keeps one scene loaded at a time, so there is no additive or multi-scene loading. Save data is
 * a set of named values written to disk at the end of any frame they change. */
#include "fsm_actions.h"

/* ==========================================================================================
 *  scenes
 * ========================================================================================== */
static void sn_load(LxFsmCtx *c) {
    const char *ref = lx_fsm_asset(c, 0);
    if (!ref || !ref[0]) return;
    float fade = lx_fsm_float(c, 1);
    if (fade > 0) lx_load_scene_faded(ref, fade, lx_fsm_color(c, 2));
    else lx_load_scene(ref);
}
static const LxFsmActionDesc A_SN_LOAD = {"Load Scene", "Scene", "Switches to another scene at the end of the frame, optionally behind a fade.",
    "load {0}",
    {PASSET("Scene", "scene"), PT("Fade Seconds", LX_FSM_FLOAT, "0", "0 = switch straight away"),
     PT("Fade Color", LX_FSM_COLOR, "0 0 0 1", "the colour the screen fades through")},
    0, 0, LX_FSM_ONCE, sn_load, sn_load, NULL};

static void sn_reload(LxFsmCtx *c) {
    const char *name = lx_scene_name(c->scene);
    if (!name || !name[0]) return;
    char ref[LX_REF_MAX];
    snprintf(ref, sizeof ref, "Scenes/%s.lxscene", name);
    float fade = lx_fsm_float(c, 0);
    if (fade > 0) lx_load_scene_faded(ref, fade, lx_fsm_color(c, 1));
    else lx_load_scene(ref);
}
static const LxFsmActionDesc A_SN_RELOAD = {"Reload Scene", "Scene", "Loads the current scene again from scratch: restarting a level after dying.",
    "reload scene",
    {PT("Fade Seconds", LX_FSM_FLOAT, "0.3", "0 = reload straight away"), P("Fade Color", LX_FSM_COLOR, "0 0 0 1")},
    0, 0, LX_FSM_ONCE, sn_reload, sn_reload, NULL};

static void sn_get(LxFsmCtx *c) {
    lx_fsm_set_string(c, 0, lx_scene_name(c->scene));
    lx_fsm_set_int(c, 1, lx_scene_entity_count(c->scene));
}
static const LxFsmActionDesc A_SN_GET = {"Get Scene Info", "Scene", "The name of the scene that is loaded and how many objects are in it.",
    "scene -> {0}",
    {POPT("Store Scene Name", LX_FSM_STRING), POPT("Store Object Count", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, sn_get, sn_get, NULL};

static void sn_quit(LxFsmCtx *c) {
    (void)c;
    lx_quit();
}
static const LxFsmActionDesc A_SN_QUIT = {"Quit Game", "Application", "Closes the game. In the editor's Play mode this stops playing instead.",
    "quit",
    {{0}}, 0, 0, LX_FSM_ONCE, sn_quit, NULL, NULL};

static void sn_app_info(LxFsmCtx *c) {
    const LxProject *p = lx_active_project();
    lx_fsm_set_string(c, 0, p ? p->name : "");
    lx_fsm_set_string(c, 1, p ? p->version : "");
    lx_fsm_set_string(c, 2, p ? p->company : "");
    lx_fsm_set_int(c, 3, (int)lx_game_platform());
    lx_fsm_set_string(c, 4, lx_platform_name(lx_game_platform()));
    bool mobile = lx_game_platform() == LX_PLATFORM_ANDROID;
    lx_fsm_set_bool(c, 5, mobile);
    if (mobile) { const char *e = lx_fsm_event(c, 6); if (e && e[0]) lx_fsm_send(c, e); }
    else { const char *e = lx_fsm_event(c, 7); if (e && e[0]) lx_fsm_send(c, e); }
}
static const LxFsmActionDesc A_SN_APP_INFO = {"Get Application Info", "Application", "The project's name and version and which platform the game is running on: for version labels and platform-specific behaviour.",
    "app info",
    {POPT("Store Product Name", LX_FSM_STRING), POPT("Store Version", LX_FSM_STRING), POPT("Store Company", LX_FSM_STRING),
     POPTT("Store Platform", LX_FSM_INT, "0 Windows, 1 Linux, 2 Android"), POPT("Store Platform Name", LX_FSM_STRING),
     POPT("Store Is Mobile", LX_FSM_BOOL), PEV("Mobile Event"), PEV("Desktop Event")},
    0, 0, LX_FSM_ONCE, sn_app_info, sn_app_info, NULL};

/* ==========================================================================================
 *  save data
 * ========================================================================================== */
static void sv_set(LxFsmCtx *c) {
    const char *key = lx_fsm_string(c, 0);
    if (!key || !key[0]) return;
    LxFsmValue *v = lx_fsm_param_value(c, 1);
    if (!v) return;
    switch (v->type) {
    case LX_FSM_FLOAT: lx_save_set_float(key, v->f); break;
    case LX_FSM_INT: case LX_FSM_ENUM: lx_save_set_int(key, v->i); break;
    case LX_FSM_BOOL: lx_save_set_int(key, v->b ? 1 : 0); break;
    default: {
        char buf[LX_REF_MAX];
        lx_fsm_value_format(v, buf, sizeof buf, NULL);
        lx_save_set_string(key, buf);
        break;
    }
    }
    if (lx_fsm_bool(c, 2)) lx_save_write();
}
static const LxFsmActionDesc A_SV_SET = {"Save Value", "Save Data", "Stores a value under a name so it survives the game being closed. Works for every variable type.",
    "save {0} = {1}",
    {P("Key", LX_FSM_STRING, "Score"), P("Value", LX_FSM_ANY, ""),
     PT("Write Now", LX_FSM_BOOL, "false", "save data is written at the end of the frame anyway")},
    0, 0, LX_FSM_ONCE, sv_set, sv_set, NULL};

static void sv_get(LxFsmCtx *c) {
    const char *key = lx_fsm_string(c, 0);
    bool has = key && key[0] && lx_save_has(key);
    LxFsmValue *v = lx_fsm_param_value(c, 1);
    if (v && has) {
        switch (v->type) {
        case LX_FSM_FLOAT: v->f = lx_save_get_float(key, v->f); break;
        case LX_FSM_INT: case LX_FSM_ENUM: v->i = lx_save_get_int(key, v->i); break;
        case LX_FSM_BOOL: v->b = lx_save_get_int(key, v->b ? 1 : 0) != 0; break;
        default: lx_strcpy(v->s, sizeof v->s, lx_save_get_string(key, v->s)); break;
        }
    }
    lx_fsm_set_bool(c, 2, has);
    fsm_send(c, has ? 3 : 4);
}
static const LxFsmActionDesc A_SV_GET = {"Load Value", "Save Data", "Reads a saved value back. When there is nothing saved the variable keeps whatever it already had, so set a default first.",
    "load {0} -> {1}",
    {P("Key", LX_FSM_STRING, "Score"), POUTT("Store Value", LX_FSM_ANY, "keeps its current value when nothing is saved"),
     POPT("Store Found", LX_FSM_BOOL), PEV("Found Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, sv_get, sv_get, NULL};

static void sv_has(LxFsmCtx *c) {
    const char *key = lx_fsm_string(c, 0);
    bool has = key && key[0] && lx_save_has(key);
    lx_fsm_set_bool(c, 1, has);
    fsm_send(c, has ? 2 : 3);
}
static const LxFsmActionDesc A_SV_HAS = {"Has Saved Value", "Save Data", "True when something has been saved under a name: is there a game to continue?",
    "saved \"{0}\"?",
    {P("Key", LX_FSM_STRING, "Score"), POPT("Store Result", LX_FSM_BOOL), PEV("Found Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, sv_has, sv_has, NULL};

static void sv_delete(LxFsmCtx *c) {
    if (lx_fsm_bool(c, 1)) { lx_save_clear(); lx_save_write(); return; }
    const char *key = lx_fsm_string(c, 0);
    if (key && key[0]) lx_save_delete(key);
}
static const LxFsmActionDesc A_SV_DELETE = {"Delete Saved Value", "Save Data", "Removes one saved value, or wipes the lot: a \"reset progress\" button.",
    "delete {0}",
    {P("Key", LX_FSM_STRING, "Score"), PT("Delete Everything", LX_FSM_BOOL, "false", "wipes all save data; there is no undo")},
    0, 0, LX_FSM_ONCE, sv_delete, sv_delete, NULL};

static void sv_globals(LxFsmCtx *c) {
    const char *slot = lx_fsm_string(c, 1);
    int n = lx_fsm_int(c, 0) == 0 ? lx_globals_save(slot) : lx_globals_load(slot);
    lx_fsm_set_int(c, 2, n);
    fsm_send(c, n > 0 ? 3 : 4);
}
static const LxFsmActionDesc A_SV_GLOBALS = {"Save Or Load Globals", "Save Data", "Writes every global variable into a save slot, or reads them all back: a whole save game in one action.",
    "{0} globals to \"{1}\"",
    {PENUM("Action", "Save|Load", "0"), PT("Slot", LX_FSM_STRING, "slot1", "lets you keep several save games"),
     POPT("Store Count", LX_FSM_INT), PEV("Success Event"), PEV("Failed Event")},
    0, 0, LX_FSM_ONCE, sv_globals, sv_globals, NULL};

/* ==========================================================================================
 *  checkpoints
 * ========================================================================================== */
static void cp_set(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (e) lx_checkpoint_set(c->scene, e);
}
static const LxFsmActionDesc A_CP_SET = {"Set Checkpoint", "Save Data", "Remembers where an object is, so Respawn can bring it back here after dying.",
    "checkpoint at {0}",
    {POBJ("Object")}, 0, 0, LX_FSM_ONCE, cp_set, cp_set, NULL};

static void cp_respawn(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    bool ok = e && lx_checkpoint_respawn(c->scene, e);
    lx_fsm_set_bool(c, 1, ok);
    fsm_send(c, ok ? 2 : 3);
}
static const LxFsmActionDesc A_CP_RESPAWN = {"Respawn At Checkpoint", "Save Data", "Puts an object back at the last checkpoint for this scene.",
    "respawn {0}",
    {POBJ("Object"), POPT("Store Success", LX_FSM_BOOL), PEV("Respawned Event"), PEV("No Checkpoint Event")},
    0, 0, LX_FSM_ONCE, cp_respawn, cp_respawn, NULL};

/* ==========================================================================================
 *  inventory
 * ========================================================================================== */
static void iv_change(LxFsmCtx *c) {
    char item[LX_NAME_MAX];
    lx_strcpy(item, sizeof item, lx_fsm_string(c, 1));
    if (!item[0]) return;
    int count = lx_fsm_int(c, 2);
    bool ok = true;
    if (lx_fsm_int(c, 0) == 0) lx_inventory_add(item, count);
    else ok = lx_inventory_remove(item, count);
    lx_fsm_set_int(c, 3, lx_inventory_count(item));
    lx_fsm_set_bool(c, 4, ok);
    fsm_send(c, ok ? 5 : 6);
}
static const LxFsmActionDesc A_IV_CHANGE = {"Change Inventory", "Inventory", "Adds items to the inventory or takes them away. Removing fails, and changes nothing, when there are not enough.",
    "{0} {2} x {1}",
    {PENUM("Action", "Add|Remove", "0"), P("Item", LX_FSM_STRING, "Key"), P("Count", LX_FSM_INT, "1"),
     POPT("Store New Count", LX_FSM_INT), POPT("Store Success", LX_FSM_BOOL), PEV("Success Event"), PEV("Not Enough Event")},
    0, 0, LX_FSM_ONCE, iv_change, iv_change, NULL};

static void iv_count(LxFsmCtx *c) {
    char item[LX_NAME_MAX];
    lx_strcpy(item, sizeof item, lx_fsm_string(c, 0));
    int n = item[0] ? lx_inventory_count(item) : 0;
    lx_fsm_set_int(c, 1, n);
    bool enough = n >= lx_fsm_int(c, 2);
    lx_fsm_set_bool(c, 3, enough);
    fsm_send(c, enough ? 4 : 5);
}
static const LxFsmActionDesc A_IV_COUNT = {"Get Inventory Count", "Inventory", "How many of an item the player holds, and whether that is enough: locked doors, crafting, shops.",
    "count {0} -> {1}",
    {P("Item", LX_FSM_STRING, "Key"), POPT("Store Count", LX_FSM_INT),
     PT("Need At Least", LX_FSM_INT, "1", "how many count as enough"), POPT("Store Has Enough", LX_FSM_BOOL),
     PEV("Has Enough Event"), PEV("Not Enough Event")},
    0, 0, LX_FSM_ONCE, iv_count, iv_count, NULL};

static void iv_list(LxFsmCtx *c) {
    LxArray names = lx_fsm_array(c, 0), counts = lx_fsm_array(c, 1);
    if (lx_array_valid(names)) lx_array_clear(names);
    if (lx_array_valid(counts)) lx_array_clear(counts);
    int n = lx_inventory_size();
    for (int i = 0; i < n; i++) {
        int count = 0;
        const char *item = lx_inventory_item(i, &count);
        if (lx_array_valid(names)) lx_array_add_string(names, item ? item : "");
        if (lx_array_valid(counts)) lx_array_add_int(counts, count);
    }
    lx_fsm_set_int(c, 2, n);
}
static const LxFsmActionDesc A_IV_LIST = {"Get Inventory", "Inventory", "Every item the player holds and how many of each: for drawing an inventory screen.",
    "inventory -> {0}",
    {PARRT("Store Item Names", "a String array"), PARRT("Store Counts", "an Int array"), POPT("Store Item Count", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, iv_list, iv_list, NULL};

static void iv_persist(LxFsmCtx *c) {
    const char *slot = lx_fsm_string(c, 1);
    bool ok;
    switch (lx_fsm_int(c, 0)) {
    case 0: ok = lx_inventory_save(slot); break;
    case 1: ok = lx_inventory_load(slot); break;
    default: lx_inventory_clear(); ok = true; break;
    }
    fsm_send(c, ok ? 2 : 3);
}
static const LxFsmActionDesc A_IV_PERSIST = {"Save Or Load Inventory", "Inventory", "Writes the inventory into the save data, reads it back, or empties it.",
    "{0} inventory \"{1}\"",
    {PENUM("Action", "Save|Load|Clear", "0"), P("Slot", LX_FSM_STRING, "slot1"), PEV("Success Event"), PEV("Failed Event")},
    0, 0, LX_FSM_ONCE, iv_persist, iv_persist, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_scene(void) {
    static const LxFsmActionDesc *all[] = {
        &A_SN_LOAD, &A_SN_RELOAD, &A_SN_GET, &A_SN_QUIT, &A_SN_APP_INFO,
        &A_SV_SET, &A_SV_GET, &A_SV_HAS, &A_SV_DELETE, &A_SV_GLOBALS,
        &A_CP_SET, &A_CP_RESPAWN,
        &A_IV_CHANGE, &A_IV_COUNT, &A_IV_LIST, &A_IV_PERSIST,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
