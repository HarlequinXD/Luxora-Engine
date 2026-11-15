/* Game UI: HUDs, menus, bars, markers and dialogue.
 *
 * Luxora's game UI is immediate mode: nothing is created and kept, so these actions draw for as long as
 * the state runs and stop the moment it ends. That is why most of them default to Every Frame — a Draw
 * Text action in a state is on screen while that state is active, and gone when it is not.
 *
 * Positions are in canvas units: the screen scaled to 1080 units tall, x right, y down, so a layout
 * designed once fits every resolution. Anchor picks which corner or edge the position is measured from,
 * and the element lines up with that point too.
 *
 * There are no UI objects to place in the scene and wire up: a state draws what it needs, directly. */
#include "fsm_actions.h"

#define FSM_ANCHORS "Top Left|Top|Top Right|Left|Center|Right|Bottom Left|Bottom|Bottom Right"
#define FSM_FONTS "Regular|Bold|Mono"

static uint32_t fsm_rgba(LxVec4 c) {
    int r = (int)(lx_saturate(c.x) * 255.0f + 0.5f), g = (int)(lx_saturate(c.y) * 255.0f + 0.5f);
    int b = (int)(lx_saturate(c.z) * 255.0f + 0.5f), a = (int)(lx_saturate(c.w) * 255.0f + 0.5f);
    return LX_RGBA(r, g, b, a);
}

/* ==========================================================================================
 *  drawing
 * ========================================================================================== */
static void ui_text(LxFsmCtx *c) {
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 4));
    lx_ui_font((LxUiFont)lx_fsm_int(c, 5));
    LxVec3 p = lx_fsm_vec3(c, 1);
    lx_ui_text(p.x, p.y, lx_fsm_float(c, 2), fsm_rgba(lx_fsm_color(c, 3)), "%s", lx_fsm_string(c, 0));
}
static const LxFsmActionDesc A_UI_TEXT = {"Draw Text", "Game UI", "Draws a line of text on screen for as long as this state runs: scores, timers, labels.",
    "text \"{0}\"",
    {P("Text", LX_FSM_STRING, "Hello"), PT("Position", LX_FSM_VEC3, "40 40 0", "canvas units from the anchor"),
     P("Size", LX_FSM_FLOAT, "48"), P("Color", LX_FSM_COLOR, "1 1 1 1"),
     PENUMT("Anchor", FSM_ANCHORS, "0", "which corner or edge the position is measured from"),
     PENUM("Font", FSM_FONTS, "0")},
    0, 0, LX_FSM_EVERY_FRAME, ui_text, ui_text, NULL};

static void ui_rect(LxFsmCtx *c) {
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 5));
    LxVec3 p = lx_fsm_vec3(c, 0), s = lx_fsm_vec3(c, 1);
    uint32_t col = fsm_rgba(lx_fsm_color(c, 2));
    float radius = lx_fsm_float(c, 3), thickness = lx_fsm_float(c, 4);
    if (thickness > 0) lx_ui_rect_outline(p.x, p.y, s.x, s.y, col, radius, thickness);
    else lx_ui_rect(p.x, p.y, s.x, s.y, col, radius);
}
static const LxFsmActionDesc A_UI_RECT = {"Draw Rect", "Game UI", "Draws a filled or outlined rectangle: panels, backgrounds, frames.",
    "rect at {0}",
    {P("Position", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "300 120 0"), P("Color", LX_FSM_COLOR, "0 0 0 0.6"),
     PT("Corner Radius", LX_FSM_FLOAT, "8", "0 for sharp corners"),
     PT("Outline Thickness", LX_FSM_FLOAT, "0", "0 draws it filled instead of as an outline"),
     PENUM("Anchor", FSM_ANCHORS, "4")},
    0, 0, LX_FSM_EVERY_FRAME, ui_rect, ui_rect, NULL};

