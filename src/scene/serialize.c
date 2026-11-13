/* .lxscene text format: human readable, diff friendly, stable across versions. */
#include "../lx_internal.h"

static const char *light_type_name(LxLightType t) {
    return t == LX_LIGHT_POINT ? "point" : t == LX_LIGHT_SPOT ? "spot" : "directional";
}
static LxLightType light_type_parse(const char *s) {
    return !strcmp(s, "point") ? LX_LIGHT_POINT : !strcmp(s, "spot") ? LX_LIGHT_SPOT : LX_LIGHT_DIRECTIONAL;
}

static void write_entity(LxStrBuf *sb, const LxScene *s, LxEntity e, int id_of_parent, int *next_id, bool prefab_root) {
    const LxEntityData *d = lx_entity_data(s, e);
    int id = (*next_id)++;
    lx_sb_printf(sb, "\n[entity]\nid = %d\nparent = %d\nname = %s\nactive = %d\n", id, id_of_parent, d->name, d->active);
    if (d->tag[0]) lx_sb_printf(sb, "tag = %s\n", d->tag);
    if (d->prefab[0] && !prefab_root) lx_sb_printf(sb, "prefab = %s\n", d->prefab);
    if (prefab_root) lx_sb_printf(sb, "position = 0 0 0\n");   /* a prefab is placed where it's spawned */
    else lx_sb_printf(sb, "position = %.6g %.6g %.6g\n", d->position.x, d->position.y, d->position.z);
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
    static const char *shape_names[] = {"box", "sphere", "capsule", "mesh"};
    if (d->comps & LX_COMP_COLLIDER) {
        const LxCollider *c = &d->collider;
        lx_sb_printf(sb, "collider.shape = %s\ncollider.center = %g %g %g\ncollider.size = %g %g %g\ncollider.radius = %g\n",
                     shape_names[(int)c->shape & 3], c->center.x, c->center.y, c->center.z, c->size.x, c->size.y, c->size.z, c->radius);
        lx_sb_printf(sb, "collider.height = %g\ncollider.trigger = %d\ncollider.friction = %g\ncollider.bounce = %g\ncollider.layer = %d\n",
                     c->height, c->is_trigger, c->friction, c->bounciness, c->layer);
    }
    if (d->comps & LX_COMP_RIGIDBODY) {
        const LxRigidbody *b = &d->body;
        lx_sb_printf(sb, "body.mass = %g\nbody.damping = %g\nbody.angular_damping = %g\nbody.gravity = %d\nbody.kinematic = %d\n"
                         "body.freeze_rotation = %d\n",
                     b->mass, b->linear_damping, b->angular_damping, b->use_gravity, b->is_kinematic, b->freeze_rotation);
    }
    if (d->comps & LX_COMP_CHARACTER) {
        const LxCharacterController *c = &d->character;
        lx_sb_printf(sb, "character.radius = %g\ncharacter.height = %g\ncharacter.step = %g\ncharacter.slope = %g\ncharacter.push = %g\n",
                     c->radius, c->height, c->step_height, c->slope_limit_deg, c->push_power);
    }
    if (d->comps & LX_COMP_ANIMATOR) {
        const LxAnimator *a = &d->animator;
        lx_sb_printf(sb, "animator.model = %s\nanimator.clip = %s\nanimator.speed = %g\nanimator.loop = %d\nanimator.play_on_start = %d\n"
                         "animator.root_motion = %d\n",
                     a->model, a->clip, a->speed, a->loop, a->play_on_start, a->root_motion);
    }
    if (d->comps & LX_COMP_REFLECTION_PROBE) {
        const LxReflectionProbe *p = &d->probe;
        lx_sb_printf(sb, "probe.size = %g %g %g\nprobe.intensity = %g\nprobe.blend = %g\nprobe.box = %d\nprobe.realtime = %d\n", p->size.x,
                     p->size.y, p->size.z, p->intensity, p->blend_distance, p->box_projection, p->realtime);
    }
    for (LxEntity c = d->first_child; c; c = lx_entity_next_sibling(s, c)) write_entity(sb, s, c, id, next_id, false);
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
    for (LxEntity r = s->first_root; r; r = lx_entity_next_sibling(s, r)) write_entity(&sb, s, r, 0, &next_id, false);
    if (!sb.data) lx_sb_printf(&sb, "");
    return sb.data;
}

char *lx_entity_serialize(const LxScene *s, LxEntity root) {
    const LxEntityData *d = lx_entity_data(s, root);
    if (!d) return NULL;
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# Luxora prefab v1\n[prefab]\nname = %s\n", d->name);
    int next_id = 1;
    write_entity(&sb, s, root, 0, &next_id, true);
    return sb.data;
}

