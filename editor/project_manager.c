/* Project Manager: the first screen of the editor. Create, open and manage projects. */
#include "editor.h"

static struct {
    char search[64];
    char new_name[64];
    char new_location[LX_PATH_MAX];
    int new_template;
    char new_error[160];
    char browse_dir[LX_PATH_MAX];
    UiNode *list, *browse_list, *template_cards[3], *error_label;
    int nav;
} PM;

static const char *TEMPLATE_NAMES[3] = {"3D Empty", "3D Sample", "Mobile 3D"};
static const char *TEMPLATE_DESC[3] = {"Main camera, light and a cube. A clean start.",
                                       "Lit sample scene with materials, a point light and a 2,000 agent crowd.",
                                       "Tuned for phones: medium quality, touch controls, Android target."};
static const UiIcon TEMPLATE_ICONS[3] = {UI_ICON_CUBE, UI_ICON_SCENE, UI_ICON_PHONE};

static void rebuild_list(void);

static void open_recent(UiNode *n) {
    const char *root = ED.recent[n->tag];
    char proj[LX_PATH_MAX];
    lx_path_join(proj, sizeof proj, root, "project.lxproj");
    if (!lx_file_exists(proj)) {
        ui_notify(ED.ui, "Project not found: %s", root);
        return;
    }
    ed_open_project(root);
}

static void remove_recent_idx(int idx) {
    if (idx < 0 || idx >= ED.recent_count) return;
    memmove(ED.recent[idx], ED.recent[idx + 1], (size_t)(ED.recent_count - idx - 1) * LX_PATH_MAX);
    ED.recent_count--;
    ed_save_prefs();
    rebuild_list();
}
static void remove_recent(UiNode *n) { remove_recent_idx((int)n->tag); }
static void show_recent(UiNode *n) { lx_platform_open_folder(ED.recent[n->tag]); }

static void recent_menu(UiNode *n) {
    LxVec2 m = lx_mouse_pos();
    UiNode *p = ui_popup(ED.ui, m.x, m.y, 200);
    UiNode *it = ui_menu_item(ED.ui, p, "Open", NULL, open_recent);
    it->tag = n->tag;
    it->icon = UI_ICON_OPEN;
    it = ui_menu_item(ED.ui, p, "Show in Folder", NULL, show_recent);
    it->tag = n->tag;
    it->icon = UI_ICON_FOLDER_OPEN;
    ui_menu_separator(ED.ui, p);
    it = ui_menu_item(ED.ui, p, "Remove from List", NULL, remove_recent);
    it->tag = n->tag;
    it->icon = UI_ICON_TRASH;
}

static bool contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

static void rebuild_list(void) {
    if (!PM.list) return;
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    ui_clear(ui, PM.list);
    int shown = 0;
    for (int i = 0; i < ED.recent_count; i++) {
        LxProject p;
        bool ok = lx_project_load(&p, ED.recent[i]);
        const char *name = ok ? p.name : lx_path_filename(ED.recent[i]);
        if (!contains_ci(name, PM.search) && !contains_ci(ED.recent[i], PM.search)) continue;
        UiNode *row = ui_box(ui, PM.list, UI_ROW);
        row->h = 64;
        row->bg = t->panel;
        row->bg_hover = t->hover;
        row->radius = 8;
        row->gap = 14;
        ui_pad(row, 14, 0, 14, 0);
        row->cross_align = UI_ALIGN_CENTER;
        row->tag = i;
        row->on_double_click = open_recent;
        row->on_click = NULL;
        row->on_right_click = recent_menu;
        row->tooltip[0] = 0;
        UiNode *icon = ui_label(ui, row, "");
        icon->icon = UI_ICON_PROJECT;
        icon->icon_size = 30;
        icon->icon_color = ok ? t->accent : t->text_disabled;
        icon->w = 34;
        UiNode *col = ui_box(ui, row, UI_COL);
        col->grow = 1;
        col->align = UI_ALIGN_CENTER;
        UiNode *nl = ui_label(ui, col, name);
        nl->font = UI_FONT_BOLD;
        nl->h = 22;
        UiNode *pl = ui_label(ui, col, ED.recent[i]);
        pl->font = UI_FONT_SMALL;
        pl->fg = t->text_dim;
        pl->h = 18;
        if (ok) {
            UiNode *badge = ui_label(ui, row, lx_platform_name(p.build.target));
            badge->icon = p.build.target == LX_PLATFORM_ANDROID ? UI_ICON_ANDROID : p.build.target == LX_PLATFORM_WINDOWS ? UI_ICON_WINDOWS : UI_ICON_LINUX;
            badge->icon_size = 14;
            badge->font = UI_FONT_SMALL;
            badge->fg = t->text_dim;
            badge->w = 96;
        } else {
            UiNode *missing = ui_label(ui, row, "Missing");
            missing->icon = UI_ICON_WARNING;
            missing->icon_color = t->warning;
            missing->fg = t->warning;
            missing->font = UI_FONT_SMALL;
            missing->w = 96;
        }
        UiNode *open = ui_button(ui, row, "Open", open_recent);
        open->tag = i;
        open->w = 76;
        open->h = 30;
        open->bg = t->panel_alt;
        UiNode *more = ui_icon_button(ui, row, UI_ICON_DOTS, "More", recent_menu);
        more->tag = i;
        shown++;
    }
    if (!shown) {
        UiNode *empty = ui_box(ui, PM.list, UI_COL);
        empty->h = 220;
        empty->align = UI_ALIGN_CENTER;
        empty->gap = 10;
        UiNode *ic = ui_label(ui, empty, "");
        ic->icon = UI_ICON_FOLDER_OPEN;
        ic->icon_size = 48;
        ic->icon_color = t->text_disabled;
        ic->center_text = true;
        ic->h = 56;
        UiNode *l = ui_label(ui, empty, ED.recent_count ? "No projects match your search." : "No projects yet. Create your first one!");
        l->center_text = true;
        l->fg = t->text_dim;
    }
}

