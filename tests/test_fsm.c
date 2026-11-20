/* State Graph bug hunt: every action run with its defaults, then with random and hostile values
 * (huge numbers, NaN, empty and missing objects, variables of the wrong type...), on objects that
 * have every component and on bare ones. Afterwards the scene must still make sense. Included by
 * test_main.c (shares CHECK). */

static uint32_t g_fz = 0x12345678u;
static uint32_t fz(int n) { g_fz ^= g_fz << 13; g_fz ^= g_fz >> 17; g_fz ^= g_fz << 5; return n > 0 ? g_fz % (uint32_t)n : 0; }

static const char *fz_value(LxFsmType t, const char *options) {
    static char buf[256];
    static const char *F[] = {"0", "1", "-1", "0.5", "1e9", "-1e9", "nan", "inf", "-inf", "3.25", "abc", "", "1e-30"};
    static const char *I[] = {"0", "1", "-1", "3", "2147483647", "-2147483648", "99999999999", "x", ""};
    static const char *S[] = {"", "Owner", "Space", "Enemy", "Player", "Score: {0} / {1}", "{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}{0}", "%s%n%x", "name:Char",
                              "ÄÖÜ 日本", "a\\nb"};
    static const char *V[] = {"0 0 0", "1 2 3", "-5 0.5 9", "nan nan nan", "1e9 -1e9 1e9", "inf 0 0", "", "1", "0 1 0"};
    static const char *C[] = {"1 1 1 1", "0 0 0 0", "2 -1 0.5", "nan 0 0 1", "", "1 0 0"};
    static const char *O[] = {"Owner", "None", "", "name:Enemy1", "name:Nope", "tag:Enemy", "tag:Nobody", "Char", "name:Child", "name:Ground", "name:Cam"};
    static const char *A[] = {"", "Prefabs/Box.lxprefab", "Missing.lxprefab", "Sounds/beep.wav", "Scenes/Main.lxscene", "builtin:red", "Textures/t.png", "../../etc/passwd"};
    static const char *E[] = {"", "None", "GO", "FINISHED", "BACK", "TRIGGER_ENTER", "UNHANDLED"};
    switch (t) {
    case LX_FSM_FLOAT: return F[fz(13)];
    case LX_FSM_INT: return I[fz(9)];
    case LX_FSM_BOOL: return fz(2) ? "true" : "false";
    case LX_FSM_STRING: {
        if (fz(12) == 0) { memset(buf, 'x', 200); buf[200] = 0; return buf; }
        return S[fz(11)];
    }
    case LX_FSM_VEC3: return V[fz(9)];
    case LX_FSM_COLOR: return C[fz(6)];
    case LX_FSM_OBJECT: return O[fz(11)];
    case LX_FSM_ASSET: return A[fz(8)];
    case LX_FSM_EVENT: return E[fz(7)];
    case LX_FSM_ENUM: {
        int n = 1;
        for (const char *p = options; p && *p; p++) n += *p == '|';
        int k = (int)fz(n + 3) - 1;   /* -1 .. n+1: out of range too */
        if (fz(3) == 0 && options) {   /* by name */
            const char *p = options;
            for (int i = 0; i < k && p && *p; i++) { p = strchr(p, '|'); if (p) p++; }
            if (p && *p) { size_t l = strcspn(p, "|"); if (l > 60) l = 60; memcpy(buf, p, l); buf[l] = 0; return buf; }
        }
        snprintf(buf, sizeof buf, "%d", k);
        return buf;
    }
    default: return "";
    }
}

/* a scene with a bit of everything */
static LxScene *fz_scene(LxEntity *owner_out) {
    LxScene *s = lx_scene_create("Fuzz");
    LxEntity ground = lx_spawn_primitive(s, LX_PRIM_PLANE, "Ground", lx_v3(0, 0, 0));
    lx_entity_set_scale(s, ground, lx_v3(5, 1, 5));
    lx_add_collider(s, ground, LX_COLLIDER_BOX);
    LxEntity o = lx_spawn_primitive(s, LX_PRIM_CUBE, "Owner", lx_v3(0, 1, 0));
    lx_add_collider(s, o, LX_COLLIDER_BOX);
    lx_add_rigidbody(s, o);
    lx_add_light(s, o, LX_LIGHT_POINT);
    lx_add_audio_source(s, o, "Sounds/beep.wav");
    lx_add_animator(s, o, "");
    lx_add_camera(s, o);
    LxEntity child = lx_entity_create(s, "Child");
    lx_entity_set_parent(s, child, o);
    lx_entity_set_position(s, child, lx_v3(0, 1, 0));
    LxEntity ch = lx_spawn_primitive(s, LX_PRIM_CAPSULE, "Char", lx_v3(2, 1, 0));
    lx_add_character(s, ch);
    lx_entity_set_tag(s, ch, "Player");
    for (int i = 0; i < 2; i++) {
        char n[16];
        snprintf(n, sizeof n, "Enemy%d", i + 1);
        LxEntity e = lx_spawn_primitive(s, LX_PRIM_SPHERE, n, lx_v3(-2.0f - (float)i, 0.5f, 1));
        lx_entity_set_tag(s, e, "Enemy");
        lx_add_collider(s, e, LX_COLLIDER_SPHERE);
    }
    LxEntity trig = lx_spawn_primitive(s, LX_PRIM_CUBE, "Trigger", lx_v3(0, 1, 0));
    LxCollider *tc = lx_add_collider(s, trig, LX_COLLIDER_BOX);
    if (tc) tc->is_trigger = true;
    LxEntity cam = lx_entity_create(s, "Cam");
    lx_add_camera(s, cam)->priority = 5;
    lx_entity_set_position(s, cam, lx_v3(0, 3, 8));
    lx_entity_set_euler(s, cam, lx_v3(0, 180, 0));   /* looking back at the origin (-Z) */
    *owner_out = o;
    return s;
}

static bool fz_scene_sane(LxScene *s, char *why, size_t cap) {
    int n = lx_scene_entity_count(s);
    if (n < 0 || n > 5000) { snprintf(why, cap, "%d entities", n); return false; }
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        LxVec3 p = lx_entity_world_position(s, e), sc = lx_entity_scale(s, e);
        LxQuat q = lx_entity_world_rotation(s, e);
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) { snprintf(why, cap, "'%s' position is not a number", lx_entity_name(s, e)); return false; }
        if (!isfinite(sc.x) || !isfinite(sc.y) || !isfinite(sc.z)) { snprintf(why, cap, "'%s' scale is not a number", lx_entity_name(s, e)); return false; }
        if (!isfinite(q.x) || !isfinite(q.y) || !isfinite(q.z) || !isfinite(q.w)) { snprintf(why, cap, "'%s' rotation is not a number", lx_entity_name(s, e)); return false; }
        LxLight *l = lx_get_light(s, e);
        if (l && (!isfinite(l->intensity) || !isfinite(l->color.x))) { snprintf(why, cap, "'%s' light is not a number", lx_entity_name(s, e)); return false; }
        LxCamera *c = lx_get_camera(s, e);
        if (c && !isfinite(c->fov_deg)) { snprintf(why, cap, "'%s' camera FOV is not a number", lx_entity_name(s, e)); return false; }
    }
    LxVec3 g = lx_physics_settings()->gravity;
    if (!isfinite(g.x) || !isfinite(g.y) || !isfinite(g.z)) { snprintf(why, cap, "gravity is not a number"); return false; }
    if (!isfinite(lx_time_scale()) || lx_time_scale() < 0) { snprintf(why, cap, "time scale %g", (double)lx_time_scale()); return false; }
    return true;
}

#include <signal.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
static int g_fz_broken;
static char g_fz_now[200];
#if !defined(_WIN32)
/* Say which run we were in, on stderr and as a workflow annotation. Reading a job's log needs admin
   rights on the repository; annotations do not, so this is what makes a fuzz failure in CI diagnosable
   at all. Only write() from here: it is all a signal handler may safely call. */
static void fz_report(const char *what, const char *title) {
    if (write(2, "\n  FAIL fuzz: ", 14) < 0) {}
    if (write(2, what, strlen(what)) < 0) {}
    if (write(2, ": ", 2) < 0) {}
    if (write(2, g_fz_now, strlen(g_fz_now)) < 0) {}
    if (write(2, "\n", 1) < 0) {}
    if (write(1, "::error title=", 14) < 0) {}
    if (write(1, title, strlen(title)) < 0) {}
    if (write(1, "::", 2) < 0) {}
    if (write(1, what, strlen(what)) < 0) {}
    if (write(1, ": ", 2) < 0) {}
    if (write(1, g_fz_now, strlen(g_fz_now)) < 0) {}
    if (write(1, "\n", 1) < 0) {}
}
static void fz_alarm(int sig) {   /* a run that never ends: say which one */
    (void)sig;
    fz_report("this action run exceeded the watchdog", "Fuzz hang");
    _exit(3);
}
/* A crash says nothing about where it happened unless we name the run, and the run that crashes is
   exactly the one worth knowing about. */
static void fz_crash(int sig) {
    fz_report(sig == SIGFPE ? "this action run raised an arithmetic fault" : "this action run crashed", "Fuzz crash");
    _exit(139);
}
#endif
static char g_fz_first[400];
/* the slowest run of the whole sweep, reported at the end: an action that takes seconds with awkward
   parameters is a bug even where no watchdog fires, so it should not need a CI failure to notice. */
static double g_fz_slowest;
static char g_fz_slowest_name[200];

