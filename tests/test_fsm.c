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
static void fz_alarm(int sig) {   /* a run that never ends: say which one */
    (void)sig;
    static const char msg[] = "\n  FAIL fuzz: this action run hangs: ";
    if (write(2, msg, sizeof msg - 1) < 0) {}
    if (write(2, g_fz_now, strlen(g_fz_now)) < 0) {}
    if (write(2, "\n", 1) < 0) {}
    _exit(3);
}
#endif
static char g_fz_first[400];

/* one graph: state A runs the action, state B runs it again (FINISHED/GO go back and forth) */
static void fz_run(const LxFsmActionDesc *d, int variant, const char *dir) {
    snprintf(g_fz_now, sizeof g_fz_now, "%s (variant %d)", d->name, variant);
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
        if (lx_scene_entity_count(s) > 300) break;   /* a graph copying itself: the spawn budget keeps it finite */
    }
    if (!ok) {
        g_fz_broken++;
        if (!g_fz_first[0]) snprintf(g_fz_first, sizeof g_fz_first, "%s (variant %d): %s", d->name, variant, why);
        else printf("    also: %s (variant %d): %s\n", d->name, variant, why);
    }
    lx_scene_stop_simulation(s);
    lx_scene_destroy(s);
#if !defined(_WIN32)
    alarm(0);
#endif
}