static void search_changed(UiNode *n) { LX_UNUSED(n); rebuild_list(); }

/* ---------------- new project dialog ---------------- */
static void select_template(UiNode *n) {
    PM.new_template = (int)n->tag;
    for (int i = 0; i < 3; i++) if (PM.template_cards[i]) PM.template_cards[i]->selected = i == PM.new_template;
}

static void create_project(UiNode *n) {
    LX_UNUSED(n);
    const char *bad = "\\/:*?\"<>|";
    if (!PM.new_name[0]) { lx_strcpy(PM.new_error, sizeof PM.new_error, "Please enter a project name."); }
    else if (strpbrk(PM.new_name, bad)) { lx_strcpy(PM.new_error, sizeof PM.new_error, "The name can't contain \\ / : * ? \" < > |"); }
    else if (!PM.new_location[0]) { lx_strcpy(PM.new_error, sizeof PM.new_error, "Please choose a location."); }
    else {
        char root[LX_PATH_MAX], proj[LX_PATH_MAX];
        lx_path_join(root, sizeof root, PM.new_location, PM.new_name);
        lx_path_join(proj, sizeof proj, root, "project.lxproj");
        if (lx_file_exists(proj)) lx_strcpy(PM.new_error, sizeof PM.new_error, "A project with that name already exists there.");
        else if (!lx_mkdir_p(PM.new_location)) lx_strcpy(PM.new_error, sizeof PM.new_error, "Can't create the location folder.");
        else {
            LxProject p;
            if (lx_project_create(&p, PM.new_location, PM.new_name, (LxProjectTemplate)PM.new_template)) {
                ui_close_modal(ED.ui);
                ed_open_project(p.root);
                return;
            }
            lx_strcpy(PM.new_error, sizeof PM.new_error, "Project creation failed - see the log for details.");
        }
    }
    if (PM.error_label) ui_set_text(PM.error_label, "%s", PM.new_error);
}

static void cancel_modal(UiNode *n) { LX_UNUSED(n); ui_close_modal(ED.ui); }

static void new_project_dialog(UiNode *n) {
    LX_UNUSED(n);
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    lx_strcpy(PM.new_name, sizeof PM.new_name, "My Game");
    if (!PM.new_location[0]) lx_path_join(PM.new_location, sizeof PM.new_location, lx_home_dir(), "LuxoraProjects");
    PM.new_error[0] = 0;
    UiNode *body = ui_modal(ui, "Create New Project", 640, 470);
    UiNode *l = ui_label(ui, body, "Template");
    l->fg = t->text_dim;
    l->font = UI_FONT_SMALL;
    UiNode *cards = ui_box(ui, body, UI_ROW);
    cards->h = 150;
    cards->gap = 10;
    for (int i = 0; i < 3; i++) {
        UiNode *c = ui_box(ui, cards, UI_COL);
        c->grow = 1;
        c->bg = t->panel_alt;
        c->bg_hover = t->hover;
        c->radius = 8;
        c->border = t->border;
        ui_pad(c, 12, 12, 12, 12);
        c->gap = 6;
        c->tag = i;
        c->on_click = select_template;
        c->selected = i == PM.new_template;
        PM.template_cards[i] = c;
        UiNode *ic = ui_label(ui, c, "");
        ic->icon = TEMPLATE_ICONS[i];
        ic->icon_size = 32;
        ic->icon_color = t->accent;
        ic->h = 40;
        UiNode *nm = ui_label(ui, c, TEMPLATE_NAMES[i]);
        nm->font = UI_FONT_BOLD;
        UiNode *d = ui_label(ui, c, TEMPLATE_DESC[i]);
        d->font = UI_FONT_SMALL;
        d->fg = t->text_dim;
        d->h = 40;
    }
    UiNode *row = ed_prop_row(body, "Project name");
    UiNode *tf = ui_textfield(ui, row, PM.new_name, sizeof PM.new_name);
    tf->grow = 1;
    tf->h = 28;
    row->h = 30;
    row = ed_prop_row(body, "Location");
    row->h = 30;
    tf = ui_textfield(ui, row, PM.new_location, sizeof PM.new_location);
    tf->grow = 1;
    tf->h = 28;
    tf->icon = UI_ICON_FOLDER;
    PM.error_label = ui_label(ui, body, "");
    PM.error_label->fg = t->danger;
    ui_spacer(ui, body);
    UiNode *buttons = ui_box(ui, body, UI_ROW);
    buttons->h = 34;
    buttons->gap = 8;
    ui_spacer(ui, buttons);
    UiNode *cancel = ui_button(ui, buttons, "Cancel", cancel_modal);
    cancel->w = 100;
    UiNode *create = ui_button(ui, buttons, "Create Project", create_project);
    create->w = 140;
    create->bg = t->accent;
    create->bg_hover = t->accent_hover;
    create->fg = t->accent_text;
    create->icon = UI_ICON_PLUS;
}