/* one graph: state A runs the action, state B runs it again (FINISHED/GO go back and forth) */
static void fz_run(const LxFsmActionDesc *d, int variant, const char *dir) {
    snprintf(g_fz_now, sizeof g_fz_now, "%s (variant %d)", d->name, variant);
    /* LUXORA_FUZZ_TRACE=1 names each run before it starts, so a crash says which action did it. */
    if (getenv("LUXORA_FUZZ_TRACE")) { fprintf(stderr, "FUZZ %s\n", g_fz_now); fflush(stderr); }
    double fz_t0 = lx_time();
#if !defined(_WIN32)
    alarm(getenv("LUXORA_FUZZ_ALARM") ? (unsigned)atoi(getenv("LUXORA_FUZZ_ALARM")) : 20);
#endif
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "[graph]\nname = Fuzz\nstart = A\n[event]\nname = GO\n[event]\nname = BACK\n");
    static const char *VT[] = {"float", "int", "bool", "string", "vector3", "color", "object", "asset"};
    for (int i = 0; i < 8; i++) lx_sb_printf(&sb, "[variable]\nname = v%s\ntype = %s\nvalue = %s\n", VT[i], VT[i], i == 6 ? "Owner" : i == 4 ? "1 2 3" : "1");
    lx_sb_printf(&sb, "[global]\nevent = BACK\nto = A\n");
    for (int st = 0; st < 2; st++) {
        lx_sb_printf(&sb, "[state]\nname = %s\nsequence = %d\ntransition = %s\ntransition = GO > %s\n", st ? "B" : "A", variant % 5 == 4,
                     st ? "FINISHED > A" : "FINISHED > B", st ? "A" : "B");
        for (int copy = 0; copy < (variant % 3 == 2 ? 2 : 1); copy++) {
            static const char *UPD[] = {"once", "every_frame", "fixed", "late"};
            lx_sb_printf(&sb, "[action]\ntype = %s\nupdate = %s\n", d->name, variant == 0 ? (d->default_update == LX_FSM_ONCE ? "once" : UPD[d->default_update & 3]) : UPD[fz(4)]);
            for (int k = 0; k < LX_FSM_MAX_PARAMS && d->params[k].name; k++) {
                const LxFsmParamDesc *pd = &d->params[k];
                if (variant == 0) {   /* defaults, outputs into a matching variable */
                    if (pd->flags & LX_FSM_P_OUT) {
                        int vi = pd->type == LX_FSM_INT || pd->type == LX_FSM_ENUM ? 1 : pd->type == LX_FSM_BOOL ? 2 : pd->type == LX_FSM_STRING ? 3 :
                                 pd->type == LX_FSM_VEC3 ? 4 : pd->type == LX_FSM_COLOR ? 5 : pd->type == LX_FSM_OBJECT ? 6 : pd->type == LX_FSM_ASSET ? 7 : 0;
                        lx_sb_printf(&sb, "p%d = $v%s\n", k, VT[vi]);
                    } else lx_sb_printf(&sb, "p%d = %s\n", k, pd->def ? pd->def : "");
                    continue;
                }
                int r = (int)fz(10);
                if (r == 0) lx_sb_printf(&sb, "p%d = $v%s\n", k, VT[fz(8)]);          /* a variable, maybe of another type */
                else if (r == 1) lx_sb_printf(&sb, "p%d = $missing\n", k);            /* a variable that isn't there */
                else lx_sb_printf(&sb, "p%d = %s\n", k, fz_value(pd->type, pd->options));
            }
        }
    }
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, dir, "Fuzz.lxgraph");
    lx_file_write(path, sb.data, sb.len);
    lx_free(sb.data);
    lx_assets_clear();   /* fresh graph */
    lx_save_clear();     /* and fresh save data: keys from earlier runs must not pile up */
    /* Audio settings are global and some actions change them, so pin them per run. Ray-traced acoustics
     * are on by default with 256 rays a source, and they rebuild a BVH of the scene whenever its static
     * geometry changes - which a graph duplicating itself does every frame. That is most of the cost of
     * the sweep and none of what it is testing, so it goes off here; the acoustics have their own tests. */
    if (lx_audio_settings()) {
        LxAudioSettings *as = lx_audio_settings();
        *as = lx_audio_defaults();
        as->propagation = false;
    }
    LxEntity o;
    LxScene *s = fz_scene(&o);
    lx_physics_settings()->gravity = lx_v3(0, -9.81f, 0);
    lx_time_set_scale(1);
    lx_add_state_graph(s, o, "Fuzz.lxgraph");
    /* the bare objects get the graph too: no components at all */
    if (variant % 2) lx_add_state_graph(s, lx_entity_find(s, "Child"), "Fuzz.lxgraph");
    char why[300] = "";
    bool ok = true;
    for (int f = 0; f < 40 && ok; f++) {
        lx_game_ui_begin_frame(0, 0, 1280, 720, true);
        float dt = f == 7 ? 0.1f : f == 11 ? 0.0f : 1.0f / 60;
        lx_scene_simulate(s, dt);
        if (f == 5) lx_fsm_send_event(s, o, "GO", 1, 0);
        if (f == 9) lx_fsm_broadcast_event(s, "BACK", 0, 0);
        if (f == 13) lx_send_message(s, o, "GO", 2, lx_entity_find(s, "Char"));
        if (f == 17) { LxEntity e = lx_entity_find(s, "Enemy1"); if (e) lx_entity_destroy(s, e); }   /* a target goes away */
        if (f == 21) lx_fsm_system_event(s, o, "COLLISION_ENTER", lx_entity_find(s, "Char"), 0);
        if (f == 22) lx_fsm_system_event(s, o, "TRIGGER_ENTER", lx_entity_find(s, "Char"), 0);
        if (f == 30 && variant % 4 == 3 && lx_entity_valid(s, o)) lx_entity_destroy(s, o);   /* the owner itself goes */
        ok = fz_scene_sane(s, why, sizeof why);
        /* A graph that copies itself - Duplicate Object on the owner, whose copy runs the same graph -
         * grows until the per-frame spawn budget and this cap stop it. The owner is a heavy object
         * (camera, light, audio, animator, collider, rigidbody), and simulating a hundred of them for
         * the rest of the run is already far more than the spawn path needs to be exercised by. */
        if (lx_scene_entity_count(s) > 100) break;
    }
    if (!ok) {
        g_fz_broken++;
        if (!g_fz_first[0]) snprintf(g_fz_first, sizeof g_fz_first, "%s (variant %d): %s", d->name, variant, why);
        else printf("    also: %s (variant %d): %s\n", d->name, variant, why);
    }
    lx_scene_stop_simulation(s);
    lx_scene_destroy(s);
    double fz_took = lx_time() - fz_t0;
    /* name anything heading towards the watchdog, so a CI log says which action it was */
    if (fz_took > 5.0) { fprintf(stderr, "SLOW %6.2fs  %s\n", fz_took, g_fz_now); fflush(stderr); }
    if (fz_took > g_fz_slowest) {
        g_fz_slowest = fz_took;
        lx_strcpy(g_fz_slowest_name, sizeof g_fz_slowest_name, g_fz_now);
    }
#if !defined(_WIN32)
    alarm(0);
#endif
}

static void test_fsm_fuzz(void) {
#if !defined(_WIN32)
    signal(SIGALRM, fz_alarm);
    signal(SIGSEGV, fz_crash);
    signal(SIGBUS, fz_crash);
    signal(SIGFPE, fz_crash);
    signal(SIGILL, fz_crash);
#endif
    printf("state graph actions (fuzz)\n");
    char dir[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), "luxora_test_fsmfuzz");
    lx_path_remove(dir);
    lx_mkdir_p(dir);
    lx_assets_set_root(dir);
    /* a prefab, a sound and a texture for the asset parameters */
    {
        LxScene *ps = lx_scene_create("p");
        LxEntity b = lx_spawn_primitive(ps, LX_PRIM_CUBE, "Box", lx_v3(0, 0, 0));
        char pp[LX_PATH_MAX];
        lx_path_join(pp, sizeof pp, dir, "Prefabs");
        lx_mkdir_p(pp);
        lx_path_join(pp, sizeof pp, dir, "Prefabs/Box.lxprefab");
        lx_prefab_save(ps, b, pp);
        lx_scene_destroy(ps);
    }
    char savef[LX_PATH_MAX];
    lx_path_join(savef, sizeof savef, dir, "save.txt");
    lx_save_set_file(savef);
    int n = lx_fsm_action_count();
    int runs = 0;
    const char *only = getenv("LUXORA_FUZZ_ACTION");
    if (getenv("LUXORA_FUZZ_SEED")) g_fz = (uint32_t)strtoul(getenv("LUXORA_FUZZ_SEED"), NULL, 10) * 2654435761u + 1;
    int variants = getenv("LUXORA_FUZZ_VARIANTS") ? atoi(getenv("LUXORA_FUZZ_VARIANTS")) : 8;
    for (int i = 0; i < n; i++) {
        const LxFsmActionDesc *d = lx_fsm_action_at(i);
        if (!d || (only && strcmp(only, d->name))) continue;
        if (!strcmp(d->name, "Quit Game")) continue;
        for (int v = 0; v < variants; v++) { fz_run(d, v, dir); runs++; }
    }
    CHECK(g_fz_broken == 0, "%d of %d action runs left the scene broken; first: %s", g_fz_broken, runs, g_fz_first);
    /* The watchdog that stops a true hang only exists where alarm() does, and only at 20s. This catches
       the near misses everywhere, well before that. The heaviest honest run is a spawn storm - a graph
       duplicating an object that duplicates in turn, until the per-frame budget stops it - which costs a
       second or two; anything an order of magnitude past that is a runaway, not work. */
    CHECK(g_fz_slowest < 10.0, "the slowest of %d action runs was %.2fs (%s)", runs, g_fz_slowest, g_fz_slowest_name);
    printf("  slowest action run: %.3fs (%s)\n", g_fz_slowest, g_fz_slowest_name);
    lx_physics_settings()->gravity = lx_v3(0, -9.81f, 0);
    lx_time_set_scale(1);
    lx_assets_clear();
}

/* ======================================================================
 *  what each action does: small graphs on the fuzz scene, then the results are checked
 * ====================================================================== */
static LxScene *g_ss;
static LxEntity g_so;
static char g_sdir[LX_PATH_MAX];
static int g_sem_n;

static void sem_begin(const char *body) {
    char text[8192], path[LX_PATH_MAX], name[64];
    snprintf(text, sizeof text,
             "[graph]\nname = Sem\nstart = A\n[event]\nname = GO\n"
             "[variable]\nname = f\ntype = float\nvalue = 0\n[variable]\nname = f2\ntype = float\nvalue = 0\n"
             "[variable]\nname = i\ntype = int\nvalue = 0\n[variable]\nname = i2\ntype = int\nvalue = 0\n"
             "[variable]\nname = b\ntype = bool\nvalue = false\n[variable]\nname = b2\ntype = bool\nvalue = false\n"
             "[variable]\nname = s\ntype = string\nvalue = \n[variable]\nname = s2\ntype = string\nvalue = \n"
             "[variable]\nname = v\ntype = vector3\nvalue = 0 0 0\n[variable]\nname = v2\ntype = vector3\nvalue = 0 0 0\n"
             "[variable]\nname = c\ntype = color\nvalue = 0 0 0 1\n[variable]\nname = q\ntype = quaternion\nvalue = 0 0 0\n"
             "[variable]\nname = arr\ntype = array\nvalue = float: \n"
             "[variable]\nname = sarr\ntype = array\nvalue = string: \n"
             "[variable]\nname = o\ntype = object\nvalue = None\n%s", body);
    snprintf(name, sizeof name, "Sem%d.lxgraph", ++g_sem_n);
    lx_path_join(path, sizeof path, g_sdir, name);
    lx_file_write(path, text, strlen(text));
    g_ss = fz_scene(&g_so);
    lx_physics_settings()->gravity = lx_v3(0, -9.81f, 0);
    lx_time_set_scale(1);
    lx_add_state_graph(g_ss, g_so, name);
}
static void sem_run(int frames, float dt) { for (int i = 0; i < frames; i++) lx_scene_simulate(g_ss, dt); }
static float sv(const char *var) { return lx_fsm_get_var_float(g_ss, g_so, 0, var); }
static const char *svs(const char *var) {
    LxFsmRun *r = lx_fsm_run_of(g_ss, g_so, 0);
    for (int i = 0; r && i < r->var_count; i++) if (!strcmp(r->var_names[i], var)) return r->vars[i].s;
    return "";
}
static LxVec3 svv(const char *var) {
    LxFsmRun *r = lx_fsm_run_of(g_ss, g_so, 0);
    for (int i = 0; r && i < r->var_count; i++) if (!strcmp(r->var_names[i], var)) return r->vars[i].v;
    return lx_v3(-999, -999, -999);
}
static LxVec4 svc(const char *var) {   /* colours, rotations and rects all live in the four-float field */
    LxFsmRun *r = lx_fsm_run_of(g_ss, g_so, 0);
    for (int i = 0; r && i < r->var_count; i++) if (!strcmp(r->var_names[i], var)) return r->vars[i].c;
    return lx_v4(-999, -999, -999, -999);
}
static LxEntity svo(const char *var) {
    LxFsmRun *r = lx_fsm_run_of(g_ss, g_so, 0);
    for (int i = 0; r && i < r->var_count; i++) if (!strcmp(r->var_names[i], var)) return r->vars[i].e;
    return 0;
}
static const char *sst(void) { return lx_fsm_current_state(g_ss, g_so, 0); }
static void sem_end(void) {
    lx_scene_stop_simulation(g_ss);
    lx_scene_destroy(g_ss);
    g_ss = NULL;
    lx_physics_settings()->gravity = lx_v3(0, -9.81f, 0);
    lx_time_set_scale(1);
}
/* one state A running the given actions; YES/NO/GO events lead to states named after them */
#define ONE(actions) "[event]\nname = YES\n[event]\nname = NO\n[state]\nname = A\ntransition = YES > Yes\ntransition = NO > No\ntransition = FINISHED > Done\n" actions \
                     "[state]\nname = Yes\n[state]\nname = No\n[state]\nname = Done\n"
static bool VEQ(LxVec3 a, float X, float Y, float Z, float e) { return NEAR(a.x, X, e) && NEAR(a.y, Y, e) && NEAR(a.z, Z, e); }

/* Sub States and regions */
static const char *s_in(const char *scope, int region) {
    LxFsmRun *r = lx_fsm_run_of(g_ss, g_so, 0);
    if (!r || !r->graph) return "-";
    int sc = scope ? lx_graph_state_index(r->graph, scope) : -1;
    LxFsmRun *k = lx_fsm_run_for_scope(r, sc, region);
    return k && k->state >= 0 ? r->graph->states[k->state].name : "-";
}
static void s_send(const char *ev) { lx_fsm_send_event(g_ss, g_so, ev, 0, 0); }

