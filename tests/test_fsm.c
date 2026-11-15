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

/* ---------------- the PlayMaker-style actions (actions4.c) ---------------- */
static void pm_frame(void) {   /* a new input frame with the game view 1280 x 720 */
    lx_input_begin_frame();
}
static void pm_step(void) {
    lx_game_ui_begin_frame(0, 0, 1280, 720, true);
    lx_input_update(1.0f / 60);
    lx_scene_simulate(g_ss, 1.0f / 60);
}
static void test_fsm_playmaker(void) {
    printf("state graph: PlayMaker-style actions\n");
    lx_path_join(g_sdir, sizeof g_sdir, tmpdir(), "luxora_test_fsmpm");
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

    /* ---- +Z is forward (like Unity); old files' cameras and lights are turned to keep looking the same way ---- */
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

    /* ---- logic, maths, text ---- */
    sem_begin(ONE("[action]\ntype = Bool Operator\np0 = true\np1 = false\np2 = XOR\np3 = $b\n"
                  "[action]\ntype = Float Remap\np0 = 5\np1 = 0\np2 = 10\np3 = 100\np4 = 200\np6 = $f\n"
                  "[action]\ntype = Trigonometry\np0 = 1\np1 = Asin\np2 = true\np3 = $f2\n"
                  "[action]\ntype = Float Switch\np0 = 30\np1 = 25\np2 = NO\np3 = 50\np4 = YES\np9 = NO\n"));
    sem_run(1, DT);
    CHECK(sv("b") == 1 && NEAR(sv("f"), 150, 1e-4f) && NEAR(sv("f2"), 90, 1e-3f) && !strcmp(sst(), "Yes"),
          "Bool Operator, Float Remap, Trigonometry, Float Switch (%g %g %g %s)", sv("b"), sv("f"), sv("f2"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = String Replace\np0 = a-b-c\np1 = -\np2 = ::\np3 = $s\n"
                  "[action]\ntype = String Split\np0 = $s\np1 = ::\np2 = 2\np3 = $s\np4 = $i\n"
                  "[action]\ntype = String Case\np0 = $s\np1 = Upper\np2 = $s\n"
                  "[action]\ntype = String To Number\np0 = 12.5\np1 = $f\np3 = YES\np4 = NO\n"));
    sem_run(1, DT);
    CHECK(!strcmp(svs("s"), "C") && sv("i") == 3 && NEAR(sv("f"), 12.5f, 1e-5f) && !strcmp(sst(), "Yes"),
          "String Replace, String Split, String Case, String To Number (%s %g %g %s)", svs("s"), sv("i"), sv("f"), sst());
    sem_end();
    sem_begin(ONE("[action]\ntype = Get Substring\np0 = Luxora Engine\np1 = 7\np2 = 3\np3 = $s\n"
                  "[action]\ntype = String To Number\np0 = twelve\np1 = $f\np3 = YES\np4 = NO\n"));
    sem_run(1, DT);
    CHECK(!strcmp(svs("s"), "Eng") && !strcmp(sst(), "No"), "Get Substring, and String To Number on words (%s %s)", svs("s"), sst());
    sem_end();
    sem_begin("[variable]\nname = col\ntype = color\nvalue = 0 0 0 1\n"
              ONE("[action]\ntype = Color Ramp\np0 = 0.75\np1 = 1 0 0 1\np2 = 0 1 0 1\np3 = 0 0 1 1\np5 = $col\n"
                  "[action]\ntype = Get Color RGBA\np0 = $col\np2 = $f\np3 = $f2\n"));
    sem_run(1, DT);
    CHECK(NEAR(sv("f"), 0.5f, 1e-4f) && NEAR(sv("f2"), 0.5f, 1e-4f), "Color Ramp halfway between green and blue (%g %g)", sv("f"), sv("f2"));
    sem_end();
    sem_begin(ONE("[action]\ntype = Set Vector3\np0 = $v\np1 = 3 0 4\n[action]\ntype = Vector3 Clamp Magnitude\np0 = $v\np1 = 2.5\n"
                  "[action]\ntype = Get Vector Length\np0 = $v\np1 = $f\n[action]\ntype = Get Atan2\np0 = 1\np1 = 0\np2 = true\np3 = $f2\n"));
    sem_run(1, DT);
    CHECK(NEAR(sv("f"), 2.5f, 1e-4f) && VEQ(svv("v"), 1.5f, 0, 2, 1e-4f) && NEAR(sv("f2"), 90, 1e-3f), "Vector3 Clamp Magnitude, Get Vector Length, Get Atan2 (%g, %g %g %g, %g)", sv("f"), svv("v").x, svv("v").y, svv("v").z, sv("f2"));
    sem_end();
    sem_begin(ONE("[action]\ntype = Vector3 Interpolate\np0 = $v\np1 = 0 0 0\np2 = 10 0 0\np3 = 0.5\np5 = YES\n"));
    sem_run(15, DT);
    float half = svv("v").x;
    sem_run(30, DT);
    CHECK(half > 3 && half < 7 && NEAR(svv("v").x, 10, 1e-4f) && !strcmp(sst(), "Yes"), "Vector3 Interpolate goes there over time (%g then %g)", half, svv("v").x);
    sem_end();

    /* ---- transform and objects (the fuzz scene: Owner at 0 1 0 facing +Z, Char at 2 1 0 - to its left, two Enemies) ---- */
    sem_begin(ONE("[action]\ntype = Get Angle To Target\np0 = Owner\np1 = name:Char\np2 = 0 0 0\np3 = true\np4 = $f\n"
                  "[action]\ntype = Transform Point\np0 = Owner\np1 = 0 0 -2\np2 = Local Point To World\np3 = $v\n"
                  "[action]\ntype = Get Root\np0 = name:Child\np1 = $o\n"));
    sem_run(1, DT);
    CHECK(NEAR(sv("f"), -90, 0.5f) && VEQ(svv("v"), 0, 1, -2, 1e-3f) && svo("o") == g_so, "Get Angle To Target (- = left), Transform Point, Get Root (%g)", sv("f"));
    sem_end();
    sem_begin("[event]\nname = LOOP\n[event]\nname = END\n[state]\nname = A\ntransition = LOOP > B\ntransition = END > Done\n"
              "[action]\ntype = Get Next With Tag\np0 = Enemy\np1 = $i\np2 = $o\np3 = LOOP\np4 = END\n"
              "[state]\nname = B\ntransition = FINISHED > A\n[action]\ntype = Float Add\np0 = $f\np1 = 1\n[state]\nname = Done\n");
    sem_run(12, DT);
    CHECK(NEAR(sv("f"), 2, 1e-5f) && !strcmp(sst(), "Done") && sv("i") == 0, "Get Next With Tag visits both enemies, then Finished (%g %s)", sv("f"), sst());
    sem_end();

    /* Smooth Look At Direction: turns to face +X over time, then sends its event */
    sem_begin(ONE("[action]\ntype = Smooth Look At Direction\nupdate = every_frame\np0 = Owner\np1 = 3 0 0\np2 = 0.1\np3 = 0 1 0\np4 = true\np5 = World\np6 = 10\np7 = 1\np8 = YES\np9 = $f\n"));
    sem_run(3, DT);
    float part = sv("f");
    sem_run(60, DT);
    LxVec3 fw = lx_quat_rotate(lx_entity_world_rotation(g_ss, g_so), lx_v3(0, 0, 1));
    CHECK(part > 5 && part < 89 && VEQ(fw, 1, 0, 0, 0.03f) && !strcmp(sst(), "Yes"), "Smooth Look At Direction turns gradually, then finishes (%g, %.2f %.2f %.2f, %s)", part, fw.x, fw.y, fw.z, sst());
    sem_end();

    /* ---- arrays ---- */
    sem_begin("[variable]\nname = arr\ntype = array\nvalue = int: 5, 3, 9\n"
              ONE("[action]\ntype = Array Add\np0 = $arr\np1 = 7\n[action]\ntype = Array Length\np0 = $arr\np1 = $i\n"
                  "[action]\ntype = Array Sort\np0 = $arr\n[action]\ntype = Array Get\np0 = $arr\np1 = 0\np2 = $f\n"
                  "[action]\ntype = Array Join\np0 = $arr\np1 = -\np2 = $s\n"
                  "[action]\ntype = Array Contains\np0 = $arr\np1 = 9\np2 = $f2\np4 = YES\np5 = NO\n"));
    sem_run(1, DT);
    CHECK(sv("i") == 4 && sv("f") == 3 && !strcmp(svs("s"), "3-5-7-9") && sv("f2") == 3 && !strcmp(sst(), "Yes"),
          "Array Add, Length, Sort, Get, Join, Contains (%g %g %s %g %s)", sv("i"), sv("f"), svs("s"), sv("f2"), sst());
    sem_end();
    sem_begin("[variable]\nname = words\ntype = array\nvalue = string:\n[variable]\nname = k\ntype = int\nvalue = 0\n"
              "[event]\nname = LOOP\n[event]\nname = END\n[state]\nname = A\ntransition = FINISHED > B\n"
              "[action]\ntype = String Split To Array\np0 = red,green,blue\np1 = ,\np2 = $words\n"
              "[state]\nname = B\ntransition = LOOP > C\ntransition = END > Done\n"
              "[action]\ntype = Array For Each\np0 = $words\np1 = $k\np2 = $s\np4 = LOOP\np5 = END\n"
              "[state]\nname = C\ntransition = FINISHED > B\n[action]\ntype = Float Add\np0 = $f\np1 = 1\n"
              "[action]\ntype = String Join\np0 = $s\np1 = \np2 = $s\n[state]\nname = Done\n");
    sem_run(20, DT);
    CHECK(NEAR(sv("f"), 3, 1e-5f) && !strcmp(sst(), "Done"), "String Split To Array and Array For Each visit every item (%g, %s)", sv("f"), sst());
    sem_end();
    sem_begin("[variable]\nname = foes\ntype = array\nvalue = object:\n"
              ONE("[action]\ntype = Array Fill With Tag\np0 = $foes\np1 = Enemy\np2 = true\np3 = $i\n"
                  "[action]\ntype = Array Get Closest\np0 = $foes\np1 = Owner\np4 = $o\np6 = $f\n"));
    sem_run(1, DT);
    CHECK(sv("i") == 2 && svo("o") == lx_entity_find(g_ss, "Enemy1"), "Array Fill With Tag, Array Get Closest (%g)", sv("i"));
    LxArray foes = lx_fsm_get_var_array(g_ss, g_so, 0, "foes");
    CHECK(lx_array_length(foes) == 2 && lx_array_type(foes) == LX_FSM_OBJECT, "a graph's array from the scripting API");
    sem_end();
    CHECK(!lx_array_valid(foes) && lx_array_length(foes) == 0, "the graph's array goes with its object");
    {   /* the scripting API directly */
        LxArray a = lx_array_create(LX_FSM_FLOAT);
        for (int k = 0; k < 5; k++) lx_array_add_float(a, (float)(5 - k));
        lx_array_sort(a, false);
        lx_array_insert_at(a, 0);
        lx_array_set_float(a, 0, -1);
        lx_array_remove_at(a, 3);
        CHECK(lx_array_length(a) == 5 && lx_array_get_float(a, 0) == -1 && lx_array_get_float(a, 3) == 4 && lx_array_find_float(a, 5) == 4 &&
                  !strcmp(lx_array_get_string(a, 1), "1"),
              "lx_array: add, sort, insert, remove, find");
        lx_array_destroy(a);
        LxArray b = lx_array_create(LX_FSM_STRING);
        CHECK(!lx_array_valid(a) && lx_array_get_float(a, 0) == 0 && b != a, "a destroyed array's handle stays dead");
        lx_array_destroy(b);
    }
    /* ---- quaternions, vector2, rects ---- */
    sem_begin("[variable]\nname = q\ntype = quaternion\nvalue = 0 0 0\n[variable]\nname = v2\ntype = vector2\nvalue = 1 0\n"
              ONE("[action]\ntype = Make Quaternion\np0 = Euler Angles\np1 = 0 90 0\np4 = $q\n"
                  "[action]\ntype = Quaternion Rotate Vector\np0 = $q\np1 = 0 0 -1\np2 = $v\n"
                  "[action]\ntype = Vector2 Operator\np0 = $v2\np2 = Rotate by Float\np3 = 90\np4 = $v2\n"
                  "[action]\ntype = Rect Contains\nupdate = once\np0 = -1 -1 10 10\np1 = $v2\np3 = YES\np4 = NO\n"));
    sem_run(1, DT);
    {
        LxFsmRun *r = lx_fsm_run_of(g_ss, g_so, 0);
        LxVec3 v2 = lx_v3(-9, -9, -9);
        for (int k = 0; r && k < r->var_count; k++) if (!strcmp(r->var_names[k], "v2")) v2 = r->vars[k].v;
        CHECK(VEQ(svv("v"), -1, 0, 0, 1e-4f) && NEAR(v2.x, 0, 1e-4f) && NEAR(v2.y, 1, 1e-4f) && !strcmp(sst(), "Yes"),
              "Make Quaternion, Quaternion Rotate Vector, Vector2 Operator, Rect Contains (%.2f %.2f %.2f; %.2f %.2f; %s)", svv("v").x, svv("v").y, svv("v").z, v2.x, v2.y, sst());
    }
    sem_end();

    /* ---- keys: a press on the entering frame belongs to the state before ---- */
    sem_begin(ONE("[action]\ntype = Get Key Down\np0 = Space\np1 = YES\n[action]\ntype = Get Key\np0 = Space\np1 = $b\n"));
    pm_frame(); g_lx_input.keys[LX_KEY_SPACE] = true; pm_step();
    bool first = strcmp(sst(), "Yes") != 0;
    float held = sv("b");
    pm_frame(); pm_step();
    pm_frame(); g_lx_input.keys[LX_KEY_SPACE] = false; pm_step();
    bool still = strcmp(sst(), "Yes") != 0;
    pm_frame(); g_lx_input.keys[LX_KEY_SPACE] = true; pm_step();
    CHECK(first && still && held == 1 && !strcmp(sst(), "Yes"), "Get Key Down: not on the entering frame, then on a new press (%d %d %s)", first, still, sst());
    pm_frame(); g_lx_input.keys[LX_KEY_SPACE] = false; pm_step();
    sem_end();
    sem_begin(ONE("[action]\ntype = Get Button Up\np0 = Jump\np1 = YES\n"));
    pm_frame(); g_lx_input.keys[LX_KEY_SPACE] = true; pm_step();
    pm_frame(); g_lx_input.keys[LX_KEY_SPACE] = false; pm_step();
    CHECK(!strcmp(sst(), "Yes"), "Get Button Up: letting go of Space (Jump) (%s)", sst());
    pm_frame(); pm_step();
    sem_end();

    /* ---- axis vector relative to the camera: Cam turned to look along -X, W walks along -X ---- */
    sem_begin(ONE("[action]\ntype = Get Axis Vector\nupdate = every_frame\np0 = MoveX\np1 = MoveY\np2 = 2\np3 = Ground (XZ)\np4 = Camera\np6 = $v\np7 = $f\n"));
    lx_entity_set_euler(g_ss, lx_entity_find(g_ss, "Cam"), lx_v3(30, -90, 0));   /* looking along -X, a little down */
    pm_frame(); g_lx_input.keys[LX_KEY_W] = true; pm_step();
    pm_frame(); pm_step();
    CHECK(VEQ(svv("v"), -2, 0, 0, 1e-3f) && NEAR(sv("f"), 2, 1e-3f), "Get Axis Vector: forward is where the camera looks, flat on the ground (%g %g %g)", svv("v").x, svv("v").y, svv("v").z);
    pm_frame(); g_lx_input.keys[LX_KEY_W] = false; pm_step();
    sem_end();

    /* ---- picking with the mouse: put the pointer over the Owner cube ---- */
    sem_begin(ONE("[action]\ntype = Mouse Pick\nupdate = every_frame\np0 = 100\np1 = $b\np2 = $o\np3 = $v\n"));
    lx_game_ui_begin_frame(0, 0, 1280, 720, true);
    lx_scene_update_transforms(g_ss);
    LxVec2 at = lx_v2(0, 0);
    bool vis = lx_ui_world_to_canvas(g_ss, lx_v3(0, 1, 0), &at);
    LxVec2 cs = lx_ui_canvas_size();
    g_lx_input.mouse_pos = lx_v2(at.x / cs.x * 1280, at.y / cs.y * 720);
    pm_frame(); pm_step();
    pm_frame(); pm_step();
    CHECK(vis && sv("b") == 1 && svo("o") == g_so && NEAR(svv("v").z, 0.5f, 0.05f), "Mouse Pick finds the cube under the pointer (hit z %.2f)", svv("v").z);
    sem_end();
    sem_begin(ONE("[action]\ntype = Mouse Pick Event\np0 = Owner\np1 = 100\np3 = NO\np7 = YES\np8 = $b\n"));
    pm_frame(); pm_step();
    pm_frame(); g_lx_input.mouse[LX_MOUSE_LEFT] = true; pm_step();
    bool over = sv("b") == 1;
    pm_frame(); g_lx_input.mouse[LX_MOUSE_LEFT] = false; pm_step();
    CHECK(over && !strcmp(sst(), "Yes"), "Mouse Pick Event: a click on the cube (%s)", sst());
    sem_end();
    g_lx_input.mouse_pos = lx_v2(5, 5);
    sem_begin(ONE("[action]\ntype = Mouse Pick Event\np0 = Owner\np1 = 100\np7 = YES\np8 = $b\n"));
    pm_frame(); pm_step();
    pm_frame(); g_lx_input.mouse[LX_MOUSE_LEFT] = true; pm_step();
    pm_frame(); g_lx_input.mouse[LX_MOUSE_LEFT] = false; pm_step();
    CHECK(sv("b") == 0 && strcmp(sst(), "Yes"), "...and a click in the corner isn't on it");
    sem_end();

    /* ---- touch: a finger lands, then swipes left ---- */
    sem_begin(ONE("[action]\ntype = Touch Event\np0 = -1\np1 = Ended\np2 = YES\np3 = $i\np4 = $v\n[action]\ntype = Get Touch Count\np0 = $f\n"));
    pm_frame(); pm_step();
    pm_frame(); g_lx_input.touch_count = 1; g_lx_input.touches[0] = lx_v2(640, 360); pm_step();
    float fingers = sv("f");
    pm_frame(); g_lx_input.touch_count = 0; pm_step();
    CHECK(fingers == 1 && !strcmp(sst(), "Yes") && NEAR(svv("v").x, cs.x * 0.5f, 1) , "Touch Event: a finger lifting, where it lifted (%g %s %g)", fingers, sst(), svv("v").x);
    sem_end();
    sem_begin(ONE("[action]\ntype = Swipe Gesture Event\np0 = 120\np1 = 0.6\np2 = YES\np3 = NO\np4 = NO\np5 = NO\np6 = $v\n"));
    pm_frame(); pm_step();
    for (int k = 0; k <= 6; k++) { pm_frame(); g_lx_input.touch_count = 1; g_lx_input.touches[0] = lx_v2(900.0f - (float)k * 80, 400); pm_step(); }
    pm_frame(); g_lx_input.touch_count = 0; pm_step();
    CHECK(!strcmp(sst(), "Yes") && NEAR(svv("v").x, -1, 1e-3f), "Swipe Gesture Event: a quick drag to the left (%s)", sst());
    pm_frame(); pm_step();
    sem_end();

    /* ---- graph to graph ---- */
    sem_begin(ONE("[action]\ntype = Set Float\np0 = $f\np1 = 3\n[action]\ntype = Get Graph Variable\np0 = Owner\np1 = 0\np2 = f\np3 = $f2\n"
                  "[action]\ntype = Get Current State\np0 = Owner\np1 = 0\np2 = $s\np3 = A\np4 = YES\np5 = NO\n"));
    sem_run(1, DT);
    CHECK(NEAR(sv("f2"), 3, 1e-5f) && !strcmp(sst(), "Yes"), "Get Graph Variable, Get Current State (%g %s)", sv("f2"), sst());
    sem_end();
    memset(g_lx_input.keys, 0, sizeof g_lx_input.keys);
    memset(g_lx_input.mouse, 0, sizeof g_lx_input.mouse);
    g_lx_input.touch_count = 0;
    lx_input_begin_frame();
    lx_input_begin_frame();
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

/* what a user built: Get Axis ("Move X" with a space) into a vector, into Character Move */
static void test_axis_into_character_move(void) {
    printf("get axis into character move\n");
    const char *g = "[graph]\nname = Mover\nstart = Move\n"
                    "[variable]\nname = PlayerMovement\ntype = vector3\nvalue = 0 0 0\n"
                    "[state]\nname = Move\n"
                    "[action]\ntype = Get Axis\nupdate = every_frame\np0 = Move X\np1 = Move Y\np2 = 5\np3 = $PlayerMovement\n"
                    "[action]\ntype = Character Move\nupdate = every_frame\np0 = Owner\np1 = $PlayerMovement\np2 = World\n";
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
