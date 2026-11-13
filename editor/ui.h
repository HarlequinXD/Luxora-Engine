/* Luxora UI: a retained-mode widget toolkit written for the editor.
 *
 * Widgets are persistent nodes in a tree. Each frame the toolkit runs per-node update hooks,
 * lays the tree out (flex-style rows/columns with fixed, auto and grow sizing, scroll areas,
 * splitters and tab groups), routes input (hover, press, click, double-click, context click,
 * drag-and-drop, keyboard focus, text editing, wheel scrolling) and renders the result through
 * the Vulkan renderer's UI pass. Values are bound by pointer, so widgets always show live data. */
#ifndef LX_UI_H
#define LX_UI_H
#include "lx_internal.h"

typedef struct UiNode UiNode;
typedef struct UiCtx UiCtx;

typedef enum UiType {
    UI_BOX, UI_LABEL, UI_BUTTON, UI_CHECKBOX, UI_TEXTFIELD, UI_NUMBER, UI_SLIDER, UI_DROPDOWN,
    UI_IMAGE, UI_SPLIT, UI_TABS, UI_SEPARATOR, UI_COLOR, UI_CUSTOM
} UiType;

enum { UI_ROW = 0, UI_COL = 1 };
enum { UI_ALIGN_START = 0, UI_ALIGN_CENTER = 1, UI_ALIGN_END = 2 };

typedef enum UiFont { UI_FONT_BODY, UI_FONT_BOLD, UI_FONT_SMALL, UI_FONT_TITLE, UI_FONT_MONO, UI_FONT_COUNT } UiFont;

typedef enum UiIcon {
    UI_ICON_NONE,
    /* files & assets */
    UI_ICON_FOLDER, UI_ICON_FOLDER_OPEN, UI_ICON_FILE, UI_ICON_SCENE, UI_ICON_MESH, UI_ICON_MATERIAL, UI_ICON_TEXTURE,
    UI_ICON_SOUND, UI_ICON_SCRIPT, UI_ICON_PROJECT, UI_ICON_PREFAB,
    /* transport */
    UI_ICON_PLAY, UI_ICON_PAUSE, UI_ICON_STOP, UI_ICON_STEP,
    /* scene objects */
    UI_ICON_CAMERA, UI_ICON_LIGHT, UI_ICON_SUN, UI_ICON_BULB, UI_ICON_SPOT, UI_ICON_CUBE, UI_ICON_SPHERE, UI_ICON_PLANE,
    UI_ICON_CYLINDER, UI_ICON_CAPSULE, UI_ICON_ENTITY, UI_ICON_SPEAKER, UI_ICON_LISTENER, UI_ICON_CROWD, UI_ICON_BEHAVIOUR,
    UI_ICON_PROBE, UI_ICON_COLLIDER, UI_ICON_RIGIDBODY, UI_ICON_CHARACTER,
    UI_ICON_APP,   /* the Luxora app icon (a texture, see ui_set_app_icon); marks Luxora projects */
    /* window controls */
    UI_ICON_WIN_MIN, UI_ICON_WIN_MAX, UI_ICON_WIN_RESTORE, UI_ICON_WIN_CLOSE,
    /* tools */
    UI_ICON_SELECT, UI_ICON_MOVE, UI_ICON_ROTATE, UI_ICON_SCALE, UI_ICON_FOCUS, UI_ICON_GRID, UI_ICON_GLOBE, UI_ICON_LOCAL,
    /* general */
    UI_ICON_CHEVRON_RIGHT, UI_ICON_CHEVRON_DOWN, UI_ICON_CHEVRON_LEFT, UI_ICON_CHEVRON_UP, UI_ICON_PLUS, UI_ICON_MINUS,
    UI_ICON_CLOSE, UI_ICON_CHECK, UI_ICON_SEARCH, UI_ICON_EYE, UI_ICON_EYE_OFF, UI_ICON_LOCK, UI_ICON_GEAR, UI_ICON_BUILD,
    UI_ICON_BACK, UI_ICON_UP, UI_ICON_TRASH, UI_ICON_DOTS, UI_ICON_SAVE, UI_ICON_OPEN, UI_ICON_UNDO, UI_ICON_REDO,
    UI_ICON_COPY, UI_ICON_REFRESH, UI_ICON_FILTER, UI_ICON_LIST, UI_ICON_TILES, UI_ICON_LAYERS, UI_ICON_HOME,
    UI_ICON_EXTERNAL, UI_ICON_DRAG,
    /* panels */
    UI_ICON_HIERARCHY, UI_ICON_INSPECTOR, UI_ICON_CONSOLE, UI_ICON_VIEWPORT, UI_ICON_STATS,
    /* status */
    UI_ICON_INFO, UI_ICON_WARNING, UI_ICON_ERROR, UI_ICON_SUCCESS,
    /* input devices */
    UI_ICON_KEYBOARD, UI_ICON_MOUSE, UI_ICON_GAMEPAD, UI_ICON_TOUCH, UI_ICON_JOYSTICK,
    /* platforms */
    UI_ICON_WINDOWS, UI_ICON_LINUX, UI_ICON_ANDROID, UI_ICON_MONITOR, UI_ICON_PHONE,
    UI_ICON_COUNT
} UiIcon;