static void ui_image(LxFsmCtx *c) {
    const char *tex = lx_fsm_asset(c, 0);
    if (!tex || !tex[0]) return;
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 4));
    LxVec3 p = lx_fsm_vec3(c, 1), s = lx_fsm_vec3(c, 2);
    lx_ui_image(tex, p.x, p.y, s.x, s.y, fsm_rgba(lx_fsm_color(c, 3)));
}
static const LxFsmActionDesc A_UI_IMAGE = {"Draw Image", "Game UI", "Draws a texture on screen: icons, portraits, crosshairs, logos.",
    "image {0}",
    {PASSET("Texture", "texture"), P("Position", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "128 128 0"),
     PT("Tint", LX_FSM_COLOR, "1 1 1 1", "multiplies the image's colours; drop the alpha to fade it"),
     PENUM("Anchor", FSM_ANCHORS, "4")},
    0, 0, LX_FSM_EVERY_FRAME, ui_image, ui_image, NULL};

static void ui_bar(LxFsmCtx *c) {
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 6));
    LxVec3 p = lx_fsm_vec3(c, 1), s = lx_fsm_vec3(c, 2);
    float v = lx_saturate(lx_fsm_float(c, 0));
    uint32_t fill = fsm_rgba(lx_fsm_color(c, 3));
    if (lx_fsm_bool(c, 5)) {
        /* colour the fill by how full it is: green when healthy, red when nearly empty */
        LxVec4 low = lx_fsm_color(c, 4);
        LxVec4 high = lx_fsm_color(c, 3);
        fill = fsm_rgba(lx_v4(lx_lerpf(low.x, high.x, v), lx_lerpf(low.y, high.y, v),
                              lx_lerpf(low.z, high.z, v), lx_lerpf(low.w, high.w, v)));
    }
    lx_ui_bar(p.x, p.y, s.x, s.y, v, fill, fsm_rgba(lx_fsm_color(c, 4)));
}
static const LxFsmActionDesc A_UI_BAR = {"Draw Bar", "Game UI", "Draws a filled bar from a 0..1 value: health, stamina, loading, reload timers.",
    "bar {0}",
    {PT("Value", LX_FSM_FLOAT, "1", "0 is empty, 1 is full"), P("Position", LX_FSM_VEC3, "40 40 0"),
     P("Size", LX_FSM_VEC3, "400 28 0"), P("Fill Color", LX_FSM_COLOR, "0.27 0.75 0.39 1"),
     P("Background Color", LX_FSM_COLOR, "0.1 0.1 0.1 0.8"),
     PT("Color By Value", LX_FSM_BOOL, "false", "blend the fill towards the background colour as it empties"),
     PENUM("Anchor", FSM_ANCHORS, "0")},
    0, 0, LX_FSM_EVERY_FRAME, ui_bar, ui_bar, NULL};

static void ui_button(LxFsmCtx *c) {
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 4));
    LxVec3 p = lx_fsm_vec3(c, 1), s = lx_fsm_vec3(c, 2);
    char label[LX_REF_MAX];
    lx_strcpy(label, sizeof label, lx_fsm_string(c, 0));
    bool clicked = lx_ui_button(label, p.x, p.y, s.x, s.y);
    lx_fsm_set_bool(c, 5, clicked);
    if (clicked) fsm_send(c, 3);
}
static const LxFsmActionDesc A_UI_BUTTON = {"Draw Button", "Game UI", "Draws a button and sends an event when it is clicked or tapped: menus, dialogue choices, HUD controls.",
    "button \"{0}\" -> {3}",
    {P("Label", LX_FSM_STRING, "Play"), P("Position", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "320 90 0"),
     PEV("Clicked Event"), PENUM("Anchor", FSM_ANCHORS, "4"), POPT("Store Clicked", LX_FSM_BOOL)},
    0, 0, LX_FSM_EVERY_FRAME, ui_button, ui_button, NULL};

static void ui_line(LxFsmCtx *c) {
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 4));
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1);
    lx_ui_line(a.x, a.y, b.x, b.y, lx_maxf(0.5f, lx_fsm_float(c, 2)), fsm_rgba(lx_fsm_color(c, 3)));
}
static const LxFsmActionDesc A_UI_LINE = {"Draw Line", "Game UI", "Draws a straight line on screen: crosshairs, aim lines, simple diagrams.",
    "line {0} to {1}",
    {P("From", LX_FSM_VEC2, "0 0"), P("To", LX_FSM_VEC2, "100 0"), P("Thickness", LX_FSM_FLOAT, "2"),
     P("Color", LX_FSM_COLOR, "1 1 1 1"), PENUM("Anchor", FSM_ANCHORS, "4")},
    0, 0, LX_FSM_EVERY_FRAME, ui_line, ui_line, NULL};

