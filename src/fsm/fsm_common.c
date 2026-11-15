/* Helpers shared by the built-in action library, and the one entry point that registers all of it. */
#include "fsm_actions.h"

int fsm_stricmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

void fsm_send(LxFsmCtx *c, int p) {
    const char *e = lx_fsm_event(c, p);
    if (e && e[0]) lx_fsm_send(c, e);
}

LxEntity fsm_obj(LxFsmCtx *c, int p) { return lx_fsm_object(c, p); }

LxEase fsm_ease(int index) {
    switch (index) {
    case 1: return LX_EASE_IN;
    case 2: return LX_EASE_OUT;
    case 3: return LX_EASE_IN_OUT;
    case 4: return LX_EASE_BACK_OUT;
    case 5: return LX_EASE_ELASTIC_OUT;
    case 6: return LX_EASE_BOUNCE_OUT;
    default: return LX_EASE_LINEAR;
    }
}

bool fsm_array_index(LxArray a, int i, int *out) {
    if (!lx_array_valid(a)) return false;
    int n = lx_array_length(a);
    if (n <= 0) return false;
    i = fsm_index(i, n);            /* -1 is the last item, -2 the one before it, and so on */
    if (i < 0 || i >= n) return false;
    *out = i;
    return true;
}

/* ------------------------------------------------------------------------------------------
 *  Registration: every category file exposes one register function.
 * ------------------------------------------------------------------------------------------ */
void lx_fsm_register_builtin_actions(void) {
    extern void fsm_register_math(void);
    extern void fsm_register_vector(void);
    extern void fsm_register_string(void);
    extern void fsm_register_color(void);
    extern void fsm_register_list(void);
    extern void fsm_register_logic(void);
    extern void fsm_register_object(void);
    extern void fsm_register_transform(void);
    extern void fsm_register_physics(void);
    extern void fsm_register_render(void);
    extern void fsm_register_audio(void);
    extern void fsm_register_anim(void);
    extern void fsm_register_input(void);
    extern void fsm_register_scene(void);
    extern void fsm_register_ai(void);
    extern void fsm_register_ui(void);

    fsm_register_math();
    fsm_register_vector();
    fsm_register_string();
    fsm_register_color();
    fsm_register_list();
    fsm_register_logic();
    fsm_register_object();
    fsm_register_transform();
    fsm_register_physics();
    fsm_register_render();
    fsm_register_audio();
    fsm_register_anim();
    fsm_register_input();
    fsm_register_scene();
    fsm_register_ai();
    fsm_register_ui();
}