typedef struct UiTheme {
    uint32_t window, panel, panel_alt, header, field, field_hover, border, border_light;
    uint32_t text, text_dim, text_disabled, accent, accent_hover, accent_text, selection;
    uint32_t hover, active, danger, success, warning;
    uint32_t axis_x, axis_y, axis_z;
} UiTheme;

struct UiNode {
    UiType type;
    UiNode *parent, *first, *last, *next, *prev;
    /* layout (logical pixels; multiplied by the UI scale) */
    int dir;
    float w, h;            /* > 0 fixed, 0 = auto along main axis / stretch along cross axis */
    float grow;            /* share of leftover space along the parent's main axis */
    float pad[4];          /* left, top, right, bottom */
    float gap;
    int align;             /* main-axis alignment of children when they don't fill */
    int cross_align;       /* cross-axis alignment for children with explicit cross size */
    bool scroll, clip, hidden;
    bool wrap;             /* row that wraps children onto new lines (grids) */
    float min_w;
    /* computed (physical pixels) */
    float x, y, cw, ch;
    float content;         /* content size along dir */
    float scroll_pos, scroll_max;
    /* style */
    uint32_t bg, bg_hover, bg_active, fg, border;
    float radius;
    UiFont font;
    UiIcon icon;
    uint32_t icon_color;
    float icon_size;
    bool center_text;
    char text[192];
    char hint[64];
    char tooltip[192];
    char path[LX_PATH_MAX]; /* free storage for a file path / identifier (not drawn) */
    /* bindings */
    float *f;
    int *i;
    bool *b;
    char *s;
    int s_cap;
    float minv, maxv, step;
    int precision;
    const char *const *options;
    int option_count;
    uint32_t texture;
    float tex_aspect;      /* 0 = stretch */
    /* state */
    bool hover, active, focus, disabled, selected, expanded;
    float split;           /* UI_SPLIT ratio of the first child */
    float split_min;       /* minimum px per side (logical) */
    int tab_index;
    int depth;             /* tree indentation level */
    /* behaviour */
    void (*on_click)(UiNode *n);
    void (*on_double_click)(UiNode *n);
    void (*on_right_click)(UiNode *n);
    void (*on_change)(UiNode *n);
    void (*on_frame)(UiNode *n);
    void (*on_draw)(UiNode *n, UiCtx *ui);
    void (*on_drop)(UiNode *n, const char *type, const char *payload);
    void (*on_key)(UiNode *n);           /* custom nodes with focus */
    void *user;
    intptr_t tag;
    const char *drag_type;
    char drag_payload[LX_REF_MAX];
    const char *accept;                   /* comma separated drag types accepted */
    /* text editing */
    int caret, sel_anchor;
    char edit_backup[256];
    bool editing;
    UiCtx *ctx;
};

/* lifetime */
UiCtx *ui_create(void);
void ui_destroy(UiCtx *ui);
UiNode *ui_root(UiCtx *ui);
const UiTheme *ui_theme(UiCtx *ui);
float ui_scale(UiCtx *ui);
void ui_frame(UiCtx *ui, float dt, LxVec4 clear);

