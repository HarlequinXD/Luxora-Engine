/* .lxscene text format: human readable, diff friendly, stable across versions. */
#include "../lx_internal.h"

static const char *light_type_name(LxLightType t) {
    return t == LX_LIGHT_POINT ? "point" : t == LX_LIGHT_SPOT ? "spot" : "directional";
}
static LxLightType light_type_parse(const char *s) {
    return !strcmp(s, "point") ? LX_LIGHT_POINT : !strcmp(s, "spot") ? LX_LIGHT_SPOT : LX_LIGHT_DIRECTIONAL;
}

static void write_entity(LxStrBuf *sb, const LxScene *s, LxEntity e, int id_of_parent, int *next_id) {
    const LxEntityData *d = lx_entity_data(s, e);
    int id = (*next_id)++;
    lx_sb_printf(sb, "\n[entity]\nid = %d\nparent = %d\nname = %s\nactive = %d\n", id, id_of_parent, d->name, d->active);
    lx_sb_printf(sb, "position = %.6g %.6g %.6g\n", d->position.x, d->position.y, d->position.z);
    lx_sb_printf(sb, "rotation = %.6g %.6g %.6g\n", d->euler.x, d->euler.y, d->euler.z);
    lx_sb_printf(sb, "scale = %.6g %.6g %.6g\n", d->scale.x, d->scale.y, d->scale.z);
    if (d->comps & LX_COMP_CAMERA) {
        const LxCamera *c = &d->camera;
        lx_sb_printf(sb, "camera.fov = %g\ncamera.near = %g\ncamera.far = %g\ncamera.ortho = %d\ncamera.ortho_size = %g\n",
                     c->fov_deg, c->near_clip, c->far_clip, c->orthographic, c->ortho_size);
        lx_sb_printf(sb, "camera.clear = %g %g %g %g\ncamera.priority = %d\n", c->clear_color.x, c->clear_color.y,
                     c->clear_color.z, c->clear_color.w, c->priority);
    }
    if (d->comps & LX_COMP_LIGHT) {
        const LxLight *l = &d->light;
        lx_sb_printf(sb, "light.type = %s\nlight.color = %g %g %g\nlight.intensity = %g\nlight.range = %g\n",
                     light_type_name(l->type), l->color.x, l->color.y, l->color.z, l->intensity, l->range);
        lx_sb_printf(sb, "light.spot_angle = %g\nlight.shadows = %d\nlight.gi = %d\n", l->spot_angle_deg, l->cast_shadows, l->contribute_gi);
    }
    if (d->comps & LX_COMP_MESH) {
        lx_sb_printf(sb, "mesh.mesh = %s\nmesh.material = %s\nmesh.shadows = %d\nmesh.static = %d\n", d->mesh.mesh,
                     d->mesh.material, d->mesh.cast_shadows, d->mesh.static_geometry);
    }
    if (d->comps & LX_COMP_AUDIO_SOURCE) {
        const LxAudioSource *a = &d->audio;
        lx_sb_printf(sb, "audio.clip = %s\naudio.volume = %g\naudio.pitch = %g\naudio.loop = %d\naudio.play_on_start = %d\n",
                     a->clip, a->volume, a->pitch, a->loop, a->play_on_start);
        lx_sb_printf(sb, "audio.spatial = %d\naudio.min = %g\naudio.max = %g\naudio.bus = %d\n", a->spatial, a->min_distance,
                     a->max_distance, (int)a->bus);
    }
    if (d->comps & LX_COMP_AUDIO_LISTENER) lx_sb_printf(sb, "listener = 1\n");
    if (d->comps & LX_COMP_BEHAVIOUR) {
        lx_sb_printf(sb, "behaviour.name = %s\nbehaviour.params =", d->behaviour.name);
        for (int i = 0; i < LX_BEHAVIOUR_PARAMS; i++) lx_sb_printf(sb, " %g", d->behaviour.params[i]);
        lx_sb_printf(sb, "\n");
    }
    if (d->comps & LX_COMP_CROWD) {
        const LxCrowdComponent *c = &d->crowd;
        lx_sb_printf(sb, "crowd.agents = %d\ncrowd.radius = %g\ncrowd.speed = %g\ncrowd.mesh = %s\ncrowd.material = %s\n",
                     c->agent_count, c->area_radius, c->agent_speed, c->mesh, c->material);
    }
    for (LxEntity c = d->first_child; c; c = lx_entity_next_sibling(s, c)) write_entity(sb, s, c, id, next_id);
}

