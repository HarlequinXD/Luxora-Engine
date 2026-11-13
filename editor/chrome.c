/* Window chrome: Inferno draws its own title bar (Project Manager and editor), with the app icon,
 * a draggable title, minimise / maximise / close, edge resizing and the save-before-closing prompt. */
#include "editor.h"
#include "app_icon.h"

#define TITLEBAR_H 36.0f
#define RESIZE_BORDER 6.0f

static struct {
    uint32_t icon_tex;
    bool icon_tried;
    UiNode *bar, *title, *max_button;
    double last_press;
    int edge_hover;
} C;

static uint8_t *decode_icon(int *w, int *h) { return lx_png_decode(lx_app_icon_64_png, sizeof lx_app_icon_64_png, w, h); }

void ed_chrome_init(void) {
    int w, h;
    uint8_t *px = decode_icon(&w, &h);
    if (px) {
        lx_platform_set_icon(px, w, h);
        lx_free(px);
    }
    lx_platform_set_intercept_close(true);
    ui_set_app_icon(ED.ui, ed_app_icon_texture());
}

uint32_t ed_app_icon_texture(void) {
    if (!C.icon_tried) {
        C.icon_tried = true;
        int w, h;
        uint8_t *px = decode_icon(&w, &h);
        if (px) {
            C.icon_tex = lx_ui_texture_from_rgba(px, w, h);
            lx_free(px);
        }
    }
    return C.icon_tex;
}

UiNode *ed_app_icon(UiNode *parent, float size) {
    UiNode *img = ui_image(ED.ui, parent, ed_app_icon_texture());
    img->w = size;
    img->h = size;
    return img;
}

/* ---------------- title bar ---------------- */
static void wc_min(UiNode *n) { LX_UNUSED(n); lx_platform_window_minimize(); }
static void wc_max(UiNode *n) { LX_UNUSED(n); lx_platform_window_toggle_maximize(); }
static void wc_close(UiNode *n) { LX_UNUSED(n); ed_request_quit(); }

static UiNode *caption_button(UiNode *row, UiIcon icon, const char *tip, void (*cb)(UiNode *), bool danger) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *b = ui_button(ui, row, "", cb);
    b->icon = icon;
    b->icon_size = 16;
    b->icon_color = t->text;
    b->w = 46;
    b->h = TITLEBAR_H;
    b->radius = 0;
    b->bg = 0;
    b->border = 0;
    b->bg_hover = danger ? UI_HEX(0xc42b1c) : t->hover;
    b->bg_active = danger ? UI_HEX(0x9b2216) : t->active;
    lx_strcpy(b->tooltip, sizeof b->tooltip, tip);
    return b;
}

/* a thin ember line along the bottom of the title bar, brightest in the middle */
static void titlebar_glow(UiNode *n, UiCtx *ui) {
    const UiTheme *t = ui_theme(ui);
    float h = lx_maxf(1.0f, ui_scale(ui)), y = n->y + n->ch - h, half = n->cw * 0.5f;
    ui_draw_rect_gradient(ui, n->x, y, half, h, ui_alpha(t->glow, 0), ui_alpha(t->glow, 0.85f), 0, true);
    ui_draw_rect_gradient(ui, n->x + half, y, half, h, ui_alpha(t->glow, 0.85f), ui_alpha(t->glow, 0), 0, true);
}

UiNode *ed_titlebar_begin(UiNode *parent) {
    UiCtx *ui = ED.ui;
    UiNode *bar = ui_box(ui, parent, UI_ROW);
    bar->h = TITLEBAR_H;
    bar->bg = ui_theme(ui)->header;
    bar->on_draw = titlebar_glow;
    bar->cross_align = UI_ALIGN_CENTER;
    ui_pad(bar, 10, 0, 0, 0);
    bar->gap = 2;
    ed_app_icon(bar, 20)->tooltip[0] = 0;
    UiNode *gap = ui_box(ui, bar, UI_ROW);
    gap->w = 8;
    C.bar = bar;
    return bar;
}

