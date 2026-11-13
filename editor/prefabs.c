/* Prefabs in the editor: drag an object from the Hierarchy into the Content Browser (or use
 * Save as Prefab) to make one; drag a prefab into the Hierarchy or the Scene view to place it.
 * Instances remember their prefab, so changes can be applied back to it and to every other copy. */
#include "editor.h"
#include <stdio.h>
#include <string.h>

/* the nearest object at or above e that is a prefab instance */
LxEntity ed_prefab_root(LxEntity e) {
    for (LxEntity c = e; c; c = lx_entity_parent(ED.scene, c))
        if (lx_entity_prefab(ED.scene, c)[0]) return c;
    return 0;
}

static void file_safe(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (const char *c = src; *c && n + 1 < cap; c++)
        dst[n++] = (strchr("\\/:*?\"<>|", *c) || (unsigned char)*c < 32) ? '_' : *c;
    dst[n] = 0;
    if (!dst[0]) lx_strcpy(dst, cap, "Prefab");
}

/* Saves e (and its children) as a new prefab in dir; e becomes an instance of it. */
bool ed_prefab_create(LxEntity e, const char *dir) {
    if (!ED.scene || !lx_entity_valid(ED.scene, e)) return false;
    if (ED.play != ED_EDIT) { ui_notify(ED.ui, "Stop play mode before making prefabs"); return false; }
    char base[LX_NAME_MAX], name[LX_NAME_MAX + 16], path[LX_PATH_MAX];
    file_safe(base, sizeof base, lx_entity_name(ED.scene, e));
    snprintf(name, sizeof name, "%s.lxprefab", base);
    lx_path_join(path, sizeof path, dir, name);
    for (int i = 1; lx_file_exists(path) && i < 1000; i++) {
        snprintf(name, sizeof name, "%s %d.lxprefab", base, i);
        lx_path_join(path, sizeof path, dir, name);
    }
    if (!lx_prefab_save(ED.scene, e, path)) { ui_notify(ED.ui, "Could not save the prefab"); return false; }
    char ref[LX_REF_MAX];
    ed_relative_asset(ref, sizeof ref, path);
    lx_entity_set_prefab(ED.scene, e, ref);
    lx_prefab_cache_clear();
    ED.content_dirty = true;
    ED.hierarchy_hash = 0;
    ED.inspector_dirty = true;
    ed_mark_changed();
    ed_commit();
    ui_notify(ED.ui, "Prefab saved: %s", ref);
    LX_INFO("Saved prefab %s", ref);
    return true;
}

/* Places a prefab: under `parent` (at its origin), else at `pos` in the world. */
LxEntity ed_prefab_place(const char *ref, LxVec3 pos, LxEntity parent) {
    if (!ED.scene) return 0;
    if (ED.play != ED_EDIT) { ui_notify(ED.ui, "Stop play mode to add objects"); return 0; }
    LxEntity e = lx_prefab_instantiate(ED.scene, ref, parent ? lx_v3(0, 0, 0) : pos, parent);
    if (!e) { ui_notify(ED.ui, "Could not load prefab %s", ref); return 0; }
    if (parent) {
        bool open = false;
        for (int i = 0; i < ED.expanded_count; i++) open |= ED.expanded[i] == parent;
        if (!open && ED.expanded_count < 256) ED.expanded[ED.expanded_count++] = parent;
    }
    lx_scene_mark_static_dirty(ED.scene);
    ed_select(e);
    ED.hierarchy_hash = 0;
    ed_mark_changed();
    ed_commit();
    return e;
}

/* Writes the instance back to its prefab and refreshes every other copy in the scene. */
void ed_prefab_apply(LxEntity e) {
    LxEntity root = ed_prefab_root(e);
    if (!root) return;
    if (ED.play != ED_EDIT) { ui_notify(ED.ui, "Stop play mode before applying"); return; }
    char ref[LX_REF_MAX], path[LX_PATH_MAX];
    lx_strcpy(ref, sizeof ref, lx_entity_prefab(ED.scene, root));
    lx_asset_path(path, sizeof path, ref);
    if (!lx_prefab_save(ED.scene, root, path)) { ui_notify(ED.ui, "Could not save %s", ref); return; }
    lx_prefab_cache_clear();
    /* other instances: rebuild from the prefab, keeping where they are */
    LxEntity others[512];
    int n = 0;
    int count = lx_scene_entity_count(ED.scene);
    for (int i = 0; i < count && n < 512; i++) {
        LxEntity x = lx_scene_entity_at(ED.scene, i);
        if (x != root && !strcmp(lx_entity_prefab(ED.scene, x), ref)) others[n++] = x;
    }
    for (int i = 0; i < n; i++) {
        LxEntity x = others[i];
        if (!lx_entity_valid(ED.scene, x)) continue;
        LxEntity parent = lx_entity_parent(ED.scene, x);
        LxVec3 pos = lx_entity_position(ED.scene, x), rot = lx_entity_euler(ED.scene, x), scl = lx_entity_scale(ED.scene, x);
        char name[LX_NAME_MAX];
        lx_strcpy(name, sizeof name, lx_entity_name(ED.scene, x));
        bool was_selected = ED.selected == x;
        lx_entity_destroy(ED.scene, x);
        LxEntity y = lx_prefab_instantiate(ED.scene, ref, pos, parent);
        if (!y) continue;
        lx_entity_set_euler(ED.scene, y, rot);
        lx_entity_set_scale(ED.scene, y, scl);
        lx_entity_set_name(ED.scene, y, name);
        if (was_selected) ED.selected = y;
    }
    lx_scene_mark_static_dirty(ED.scene);
    ED.hierarchy_hash = 0;
    ED.inspector_dirty = true;
    ed_mark_changed();
    ed_commit();
    ui_notify(ED.ui, n ? "Applied to %s (and %d other cop%s)" : "Applied to %s", ref, n, n == 1 ? "y" : "ies");
}

void ed_prefab_unlink(LxEntity e) {
    LxEntity root = ed_prefab_root(e);
    if (!root) return;
    lx_entity_set_prefab(ED.scene, root, "");
    ED.hierarchy_hash = 0;
    ED.inspector_dirty = true;
    ed_mark_changed();
    ed_commit();
}

void ed_prefab_select_asset(LxEntity e) {
    LxEntity root = ed_prefab_root(e);
    if (!root) return;
    char path[LX_PATH_MAX], dir[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, lx_entity_prefab(ED.scene, root));
    lx_path_parent(dir, sizeof dir, path);
    ed_content_open_dir(dir);
    ED.selected = 0;
    ed_select_asset(path);
}