static void test_fsm_nesting(void) {
    printf("state graph Sub States, Exits, history and regions\n");
    const float DT = 1.0f / 60;
    const char *body =
        "[event]\nname = FIRE\n[event]\nname = RELOAD\n[event]\nname = LEAVE\n[event]\nname = HURT\n[event]\nname = DONE\n"
        "[event]\nname = SIGNAL\n[event]\nname = POKE\n"
        "[global]\nevent = HURT\nto = Reload\n[global]\nevent = SIGNAL\nto = Signal\n"
        "[state]\nname = A\ntransition = GO > Combat\n"
        "[state]\nname = Combat\nkind = sub\nhistory = 1\ntransition = DONE > A\ntransition = LEAVE > A\n"
        "[action]\ntype = Float Add\nupdate = every_frame\np0 = $f\np1 = 1\n"
        "[state]\nname = Aim\nparent = Combat\ntransition = FIRE > Shoot\ntransition = RELOAD > Reload\n"
        "[state]\nname = Shoot\nparent = Combat\ntransition = FINISHED > Out\n"
        "[state]\nname = Out\nparent = Combat\nkind = exit\nexit_event = DONE\n"
        "[state]\nname = Reload\nparent = Combat\n[action]\ntype = Wait\np0 = 100\n"
        "[state]\nname = Signal\nparent = Combat\n[action]\ntype = Send Event\np0 = LEAVE\np1 = 0\n"
        "[state]\nname = Breathe\nparent = Combat\nregion = 1\ntransition = POKE > Gasp\n[action]\ntype = Int Add\nupdate = every_frame\np0 = $i\np1 = 1\n"
        "[state]\nname = Gasp\nparent = Combat\nregion = 1\n"
        "[state]\nname = Radio\nregion = 1\n[action]\ntype = Float Add\nupdate = every_frame\np0 = $f2\np1 = 1\n"
        "[region]\nparent = Combat\nindex = 0\nname = Body\nstart = Aim\n"
        "[region]\nparent = Combat\nindex = 1\nname = Lungs\nstart = Breathe\n"
        "[region]\nparent = \nindex = 1\nname = Music\nstart = Radio\n";
    sem_begin(body);
    sem_run(3, DT);
    CHECK(!strcmp(sst(), "A") && !strcmp(s_in(NULL, 1), "Radio") && sv("f2") >= 2,
          "the graph's second region plays beside the first (%s, %s, %g)", sst(), s_in(NULL, 1), sv("f2"));
    s_send("GO");
    CHECK(!strcmp(sst(), "Combat") && !strcmp(s_in("Combat", 0), "Aim") && !strcmp(s_in("Combat", 1), "Breathe"),
          "entering a Sub State starts each of its regions (%s: %s, %s)", sst(), s_in("Combat", 0), s_in("Combat", 1));
    sem_run(3, DT);
    CHECK(sv("i") >= 2 && sv("f") >= 2, "the Sub State's own actions and its regions' actions run (%g, %g)", sv("f"), sv("i"));
    s_send("POKE");
    CHECK(!strcmp(s_in("Combat", 1), "Gasp") && !strcmp(s_in("Combat", 0), "Aim"), "an event changes the region that reacts to it (%s, %s)",
          s_in("Combat", 0), s_in("Combat", 1));
    s_send("FIRE");
    sem_run(1, DT);
    CHECK(!strcmp(sst(), "A") && !strcmp(s_in("Combat", 0), "-"),
          "FINISHED inside leads to an Exit, whose event takes the Sub State out (%s, %s)", sst(), s_in("Combat", 0));
    LxFsmRun *root = lx_fsm_run_of(g_ss, g_so, 0);
    bool saw_out = false;
    for (int h = 0; root && h < root->history_count; h++) saw_out |= root->history[h].state == lx_graph_state_index(root->graph, "Out");
    CHECK(saw_out, "the Exit shows in the history");
    s_send("GO");
    CHECK(!strcmp(s_in("Combat", 0), "Aim"), "history never brings it back to an Exit (%s)", s_in("Combat", 0));
    s_send("RELOAD");
    s_send("LEAVE");
    CHECK(!strcmp(sst(), "A"), "a transition on the Sub State leaves from any state inside (%s)", sst());
    s_send("GO");
    CHECK(!strcmp(s_in("Combat", 0), "Reload") && !strcmp(s_in("Combat", 1), "Gasp"),
          "with history, coming back carries on where each region left off (%s, %s)", s_in("Combat", 0), s_in("Combat", 1));
    s_send("LEAVE");
    s_send("HURT");
    CHECK(!strcmp(sst(), "Combat") && !strcmp(s_in("Combat", 0), "Reload") && !strcmp(s_in(NULL, 1), "Radio"),
          "a global transition jumps straight into a state inside a Sub State (%s / %s)", sst(), s_in("Combat", 0));
    s_send("LEAVE");
    s_send("SIGNAL");
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "A"), "an action inside a Sub State sends an event the Sub State itself takes (%s)", sst());
    for (int k = 0; k < 200; k++) { s_send(k % 3 == 0 ? "GO" : k % 3 == 1 ? "HURT" : "LEAVE"); sem_run(1, DT); }
    CHECK(strcmp(sst(), ""), "going in and out many times stays sound (%s)", sst());
    sem_end();

    /* the file keeps all of it, and problems are named */
    LxGraph *g = lx_graph_from_text(body);
    CHECK(g != NULL, "a graph with Sub States reads");
    if (g) {
        char *t = lx_graph_to_text(g);
        LxGraph *b = lx_graph_from_text(t);
        int ci = b ? lx_graph_state_index(b, "Combat") : -1, oi = b ? lx_graph_state_index(b, "Out") : -1;
        CHECK(b && ci >= 0 && b->states[ci].kind == LX_GS_SUB && b->states[ci].history && oi >= 0 && b->states[oi].kind == LX_GS_EXIT &&
                  !strcmp(b->states[oi].exit_event, "DONE") && lx_graph_region_count(b, ci) == 2 && lx_graph_region_count(b, -1) == 2 &&
                  !strcmp(lx_graph_region_name(b, ci, 1), "Lungs") && lx_graph_region_start(b, ci, 1) == lx_graph_state_index(b, "Breathe"),
              "Sub States, Exits and regions survive saving");
        if (b) {
            lx_graph_rename_state(b, ci, "Fight");
            int ai = lx_graph_state_index(b, "Aim");
            CHECK(!strcmp(b->states[ai].parent, "Fight") && lx_graph_region_start(b, lx_graph_state_index(b, "Fight"), 1) == lx_graph_state_index(b, "Breathe"),
                  "renaming a Sub State keeps what is inside it");
            int before = b->state_count;
            lx_graph_remove_state(b, lx_graph_state_index(b, "Fight"));
            CHECK(b->state_count == before - 8 && lx_graph_state_index(b, "Aim") < 0 && lx_graph_region_count(b, -1) == 2,
                  "deleting a Sub State deletes what is inside it (%d of %d left)", b->state_count, before);
            lx_graph_free(b);
        }
        lx_free(t);
        /* a transition that crosses into another Sub State is a problem */
        LxGraphState *a = lx_graph_find_state(g, "A");
        LxGraphTransition *x = lx_graph_add_transition(g, a, "FIRE");
        lx_strcpy(x->to, sizeof x->to, "Aim");
        char probs[16][160];
        int np = lx_graph_problems(g, probs, 16);
        bool named = false;
        for (int k = 0; k < np && k < 16; k++) named |= strstr(probs[k], "inside another Sub State") != NULL;
        CHECK(named, "a transition into another Sub State is listed as a problem");
        lx_graph_free(g);
    }
}