void ed_titlebar_end(UiNode *bar, const char *title) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    /* everything that isn't a button is the drag area; the title sits centred in it */
    UiNode *drag = ui_box(ui, bar, UI_ROW);
    drag->grow = 1;
    drag->h = TITLEBAR_H;
    drag->cross_align = UI_ALIGN_CENTER;
    UiNode *l = ui_label(ui, drag, title);
    l->grow = 1;
    l->center_text = true;
    l->fg = t->text_dim;
    C.title = l;
    caption_button(bar, UI_ICON_WIN_MIN, "Minimise", wc_min, false);
    C.max_button = caption_button(bar, lx_platform_window_maximized() ? UI_ICON_WIN_RESTORE : UI_ICON_WIN_MAX, "Maximise", wc_max, false);
    caption_button(bar, UI_ICON_WIN_CLOSE, "Close", wc_close, true);
}

void ed_titlebar_set_title(const char *title) {
    if (C.title) ui_set_text(C.title, "%s", title);
}

void ed_chrome_forget(void) { C.bar = C.title = C.max_button = NULL; }

/* ---------------- per-frame: drag, double-click, edge resize ---------------- */
static int edge_at(LxVec2 m) {
    if (lx_platform_window_maximized()) return 0;
    float k = ui_scale(ED.ui), b = RESIZE_BORDER * k;
    float w = (float)g_lx_input.width, h = (float)g_lx_input.height;
    int e = 0;
    if (m.x < b) e |= LX_EDGE_LEFT;
    if (m.x >= w - b) e |= LX_EDGE_RIGHT;
    if (m.y < b * 0.6f) e |= LX_EDGE_TOP;   /* thinner on top: the title bar is right there */
    if (m.y >= h - b) e |= LX_EDGE_BOTTOM;
    return e;
}

static int edge_cursor(int e) {
    if ((e & LX_EDGE_LEFT && e & LX_EDGE_TOP) || (e & LX_EDGE_RIGHT && e & LX_EDGE_BOTTOM)) return 5;
    if ((e & LX_EDGE_RIGHT && e & LX_EDGE_TOP) || (e & LX_EDGE_LEFT && e & LX_EDGE_BOTTOM)) return 6;
    if (e & (LX_EDGE_LEFT | LX_EDGE_RIGHT)) return 2;
    return 3;
}

/* before the UI handles input: window edges and the title bar take the press first */
void ed_chrome_frame_before_ui(void) {
    LxVec2 m = lx_mouse_pos();
    C.edge_hover = ui_overlay_open(ED.ui) ? 0 : edge_at(m);
    if (!lx_mouse_pressed(LX_MOUSE_LEFT)) return;
    if (C.edge_hover) {
        lx_platform_window_begin_resize(C.edge_hover);
        return;
    }
    if (!C.bar || ui_overlay_open(ED.ui)) return;
    UiNode *b = C.bar;
    bool in_bar = m.x >= b->x && m.y >= b->y && m.x < b->x + b->cw && m.y < b->y + b->ch;
    UiNode *hot = ui_hot_node(ED.ui);
    bool on_control = hot && hot != b && hot->type == UI_BUTTON;
    if (!in_bar || on_control) return;
    double now = lx_time();
    if (now - C.last_press < 0.35) {
        C.last_press = 0;
        lx_platform_window_toggle_maximize();
    } else {
        C.last_press = now;
        lx_platform_window_begin_move();
    }
}

/* after the UI: the resize cursor wins over whatever the UI asked for */
void ed_chrome_frame_after_ui(void) {
    if (C.edge_hover) lx_platform_set_cursor(edge_cursor(C.edge_hover));
    if (C.max_button) {
        UiIcon want = lx_platform_window_maximized() ? UI_ICON_WIN_RESTORE : UI_ICON_WIN_MAX;
        if (C.max_button->icon != want) {
            C.max_button->icon = want;
            lx_strcpy(C.max_button->tooltip, sizeof C.max_button->tooltip, want == UI_ICON_WIN_RESTORE ? "Restore" : "Maximise");
        }
    }
}

/* ---------------- quitting ---------------- */
static void quit_now(void) { ED.quit = true; }

void ed_request_quit(void) {
    if (ui_has_modal(ED.ui) && ED.save_prompt_open) return;   /* already asking */
    if (ED.mode == ED_MODE_EDITOR && ED.play != ED_EDIT) ed_stop();
    if (ED.mode != ED_MODE_EDITOR || !ED.dirty) { quit_now(); return; }
    ed_save_prompt("closing Inferno", quit_now);
}