/* ---------------- open project dialog (folder browser) ---------------- */
static void browse_fill(void);
static char g_open_path[LX_PATH_MAX];

static void browse_into(UiNode *n) {
    char next[LX_PATH_MAX];
    lx_path_join(next, sizeof next, PM.browse_dir, n->text);
    lx_strcpy(PM.browse_dir, sizeof PM.browse_dir, next);
    lx_strcpy(g_open_path, sizeof g_open_path, next);
    browse_fill();
}
static void browse_up(UiNode *n) {
    LX_UNUSED(n);
    char up[LX_PATH_MAX];
    lx_path_parent(up, sizeof up, PM.browse_dir);
    if (!up[0]) lx_strcpy(up, sizeof up, "/");
    lx_strcpy(PM.browse_dir, sizeof PM.browse_dir, up);
    lx_strcpy(g_open_path, sizeof g_open_path, up);
    browse_fill();
}
static void browse_pick(UiNode *n) {
    lx_path_join(g_open_path, sizeof g_open_path, PM.browse_dir, n->text);
}
static void do_open(UiNode *n) {
    LX_UNUSED(n);
    char proj[LX_PATH_MAX];
    lx_path_join(proj, sizeof proj, g_open_path, "project.lxproj");
    if (!lx_file_exists(proj)) { ui_notify(ED.ui, "No project.lxproj in that folder"); return; }
    ui_close_modal(ED.ui);
    ed_open_project(g_open_path);
}
static void path_typed(UiNode *n) {
    LX_UNUSED(n);
    if (lx_dir_exists(g_open_path)) { lx_strcpy(PM.browse_dir, sizeof PM.browse_dir, g_open_path); browse_fill(); }
}

static void browse_fill(void) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    if (!PM.browse_list) return;
    ui_clear(ui, PM.browse_list);
    LxDirEntry *e;
    int n = lx_dir_list(PM.browse_dir, &e);
    for (int i = 0; i < n; i++) {
        if (!e[i].is_dir || e[i].name[0] == '.') continue;
        char full[LX_PATH_MAX], proj[LX_PATH_MAX];
        lx_path_join(full, sizeof full, PM.browse_dir, e[i].name);
        lx_path_join(proj, sizeof proj, full, "project.lxproj");
        bool is_proj = lx_file_exists(proj);
        UiNode *b = ui_button(ui, PM.browse_list, e[i].name, browse_pick);
        b->on_double_click = is_proj ? do_open : browse_into;
        b->icon = is_proj ? UI_ICON_PROJECT : UI_ICON_FOLDER;
        b->icon_color = is_proj ? t->accent : t->warning;
        b->center_text = false;
        b->bg = 0;
        b->h = 26;
        if (is_proj) lx_strcpy(b->hint, sizeof b->hint, "Luxora project");
    }
    lx_free(e);
}