static void test_fsm_semantics(void) {
    printf("state graph actions (what they do)\n");
    lx_path_join(g_sdir, sizeof g_sdir, tmpdir(), "luxora_test_fsmsem");
    lx_path_remove(g_sdir);
    lx_mkdir_p(g_sdir);
    lx_assets_set_root(g_sdir);
    {
        LxScene *ps = lx_scene_create("p");
        LxEntity b = lx_spawn_primitive(ps, LX_PRIM_CUBE, "Box", lx_v3(0, 0, 0));
        char pp[LX_PATH_MAX];
        lx_path_join(pp, sizeof pp, g_sdir, "Prefabs");
        lx_mkdir_p(pp);
        lx_path_join(pp, sizeof pp, g_sdir, "Prefabs/Box.lxprefab");
        lx_prefab_save(ps, b, pp);
        lx_scene_destroy(ps);
    }
    char savef[LX_PATH_MAX];
    lx_path_join(savef, sizeof savef, g_sdir, "save.txt");
    lx_save_set_file(savef);
    const float DT = 1.0f / 60;

    /* ================= numbers ================= */
    sem_begin(ONE("[action]\ntype = Float Set Value\np0 = 2.5\np1 = $f\n"
                  "[action]\ntype = Float Operator\np0 = $f\np1 = 4\np2 = Multiply\np3 = $f2\n"
                  "[action]\ntype = Float Add\np0 = $f\np1 = 1\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 3.5f, 1e-5f) && NEAR(sv("f2"), 10, 1e-5f) && !strcmp(sst(), "Done"),
          "Float Set Value, Float Operator, Float Add, then FINISHED (%g %g %s)", sv("f"), sv("f2"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Int Operator\np0 = 7\np1 = 0\np2 = Divide\np3 = $i\n"
                  "[action]\ntype = Sqrt\np0 = 16\np1 = $f\n"
                  "[action]\ntype = Convert Float To Int\np0 = 2.6\np1 = Round\np2 = $i2\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 0 && NEAR(sv("f"), 4, 1e-5f) && sv("i2") == 3,
          "dividing by zero gives 0, square root, rounding (%g %g %g)", sv("i"), sv("f"), sv("i2"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Clamp\np0 = 9\np1 = 0\np2 = 1\np3 = $f\n"
                  "[action]\ntype = Float Remap\np0 = 5\np1 = 0\np2 = 10\np3 = 0\np4 = 100\np5 = true\np6 = $f2\n"
                  "[action]\ntype = Lerp\np0 = 0\np1 = 10\np2 = 0.25\np3 = false\np4 = $i\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 1, 1e-5f) && NEAR(sv("f2"), 50, 1e-4f) && sv("i") == 3,
          "Clamp, Float Remap, Lerp into an Int (%g %g %g)", sv("f"), sv("f2"), sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = In Range\np0 = 5\np1 = 0\np2 = 10\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "In Range stores true and takes the In Range event (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Float Compare\np0 = 1\np1 = 5\np2 = 0\np3 = \np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "Float Compare takes the Less Than event (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Int Add Wrap\np0 = $i\np1 = 1\np2 = 0\np3 = 2\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 1, "Int Add Wrap counts up (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Int Add Wrap\np0 = $i\np1 = 5\np2 = 0\np3 = 2\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 2, "Int Add Wrap wraps round past Max (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Bool Operator\np0 = true\np1 = false\np2 = XOR\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "Bool Operator XOR (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Bool Invert\np0 = $b\n[action]\ntype = Bool Test\np0 = $b\np1 = YES\np2 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "Bool Invert then Bool Test (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Random Float\np0 = 5\np1 = 6\np2 = $f\n"
                  "[action]\ntype = Random Int\np0 = 3\np1 = 3\np2 = $i\n"));
    sem_run(2, DT);
    CHECK(sv("f") >= 5 && sv("f") <= 6 && sv("i") == 3, "Random Float stays in range; Random Int with Min == Max (%g %g)", sv("f"), sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Atan2\np0 = 1\np1 = 0\np2 = true\np3 = $f\n"
                  "[action]\ntype = Sin\np0 = 90\np1 = true\np2 = $f2\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 90, 1e-3f) && NEAR(sv("f2"), 1, 1e-5f), "Atan2 and Sin in degrees (%g %g)", sv("f"), sv("f2"));
    sem_end();

    /* the whole int range, including the values that used to overflow the doubling loop */
    sem_begin(ONE("[action]\ntype = Closest Power Of Two\np0 = 100\np1 = $i\n"
                  "[action]\ntype = Next Power Of Two\np0 = 100\np1 = $i2\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 128 && sv("i2") == 128, "Closest and Next Power Of Two for 100 (%g %g)", sv("i"), sv("i2"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Closest Power Of Two\np0 = 2000000000\np1 = $i\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 1073741824, "Closest Power Of Two near the top of the int range stops at 2^30 (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Closest Power Of Two\np0 = -5\np1 = $i\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 1, "Closest Power Of Two of a negative number is 1 (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Int Operator\np0 = 2000000000\np1 = 2000000000\np2 = Add\np3 = $i\n"
                  "[action]\ntype = Int Multiply\np0 = $i2\np1 = 2\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 2147483647, "Int Operator saturates instead of overflowing (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Enum Switch\np0 = 2\np1 = \np2 = \np3 = YES\np4 = \np5 = \np6 = \np7 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "Enum Switch picks the event for case 2 (%s)", sst());
    sem_end();

    /* ================= text ================= */
    sem_begin(ONE("[action]\ntype = Float Set Value\np0 = 7\np1 = $f\n"
                  "[action]\ntype = Format String\np0 = Score: {0}, time {1}\np1 = $f\np2 = 1.5\np5 = $s\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "Score: 7, time 1.5"), "Format String fills {0} and {1} (\"%s\")", svs("s"));
    sem_end();

    sem_begin(ONE("[action]\ntype = String Change Case\np0 = Hello World\np1 = Upper\np2 = $s\n"
                  "[action]\ntype = String Length\np0 = Hello\np1 = $i\n"
                  "[action]\ntype = String Substring\np0 = Hello World\np1 = 6\np2 = 0\np3 = $s2\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "HELLO WORLD") && sv("i") == 5 && !strcmp(svs("s2"), "World"),
          "String Change Case, String Length, String Substring (\"%s\" %g \"%s\")", svs("s"), sv("i"), svs("s2"));
    sem_end();

    sem_begin(ONE("[action]\ntype = String Substring\np0 = Hello\np1 = -3\np2 = 0\np3 = $s\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "llo"), "a negative Start Index counts back from the end (\"%s\")", svs("s"));
    sem_end();

    sem_begin(ONE("[action]\ntype = String Index Of\np0 = Hello World\np1 = World\np2 = First\np3 = false\np4 = 2000000000\np5 = $i\n"));
    sem_run(2, DT);
    CHECK(sv("i") == -1, "String Index Of survives an absurd Start Index and reports not found (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = String Replace\np0 = a-b-c\np1 = -\np2 = +\np3 = false\np4 = $s\np5 = $i\n"
                  "[action]\ntype = String Contains\np0 = abcdef\np1 = CDE\np2 = true\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "a+b+c") && sv("i") == 2 && sv("b") == 1 && !strcmp(sst(), "Yes"),
          "String Replace counts its work; String Contains ignoring case (\"%s\" %g %g %s)", svs("s"), sv("i"), sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Float Parse\np0 = 12.5\np1 = $f\np2 = 0\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 12.5f, 1e-5f) && sv("b") == 1 && !strcmp(sst(), "Yes"), "Float Parse reads a number out of text (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Float Parse\np0 = not a number\np1 = $f\np2 = -1\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), -1, 1e-5f) && sv("b") == 0 && !strcmp(sst(), "No"), "Float Parse falls back to the default (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = String Pad\np0 = 7\np1 = 3\np2 = Left\np3 = 0\np4 = $s\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "007"), "String Pad makes a fixed-width number (\"%s\")", svs("s"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Convert Seconds To String\np0 = 90\np1 = m:ss\np2 = $s\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "1:30"), "Convert Seconds To String writes a clock (\"%s\")", svs("s"));
    sem_end();

    /* ================= vectors, rotations, colour ================= */
    sem_begin(ONE("[action]\ntype = Vector3 Set Value\np0 = 3 0 4\np1 = $v\n"
                  "[action]\ntype = Vector3 Magnitude\np0 = $v\np1 = $f\np2 = $f2\n"
                  "[action]\ntype = Vector3 Get XYZ\np0 = $v\np1 = $i\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 5, 1e-4f) && NEAR(sv("f2"), 25, 1e-3f) && sv("i") == 3,
          "Vector3 Set Value, Magnitude and Get XYZ (%g %g %g)", sv("f"), sv("f2"), sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Vector3 Operator\np0 = 1 2 3\np1 = 1 1 1\np2 = Add\np3 = $v\n"));
    sem_run(2, DT);
    CHECK(VEQ(svv("v"), 2, 3, 4, 1e-5f), "Vector3 Operator adds");
    sem_end();

    sem_begin(ONE("[action]\ntype = Vector3 Lerp\np0 = 0 0 0\np1 = 10 0 0\np2 = 0.5\np3 = false\np4 = $v\n"));
    sem_run(2, DT);
    CHECK(VEQ(svv("v"), 5, 0, 0, 1e-4f), "Vector3 Lerp lands halfway");
    sem_end();

    sem_begin(ONE("[action]\ntype = Vector3 Project On Plane\np0 = 1 5 0\np1 = 0 1 0\np2 = $v\n"));
    sem_run(2, DT);
    CHECK(VEQ(svv("v"), 1, 0, 0, 1e-5f), "Vector3 Project On Plane takes the vertical out");
    sem_end();

    sem_begin(ONE("[action]\ntype = Quaternion From Euler\np0 = 0 90 0\np1 = $q\n"
                  "[action]\ntype = Quaternion Rotate Vector\np0 = $q\np1 = 0 0 1\np2 = $v\n"));
    sem_run(2, DT);
    CHECK(VEQ(svv("v"), 1, 0, 0, 1e-4f), "a 90 degree yaw turns forward into right");
    sem_end();

    sem_begin(ONE("[action]\ntype = Color From RGBA\np0 = 255\np1 = 128\np2 = 0\np3 = 255\np4 = true\np5 = $c\n"
                  "[action]\ntype = Color Get RGBA\np0 = $c\np1 = false\np2 = $f\np3 = $f2\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 1, 1e-4f) && NEAR(sv("f2"), 128.0f / 255.0f, 1e-3f),
          "Color From RGBA reads 0-255 and Color Get RGBA gives back 0-1 (%g %g)", sv("f"), sv("f2"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Color From Hex String\np0 = #FF8000\np1 = $c\np2 = $b\np3 = YES\np4 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && NEAR(svc("c").x, 1, 1e-3f) && NEAR(svc("c").z, 0, 1e-3f) && !strcmp(sst(), "Yes"),
          "Color From Hex String reads #RRGGBB (%g)", svc("c").y);
    sem_end();

    sem_begin(ONE("[action]\ntype = Color HSV To RGB\np0 = 0\np1 = 1\np2 = 1\np3 = 1\np4 = $c\n"));
    sem_run(2, DT);
    CHECK(NEAR(svc("c").x, 1, 1e-4f) && NEAR(svc("c").y, 0, 1e-4f) && NEAR(svc("c").z, 0, 1e-4f),
          "hue 0, full saturation and value is pure red");
    sem_end();

    sem_begin(ONE("[action]\ntype = Rect Set Value\np0 = 0\np1 = 0\np2 = 100\np3 = 50\np4 = $c\n"
                  "[action]\ntype = Rect Contains\np0 = $c\np1 = 10 10\np2 = $b\np3 = YES\np4 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "Rect Set Value then Rect Contains (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Bounds Contains\np0 = 0 0 0\np1 = 2 2 2\np2 = 0.5 0.5 0.5\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "Bounds Contains finds a point inside the box (%g %s)", sv("b"), sst());
    sem_end();

    /* ================= lists ================= */
    sem_begin(ONE("[action]\ntype = List Add\np0 = $arr\np1 = 3\n"
                  "[action]\ntype = List Add\np0 = $arr\np1 = 1\n"
                  "[action]\ntype = List Add\np0 = $arr\np1 = 2\n"
                  "[action]\ntype = List Count\np0 = $arr\np1 = $i\np2 = $b\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 3 && sv("b") == 0, "List Add three times, List Count (%g %g)", sv("i"), sv("b"));
    sem_end();

    sem_begin(ONE("[action]\ntype = List Add\np0 = $arr\np1 = 3\n"
                  "[action]\ntype = List Add\np0 = $arr\np1 = 1\n"
                  "[action]\ntype = List Add\np0 = $arr\np1 = 2\n"
                  "[action]\ntype = List Sort\np0 = $arr\np1 = Ascending\n"
                  "[action]\ntype = List Get\np0 = $arr\np1 = 0\np2 = $f\n"
                  "[action]\ntype = List Get\np0 = $arr\np1 = -1\np2 = $f2\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 1, 1e-5f) && NEAR(sv("f2"), 3, 1e-5f),
          "List Sort orders them; List Get with -1 is the last item (%g %g)", sv("f"), sv("f2"));
    sem_end();

    sem_begin(ONE("[action]\ntype = List Add\np0 = $arr\np1 = 5\n"
                  "[action]\ntype = List Contains\np0 = $arr\np1 = 5\np2 = $b\np3 = YES\np4 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "List Contains finds a value (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = List Get\np0 = $arr\np1 = 99\np2 = $f\np3 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "No"), "List Get past the end takes the Out Of Range event rather than crashing (%s)", sst());
    sem_end();

    /* an absurd length must be capped, not spend minutes allocating billions of items */
    sem_begin(ONE("[action]\ntype = List Resize\np0 = $arr\np1 = 2000000000\np2 = $i\n"));
    {
        double t0 = lx_time();
        sem_run(2, DT);
        double took = lx_time() - t0;
        CHECK(sv("i") > 0 && sv("i") <= 1048576 && took < 5.0,
              "List Resize caps a silly length and returns promptly (%g items in %.2fs)", sv("i"), took);
    }
    sem_end();

    sem_begin(ONE("[action]\ntype = String Split\np0 = a,b,c\np1 = ,\np2 = $sarr\np3 = false\np4 = $i\n"
                  "[action]\ntype = String Join\np0 = $sarr\np1 = -\np2 = $s\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 3 && !strcmp(svs("s"), "a-b-c"), "String Split into a list, then String Join back (%g \"%s\")", sv("i"), svs("s"));
    sem_end();

    sem_begin(ONE("[action]\ntype = List Add\np0 = $arr\np1 = 4\n"
                  "[action]\ntype = List Add\np0 = $arr\np1 = 6\n"
                  "[action]\ntype = Float List Sum\np0 = $arr\np1 = $f\np2 = $f2\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 10, 1e-4f) && NEAR(sv("f2"), 5, 1e-4f), "Float List Sum totals and averages (%g %g)", sv("f"), sv("f2"));
    sem_end();

    /* ================= flow, events and time ================= */
    sem_begin(ONE("[action]\ntype = Int Switch\np0 = 1\np1 = 0\np2 = NO\np3 = 1\np4 = YES\np11 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "Int Switch picks the matching case (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Int Switch\np0 = 9\np1 = 0\np2 = NO\np3 = 1\np4 = NO\np11 = YES\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "Int Switch falls through to the default (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = String Switch\np0 = red\np1 = blue\np2 = NO\np3 = red\np4 = YES\np11 = false\np12 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "String Switch matches text (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Float Switch\np0 = 25\np1 = 20\np2 = NO\np3 = 50\np4 = YES\np9 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "Float Switch takes the first band the value falls below (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Wait\np0 = 0.05\np1 = YES\np3 = $f\np4 = $f2\n"));
    sem_run(1, DT);
    CHECK(!strcmp(sst(), "A"), "Wait holds the state while it counts (%s)", sst());
    sem_run(5, DT);
    CHECK(!strcmp(sst(), "Yes") && sv("f") >= 0.049f, "Wait then sends its event (%s %g)", sst(), sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Wait For Frames\np0 = 3\np1 = YES\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "A"), "Wait For Frames is still counting after 2 frames (%s)", sst());
    sem_run(3, DT);
    CHECK(!strcmp(sst(), "Yes"), "Wait For Frames finishes on the third (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Countdown Timer\np0 = 0.1\np1 = false\np2 = $f\np3 = $f2\np4 = YES\n"));
    sem_run(2, DT);
    CHECK(sv("f") > 0 && sv("f") < 0.1f, "Countdown Timer counts down (%g left)", sv("f"));
    sem_run(10, DT);
    CHECK(!strcmp(sst(), "Yes") && NEAR(sv("f"), 0, 1e-4f), "Countdown Timer reaches zero and fires (%s %g)", sst(), sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Tick Every\np0 = 0.05\np1 = YES\np2 = $i\n"));
    sem_run(1, DT);
    CHECK(!strcmp(sst(), "A"), "Tick Every has not fired yet (%s)", sst());
    sem_run(5, DT);
    CHECK(!strcmp(sst(), "Yes"), "Tick Every fires on its interval (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Add Per Second\np0 = $f\np1 = 60\np2 = 0\np3 = 1000\n"));
    sem_run(60, DT);
    CHECK(sv("f") > 55 && sv("f") < 65, "Add Per Second adds about its rate over a second (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Loop\np0 = 1\np1 = $i\np2 = NO\np3 = YES\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes") && sv("i") == 0, "Loop counts, then sends Finished when the count is reached (%g %s)", sv("i"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Send Event\np0 = YES\np1 = This Graph\np4 = 0\np5 = 3\n"
                  "[action]\ntype = Get Event Info\np0 = $f\n"));
    sem_run(3, DT);
    CHECK(!strcmp(sst(), "Yes"), "Send Event moves the graph on (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Get Time Info\np0 = $f\np1 = $f2\n"));
    sem_run(3, DT);
    CHECK(sv("f") > 0 && NEAR(sv("f2"), DT, 1e-4f), "Get Time Info reports the game time and the frame time (%g %g)", sv("f"), sv("f2"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Format Time\np0 = 125\np1 = m:ss\np2 = $s\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "2:05"), "Format Time writes a HUD clock (\"%s\")", svs("s"));
    sem_end();

    /* ================= graph variables and state ================= */
    sem_begin(ONE("[action]\ntype = Float Set Value\np0 = 11\np1 = $f\n"
                  "[action]\ntype = Get Graph Variable\np0 = Owner\np1 = 0\np2 = f\np3 = $f2\np4 = $b\np5 = NO\n"
                  "[action]\ntype = Get Graph State\np0 = Owner\np1 = 0\np2 = $s\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f2"), 11, 1e-5f) && sv("b") == 1 && !strcmp(svs("s"), "A"),
          "Get Graph Variable and Get Graph State read this graph (%g %g \"%s\")", sv("f2"), sv("b"), svs("s"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Graph Variable\np0 = Owner\np1 = 0\np2 = f\np3 = 42\np4 = $b\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 42, 1e-5f) && sv("b") == 1, "Set Graph Variable writes into a graph (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Get Graph Variable\np0 = Owner\np1 = 0\np2 = nosuchvariable\np3 = $f\np4 = $b\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 0 && !strcmp(sst(), "No"), "a missing variable takes the Not Found event (%g %s)", sv("b"), sst());
    sem_end();

    /* ================= save data and inventory ================= */
    lx_save_clear();
    sem_begin(ONE("[action]\ntype = Save Value\np0 = Score\np1 = 1234\np2 = true\n"
                  "[action]\ntype = Has Saved Value\np0 = Score\np1 = $b\np2 = YES\np3 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "Save Value then Has Saved Value (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Load Value\np0 = Score\np1 = $f\np2 = $b\np3 = YES\np4 = NO\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 1234, 1e-3f) && sv("b") == 1, "Load Value reads it back (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Delete Saved Value\np0 = Score\np1 = false\n"
                  "[action]\ntype = Has Saved Value\np0 = Score\np1 = $b\np2 = YES\np3 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 0 && !strcmp(sst(), "No"), "Delete Saved Value removes it (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Load Value\np0 = NeverSaved\np1 = $f\np2 = $b\np3 = YES\np4 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 0 && !strcmp(sst(), "No"), "Load Value of something never saved reports Not Found (%s)", sst());
    sem_end();

    lx_inventory_clear();
    sem_begin(ONE("[action]\ntype = Change Inventory\np0 = Add\np1 = Key\np2 = 2\np3 = $i\np4 = $b\np5 = YES\np6 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 2 && sv("b") == 1 && !strcmp(sst(), "Yes"), "Change Inventory adds items (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Change Inventory\np0 = Remove\np1 = Key\np2 = 5\np3 = $i\np4 = $b\np5 = YES\np6 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 0 && sv("i") == 2 && !strcmp(sst(), "No"),
          "removing more than you hold fails and changes nothing (%g %g %s)", sv("b"), sv("i"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Get Inventory Count\np0 = Key\np1 = $i\np2 = 2\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 2 && sv("b") == 1 && !strcmp(sst(), "Yes"), "Get Inventory Count says whether there is enough (%g)", sv("i"));
    sem_end();
    lx_inventory_clear();

    /* The music keeps the scene it plays in so it can cross-fade. Stopping it after that scene has gone
     * must not reach back into it: play, destroy, stop, and play again somewhere else. */
    {
        LxScene *a = lx_scene_create("MusicA");
        lx_music_play(a, "Sounds/beep.wav", 1.0f, 0.1f);
        CHECK(!strcmp(lx_music_current(), "Sounds/beep.wav"), "music starts in a scene (\"%s\")", lx_music_current());
        lx_scene_destroy(a);
        lx_music_stop(0.1f);        /* used to use the freed scene */
        CHECK(true, "stopping music after its scene was destroyed is safe");
        LxScene *b = lx_scene_create("MusicB");
        lx_music_play(b, "Sounds/beep.wav", 1.0f, 0.1f);
        CHECK(!strcmp(lx_music_current(), "Sounds/beep.wav"), "music starts again in a new scene");
        lx_scene_destroy(b);
        lx_music_stop(0);
    }

    /* ================= breakpoints ================= */
    lx_fsm_breakpoints_clear();
    sem_begin("[state]\nname = A\ntransition = FINISHED > B\n[action]\ntype = Wait\np0 = 0\n"
              "[state]\nname = B\nbreakpoint = 1\n");
    {
        LxFsmBreak bp;
        CHECK(!lx_fsm_breakpoint_taken(&bp), "nothing has tripped a breakpoint yet");
        sem_run(3, DT);
        bool hit = lx_fsm_breakpoint_taken(&bp);
        CHECK(hit && !strcmp(bp.state, "B"), "entering a state marked with a breakpoint records it (%s)", hit ? bp.state : "none");
        CHECK(!lx_fsm_breakpoint_taken(&bp), "reading it clears it, so Play is paused once and not every frame");
    }
    sem_end();

    {
        /* a breakpoint survives a save and load, so it stays set between editor sessions */
        LxGraph *g = lx_graph_new("Bp");                       /* comes with one state already */
        LxGraphState *st = lx_graph_add_state(g, "Stop Here", 0, 0);
        st->breakpoint = true;
        char *text = lx_graph_to_text(g);
        LxGraph *back = lx_graph_from_text(text);
        CHECK(back && back->state_count == 2 && !back->states[0].breakpoint && back->states[1].breakpoint,
              "a breakpoint is saved with the graph and read back, and only on the state that has one");
        lx_free(text);
        if (back) lx_graph_free(back);
        lx_graph_free(g);
    }

    {
        char gp[LX_PATH_MAX];
        lx_path_join(gp, sizeof gp, g_sdir, "save.txt");
        remove(gp);
        lx_path_join(gp, sizeof gp, g_sdir, "Globals.lxglobals");
        remove(gp);
    }
    lx_assets_clear();
}

/* ---------------- the built-in action library on a real scene ---------------- */
static void in_frame(void) {   /* a new input frame with the game view 1280 x 720 */
    lx_input_begin_frame();
}
static void in_step(void) {
    lx_game_ui_begin_frame(0, 0, 1280, 720, true);
    lx_input_update(1.0f / 60);
    lx_scene_simulate(g_ss, 1.0f / 60);
}
static void test_fsm_action_library(void) {
    printf("state graph: built-in action library\n");
    lx_path_join(g_sdir, sizeof g_sdir, tmpdir(), "luxora_test_fsmlib");
    lx_path_remove(g_sdir);
    lx_mkdir_p(g_sdir);
    lx_assets_set_root(g_sdir);
    const float DT = 1.0f / 60;
    memset(g_lx_input.keys, 0, sizeof g_lx_input.keys);
    memset(g_lx_input.mouse, 0, sizeof g_lx_input.mouse);
    g_lx_input.touch_count = 0;
    g_lx_input.width = 1280; g_lx_input.height = 720;
    lx_input_begin_frame();
    lx_input_begin_frame();
    {
        LxScene *ps = lx_scene_create("p");
        LxEntity b = lx_spawn_primitive(ps, LX_PRIM_CUBE, "Box", lx_v3(0, 0, 0));
        char pp[LX_PATH_MAX];
        lx_path_join(pp, sizeof pp, g_sdir, "Prefabs");
        lx_mkdir_p(pp);
        lx_path_join(pp, sizeof pp, g_sdir, "Prefabs/Box.lxprefab");
        lx_prefab_save(ps, b, pp);
        lx_scene_destroy(ps);
    }

    /* ---- +Z is forward; old files' cameras and lights are turned to keep looking the same way ---- */
    {
        LxQuat q = lx_quat_look(lx_v3(0, 0, 1), lx_v3(0, 1, 0));
        LxVec3 f = lx_quat_rotate(lx_quat_look(lx_v3(1, 0, 0), lx_v3(0, 1, 0)), lx_v3(0, 0, 1));
        CHECK(fabsf(q.w) > 0.9999f && VEQ(f, 1, 0, 0, 1e-5f), "lx_quat_look points +Z along the direction");
        const char *old = "[scene]\nname = Old\n[entity]\nid = 1\nparent = 0\nname = Cam\nactive = 1\nposition = 0 0 0\nrotation = 0 0 0 1\nscale = 1 1 1\n"
                          "camera.fov = 60\n[entity]\nid = 2\nparent = 0\nname = Box\nactive = 1\nposition = 0 0 0\nrotation = 0 0 0 1\nscale = 1 1 1\n";
        LxScene *os = lx_scene_deserialize(old);
        LxVec3 cf = lx_entity_forward(os, lx_entity_find(os, "Cam")), bf = lx_entity_forward(os, lx_entity_find(os, "Box"));
        char *again = lx_scene_serialize(os);
        LxScene *ns = lx_scene_deserialize(again);
        LxVec3 cf2 = lx_entity_forward(ns, lx_entity_find(ns, "Cam"));
        CHECK(VEQ(cf, 0, 0, -1, 1e-4f) && VEQ(bf, 0, 0, 1, 1e-4f) && VEQ(cf2, 0, 0, -1, 1e-4f),
              "an old scene's camera keeps looking along -Z (now its forward); other objects aren't turned; saving keeps it");
        lx_free(again);
        lx_scene_destroy(ns);
        lx_scene_destroy(os);
    }

    /* ---- transform ---- */
    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Trigger\np1 = 1 2 3\np5 = World\n"
                  "[action]\ntype = Get Position\np0 = name:Trigger\np1 = World\np2 = $v\np3 = $f\n"));
    sem_run(2, DT);
    CHECK(VEQ(svv("v"), 1, 2, 3, 1e-4f) && NEAR(sv("f"), 1, 1e-4f), "Set Position then Get Position");
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Trigger\np3 = 9\np5 = World\n"
                  "[action]\ntype = Get Position\np0 = name:Trigger\np1 = World\np2 = $v\n"));
    sem_run(2, DT);
    CHECK(NEAR(svv("v").y, 9, 1e-4f) && NEAR(svv("v").x, 0, 1e-4f),
          "Set Position with only Y set moves that axis and leaves the rest (%g %g)", svv("v").x, svv("v").y);
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Trigger\np1 = 0 0 0\np5 = World\n"
                  "[action]\ntype = Translate\np0 = name:Trigger\np1 = 0 0 6\np2 = World\np3 = true\n"));
    sem_run(60, DT);
    {
        LxVec3 p = lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Trigger"));
        CHECK(p.z > 5 && p.z < 7, "Translate with Per Second moves about its speed in a second (z %.2f)", p.z);
    }
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Scale\np0 = name:Trigger\np2 = 3\n"
                  "[action]\ntype = Get Scale\np0 = name:Trigger\np1 = $v\n"));
    sem_run(2, DT);
    CHECK(VEQ(svv("v"), 3, 3, 3, 1e-4f), "Set Scale with Uniform sets all three axes");
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Rotation\np0 = name:Trigger\np2 = 0 90 0\np6 = World\n"
                  "[action]\ntype = Get Direction Vectors\np0 = name:Trigger\np1 = $v\n"));
    sem_run(2, DT);
    CHECK(NEAR(svv("v").x, 1, 1e-3f), "a 90 degree yaw points forward along +X (%g %g %g)", svv("v").x, svv("v").y, svv("v").z);
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Trigger\np1 = 0 0 0\np5 = World\n"
                  "[action]\ntype = Get Distance\np0 = name:Trigger\np1 = name:Char\np3 = false\np4 = $f\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), sqrtf(2 * 2 + 1 * 1), 1e-3f), "Get Distance measures between two objects (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Trigger\np1 = 0 0 0\np5 = World\n"
                  "[action]\ntype = Transform Point\np0 = name:Trigger\np1 = 0 0 1\np2 = Local To World\np3 = false\np4 = $v\n"));
    sem_run(2, DT);
    CHECK(VEQ(svv("v"), 0, 0, 1, 1e-4f), "Transform Point takes a local point into the world");
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Trigger\np1 = 0 0 0\np5 = World\n"
                  "[action]\ntype = Look At\np0 = name:Trigger\np1 = name:Char\np4 = true\np5 = 0\np6 = $f\np7 = 1\np8 = YES\n"));
    sem_run(3, DT);
    CHECK(!strcmp(sst(), "Yes") && sv("f") < 1.0f, "Look At snaps round and reports it is facing the target (%g %s)", sv("f"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Trigger\np1 = 0 0 0\np5 = World\n"
                  "[action]\ntype = Move To Over Time\np0 = name:Trigger\np2 = 0 0 5\np3 = 0.1\np4 = Linear\np5 = YES\n"));
    sem_run(12, DT);
    {
        LxVec3 p = lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Trigger"));
        CHECK(!strcmp(sst(), "Yes") && p.z > 4.5f, "Move To Over Time arrives and fires (z %.2f %s)", p.z, sst());
    }
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Trigger\np1 = 0 0 0\np5 = World\n"
                  "[action]\ntype = Rotate Around\np0 = name:Trigger\np2 = 0 0 0\np3 = 0 1 0\np4 = 90\np5 = false\np6 = false\n"));
    sem_run(2, DT);
    CHECK(VEQ(lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Trigger")), 0, 0, 0, 1e-4f), "Rotate Around a pivot the object sits on leaves it put");
    sem_end();

    /* ---- objects ---- */
    sem_begin(ONE("[action]\ntype = Find Object\np0 = Char\np2 = $o\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && svo("o") && !strcmp(lx_entity_name(g_ss, svo("o")), "Char"),
          "Find Object by name (%s)", svo("o") ? lx_entity_name(g_ss, svo("o")) : "none");
    sem_end();

    sem_begin(ONE("[action]\ntype = Find Object\np0 = NoSuchThing\np2 = $o\np3 = $b\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 0 && !strcmp(sst(), "No"), "Find Object reports a miss rather than guessing (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Compare Tag\np0 = name:Char\np1 = Player\np2 = $b\np3 = YES\np4 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "Compare Tag (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Count With Tag\np0 = Enemy\np1 = $i\np2 = NO\np3 = YES\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 2 && !strcmp(sst(), "Yes"), "Count With Tag counts the enemies (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = Owner\np1 = 0 0 0\np5 = World\n"
                  "[action]\ntype = Find Closest With Tag\np0 = Owner\np1 = Enemy\np3 = 0\np4 = $o\np5 = $f\np6 = YES\np7 = NO\n"));
    sem_run(2, DT);
    CHECK(svo("o") && !strcmp(lx_entity_name(g_ss, svo("o")), "Enemy1"),
          "Find Closest With Tag picks the nearer one (%s at %g)", svo("o") ? lx_entity_name(g_ss, svo("o")) : "none", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Get Owner\np0 = $o\n[action]\ntype = Get Child\np0 = Owner\np1 = Child\np3 = $o\np5 = NO\np6 = YES\n"));
    sem_run(2, DT);
    CHECK(svo("o") && !strcmp(lx_entity_name(g_ss, svo("o")), "Child") && !strcmp(sst(), "Yes"),
          "Get Child by name (%s)", svo("o") ? lx_entity_name(g_ss, svo("o")) : "none");
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Object Active\np0 = name:Char\np1 = Deactivate\np2 = false\n"
                  "[action]\ntype = Is Object Active\np0 = name:Char\np1 = $b\np2 = YES\np3 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 0 && !strcmp(sst(), "No"), "Set Object Active then Is Object Active (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Create Object\np0 = Prefabs/Box.lxprefab\np2 = 4 0 4\np5 = $o\np6 = YES\np7 = NO\n"));
    sem_run(2, DT);
    CHECK(svo("o") && lx_entity_valid(g_ss, svo("o")) && !strcmp(sst(), "Yes"), "Create Object spawns a prefab (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Duplicate Object\np0 = name:Enemy1\np3 = $o\n"
                  "[action]\ntype = Count With Tag\np0 = Enemy\np1 = $i\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 3, "Duplicate Object copies the tag too (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Destroy Object\np0 = name:Enemy1\np1 = 0\n"
                  "[action]\ntype = Count With Tag\np0 = Enemy\np1 = $i\n"));
    sem_run(3, DT);
    CHECK(sv("i") == 1, "Destroy Object removes it (%g)", sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Parent\np0 = name:Char\np1 = Owner\np2 = true\n"
                  "[action]\ntype = Get Parent\np0 = name:Char\np1 = $o\np2 = YES\np3 = NO\n"));
    sem_run(2, DT);
    CHECK(svo("o") == g_so && !strcmp(sst(), "Yes"), "Set Parent then Get Parent (%s)", sst());
    sem_end();

    /* ---- physics ---- */
    sem_begin(ONE("[action]\ntype = Set Velocity\np0 = Owner\np1 = 0 0 4\np5 = World\n"
                  "[action]\ntype = Get Velocity\np0 = Owner\np1 = World\np2 = $v\np3 = $f\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 4, 0.5f), "Set Velocity then Get Velocity (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Rigidbody Properties\np0 = Owner\np1 = 7\np4 = false\n"
                  "[action]\ntype = Get Rigidbody Properties\np0 = Owner\np1 = $f\np2 = $b\np5 = $b2\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 7, 1e-4f) && sv("b") == 0 && sv("b2") == 1,
          "Set then Get Rigidbody Properties (%g %g %g)", sv("f"), sv("b"), sv("b2"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = Owner\np1 = 0 3 0\np5 = World\n"
                  "[action]\ntype = Raycast\np0 = Owner\np2 = 0 -1 0\np3 = World\np4 = 20\np5 = true\n"
                  "p7 = $b\np8 = $o\np9 = $v\np11 = $f\np12 = YES\np13 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes") && sv("f") > 0, "Raycast straight down finds the ground (%g %s)", sv("f"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = Owner\np1 = 0 50 0\np5 = World\n"
                  "[action]\ntype = Raycast\np0 = Owner\np2 = 0 1 0\np3 = World\np4 = 5\np5 = true\np7 = $b\np12 = YES\np13 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 0 && !strcmp(sst(), "No"), "a ray into empty sky misses (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = Owner\np1 = -2 0.5 1\np5 = World\n"
                  "[action]\ntype = Overlap Sphere\np0 = Owner\np2 = 3\np3 = Enemy\np4 = true\np5 = $arr\np6 = $i\np7 = $o\np8 = YES\np9 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("i") >= 1 && !strcmp(sst(), "Yes"), "Overlap Sphere finds the tagged enemies nearby (%g %s)", sv("i"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Physics Gravity\np0 = 0 -20 0\np1 = $v\n"));
    sem_run(2, DT);
    CHECK(NEAR(lx_physics_settings()->gravity.y, -20, 1e-4f), "Physics Gravity sets the scene gravity (%g)",
          lx_physics_settings()->gravity.y);
    sem_end();
    lx_physics_settings()->gravity = lx_v3(0, -9.81f, 0);

    sem_begin(ONE("[action]\ntype = Collision Event\np0 = Enter\np1 = \np2 = YES\np3 = $o\np4 = $b\n"));
    sem_run(1, DT);
    lx_fsm_system_event(g_ss, g_so, "COLLISION_ENTER", lx_entity_find(g_ss, "Char"), 3);
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes") && svo("o") && !strcmp(lx_entity_name(g_ss, svo("o")), "Char"),
          "Collision Event reports who we hit (%s %s)", sst(), svo("o") ? lx_entity_name(g_ss, svo("o")) : "none");
    sem_end();

    sem_begin(ONE("[action]\ntype = Trigger Event\np0 = Enter\np1 = Enemy\np2 = YES\np3 = $o\n"));
    sem_run(1, DT);
    lx_fsm_system_event(g_ss, g_so, "TRIGGER_ENTER", lx_entity_find(g_ss, "Char"), 0);   /* tagged Player, not Enemy */
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "A"), "Trigger Event ignores an object that does not carry the tag (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Is Grounded\np0 = name:Char\np1 = $b\np2 = YES\np3 = NO\n"));
    sem_run(3, DT);
    CHECK(!strcmp(sst(), "Yes") || !strcmp(sst(), "No"), "Is Grounded answers one way or the other (%s)", sst());
    sem_end();

    /* ---- input ---- */
    in_frame();
    g_lx_input.keys[LX_KEY_A] = true;
    sem_begin(ONE("[action]\ntype = Get Key\np0 = A\np1 = Held\np2 = $b\np3 = YES\np4 = NO\n"));
    in_step();
    in_step();
    CHECK(!strcmp(sst(), "Yes"), "Get Key sees A held down (%s)", sst());
    sem_end();
    g_lx_input.keys[LX_KEY_A] = false;
    in_frame();

    in_frame();
    g_lx_input.keys[LX_KEY_SPACE] = true;
    sem_begin(ONE("[action]\ntype = Get Input Action\np0 = Jump\np1 = Held\np2 = $b\np3 = YES\np4 = NO\n"));
    in_step();
    in_step();
    CHECK(!strcmp(sst(), "Yes"), "Get Input Action sees the Jump action through its default Space binding (%s)", sst());
    sem_end();
    g_lx_input.keys[LX_KEY_SPACE] = false;
    in_frame();

    in_frame();
    g_lx_input.keys[LX_KEY_D] = true;
    sem_begin(ONE("[action]\ntype = Get Input Axis\np0 = MoveX\np1 = MoveY\np2 = 1\np3 = $f\np4 = $f2\np7 = $i\np8 = YES\np9 = NO\n"));
    in_step();
    in_step();
    CHECK(sv("f") > 0.5f && !strcmp(sst(), "Yes"), "Get Input Axis reads MoveX while D is held (%g %s)", sv("f"), sst());
    sem_end();
    g_lx_input.keys[LX_KEY_D] = false;
    in_frame();

    /* Mouse Look keeps the pitch itself - it is an angle the player has arrived at, not one the input
       reports - so the first thing it must do is take whatever the head is already at, make sense of it,
       and bring it inside the limits. A head left at 200 degrees is really 160 the other way. */
    in_frame();
    sem_begin(ONE("[action]\ntype = Mouse Look\np0 = Owner\np1 = name:Child\np2 = Input Axes\np6 = -40\np7 = 40\np8 = \np9 = \np10 = false\np11 = $f\np12 = $f2\n"));
    {
        LxEntity head = lx_entity_find(g_ss, "Child");
        LxVec3 before = lx_entity_euler(g_ss, g_so);
        lx_entity_set_euler(g_ss, head, lx_v3(200, 0, 0));
        in_step();
        in_step();
        CHECK(NEAR(sv("f"), -40, 0.5f), "Mouse Look brings the pitch inside its limits (%g)", (double)sv("f"));
        CHECK(NEAR(lx_entity_euler(g_ss, head).x, -40, 0.5f), "and puts the head there (%g)",
              (double)lx_entity_euler(g_ss, head).x);
        CHECK(NEAR(lx_entity_euler(g_ss, g_so).y, before.y, 0.5f), "with nothing to look with, the body does not turn (%g)",
              (double)lx_entity_euler(g_ss, g_so).y);
    }
    sem_end();

    in_frame();
    g_lx_input.mouse[LX_MOUSE_LEFT] = true;
    sem_begin(ONE("[action]\ntype = Get Mouse Button\np0 = Left\np1 = Held\np2 = $b\np3 = YES\np4 = NO\n"));
    in_step();
    in_step();
    CHECK(!strcmp(sst(), "Yes"), "Get Mouse Button sees the left button held (%s)", sst());
    sem_end();
    g_lx_input.mouse[LX_MOUSE_LEFT] = false;
    in_frame();

    sem_begin(ONE("[action]\ntype = Get Touch\np0 = $i\np1 = 0\np6 = NO\np7 = YES\n"));
    in_step();
    in_step();
    CHECK(sv("i") == 0 && !strcmp(sst(), "No"), "Get Touch with no fingers down takes the No Touch event (%g %s)", sv("i"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Get Input Device\np0 = $i\np1 = $i2\np2 = YES\np3 = NO\np4 = NO\n"));
    in_step();
    in_step();
    CHECK(sv("i") >= 0 && sv("i") <= 2, "Get Input Device names the last device used (%g)", sv("i"));
    sem_end();

    /* ---- rendering and UI ---- */
    sem_begin(ONE("[action]\ntype = Set Camera\np0 = name:Cam\np1 = 35\n"
                  "[action]\ntype = Get Camera\np0 = name:Cam\np1 = $o\np2 = $f\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 35, 1e-4f), "Set Camera then Get Camera reads the field of view back (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Light\np0 = Owner\np2 = 4\n"
                  "[action]\ntype = Get Light\np0 = Owner\np2 = $f\np4 = $b\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 4, 1e-4f) && sv("b") == 1, "Set Light then Get Light (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Fade Light\np0 = Owner\np1 = 0\np2 = 0.05\np3 = YES\n"));
    sem_run(6, DT);
    {
        LxLight *l = lx_get_light(g_ss, g_so);
        CHECK(!strcmp(sst(), "Yes") && l && NEAR(l->intensity, 0, 1e-3f), "Fade Light reaches its target and fires (%s)", sst());
    }
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Environment\np0 = 0.05\np2 = 2\n"));
    sem_run(2, DT);
    {
        LxEnvironment *e = lx_scene_environment(g_ss);
        CHECK(e && NEAR(e->fog_density, 0.05f, 1e-5f) && NEAR(e->ambient_intensity, 2, 1e-5f),
              "Set Environment changes only the settings that are given");
    }
    sem_end();

    sem_begin(ONE("[action]\ntype = Get Screen Info\np0 = $i\np1 = $i2\np2 = $f\n"));
    in_step();
    in_step();
    CHECK(sv("i") == 1280 && sv("i2") == 720 && NEAR(sv("f"), 1280.0f / 720.0f, 1e-3f),
          "Get Screen Info reports the game view (%g x %g)", sv("i"), sv("i2"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Show Message\np0 = Checkpoint reached\np1 = 2\n"));
    in_step();
    in_step();
    CHECK(true, "Show Message runs without upsetting the frame");
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Objective\np0 = Find the key\n"));
    in_step();
    CHECK(!strcmp(lx_ui_objective(), "Find the key"), "Set Objective puts a line on screen (\"%s\")", lx_ui_objective());
    sem_end();
    lx_ui_set_objective("");
    lx_ui_overlays_clear();

    sem_begin(ONE("[action]\ntype = Draw Text\np0 = Hello\np1 = 10 10 0\np2 = 32\np3 = 1 1 1 1\np4 = Top Left\np5 = Regular\n"
                  "[action]\ntype = Draw Bar\np0 = 0.5\np1 = 10 60 0\np2 = 200 20 0\n"
                  "[action]\ntype = Draw Button\np0 = Play\np1 = 0 0 0\np2 = 200 60 0\np3 = YES\np4 = Center\np5 = $b\n"));
    in_step();
    in_step();
    CHECK(sv("b") == 0 && !strcmp(sst(), "A"), "Draw Text, Draw Bar and an unclicked Draw Button keep drawing (%s)", sst());
    sem_end();

    /* ---- gameplay ---- */
    sem_begin(ONE("[action]\ntype = Float Set Value\np0 = 100\np1 = $f\n"
                  "[action]\ntype = Apply Damage\np0 = $f\np1 = 30\np2 = 100\np3 = $f2\np4 = NO\np5 = YES\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 70, 1e-4f) && NEAR(sv("f2"), 0.7f, 1e-4f) && !strcmp(sst(), "Yes"),
          "Apply Damage takes health off and reports it as 0..1 (%g %g %s)", sv("f"), sv("f2"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Float Set Value\np0 = 10\np1 = $f\n"
                  "[action]\ntype = Apply Damage\np0 = $f\np1 = 30\np2 = 100\np3 = $f2\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("f") <= 0 && !strcmp(sst(), "Yes"), "Apply Damage sends the Died event when health runs out (%g %s)", sv("f"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = Owner\np1 = 0 1 0\np5 = World\n"
                  "[action]\ntype = Area Damage\np0 = Owner\np2 = 8\np3 = 10\np4 = true\np5 = Damage\np6 = Enemy\np7 = 0\np8 = $i\np9 = YES\np10 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("i") >= 1 && !strcmp(sst(), "Yes"), "Area Damage reaches the tagged enemies (%g %s)", sv("i"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = Owner\np1 = 0 1 0\np5 = World\n"
                  "[action]\ntype = Can See Target\np0 = Owner\np1 = name:Char\np2 = 50\np3 = 360\np4 = 0.5\np5 = 0.5\np6 = $b\np7 = $f\np8 = NO\np9 = YES\n"));
    sem_run(2, DT);
    CHECK(sv("f") > 0, "Can See Target reports the distance to the target (%g)", sv("f"));
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Position\np0 = Owner\np1 = 0 1 0\np5 = World\n"
                  "[action]\ntype = Check Angle To Target\np0 = Owner\np1 = name:Char\np2 = true\np3 = $f\np4 = 360\np5 = YES\np6 = $b\np7 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes") && sv("b") == 1, "a 360 degree view cone always counts as facing (%s)", sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Set Checkpoint\np0 = name:Char\n"));
    sem_run(2, DT);
    sem_end();
    sem_begin(ONE("[action]\ntype = Respawn At Checkpoint\np0 = name:Char\np1 = $b\np2 = YES\np3 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("b") == 1 && !strcmp(sst(), "Yes"), "Set Checkpoint, then Respawn At Checkpoint finds it (%g %s)", sv("b"), sst());
    sem_end();

    sem_begin(ONE("[action]\ntype = Spawner\np0 = Prefabs/Box.lxprefab\np1 = Owner\np2 = 0\np3 = 0.02\np4 = 4\np5 = 2\n"
                  "p6 = $i\np7 = $i2\np8 = $o\np9 = YES\np10 = NO\n"));
    sem_run(10, DT);
    CHECK(sv("i2") >= 1, "Spawner spawns on its interval (%g made, %g alive)", sv("i2"), sv("i"));
    sem_end();

    sem_begin(ONE("[action]\ntype = For Each With Tag\np0 = Enemy\np1 = $o\np2 = $i\np3 = YES\np4 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes") && sv("i") == 0 && svo("o") != 0,
          "For Each With Tag hands out the first tagged object (%g %s)", sv("i"), sst());
    sem_end();

    lx_assets_clear();
}


/* ---------------- the FPS and Third Person project templates ---------------- */
static LxScene *tp_open(const char *name, LxProjectTemplate tpl, LxProject *proj) {
    char dir[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), name);
    lx_path_remove(dir);
    CHECK(lx_project_create(proj, dir, "Game", tpl), "%s template creates", name);
    lx_assets_set_root(proj->assets);
    lx_fsm_globals_reload();
    /* every graph loads with no problems */
    char gdir[LX_PATH_MAX];
    lx_path_join(gdir, sizeof gdir, proj->assets, "Graphs");
    LxDirEntry *ents = 0;
    int n = lx_dir_list(gdir, &ents), graphs = 0;
    for (int i = 0; i < n; i++) {
        char ref[LX_REF_MAX];
        snprintf(ref, sizeof ref, "Graphs/%s", ents[i].name);
        LxGraph *g = lx_asset_graph(ref);
        char probs[8][160];
        int np = g ? lx_graph_problems(g, probs, 8) : -1;
        for (int k = 0; k < np && k < 8; k++) printf("    %s: %s\n", ents[i].name, probs[k]);
        CHECK(np == 0, "%s: %s has no problems", name, ents[i].name);
        graphs++;
    }
    lx_free(ents);
    CHECK(graphs == 0, "%s is all scripts: no graphs (%d)", name, graphs);
    char sp[LX_PATH_MAX];
    lx_path_join(sp, sizeof sp, proj->assets, "Scenes/Main.lxscene");
    LxScene *s = lx_scene_load(sp);
    CHECK(s != 0, "%s scene loads", name);
    return s;
}
static int tp_count_tag(LxScene *s, const char *tag) {
    int n = 0;
    for (int i = 0; i < lx_scene_entity_count(s); i++) { LxEntity e = lx_scene_entity_at(s, i); if (e && !strcmp(lx_entity_tag(s, e), tag)) n++; }
    return n;
}

#if LUXORA_SCRIPT_COMPILER
/* compiles and loads a project's Assets/Scripts like the editor does */
static bool tp_scripts(const LxProject *proj) {
    char dir[LX_PATH_MAX], sdk[LX_PATH_MAX], out[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, proj->assets, "Scripts");
    lx_path_join(sdk, sizeof sdk, proj->root, "Library/sdk");
    lx_path_join(out, sizeof out, proj->root, "Library/scripts.lxo");
    LxDirEntry *ents = NULL;
    int n = lx_dir_list(dir, &ents);
    static char paths[32][LX_PATH_MAX];
    const char *files[32];
    int nf = 0;
    for (int i = 0; i < n && nf < 32; i++) {
        size_t l = strlen(ents[i].name);
        if (ents[i].is_dir || l < 3 || strcmp(ents[i].name + l - 2, ".c")) continue;
        lx_path_join(paths[nf], sizeof paths[nf], dir, ents[i].name);
        files[nf] = paths[nf];
        nf++;
    }
    lx_free(ents);
    LxScriptCompileStats st;
    g_diag_errors = 0;
    bool ok = lx_script_compile(files, nf, sdk, out, test_diag, NULL, &st);
    CHECK(ok && st.errors == 0 && st.warnings == 0, "%s: %d scripts compile without errors or warnings (%d errors, %d warnings)", proj->name, nf, st.errors, st.warnings);
    CHECK(ok && lx_scripts_load(out), "%s: scripts load", proj->name);
    return ok;
}
static void tp_frame(LxScene *s) {
    lx_game_ui_begin_frame(0, 0, 1280, 720, true);
    lx_input_update(1.0f / 60);
    lx_scene_simulate(s, 1.0f / 60);
}
/* hold or tap something for one frame, then let go */
static void tp_tap(LxScene *s, bool *button) {
    lx_input_begin_frame(); *button = true; tp_frame(s);
    lx_input_begin_frame(); *button = false; tp_frame(s);
}
static float tp_param(LxScene *s, const char *ent, int i) {
    LxEntity e = lx_entity_find(s, ent);
    LxBehaviourRef *b = e ? lx_get_behaviour(s, e) : NULL;
    return b ? b->params[i] : -999;
}
static float G(const char *name) { return lx_fsm_global_float(name); }

static void test_templates(void) {
    printf("game templates (C scripts)\n");
    lx_input_begin_frame();   /* no mouse movement left over from the input tests */
    g_lx_input.prev_mouse_pos = g_lx_input.mouse_pos;
    memset(g_lx_input.keys, 0, sizeof g_lx_input.keys);
    memset(g_lx_input.mouse, 0, sizeof g_lx_input.mouse);
    lx_input_map_defaults(lx_input_map());
    lx_input_update(1.0f / 60);
    LxProject proj;
    /* ---- first person ---- */
    LxScene *s = tp_open("luxora_tpl_fps", LX_TEMPLATE_FPS, &proj);
    if (s && tp_scripts(&proj)) {
        lx_scene_destroy(s);   /* loaded again now its behaviours exist */
        char sp[LX_PATH_MAX];
        lx_path_join(sp, sizeof sp, proj.assets, "Scenes/Main.lxscene");
        s = lx_scene_load(sp);
        LxEntity player = lx_entity_find(s, "Player"), cam = lx_entity_find(s, "Main Camera");
        CHECK(player && cam && lx_entity_parent(s, cam) == player, "FPS: the camera rides on the player");
        CHECK(tp_count_tag(s, "Enemy") == 6 && lx_entity_find(s, "Door"), "FPS: six enemies and a door");
        for (int i = 0; i < 20; i++) tp_frame(s);
        CHECK(G("Game Phase") == 0 && G("Ammo") == 24 && G("Reserve Ammo") == 72 && G("Health") == 100, "FPS: the game starts (ammo %g / %g)", G("Ammo"), G("Reserve Ammo"));
        CHECK(lx_mouse_lock_wanted(), "FPS: the mouse is locked for looking");
        LxVec3 p0 = lx_entity_world_position(s, player);
        CHECK(p0.y > -0.1f && p0.y < 0.1f, "FPS: the player stands on the ground (%.2f)", p0.y);
        /* walking forward with W */
        lx_input_begin_frame(); g_lx_input.keys[LX_KEY_W] = true;
        for (int i = 0; i < 30; i++) { tp_frame(s); lx_input_begin_frame(); }
        g_lx_input.keys[LX_KEY_W] = false;
        tp_frame(s);
        LxVec3 p1 = lx_entity_world_position(s, player);
        CHECK(p1.z < p0.z - 1.5f, "FPS: W walks forward (%.2f -> %.2f)", p0.z, p1.z);
        /* shooting: Enemy 1 straight ahead, then the trigger */
        LxEntity e1 = lx_entity_find(s, "Enemy 1");
        lx_entity_set_position(s, e1, lx_v3(p1.x, 0, p1.z - 5));
        tp_frame(s);
        tp_tap(s, &g_lx_input.mouse[LX_MOUSE_LEFT]);
        for (int i = 0; i < 5; i++) tp_frame(s);
        float hp = tp_param(s, "Enemy 1", 0);
        CHECK(G("Ammo") == 23 && hp < 60, "FPS: a shot uses a bullet and hits (ammo %g, enemy health %g)", G("Ammo"), hp);
        CHECK(lx_entity_find(s, "Impact") != 0, "FPS: an impact spark");
        /* reload */
        for (int i = 0; i < 30; i++) tp_frame(s);
        tp_tap(s, &g_lx_input.keys[LX_KEY_R]);
        for (int i = 0; i < 100; i++) tp_frame(s);
        CHECK(G("Ammo") == 24 && G("Reserve Ammo") == 71, "FPS: reload (%g / %g)", G("Ammo"), G("Reserve Ammo"));
        /* a dead enemy scores and goes */
        lx_send_message(s, e1, "Damage", 1000, player);
        for (int i = 0; i < 60; i++) tp_frame(s);
        CHECK(!lx_entity_find(s, "Enemy 1") && G("Score") == 100, "FPS: a dead enemy scores and goes (score %g)", G("Score"));
        /* an enemy near the player chases and hurts them */
        LxEntity e2 = lx_entity_find(s, "Enemy 2");
        LxVec3 pp = lx_entity_world_position(s, player);
        lx_entity_set_position(s, e2, lx_v3(pp.x + 6, 0, pp.z));
        tp_tap(s, &g_lx_input.mouse[LX_MOUSE_LEFT]);   /* the shot is heard */
        for (int i = 0; i < 300 && G("Health") >= 100; i++) tp_frame(s);
        CHECK(G("Health") < 100, "FPS: an enemy behind the player hears the shot, chases and attacks (health %g)", G("Health"));
        lx_send_message(s, e2, "Damage", 1000, player);
        for (int i = 0; i < 40; i++) tp_frame(s);
        /* the door: stand in front of it, look at it, press E */
        LxEntity door = lx_entity_find(s, "Door");
        LxVec3 d0 = lx_entity_world_position(s, door);
        lx_entity_set_position(s, player, lx_v3(d0.x, 0, d0.z + 2.0f));
        for (int i = 0; i < 3; i++) tp_frame(s);
        tp_tap(s, &g_lx_input.keys[LX_KEY_E]);
        for (int i = 0; i < 60; i++) tp_frame(s);
        LxVec3 d1 = lx_entity_world_position(s, door);
        CHECK(fabsf(d1.x - d0.x) > 2, "FPS: E opens the door it looks at (x %.2f -> %.2f)", d0.x, d1.x);
        /* pause and resume with Esc */
        tp_tap(s, &g_lx_input.keys[LX_KEY_ESCAPE]);
        CHECK(G("Game Phase") == 1 && lx_time_scale() == 0 && !lx_mouse_lock_wanted(), "FPS: Esc pauses, freezes time and frees the mouse");
        tp_tap(s, &g_lx_input.keys[LX_KEY_ESCAPE]);
        CHECK(G("Game Phase") == 0 && lx_time_scale() == 1 && lx_mouse_lock_wanted(), "FPS: Esc again resumes");
        /* winning: every enemy gone */
        for (int i = 2; i <= 6; i++) {
            char n[16];
            snprintf(n, sizeof n, "Enemy %d", i);
            LxEntity e = lx_entity_find(s, n);
            if (e) lx_send_message(s, e, "Damage", 1000, player);
        }
        for (int i = 0; i < 60; i++) tp_frame(s);
        CHECK(G("Game Phase") == 2 && G("Score") == 3 * 100 + 3 * 150, "FPS: clearing the area wins (phase %g, score %g)", G("Game Phase"), G("Score"));
        lx_scene_stop_simulation(s);
        lx_scene_destroy(s);
        lx_scripts_unload();
    } else if (s) lx_scene_destroy(s);
    lx_assets_clear();

    /* ---- third person ---- */
    s = tp_open("luxora_tpl_tps", LX_TEMPLATE_THIRD_PERSON, &proj);
    if (s && tp_scripts(&proj)) {
        lx_scene_destroy(s);
        char sp[LX_PATH_MAX];
        lx_path_join(sp, sizeof sp, proj.assets, "Scenes/Main.lxscene");
        s = lx_scene_load(sp);
        LxEntity player = lx_entity_find(s, "Player");
        CHECK(tp_count_tag(s, "Coin") == 12 && tp_count_tag(s, "Enemy") == 4, "TPS: coins and enemies");
        for (int i = 0; i < 60; i++) tp_frame(s);
        LxVec3 cp = lx_entity_world_position(s, lx_entity_find(s, "Main Camera")), pp = lx_entity_world_position(s, player);
        float d = lx_len3(lx_sub3(cp, pp));
        CHECK(d > 3 && d < 8 && cp.z > pp.z, "TPS: the camera follows from behind (%.2f m)", d);
        CHECK(pp.y > -0.1f && pp.y < 0.2f, "TPS: the player stands on the ground (y %.2f)", pp.y);
        /* a coin */
        LxVec3 c1 = lx_entity_world_position(s, lx_entity_find(s, "Coin 1"));
        lx_entity_set_position(s, player, lx_v3(c1.x, 0, c1.z));
        for (int i = 0; i < 30; i++) tp_frame(s);
        CHECK(!lx_entity_find(s, "Coin 1") && G("Score") == 50 && G("Coins") == 1, "TPS: a coin is picked up (score %g, coins %g)", G("Score"), G("Coins"));
        /* the sword: a three-hit combo on a tough enemy right next to the player */
        LxEntity e1 = lx_entity_find(s, "Enemy 1");
        lx_get_behaviour(s, e1)->params[0] = 500;
        lx_get_behaviour(s, e1)->params[4] = 0;   /* a punching bag: it doesn't hit back */
        pp = lx_entity_world_position(s, player);
        lx_entity_set_position(s, e1, lx_v3(pp.x + 1.4f, 0, pp.z));
        tp_frame(s);
        for (int k = 0; k < 3; k++) {
            tp_tap(s, &g_lx_input.mouse[LX_MOUSE_LEFT]);
            for (int i = 0; i < 10; i++) { lx_entity_set_position(s, e1, lx_v3(pp.x + 1.4f, 0, pp.z)); tp_frame(s); }
        }
        for (int i = 0; i < 40; i++) tp_frame(s);
        float hp = tp_param(s, "Enemy 1", 0);
        CHECK(hp < 500 - 24 * 2 - 24 * 1.9f + 0.5f, "TPS: a three-hit combo (enemy health 500 -> %g)", hp);
        /* the dodge roll can't be hit early on */
        float before = G("Health");
        lx_input_begin_frame(); g_lx_input.keys[LX_KEY_C] = true; tp_frame(s);
        lx_send_message(s, player, "Damage", 10, e1);
        lx_input_begin_frame(); g_lx_input.keys[LX_KEY_C] = false; tp_frame(s);
        CHECK(G("Health") == before, "TPS: dodging avoids damage (%g -> %g)", before, G("Health"));
        for (int i = 0; i < 60; i++) tp_frame(s);
        before = G("Health");
        lx_send_message(s, player, "Damage", 10, e1);
        tp_frame(s);
        CHECK(G("Health") == before - 10, "TPS: ...but not standing still (%g)", G("Health"));
        /* lock on: the camera turns to keep the target in view */
        tp_tap(s, &g_lx_input.keys[LX_KEY_TAB]);
        for (int i = 0; i < 90; i++) tp_frame(s);
        LxVec3 camf = lx_quat_rotate(lx_entity_world_rotation(s, lx_entity_find(s, "Main Camera")), lx_v3(0, 0, 1));
        LxVec3 to = lx_sub3(lx_entity_world_position(s, e1), lx_entity_world_position(s, lx_entity_find(s, "Main Camera")));
        CHECK(lx_dot3(lx_norm3(lx_v3(camf.x, 0, camf.z)), lx_norm3(lx_v3(to.x, 0, to.z))) > 0.9f, "TPS: lock-on keeps the enemy in view");
        /* dying ends the game, and pickups don't bring you back */
        lx_send_message(s, player, "Damage", 1000, e1);
        for (int i = 0; i < 10; i++) tp_frame(s);
        CHECK(G("Game Phase") == 3 && G("Health") == 0 && !lx_mouse_lock_wanted(), "TPS: dying ends the game (phase %g)", G("Game Phase"));
        lx_scene_stop_simulation(s);
        lx_scene_destroy(s);
        lx_scripts_unload();
    } else if (s) lx_scene_destroy(s);
    lx_assets_clear();
    lx_time_set_scale(1);
    lx_mouse_reset();
}
#else
static void test_templates(void) {}
#endif

/* what a user built: Get Input Axis (named "Move X", with a space) into a vector, into Character Move */
static void test_axis_into_character_move(void) {
    printf("get axis into character move\n");
    const char *g = "[graph]\nname = Mover\nstart = Move\n"
                    "[variable]\nname = PlayerMovement\ntype = vector3\nvalue = 0 0 0\n"
                    "[state]\nname = Move\n"
                    "[action]\ntype = Get Input Axis\nupdate = every_frame\np0 = Move X\np1 = Move Y\np2 = 1\np6 = $PlayerMovement\n"
                    "[action]\ntype = Character Move\nupdate = every_frame\np0 = Owner\np1 = $PlayerMovement\np2 = World\np3 = 5\n";
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, tmpdir(), "luxora_axis_move");
    lx_mkdir_p(dir);
    lx_path_join(path, sizeof path, dir, "Mover.lxgraph");
    lx_file_write(path, g, strlen(g));
    lx_assets_set_root(dir);
    for (int with_cc = 0; with_cc < 2; with_cc++) {
        LxScene *s = lx_scene_create("Axis");
        LxEntity e = lx_entity_create(s, "Mover");
        if (with_cc) lx_add_character(s, e);
        lx_add_state_graph(s, e, "Mover.lxgraph");
        lx_input_begin_frame();
        g_lx_input.keys[LX_KEY_D] = true;   /* hold D: Move X = 1 */
        lx_input_update(1.0f / 60);
        for (int i = 0; i < 60; i++) lx_scene_simulate(s, 1.0f / 60);
        g_lx_input.keys[LX_KEY_D] = false;
        lx_input_begin_frame();
        lx_input_update(1.0f / 60);
        LxVec3 p = lx_entity_world_position(s, e);
        CHECK(p.x < -4 && p.x > -6, "holding D moves it right (-X, as a camera looking along +Z sees it) at 5 m/s, %s a Character Controller (x %.2f)", with_cc ? "with" : "without", p.x);
        lx_scene_stop_simulation(s);
        lx_scene_destroy(s);
    }
    lx_assets_clear();
}