/* ==========================================================================================
 *  world-anchored UI
 * ========================================================================================== */
static void ui_bar_over(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 at = lx_add3(lx_entity_world_position(c->scene, e), lx_fsm_vec3(c, 2));
    lx_ui_bar_over(c->scene, at, lx_saturate(lx_fsm_float(c, 1)), lx_fsm_float(c, 3), lx_fsm_float(c, 4),
                   fsm_rgba(lx_fsm_color(c, 5)), fsm_rgba(lx_fsm_color(c, 6)));
}
static const LxFsmActionDesc A_UI_BAR_OVER = {"Draw Bar Over Object", "Game UI", "Draws a small bar floating above an object in the world: enemy health, capture progress.",
    "bar over {0}",
    {POBJ("Object"), PT("Value", LX_FSM_FLOAT, "1", "0 is empty, 1 is full"),
     PT("Offset", LX_FSM_VEC3, "0 2 0", "where above the object it sits"),
     P("Width", LX_FSM_FLOAT, "80"), P("Height", LX_FSM_FLOAT, "10"),
     P("Fill Color", LX_FSM_COLOR, "0.9 0.28 0.3 1"), P("Background Color", LX_FSM_COLOR, "0 0 0 0.7")},
    0, 0, LX_FSM_LATE, ui_bar_over, ui_bar_over, NULL};

static void ui_marker(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 at = lx_add3(lx_entity_world_position(c->scene, e), lx_fsm_vec3(c, 2));
    char label[LX_REF_MAX];
    lx_strcpy(label, sizeof label, lx_fsm_string(c, 1));
    lx_ui_marker(c->scene, at, label, fsm_rgba(lx_fsm_color(c, 3)), lx_fsm_bool(c, 4), lx_fsm_bool(c, 5));
}
static const LxFsmActionDesc A_UI_MARKER = {"Draw World Marker", "Game UI", "Marks a place in the world on screen: objectives, waypoints, teammates. It can stick to the screen edge when off screen.",
    "marker on {0}",
    {POBJ("Object"), P("Label", LX_FSM_STRING, "Objective"), PT("Offset", LX_FSM_VEC3, "0 1 0", "where above the object it sits"),
     P("Color", LX_FSM_COLOR, "1 0.85 0.2 1"),
     PT("Clamp To Edge", LX_FSM_BOOL, "true", "keep it on screen, pinned to the nearest edge, when the target is behind you"),
     PT("Show Distance", LX_FSM_BOOL, "true", "write how far away it is under the label")},
    0, 0, LX_FSM_LATE, ui_marker, ui_marker, NULL};

static void ui_popup(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxVec3 at = e ? lx_add3(lx_entity_world_position(c->scene, e), lx_fsm_vec3(c, 2)) : lx_fsm_vec3(c, 2);
    char text[LX_REF_MAX];
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 1));
    lx_ui_popup_text(c->scene, at, text, fsm_rgba(lx_fsm_color(c, 3)), lx_fsm_float(c, 4), lx_fsm_float(c, 5));
}
static const LxFsmActionDesc A_UI_POPUP = {"Popup Text", "Game UI", "Floats a short piece of text up from a point in the world and fades it out: damage numbers, \"+50\", pickup names.",
    "popup \"{1}\"",
    {POBJT("At Object", "the text rises from here"), P("Text", LX_FSM_STRING, "+50"),
     PT("Offset", LX_FSM_VEC3, "0 1.5 0", "where it starts, above the object"),
     P("Color", LX_FSM_COLOR, "1 1 1 1"), P("Size", LX_FSM_FLOAT, "36"), P("Seconds", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_ONCE, ui_popup, ui_popup, NULL};

/* ==========================================================================================
 *  overlays — these keep showing without being drawn every frame
 * ========================================================================================== */
static void ui_toast(LxFsmCtx *c) {
    char text[LX_REF_MAX];
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 0));
    lx_ui_toast(text, lx_maxf(0.1f, lx_fsm_float(c, 1)));
}
static const LxFsmActionDesc A_UI_TOAST = {"Show Message", "Game UI", "Shows a message near the bottom of the screen for a few seconds: \"Checkpoint reached\", \"Door locked\". It keeps showing after the state ends.",
    "toast \"{0}\"",
    {P("Text", LX_FSM_STRING, "Checkpoint reached"), P("Seconds", LX_FSM_FLOAT, "2")},
    0, 0, LX_FSM_ONCE, ui_toast, ui_toast, NULL};