char *lx_scene_serialize(const LxScene *s) {
    LxStrBuf sb = {0};
    const LxEnvironment *e = &s->env;
    lx_sb_printf(&sb, "# Luxora scene v1\n[scene]\nname = %s\n", s->name);
    lx_sb_printf(&sb, "sky_zenith = %g %g %g\nsky_horizon = %g %g %g\nground = %g %g %g\n", e->sky_zenith.x, e->sky_zenith.y,
                 e->sky_zenith.z, e->sky_horizon.x, e->sky_horizon.y, e->sky_horizon.z, e->ground_color.x, e->ground_color.y,
                 e->ground_color.z);
    lx_sb_printf(&sb, "ambient = %g\nexposure = %g\nfog_color = %g %g %g\nfog_density = %g\n", e->ambient_intensity, e->exposure,
                 e->fog_color.x, e->fog_color.y, e->fog_color.z, e->fog_density);
    int next_id = 1;
    for (LxEntity r = s->first_root; r; r = lx_entity_next_sibling(s, r)) write_entity(&sb, s, r, 0, &next_id);
    if (!sb.data) lx_sb_printf(&sb, "");
    return sb.data;
}

LxScene *lx_scene_deserialize(const char *text) {
    LxScene *s = lx_scene_create("Untitled");
    const char *cur = text;
    char k[96], v[1024];
    int r;
    bool in_scene = false;
    LxEntity cur_e = 0;
    LxEntity *ids = NULL;
    int ids_cap = 0;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
        if (r == 2) {
            in_scene = !strcmp(k, "scene");
            if (!strcmp(k, "entity")) cur_e = lx_entity_create(s, "Entity");
            else cur_e = 0;
            continue;
        }
        if (in_scene) {
            LxEnvironment *e = &s->env;
            if (!strcmp(k, "name")) lx_strcpy(s->name, sizeof s->name, v);
            else if (!strcmp(k, "sky_zenith")) e->sky_zenith = lx_parse_vec3(v);
            else if (!strcmp(k, "sky_horizon")) e->sky_horizon = lx_parse_vec3(v);
            else if (!strcmp(k, "ground")) e->ground_color = lx_parse_vec3(v);
            else if (!strcmp(k, "ambient")) e->ambient_intensity = (float)atof(v);
            else if (!strcmp(k, "exposure")) e->exposure = (float)atof(v);
            else if (!strcmp(k, "fog_color")) e->fog_color = lx_parse_vec3(v);
            else if (!strcmp(k, "fog_density")) e->fog_density = (float)atof(v);
            continue;
        }
        if (!cur_e) continue;
        LxEntityData *d = lx_entity_data(s, cur_e);
        if (!strcmp(k, "id")) {
            int id = atoi(v);
            if (id >= 0 && id < 1 << 20) {
                if (id >= ids_cap) {
                    int nc = ids_cap ? ids_cap : 64;
                    while (nc <= id) nc *= 2;
                    ids = lx_realloc(ids, sizeof(LxEntity) * (size_t)nc);
                    memset(ids + ids_cap, 0, sizeof(LxEntity) * (size_t)(nc - ids_cap));
                    ids_cap = nc;
                }
                ids[id] = cur_e;
            }
        } else if (!strcmp(k, "parent")) {
            int pid = atoi(v);
            if (pid > 0 && pid < ids_cap && ids[pid]) lx_entity_set_parent(s, cur_e, ids[pid]);
        } else if (!strcmp(k, "name")) lx_strcpy(d->name, sizeof d->name, v);
        else if (!strcmp(k, "active")) d->active = lx_parse_bool(v);
        else if (!strcmp(k, "position")) d->position = lx_parse_vec3(v);
        else if (!strcmp(k, "rotation")) lx_entity_set_euler(s, cur_e, lx_parse_vec3(v));
        else if (!strcmp(k, "scale")) d->scale = lx_parse_vec3(v);
        else if (!strncmp(k, "camera.", 7)) {
            LxCamera *c = lx_add_camera(s, cur_e);
            const char *f = k + 7;
            if (!strcmp(f, "fov")) c->fov_deg = (float)atof(v);
            else if (!strcmp(f, "near")) c->near_clip = (float)atof(v);
            else if (!strcmp(f, "far")) c->far_clip = (float)atof(v);
            else if (!strcmp(f, "ortho")) c->orthographic = lx_parse_bool(v);
            else if (!strcmp(f, "ortho_size")) c->ortho_size = (float)atof(v);
            else if (!strcmp(f, "clear")) c->clear_color = lx_parse_vec4(v);
            else if (!strcmp(f, "priority")) c->priority = atoi(v);
        } else if (!strncmp(k, "light.", 6)) {
            const char *f = k + 6;
            LxLight *l = (d->comps & LX_COMP_LIGHT) ? &d->light : lx_add_light(s, cur_e, LX_LIGHT_DIRECTIONAL);
            if (!strcmp(f, "type")) l->type = light_type_parse(v);
            else if (!strcmp(f, "color")) l->color = lx_parse_vec3(v);
            else if (!strcmp(f, "intensity")) l->intensity = (float)atof(v);
            else if (!strcmp(f, "range")) l->range = (float)atof(v);
            else if (!strcmp(f, "spot_angle")) l->spot_angle_deg = (float)atof(v);
            else if (!strcmp(f, "shadows")) l->cast_shadows = lx_parse_bool(v);
            else if (!strcmp(f, "gi")) l->contribute_gi = lx_parse_bool(v);
        } else if (!strncmp(k, "mesh.", 5)) {
            const char *f = k + 5;
            LxMeshRenderer *m = (d->comps & LX_COMP_MESH) ? &d->mesh : lx_add_mesh(s, cur_e, NULL, NULL);
            if (!strcmp(f, "mesh")) lx_strcpy(m->mesh, sizeof m->mesh, v);
            else if (!strcmp(f, "material")) lx_strcpy(m->material, sizeof m->material, v);
            else if (!strcmp(f, "shadows")) m->cast_shadows = lx_parse_bool(v);
            else if (!strcmp(f, "static")) m->static_geometry = lx_parse_bool(v);
        } else if (!strncmp(k, "audio.", 6)) {
            const char *f = k + 6;
            LxAudioSource *a = (d->comps & LX_COMP_AUDIO_SOURCE) ? &d->audio : lx_add_audio_source(s, cur_e, NULL);
            if (!strcmp(f, "clip")) lx_strcpy(a->clip, sizeof a->clip, v);
            else if (!strcmp(f, "volume")) a->volume = (float)atof(v);
            else if (!strcmp(f, "pitch")) a->pitch = (float)atof(v);
            else if (!strcmp(f, "loop")) a->loop = lx_parse_bool(v);
            else if (!strcmp(f, "play_on_start")) a->play_on_start = lx_parse_bool(v);
            else if (!strcmp(f, "spatial")) a->spatial = lx_parse_bool(v);
            else if (!strcmp(f, "min")) a->min_distance = (float)atof(v);
            else if (!strcmp(f, "max")) a->max_distance = (float)atof(v);
            else if (!strcmp(f, "bus")) a->bus = (LxAudioBus)atoi(v);
        } else if (!strcmp(k, "listener")) lx_add_audio_listener(s, cur_e);
        else if (!strcmp(k, "behaviour.name")) lx_add_behaviour(s, cur_e, v);
        else if (!strcmp(k, "behaviour.params")) {
            const char *p = v;
            for (int i = 0; i < LX_BEHAVIOUR_PARAMS && *p; i++) {
                char *end;
                d->behaviour.params[i] = strtof(p, &end);
                if (end == p) break;
                p = end;
            }
        } else if (!strncmp(k, "crowd.", 6)) {
            const char *f = k + 6;
            LxCrowdComponent *c = (d->comps & LX_COMP_CROWD) ? &d->crowd : lx_add_crowd(s, cur_e, 100);
            if (!strcmp(f, "agents")) c->agent_count = atoi(v);
            else if (!strcmp(f, "radius")) c->area_radius = (float)atof(v);
            else if (!strcmp(f, "speed")) c->agent_speed = (float)atof(v);
            else if (!strcmp(f, "mesh")) lx_strcpy(c->mesh, sizeof c->mesh, v);
            else if (!strcmp(f, "material")) lx_strcpy(c->material, sizeof c->material, v);
        }
    }
    lx_free(ids);
    lx_scene_update_transforms(s);
    return s;
}

bool lx_scene_save(const LxScene *s, const char *path) {
    char *text = lx_scene_serialize(s);
    bool ok = lx_file_write(path, text, strlen(text));
    lx_free(text);
    if (!ok) LX_ERROR("Could not save scene to %s", path);
    return ok;
}

LxScene *lx_scene_load(const char *path) {
    char *text = lx_file_read(path, NULL);
    if (!text) { LX_ERROR("Could not open scene %s", path); return NULL; }
    LxScene *s = lx_scene_deserialize(text);
    lx_free(text);
    return s;
}