static void open_project_dialog(UiNode *n) {
    LX_UNUSED(n);
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    if (!PM.browse_dir[0]) {
        lx_path_join(PM.browse_dir, sizeof PM.browse_dir, lx_home_dir(), "LuxoraProjects");
        if (!lx_dir_exists(PM.browse_dir)) lx_strcpy(PM.browse_dir, sizeof PM.browse_dir, lx_home_dir());
    }
    lx_strcpy(g_open_path, sizeof g_open_path, PM.browse_dir);
    UiNode *body = ui_modal(ui, "Open Project", 620, 480);
    UiNode *row = ui_box(ui, body, UI_ROW);
    row->h = 28;
    row->gap = 6;
    ui_icon_button(ui, row, UI_ICON_UP, "Parent folder", browse_up);
    UiNode *tf = ui_textfield(ui, row, g_open_path, sizeof g_open_path);
    tf->grow = 1;
    tf->on_change = path_typed;
    UiNode *hint = ui_label(ui, body, "Double-click folders to browse. Projects are marked with the Luxora icon.");
    hint->font = UI_FONT_SMALL;
    hint->fg = t->text_dim;
    PM.browse_list = ui_box(ui, body, UI_COL);
    PM.browse_list->grow = 1;
    PM.browse_list->scroll = true;
    PM.browse_list->bg = t->field;
    PM.browse_list->radius = 6;
    ui_pad(PM.browse_list, 4, 4, 4, 4);
    browse_fill();
    UiNode *buttons = ui_box(ui, body, UI_ROW);
    buttons->h = 34;
    buttons->gap = 8;
    ui_spacer(ui, buttons);
    UiNode *c = ui_button(ui, buttons, "Cancel", cancel_modal);
    c->w = 100;
    UiNode *o = ui_button(ui, buttons, "Open", do_open);
    o->w = 120;
    o->bg = t->accent;
    o->bg_hover = t->accent_hover;
    o->fg = t->accent_text;
}

static void nav_click(UiNode *n) {
    PM.nav = (int)n->tag;
    if (PM.nav == 1) new_project_dialog(NULL);
    else if (PM.nav == 2) ed_about();
    PM.nav = 0;
}

void ed_project_manager_build(void) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *root = ui_root(ui);
    root->dir = UI_COL;
    UiNode *bar = ed_titlebar_begin(root);
    ed_titlebar_end(bar, "Luxora Project Manager");
    UiNode *content = ui_box(ui, root, UI_ROW);
    content->grow = 1;
    /* sidebar */
    UiNode *side = ui_box(ui, content, UI_COL);
    side->w = 250;
    side->bg = t->header;
    ui_pad(side, 18, 22, 18, 18);
    side->gap = 4;
    UiNode *brand = ui_box(ui, side, UI_ROW);
    brand->h = 44;
    brand->gap = 10;
    brand->cross_align = UI_ALIGN_CENTER;
    ed_app_icon(brand, 36);
    UiNode *name = ui_label(ui, brand, "LUXORA");
    name->font = UI_FONT_TITLE;
    UiNode *ver = ui_label(ui, side, "Engine " LX_VERSION_STRING " · Vulkan");
    ver->font = UI_FONT_SMALL;
    ver->fg = t->text_dim;
    ver->h = 26;
    UiNode *sp = ui_box(ui, side, UI_COL);
    sp->h = 16;
    static const char *nav[] = {"Projects", "New Project", "About"};
    static const UiIcon nav_icons[] = {UI_ICON_PROJECT, UI_ICON_PLUS, UI_ICON_INFO};
    for (int i = 0; i < 3; i++) {
        UiNode *b = ui_button(ui, side, nav[i], nav_click);
        b->icon = nav_icons[i];
        b->center_text = false;
        b->bg = 0;
        b->h = 34;
        b->tag = i;
        b->selected = i == 0;
        b->bg_active = t->selection;
    }
    ui_spacer(ui, side);
    UiNode *tip = ui_label(ui, side, "Tip: right-click a project for more options.");
    tip->font = UI_FONT_SMALL;
    tip->fg = t->text_disabled;

    /* main */
    UiNode *main = ui_box(ui, content, UI_COL);
    main->grow = 1;
    ui_pad(main, 36, 30, 36, 24);
    main->gap = 16;
    UiNode *header = ui_box(ui, main, UI_ROW);
    header->h = 40;
    header->gap = 10;
    header->cross_align = UI_ALIGN_CENTER;
    UiNode *title = ui_label(ui, header, "Projects");
    title->font = UI_FONT_TITLE;
    title->grow = 1;
    UiNode *open = ui_button(ui, header, "Open", open_project_dialog);
    open->icon = UI_ICON_FOLDER_OPEN;
    open->h = 34;
    open->w = 100;
    UiNode *create = ui_button(ui, header, "New Project", new_project_dialog);
    create->icon = UI_ICON_PLUS;
    create->h = 34;
    create->w = 140;
    create->bg = t->accent;
    create->bg_hover = t->accent_hover;
    create->fg = t->accent_text;
    UiNode *search = ui_textfield(ui, main, PM.search, sizeof PM.search);
    search->h = 32;
    search->icon = UI_ICON_SEARCH;
    lx_strcpy(search->hint, sizeof search->hint, "Search projects");
    search->on_change = search_changed;
    search->tag = 0x6c697665;
    PM.list = ui_box(ui, main, UI_COL);
    PM.list->grow = 1;
    PM.list->scroll = true;
    PM.list->gap = 8;
    rebuild_list();
}