static void ui_objective(LxFsmCtx *c) {
    char text[LX_REF_MAX];
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 0));
    lx_ui_set_objective(text);
}
static const LxFsmActionDesc A_UI_OBJECTIVE = {"Set Objective", "Game UI", "Puts a line at the top left and leaves it there until it is changed. Set it to nothing to clear it.",
    "objective \"{0}\"",
    {PT("Text", LX_FSM_STRING, "Find the key", "empty hides the objective")},
    0, 0, LX_FSM_ONCE, ui_objective, ui_objective, NULL};

static void ui_flash(LxFsmCtx *c) {
    lx_ui_flash(lx_fsm_color(c, 0), lx_maxf(0.01f, lx_fsm_float(c, 1)));
}
static const LxFsmActionDesc A_UI_FLASH = {"Flash Screen", "Game UI", "Washes the whole screen in a colour and fades it out: taking a hit, a camera flash, fading between scenes.",
    "flash {0}",
    {P("Color", LX_FSM_COLOR, "1 0 0 0.4"), P("Seconds", LX_FSM_FLOAT, "0.3")},
    0, 0, LX_FSM_ONCE, ui_flash, ui_flash, NULL};

static void ui_prompt(LxFsmCtx *c) {
    char text[LX_REF_MAX];
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 0));
    lx_ui_prompt(text);
}
static const LxFsmActionDesc A_UI_PROMPT = {"Show Prompt", "Game UI", "Shows \"[E]  Open\" near the bottom, with whichever key or button suits the player's current device.",
    "prompt \"{0}\"",
    {P("Text", LX_FSM_STRING, "Open")}, 0, 0, LX_FSM_EVERY_FRAME, ui_prompt, ui_prompt, NULL};

static void ui_clear(LxFsmCtx *c) {
    (void)c;
    lx_ui_overlays_clear();
}
static const LxFsmActionDesc A_UI_CLEAR = {"Clear Overlays", "Game UI", "Removes every message, popup, objective and flash at once: changing scene, opening a menu.",
    "clear overlays",
    {{0}}, 0, 0, LX_FSM_ONCE, ui_clear, NULL, NULL};

/* ==========================================================================================
 *  dialogue and menus
 * ========================================================================================== */
static void ui_dialogue(LxFsmCtx *c) {
    LxDialogue *d = c->state;
    char speaker[LX_REF_MAX], text[LX_REF_MAX];
    lx_strcpy(speaker, sizeof speaker, lx_fsm_string(c, 0));
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 1));
    bool done = lx_ui_dialogue(d, speaker, text, lx_maxf(1.0f, lx_fsm_float(c, 2)), c->dt);
    lx_fsm_set_int(c, 3, d->page);
    if (done) { lx_fsm_finish(c); fsm_send(c, 4); }
}
static const LxFsmActionDesc A_UI_DIALOGUE = {"Show Dialogue", "Game UI", "A dialogue box typed out a character at a time. Split pages with a | . Interact, Jump, Enter or a click skips the typing or turns the page.",
    "dialogue {0}",
    {P("Speaker", LX_FSM_STRING, "Guard"), PT("Text", LX_FSM_STRING, "Halt!|Who goes there?", "separate pages with a | character"),
     PT("Chars Per Second", LX_FSM_FLOAT, "40", "how fast it types"), POPT("Store Page", LX_FSM_INT), PEV("Finish Event")},
    sizeof(LxDialogue), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, ui_dialogue, NULL};