/* Reads scene or prefab text into `s`. Entities without a parent in the text go under `attach`. */
static LxEntity deserialize_into(LxScene *s, const char *text, LxEntity attach) {
    LxEntity first_root = 0;
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
            if (!strcmp(k, "entity")) {
                cur_e = lx_entity_create(s, "Entity");
                if (attach) lx_entity_set_parent(s, cur_e, attach);
                if (!first_root) first_root = cur_e;
            } else cur_e = 0;
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
        else if (!strcmp(k, "tag")) lx_strcpy(d->tag, sizeof d->tag, v);
        else if (!strcmp(k, "prefab")) lx_strcpy(d->prefab, sizeof d->prefab, v);
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
        } else if (!strncmp(k, "collider.", 9)) {
            const char *f = k + 9;
            LxCollider *c = (d->comps & LX_COMP_COLLIDER) ? &d->collider : lx_add_collider(s, cur_e, LX_COLLIDER_BOX);
            if (!strcmp(f, "shape")) c->shape = !strcmp(v, "sphere") ? LX_COLLIDER_SPHERE : !strcmp(v, "capsule") ? LX_COLLIDER_CAPSULE
                                              : !strcmp(v, "mesh") ? LX_COLLIDER_MESH : LX_COLLIDER_BOX;
            else if (!strcmp(f, "center")) c->center = lx_parse_vec3(v);
            else if (!strcmp(f, "size")) c->size = lx_parse_vec3(v);
            else if (!strcmp(f, "radius")) c->radius = (float)atof(v);
            else if (!strcmp(f, "height")) c->height = (float)atof(v);
            else if (!strcmp(f, "trigger")) c->is_trigger = lx_parse_bool(v);
            else if (!strcmp(f, "friction")) c->friction = (float)atof(v);
            else if (!strcmp(f, "bounce")) c->bounciness = (float)atof(v);
            else if (!strcmp(f, "layer")) c->layer = atoi(v);
        } else if (!strncmp(k, "body.", 5)) {
            const char *f = k + 5;
            bool had_collider = (d->comps & LX_COMP_COLLIDER) != 0;
            LxRigidbody *b = lx_add_rigidbody(s, cur_e);
            d = lx_entity_data(s, cur_e);
            if (!had_collider) d->comps &= ~(uint32_t)LX_COMP_COLLIDER;   /* the file lists its own collider */
            if (!strcmp(f, "mass")) b->mass = (float)atof(v);
            else if (!strcmp(f, "damping")) b->linear_damping = (float)atof(v);
            else if (!strcmp(f, "angular_damping")) b->angular_damping = (float)atof(v);
            else if (!strcmp(f, "gravity")) b->use_gravity = lx_parse_bool(v);
            else if (!strcmp(f, "kinematic")) b->is_kinematic = lx_parse_bool(v);
            else if (!strcmp(f, "freeze_rotation")) b->freeze_rotation = lx_parse_bool(v);
        } else if (!strncmp(k, "character.", 10)) {
            const char *f = k + 10;
            LxCharacterController *c = lx_add_character(s, cur_e);
            if (!strcmp(f, "radius")) c->radius = (float)atof(v);
            else if (!strcmp(f, "height")) c->height = (float)atof(v);
            else if (!strcmp(f, "step")) c->step_height = (float)atof(v);
            else if (!strcmp(f, "slope")) c->slope_limit_deg = (float)atof(v);
            else if (!strcmp(f, "push")) c->push_power = (float)atof(v);
        } else if (!strncmp(k, "animator.", 9)) {
            const char *f = k + 9;
            LxAnimator *a = lx_add_animator(s, cur_e, NULL);
            if (!strcmp(f, "model")) lx_strcpy(a->model, sizeof a->model, v);
            else if (!strcmp(f, "clip")) lx_strcpy(a->clip, sizeof a->clip, v);
            else if (!strcmp(f, "speed")) a->speed = (float)atof(v);
            else if (!strcmp(f, "loop")) a->loop = lx_parse_bool(v);
            else if (!strcmp(f, "play_on_start")) a->play_on_start = lx_parse_bool(v);
            else if (!strcmp(f, "root_motion")) a->root_motion = lx_parse_bool(v);
        } else if (!strncmp(k, "probe.", 6)) {
            const char *f = k + 6;
            LxReflectionProbe *p = lx_add_reflection_probe(s, cur_e);
            if (!strcmp(f, "size")) p->size = lx_parse_vec3(v);
            else if (!strcmp(f, "intensity")) p->intensity = (float)atof(v);
            else if (!strcmp(f, "blend")) p->blend_distance = (float)atof(v);
            else if (!strcmp(f, "box")) p->box_projection = lx_parse_bool(v);
            else if (!strcmp(f, "realtime")) p->realtime = lx_parse_bool(v);
        }
    }
    lx_free(ids);
    lx_scene_update_transforms(s);
    return first_root;
}

LxEntity lx_scene_deserialize_into(LxScene *s, const char *text, LxEntity parent) { return deserialize_into(s, text, parent); }

LxScene *lx_scene_deserialize(const char *text) {
    LxScene *s = lx_scene_create("Untitled");
    deserialize_into(s, text, 0);
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