static void test_fsm_fuzz(void) {
#if !defined(_WIN32)
    signal(SIGALRM, fz_alarm);
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
             "[variable]\nname = i\ntype = int\nvalue = 0\n[variable]\nname = b\ntype = bool\nvalue = false\n"
             "[variable]\nname = s\ntype = string\nvalue = \n[variable]\nname = v\ntype = vector3\nvalue = 0 0 0\n"
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

    /* ---- variables & maths ---- */
    sem_begin(ONE("[action]\ntype = Set Float\np0 = $f\np1 = 2.5\n[action]\ntype = Float Operator\np0 = $f\np1 = 4\np2 = Multiply\np3 = $f2\n"
                  "[action]\ntype = Float Add\np0 = $f\np1 = 1\np2 = false\n"));
    sem_run(2, DT);
    CHECK(NEAR(sv("f"), 3.5f, 1e-5f) && NEAR(sv("f2"), 10, 1e-5f) && !strcmp(sst(), "Done"), "Set Float, Float Operator, Float Add, then FINISHED (%g %g %s)", sv("f"), sv("f2"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Int Operator\np0 = 7\np1 = 0\np2 = Divide\np3 = $i\n[action]\ntype = Float Function\np0 = 16\np1 = Square Root\np2 = $f\n"
                  "[action]\ntype = Convert\np0 = Float to Int\np1 = 2.6\np3 = $f2\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 0 && NEAR(sv("f"), 4, 1e-5f) && NEAR(sv("f2"), 3, 1e-5f), "divide by zero gives 0, sqrt, rounding (%g %g %g)", sv("i"), sv("f"), sv("f2"));
    sem_end();
    sem_begin(ONE("[action]\ntype = Set Float\np0 = $f\np1 = 7\n[action]\ntype = Build String\np0 = Score: {0}, time {1}\np1 = $f\np2 = 1.5\np3 = $s\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "Score: 7, time 1.50"), "Build String (%s)", svs("s"));
    sem_end();
    sem_begin(ONE("[action]\ntype = String Join\np0 = Hello\\s\np1 = World\np2 = $s\n[action]\ntype = String Length\np0 = $s\np1 = $i\n"
                  "[action]\ntype = String Contains\np0 = $s\np1 = WORLD\np2 = YES\np3 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(svs("s"), "Hello World") && sv("i") == 11 && !strcmp(sst(), "Yes"), "String Join / Length / Contains (%s %g %s)", svs("s"), sv("i"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Int Switch\np0 = 2\np1 = 1\np2 = NO\np3 = 2\np4 = YES\np9 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "Int Switch (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Float In Range\np0 = 5\np1 = 0\np2 = 10\np3 = NO\np4 = YES\np5 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "Float In Range (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = String Compare\np0 = abc\np1 = ABC\np2 = true\np3 = YES\np4 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "String Compare ignoring case (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Bool Flip\np0 = $b\n[action]\ntype = Bool Test\np0 = $b\np1 = YES\np2 = NO\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes"), "Bool Flip then Bool Test (%s)", sst());
    sem_end();
    {   /* random numbers stay in range */
        bool in = true;
        for (int k = 0; k < 40 && in; k++) {
            sem_begin(ONE("[action]\ntype = Random Int\np0 = 3\np1 = 5\np2 = $i\n[action]\ntype = Random Float\np0 = -1\np1 = 1\np2 = $f\n"
                          "[action]\ntype = Random Point\np0 = 10 0 10\np1 = 2\np2 = true\np3 = $v\n"));
            sem_run(1, DT);
            LxVec3 p = svv("v");
            in = sv("i") >= 3 && sv("i") <= 5 && sv("f") >= -1 && sv("f") <= 1 && p.y == 0 && lx_len3(lx_sub3(p, lx_v3(10, 0, 10))) <= 2.001f;
            sem_end();
        }
        CHECK(in, "Random Int / Float / Point stay in range");
    }
    sem_begin(ONE("[action]\ntype = Vector3 Operator\np0 = 1 2 3\np1 = 1 1 1\np2 = Add\np4 = $v\n[action]\ntype = Get Vector3 XYZ\np0 = $v\np2 = $f\n"
                  "[action]\ntype = Vector3 Angle\np0 = 1 0 0\np1 = 0 0 1\np2 = $f2\n"));
    sem_run(2, DT);
    CHECK(VEQ(svv("v"), 2, 3, 4, 1e-5f) && NEAR(sv("f"), 3, 1e-5f) && NEAR(sv("f2"), 90, 1e-3f), "Vector3 Operator / Get XYZ / Angle");
    sem_end();
    sem_begin(ONE("[action]\ntype = Float Interpolate\np0 = 0\np1 = 10\np2 = 0.5\np3 = Linear\np4 = $f\n"));
    sem_run(16, DT);
    float mid = sv("f");
    sem_run(30, DT);
    CHECK(mid > 2 && mid < 8 && NEAR(sv("f"), 10, 1e-4f) && !strcmp(sst(), "Done"), "Float Interpolate reaches the end, then FINISHED (mid %g, end %g, %s)", mid, sv("f"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Counter\np0 = $i\np1 = 3\np2 = YES\np3 = NO\np4 = false\n"));
    sem_run(1, DT);
    CHECK(sv("i") == 1 && !strcmp(sst(), "No"), "Counter counts the first time");
    sem_end();

    /* ---- time & flow ---- */
    sem_begin(ONE("[action]\ntype = Wait\np0 = 0.5\n"));
    sem_run(1, DT);
    sem_run(28, DT);
    bool early = strcmp(sst(), "A") == 0;
    sem_run(4, DT);
    CHECK(early && !strcmp(sst(), "Done"), "Wait 0.5 s finishes on time (%s)", sst());
    sem_end();
    sem_begin("[event]\nname = TICK\n[state]\nname = A\nsequence = 1\ntransition = FINISHED > Done\n"
              "[action]\ntype = Wait\np0 = 0.2\n[action]\ntype = Set Float\np0 = $f\np1 = 1\n[action]\ntype = Wait\np0 = 0.2\n[state]\nname = Done\n");
    sem_run(1, DT);
    sem_run(6, DT);
    bool not_yet = sv("f") == 0;
    sem_run(8, DT);
    bool second = sv("f") == 1 && !strcmp(sst(), "A");
    sem_run(14, DT);
    CHECK(not_yet && second && !strcmp(sst(), "Done"), "a sequence state runs its actions one after another (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Wait\np0 = 0.1\n[action]\ntype = Float Compare\nupdate = every_frame\np0 = 1\np1 = 2\n"));
    sem_run(30, DT);
    CHECK(!strcmp(sst(), "A"), "a state with an every-frame action doesn't send FINISHED (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Send Event\np0 = YES\np1 = This Graph\np4 = 0.3\n"));
    sem_run(10, DT);
    bool waiting = !strcmp(sst(), "A");
    sem_run(15, DT);
    CHECK(waiting && !strcmp(sst(), "Yes"), "Send Event with a delay (%s)", sst());
    sem_end();
    sem_begin("[event]\nname = YES\n[state]\nname = A\ntransition = GO > B\n[action]\ntype = Send Event\np0 = YES\np1 = This Graph\np4 = 0.3\n"
              "[state]\nname = B\ntransition = YES > Yes\n[state]\nname = Yes\n");
    sem_run(5, DT);
    lx_fsm_send_event(g_ss, g_so, "GO", 0, 0);
    sem_run(30, DT);
    CHECK(!strcmp(sst(), "B"), "leaving the state cancels a delayed Send Event (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Next Frame Event\np0 = YES\n"));
    sem_run(1, DT);
    bool same = !strcmp(sst(), "A");
    sem_run(1, DT);
    CHECK(same && !strcmp(sst(), "Yes"), "Next Frame Event (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Repeat Event\np0 = 0.1\np1 = GO\np2 = 3\np3 = YES\n"));
    sem_run(40, DT);
    CHECK(!strcmp(sst(), "Yes"), "Repeat Event N times then Done Event (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Set Float\np0 = $f\np1 = 0.25\n[action]\ntype = Countdown\np0 = $f\np1 = YES\n"));
    sem_run(10, DT);
    bool counting = sv("f") > 0 && sv("f") < 0.25f;
    sem_run(10, DT);
    CHECK(counting && !strcmp(sst(), "Yes") && sv("f") == 0, "Countdown (%g %s)", sv("f"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Get Time\nupdate = every_frame\np2 = $f\n"));
    sem_run(31, DT);
    CHECK(NEAR(sv("f"), 0.5f, 0.02f), "Get Time: time in the state (%g)", sv("f"));
    sem_end();
    sem_begin(ONE("[action]\ntype = Set Time Scale\np0 = 0.5\n"));
    sem_run(1, DT);
    CHECK(NEAR(lx_time_scale(), 0.5f, 1e-5f), "Set Time Scale");
    sem_end();

    /* ---- events ---- */
    sem_begin("[state]\nname = A\ntransition = GO > B\n[state]\nname = B\n[action]\ntype = Get Event Info\np0 = $f\np1 = $o\np3 = $s\n");
    sem_run(1, DT);
    LxEntity chr = lx_entity_find(g_ss, "Char");
    lx_fsm_send_event(g_ss, g_so, "GO", 42, chr);
    CHECK(!strcmp(sst(), "B") && NEAR(sv("f"), 42, 1e-5f) && svo("o") == chr && !strcmp(svs("s"), "GO"), "Get Event Info: value, sender, name");
    sem_end();
    sem_begin("[global]\nevent = GO\nto = Far\n[state]\nname = A\ntransition = FINISHED > B\n[state]\nname = B\n[action]\ntype = Wait\np0 = 10\n[state]\nname = Far\n");
    sem_run(3, DT);
    lx_fsm_send_event(g_ss, g_so, "GO", 0, 0);
    CHECK(!strcmp(sst(), "Far"), "a global transition works from any state (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Send Event\np0 = YES\np1 = This Graph\n[action]\ntype = Set Float\np0 = $f\np1 = 5\n"));
    sem_run(1, DT);
    CHECK(!strcmp(sst(), "Yes") && NEAR(sv("f"), 0, 1e-5f), "an event sent by an action leaves the state before the next action (%s %g)", sst(), sv("f"));
    sem_end();
    {   /* to every object with a tag */
        sem_begin(ONE("[action]\ntype = Send Event\np0 = YES\np1 = Tag\np3 = Enemy\n"));
        char gp[LX_PATH_MAX];
        lx_path_join(gp, sizeof gp, g_sdir, "Recv.lxgraph");
        const char *recv = "[graph]\nstart = Idle\n[event]\nname = YES\n[state]\nname = Idle\ntransition = YES > Got\n[state]\nname = Got\n";
        lx_file_write(gp, recv, strlen(recv));
        LxEntity e1 = lx_entity_find(g_ss, "Enemy1"), e2 = lx_entity_find(g_ss, "Enemy2");
        lx_add_state_graph(g_ss, e1, "Recv.lxgraph");
        lx_add_state_graph(g_ss, e2, "Recv.lxgraph");
        g_ss->entities[0].alive = g_ss->entities[0].alive;   /* (no-op) */
        sem_run(1, DT);   /* the receivers start this frame */
        lx_fsm_send_event(g_ss, g_so, "NOTHING", 0, 0);
        sem_end();
    }
    {
        char gp[LX_PATH_MAX];
        lx_path_join(gp, sizeof gp, g_sdir, "Recv2.lxgraph");
        const char *recv = "[graph]\nstart = Idle\n[event]\nname = HEY\n[state]\nname = Idle\ntransition = HEY > Got\n[state]\nname = Got\n";
        lx_file_write(gp, recv, strlen(recv));
        sem_begin("[event]\nname = HEY\n[state]\nname = A\ntransition = FINISHED > B\n[action]\ntype = Wait\np0 = 0.1\n[state]\nname = B\n"
                  "[action]\ntype = Send Event\np0 = HEY\np1 = Tag\np3 = Enemy\n[action]\ntype = Send Event\np0 = HEY\np1 = Children\np2 = Owner\n");
        LxEntity e1 = lx_entity_find(g_ss, "Enemy1"), e2 = lx_entity_find(g_ss, "Enemy2"), kid = lx_entity_find(g_ss, "Child"), ch = lx_entity_find(g_ss, "Char");
        lx_add_state_graph(g_ss, e1, "Recv2.lxgraph");
        lx_add_state_graph(g_ss, e2, "Recv2.lxgraph");
        lx_add_state_graph(g_ss, kid, "Recv2.lxgraph");
        lx_add_state_graph(g_ss, ch, "Recv2.lxgraph");
        sem_run(12, DT);
        CHECK(!strcmp(lx_fsm_current_state(g_ss, e1, 0), "Got") && !strcmp(lx_fsm_current_state(g_ss, e2, 0), "Got") &&
                  !strcmp(lx_fsm_current_state(g_ss, kid, 0), "Got") && !strcmp(lx_fsm_current_state(g_ss, ch, 0), "Idle"),
              "Send Event to a tag and to children reaches exactly those");
        sem_end();
    }

    /* ---- transform ---- */
    sem_begin(ONE("[action]\ntype = Set Position\np0 = name:Child\np1 = 5 6 7\np2 = World\n[action]\ntype = Get Position\np0 = name:Child\np1 = $v\np2 = World\n"));
    sem_run(1, DT);
    CHECK(VEQ(svv("v"), 5, 6, 7, 1e-4f), "Set Position in world space under a parent, Get Position");
    sem_end();
    sem_begin(ONE("[action]\ntype = Translate\nupdate = every_frame\np0 = name:Cam\np1 = 1 0 0\np2 = World\np3 = true\n"));
    LxVec3 c0 = lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Cam"));
    sem_run(61, DT);
    LxVec3 c1 = lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Cam"));
    CHECK(NEAR(c1.x - c0.x, 1, 0.03f), "Translate per second (%g)", c1.x - c0.x);
    sem_end();
    sem_begin(ONE("[action]\ntype = Move Towards\nupdate = every_frame\np0 = name:Cam\np1 = None\np2 = 0 3 4\np3 = 4\np4 = 0\np5 = YES\np6 = false\n"));
    sem_run(80, DT);
    CHECK(!strcmp(sst(), "Yes") && VEQ(lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Cam")), 0, 3, 4, 1e-3f), "Move Towards arrives and sends its event (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Look At\np0 = name:Cam\np1 = name:Char\np3 = false\n"));
    sem_run(1, DT);
    {
        LxEntity cam = lx_entity_find(g_ss, "Cam"), ch = lx_entity_find(g_ss, "Char");
        LxVec3 f = lx_entity_forward(g_ss, cam), want = lx_norm3(lx_sub3(lx_entity_world_position(g_ss, ch), lx_entity_world_position(g_ss, cam)));
        CHECK(lx_dot3(f, want) > 0.999f, "Look At faces the target (%g)", lx_dot3(f, want));
    }
    sem_end();
    sem_begin(ONE("[action]\ntype = Move Object\np0 = name:Cam\np1 = 1 1 1\np2 = 0.3\np3 = Ease In Out\np5 = false\n"));
    sem_run(30, DT);
    CHECK(VEQ(lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Cam")), 1, 1, 1, 1e-3f) && !strcmp(sst(), "Done"), "Move Object ends at its target, then FINISHED (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Get Distance\np0 = name:Enemy1\np1 = name:Enemy2\np2 = $f\np3 = 2\np4 = YES\np5 = NO\n"));
    sem_run(1, DT);
    CHECK(NEAR(sv("f"), 1, 1e-4f) && !strcmp(sst(), "Yes"), "Get Distance with a range (%g %s)", sv("f"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Set Parent\np0 = name:Cam\np1 = Owner\np2 = true\n"));
    LxVec3 before = lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Cam"));
    sem_run(1, DT);
    LxEntity camp = lx_entity_find(g_ss, "Cam");
    CHECK(lx_entity_parent(g_ss, camp) == g_so && lx_len3(lx_sub3(lx_entity_world_position(g_ss, camp), before)) < 0.05f, "Set Parent keeping the world position");
    sem_end();
    sem_begin(ONE("[action]\ntype = Orbit\nupdate = every_frame\np0 = name:Cam\np1 = None\np2 = 3\np3 = 90\np4 = 0 0 0\np5 = 1\n"));
    sem_run(30, DT);
    {
        LxVec3 p = lx_entity_world_position(g_ss, lx_entity_find(g_ss, "Cam"));
        CHECK(NEAR(sqrtf(p.x * p.x + p.z * p.z), 3, 1e-3f) && NEAR(p.y, 1, 1e-4f), "Orbit keeps its radius and height");
    }
    sem_end();
    sem_begin(ONE("[action]\ntype = Shake\np0 = name:Cam\np1 = 0.5\np2 = 0.2\n"));
    LxVec3 sh0 = lx_entity_position(g_ss, lx_entity_find(g_ss, "Cam"));
    sem_run(30, DT);
    CHECK(VEQ(lx_entity_position(g_ss, lx_entity_find(g_ss, "Cam")), sh0.x, sh0.y, sh0.z, 1e-5f) && !strcmp(sst(), "Done"), "Shake puts the object back where it was");
    sem_end();

    /* ---- objects ---- */
    sem_begin(ONE("[action]\ntype = Spawn Prefab\np0 = Prefabs/Box.lxprefab\np1 = None\np2 = 1 2 3\np5 = $o\n"));
    int n0 = lx_scene_entity_count(g_ss);
    sem_run(1, DT);
    CHECK(lx_scene_entity_count(g_ss) == n0 + 1 && svo("o") && VEQ(lx_entity_world_position(g_ss, svo("o")), 1, 2, 3, 1e-4f), "Spawn Prefab");
    sem_end();
    sem_begin(ONE("[action]\ntype = Find Object\np0 = Tag\np1 = Player\np2 = $o\np3 = YES\np4 = NO\n[action]\ntype = Has Tag\np0 = $o\np1 = Player\n"));
    sem_run(1, DT);
    CHECK(svo("o") == lx_entity_find(g_ss, "Char") && !strcmp(sst(), "Yes"), "Find Object by tag");
    sem_end();
    sem_begin(ONE("[action]\ntype = Count With Tag\np0 = Enemy\np1 = $i\n[action]\ntype = Find Closest With Tag\np0 = Owner\np1 = Enemy\np2 = $o\np4 = YES\np5 = NO\n"));
    sem_run(1, DT);
    CHECK(svo("o") == lx_entity_find(g_ss, "Enemy1") && sv("i") == 2 && !strcmp(sst(), "Yes"), "Find Closest With Tag, Count With Tag (%u %u %g %s)", svo("o"), lx_entity_find(g_ss, "Enemy1"), sv("i"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Get Child\np0 = Owner\np1 = Child\np2 = $o\n[action]\ntype = Set Active\np0 = $o\np1 = false\n"));
    sem_run(1, DT);
    CHECK(svo("o") == lx_entity_find(g_ss, "Child") && !lx_entity_active(g_ss, svo("o")), "Get Child, Set Active");
    sem_end();
    sem_begin(ONE("[action]\ntype = Destroy Object\np0 = name:Enemy2\np1 = 0\n[action]\ntype = Set Float\np0 = $f\np1 = 1\n"));
    LxEntity en2 = lx_entity_find(g_ss, "Enemy2");
    sem_run(2, DT);
    CHECK(!lx_entity_valid(g_ss, en2) && NEAR(sv("f"), 1, 1e-5f), "Destroy Object (the rest of the state still runs)");
    sem_end();
    sem_begin(ONE("[action]\ntype = Destroy Object\np0 = Owner\np1 = 0\n[action]\ntype = Set Float\np0 = $f\np1 = 1\n"));
    LxEntity own = g_so;
    sem_run(3, DT);
    CHECK(!lx_entity_valid(g_ss, own), "a graph can destroy its own object");
    sem_end();
    sem_begin(ONE("[action]\ntype = Duplicate Object\np0 = name:Enemy1\np1 = 0 5 0\np2 = $o\n"));
    sem_run(1, DT);
    CHECK(svo("o") && NEAR(lx_entity_world_position(g_ss, svo("o")).y, 5.5f, 1e-4f), "Duplicate Object with an offset");
    sem_end();

    /* ---- physics ---- */
    sem_begin(ONE("[action]\ntype = Set Velocity\np0 = Owner\np1 = 0 0 3\n[action]\ntype = Get Velocity\nupdate = once\np0 = Owner\np1 = $v\np2 = $f\n"));
    sem_run(1, DT);
    CHECK(NEAR(svv("v").z, 3, 0.2f) && NEAR(sv("f"), 3, 0.3f), "Set Velocity / Get Velocity (%g)", svv("v").z);
    sem_end();
    sem_begin(ONE("[action]\ntype = Add Force\np0 = Owner\np1 = 0 10 0\np2 = Velocity Change\np3 = World\n[action]\ntype = Get Velocity\nupdate = once\np0 = Owner\np1 = $v\n"));
    sem_run(1, DT);
    CHECK(svv("v").y > 9, "Add Force as a velocity change (%g)", svv("v").y);
    sem_end();
    sem_begin(ONE("[action]\ntype = Raycast\np0 = name:Cam\np1 = 0 0 0\np2 = 0 -1 0\np3 = World\np4 = 50\np5 = YES\np6 = NO\np8 = $o\np9 = $v\n"));
    sem_run(2, DT);
    CHECK(!strcmp(sst(), "Yes") && svo("o") == lx_entity_find(g_ss, "Ground") && NEAR(svv("v").y, 0, 0.05f), "Raycast hits the ground under it (%s)", sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Overlap Sphere\nupdate = once\np0 = name:Enemy1\np2 = 1.5\np3 = Enemy\np4 = $i\np6 = YES\np7 = NO\n"));
    sem_run(2, DT);
    CHECK(sv("i") == 1 && !strcmp(sst(), "Yes"), "Overlap Sphere with a tag (%g %s)", sv("i"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Set Gravity\np0 = 0 -1.6 0\n"));
    sem_run(1, DT);
    CHECK(NEAR(lx_physics_settings()->gravity.y, -1.6f, 1e-5f), "Set Gravity");
    sem_end();
    sem_begin(ONE("[action]\ntype = Collision Event\np0 = Enter\np1 = Player\np2 = YES\np3 = $o\n"));
    sem_run(2, DT);
    lx_fsm_system_event(g_ss, g_so, "COLLISION_ENTER", lx_entity_find(g_ss, "Enemy1"), 0);   /* wrong tag */
    sem_run(1, DT);
    bool ignored = !strcmp(sst(), "A");
    lx_fsm_system_event(g_ss, g_so, "COLLISION_ENTER", lx_entity_find(g_ss, "Enemy1"), 0);
    lx_fsm_system_event(g_ss, g_so, "COLLISION_ENTER", lx_entity_find(g_ss, "Char"), 0);   /* two in one frame */
    sem_run(1, DT);
    CHECK(ignored && !strcmp(sst(), "Yes") && svo("o") == lx_entity_find(g_ss, "Char"), "Collision Event filters by tag, sees every collision of a frame (%s)", sst());
    sem_end();

    /* ---- rendering, camera, audio, save ---- */
    sem_begin(ONE("[action]\ntype = Set Light\np0 = Owner\np1 = 1 0 0 1\np2 = 3\n[action]\ntype = Set Field Of View\np0 = name:Cam\np1 = 30\n"
                  "[action]\ntype = Set Volume And Pitch\np0 = Owner\np1 = 0.5\np2 = 2\n"));
    sem_run(1, DT);
    CHECK(NEAR(lx_get_light(g_ss, g_so)->intensity, 3, 1e-5f) && NEAR(lx_get_light(g_ss, g_so)->color.y, 0, 1e-5f) &&
              NEAR(lx_get_camera(g_ss, lx_entity_find(g_ss, "Cam"))->fov_deg, 30, 1e-5f) && NEAR(lx_get_audio_source(g_ss, g_so)->pitch, 2, 1e-5f),
          "Set Light, Set Field Of View, Set Volume And Pitch");
    sem_end();
    sem_begin(ONE("[action]\ntype = Fade Light\np0 = Owner\np1 = 0\np2 = 0.2\n"));
    sem_run(20, DT);
    CHECK(NEAR(lx_get_light(g_ss, g_so)->intensity, 0, 1e-5f) && !strcmp(sst(), "Done"), "Fade Light (%g)", lx_get_light(g_ss, g_so)->intensity);
    sem_end();
    sem_begin("[state]\nname = A\ntransition = GO > B\n[action]\ntype = Light Flicker\np0 = Owner\np1 = 0.9\np2 = 3\n[state]\nname = B\n");
    float base = lx_get_light(g_ss, g_so)->intensity;
    sem_run(20, DT);
    lx_fsm_send_event(g_ss, g_so, "GO", 0, 0);
    CHECK(NEAR(lx_get_light(g_ss, g_so)->intensity, base, 1e-5f), "Light Flicker restores the light when the state ends");
    sem_end();
    sem_begin(ONE("[action]\ntype = Save Value\np0 = test_key\np1 = Int\np2 = 77\n[action]\ntype = Load Value\np0 = test_key\np1 = Float\np2 = 0\np3 = $f\np4 = YES\np5 = NO\n"));
    sem_run(1, DT);
    CHECK(NEAR(sv("f"), 77, 1e-4f) && !strcmp(sst(), "Yes"), "Save Value then Load Value (%g)", sv("f"));
    sem_end();
    sem_begin(ONE("[action]\ntype = Delete Saved Value\np0 = test_key\n[action]\ntype = Load Value\np0 = test_key\np2 = 5\np3 = $f\np4 = NO\np5 = YES\n"));
    sem_run(1, DT);
    CHECK(NEAR(sv("f"), 5, 1e-4f) && !strcmp(sst(), "Yes"), "Delete Saved Value: Load gives the default");
    sem_end();
    {   /* global variables: one graph writes, another reads */
        char gp[LX_PATH_MAX];
        lx_path_join(gp, sizeof gp, g_sdir, "Globals.lxglobals");
        const char *gl = "[variable]\nname = score\ntype = float\nvalue = 1\n";
        lx_file_write(gp, gl, strlen(gl));
        lx_assets_clear();   /* reloads the globals */
        lx_fsm_globals_reset_values();
        lx_path_join(gp, sizeof gp, g_sdir, "Reader.lxgraph");
        const char *rd = "[graph]\nstart = Idle\n[event]\nname = HIGH\n[state]\nname = Idle\ntransition = HIGH > High\n"
                         "[action]\ntype = Float Compare\nupdate = every_frame\np0 = $score\np1 = 6\np3 = HIGH\n[state]\nname = High\n";
        lx_file_write(gp, rd, strlen(rd));
        sem_begin(ONE("[action]\ntype = Float Add\np0 = $score\np1 = 5\n"));
        LxEntity reader = lx_entity_find(g_ss, "Enemy1");
        lx_add_state_graph(g_ss, reader, "Reader.lxgraph");
        sem_run(3, DT);
        CHECK(NEAR(lx_fsm_global_float("score"), 6, 1e-4f) && !strcmp(lx_fsm_current_state(g_ss, reader, 0), "High"), "a global variable set by one graph is seen by another (%g)",
              lx_fsm_global_float("score"));
        sem_end();
        remove(gp);
        lx_path_join(gp, sizeof gp, g_sdir, "Globals.lxglobals");
        remove(gp);
    }
    lx_assets_clear();
}