/* building */
UiNode *ui_new(UiCtx *ui, UiNode *parent, UiType type);
void ui_remove(UiCtx *ui, UiNode *n);            /* destroys node + subtree */
void ui_clear(UiCtx *ui, UiNode *n);             /* destroys children */
UiNode *ui_box(UiCtx *ui, UiNode *parent, int dir);
UiNode *ui_label(UiCtx *ui, UiNode *parent, const char *text);
UiNode *ui_button(UiCtx *ui, UiNode *parent, const char *text, void (*on_click)(UiNode *));
UiNode *ui_icon_button(UiCtx *ui, UiNode *parent, UiIcon icon, const char *tooltip, void (*on_click)(UiNode *));
UiNode *ui_checkbox(UiCtx *ui, UiNode *parent, const char *label, bool *value);
UiNode *ui_textfield(UiCtx *ui, UiNode *parent, char *buf, int cap);
UiNode *ui_number(UiCtx *ui, UiNode *parent, float *value, float step);
UiNode *ui_int(UiCtx *ui, UiNode *parent, int *value, int minv, int maxv);
UiNode *ui_slider(UiCtx *ui, UiNode *parent, float *value, float minv, float maxv);
UiNode *ui_dropdown(UiCtx *ui, UiNode *parent, int *value, const char *const *options, int count);
UiNode *ui_image(UiCtx *ui, UiNode *parent, uint32_t texture);
UiNode *ui_separator(UiCtx *ui, UiNode *parent);
UiNode *ui_color(UiCtx *ui, UiNode *parent, float *rgb, bool alpha);
UiNode *ui_split(UiCtx *ui, UiNode *parent, int dir, float ratio);
UiNode *ui_tabs(UiCtx *ui, UiNode *parent);
UiNode *ui_spacer(UiCtx *ui, UiNode *parent);
void ui_set_text(UiNode *n, const char *fmt, ...);
void ui_pad(UiNode *n, float l, float t, float r, float b);

/* popups, menus, modals */
UiNode *ui_popup(UiCtx *ui, float x, float y, float min_w);       /* positioned overlay, closes on outside click */
UiNode *ui_menu_item(UiCtx *ui, UiNode *popup, const char *text, const char *shortcut, void (*on_click)(UiNode *));
UiNode *ui_menu_separator(UiCtx *ui, UiNode *popup);
UiNode *ui_modal(UiCtx *ui, const char *title, float w, float h); /* centred dialog with dimmed backdrop */
void ui_close_popups(UiCtx *ui);
void ui_close_modal(UiCtx *ui);
bool ui_has_modal(UiCtx *ui);
bool ui_popup_open(UiCtx *ui);

/* queries */
bool ui_wants_keyboard(UiCtx *ui);
bool ui_wants_mouse(UiCtx *ui);
bool ui_is_hot(UiCtx *ui, UiNode *n);       /* mouse is over n (topmost) */
UiNode *ui_hot_node(UiCtx *ui);
void ui_set_app_icon(UiCtx *ui, uint32_t texture);             /* interactive node under the mouse, if any */
bool ui_overlay_open(UiCtx *ui);            /* a popup menu or modal dialog is showing */
bool ui_is_inside(UiCtx *ui, UiNode *n);    /* mouse is inside n's rect, regardless of children */
UiNode *ui_active(UiCtx *ui);
void ui_focus(UiCtx *ui, UiNode *n);
const char *ui_dragging(UiCtx *ui, const char **payload); /* active drag type or NULL */
void ui_notify(UiCtx *ui, const char *fmt, ...);          /* toast notification */

/* drawing helpers for custom nodes (physical pixels) */
float ui_text_width(UiCtx *ui, UiFont font, const char *text);
float ui_line_height(UiCtx *ui, UiFont font);
void ui_draw_rect(UiCtx *ui, float x, float y, float w, float h, uint32_t color, float radius);
void ui_draw_border(UiCtx *ui, float x, float y, float w, float h, uint32_t color, float radius, float thickness);
void ui_draw_text(UiCtx *ui, UiFont font, float x, float y, const char *text, uint32_t color);
void ui_draw_line(UiCtx *ui, float x0, float y0, float x1, float y1, float thickness, uint32_t color);
void ui_draw_triangle(UiCtx *ui, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color);
void ui_draw_circle(UiCtx *ui, float cx, float cy, float r, uint32_t color, int segments);
void ui_draw_ring(UiCtx *ui, float cx, float cy, float r, float thickness, uint32_t color, int segments);
void ui_draw_image(UiCtx *ui, uint32_t texture, float x, float y, float w, float h);
void ui_draw_icon(UiCtx *ui, UiIcon icon, float x, float y, float size, uint32_t color);
void ui_push_clip(UiCtx *ui, float x, float y, float w, float h);
void ui_pop_clip(UiCtx *ui);

#define UI_RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define UI_HEX(h) UI_RGBA(((h) >> 16) & 255, ((h) >> 8) & 255, (h)&255, 255)
uint32_t ui_alpha(uint32_t c, float a);
uint32_t ui_lerp_color(uint32_t a, uint32_t b, float t);

#endif