static void ui_choice(LxFsmCtx *c) {
    LxChoice *ch = c->state;
    char title[LX_REF_MAX], opts[4][LX_REF_MAX];
    lx_strcpy(title, sizeof title, lx_fsm_string(c, 0));
    const char *ptr[4];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (lx_fsm_is_none(c, 1 + i)) continue;
        lx_strcpy(opts[n], sizeof opts[n], lx_fsm_string(c, 1 + i));
        if (!opts[n][0]) continue;
        ptr[n] = opts[n];
        n++;
    }
    if (n == 0) { lx_fsm_finish(c); return; }
    int picked = lx_ui_choice(ch, title, ptr, n);
    lx_fsm_set_int(c, 5, ch->selected);
    if (picked >= 0) {
        lx_fsm_set_int(c, 6, picked);
        lx_fsm_finish(c);
        fsm_send(c, 7 + picked);
    }
}
static const LxFsmActionDesc A_UI_CHOICE = {"Show Choice", "Game UI", "A menu of up to four options, driven by the arrow keys, the stick or the mouse. Sends a different event for whichever is picked.",
    "choice \"{0}\"",
    {P("Title", LX_FSM_STRING, "What will you do?"), PIN_OPT("Option 1", LX_FSM_STRING), PIN_OPT("Option 2", LX_FSM_STRING),
     PIN_OPT("Option 3", LX_FSM_STRING), PIN_OPT("Option 4", LX_FSM_STRING),
     POPTT("Store Highlighted", LX_FSM_INT, "which option the player is hovering over"),
     POPT("Store Picked Index", LX_FSM_INT),
     PEV("Option 1 Event"), PEV("Option 2 Event"), PEV("Option 3 Event"), PEV("Option 4 Event")},
    sizeof(LxChoice), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, ui_choice, NULL};

/* ==========================================================================================
 *  helpers
 * ========================================================================================== */
static void ui_pointer(LxFsmCtx *c) {
    LxVec2 p = lx_ui_pointer();
    lx_fsm_set_vec3(c, 0, lx_v3(p.x, p.y, 0));
    bool over = lx_ui_pointer_over();
    lx_fsm_set_bool(c, 1, over);
    LxVec2 canvas = lx_ui_canvas_size();
    lx_fsm_set_vec3(c, 2, lx_v3(canvas.x, canvas.y, 0));
    fsm_send(c, over ? 3 : 4);
}
static const LxFsmActionDesc A_UI_POINTER = {"Get UI Pointer", "Game UI", "Where the pointer is in canvas units, and whether it is over a button — so a click on the UI is not also treated as a click in the world.",
    "pointer -> {0}",
    {POPT("Store Position", LX_FSM_VEC2), POPTT("Store Over UI", LX_FSM_BOOL, "true when it is over a button"),
     POPT("Store Canvas Size", LX_FSM_VEC2), PEV("Over UI Event"), PEV("Not Over UI Event")},
    0, 0, LX_FSM_EVERY_FRAME, ui_pointer, ui_pointer, NULL};

static void ui_format_time(LxFsmCtx *c) {
    char out[64];
    lx_format_time(out, sizeof out, lx_maxf(0, lx_fsm_float(c, 0)), lx_fsm_int(c, 1));
    lx_fsm_set_string(c, 2, out);
}
static const LxFsmActionDesc A_UI_FORMAT_TIME = {"Format Time", "Game UI", "Writes a number of seconds as a clock for a HUD: \"1:05\", \"1:02:03\", \"1:05.42\".",
    "{0}s -> {2}",
    {P("Seconds", LX_FSM_FLOAT, "90"), PENUM("Format", "m:ss|h:mm:ss|m:ss.cc", "0"), POUT("Store Text", LX_FSM_STRING)},
    0, 0, LX_FSM_EVERY_FRAME, ui_format_time, ui_format_time, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_ui(void) {
    static const LxFsmActionDesc *all[] = {
        &A_UI_TEXT, &A_UI_RECT, &A_UI_IMAGE, &A_UI_BAR, &A_UI_BUTTON, &A_UI_LINE,
        &A_UI_BAR_OVER, &A_UI_MARKER, &A_UI_POPUP,
        &A_UI_TOAST, &A_UI_OBJECTIVE, &A_UI_FLASH, &A_UI_PROMPT, &A_UI_CLEAR,
        &A_UI_DIALOGUE, &A_UI_CHOICE, &A_UI_POINTER, &A_UI_FORMAT_TIME,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
