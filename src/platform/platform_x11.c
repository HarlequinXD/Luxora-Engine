/* Linux platform layer: X11 window + Vulkan surface.
 * libvulkan is loaded at runtime with dlopen, so no Vulkan SDK is needed to build. */
#include "../lx_internal.h"
#if LX_OS_LINUX
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/wait.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>

static struct {
    Display *dpy;
    Window win;
    Atom wm_delete, net_wm_state, net_wm_max_h, net_wm_max_v, net_wm_fullscreen, net_wm_moveresize, motif_hints;
    XIM im;
    XIC ic;
    void *libvk;
    PFN_vkGetInstanceProcAddr gipa;
    Cursor cursors[7];
    bool intercept_close, close_requested;
    int current_cursor;
    char clipboard[8192];
} X;

static LxKey map_keysym(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) return (LxKey)(LX_KEY_A + (ks - XK_a));
    if (ks >= XK_A && ks <= XK_Z) return (LxKey)(LX_KEY_A + (ks - XK_A));
    if (ks >= XK_0 && ks <= XK_9) return (LxKey)(LX_KEY_0 + (ks - XK_0));
    if (ks >= XK_F1 && ks <= XK_F12) return (LxKey)(LX_KEY_F1 + (ks - XK_F1));
    switch (ks) {
    case XK_space: return LX_KEY_SPACE;
    case XK_Escape: return LX_KEY_ESCAPE;
    case XK_Return: case XK_KP_Enter: return LX_KEY_ENTER;
    case XK_Tab: case XK_ISO_Left_Tab: return LX_KEY_TAB;
    case XK_BackSpace: return LX_KEY_BACKSPACE;
    case XK_Delete: return LX_KEY_DELETE;
    case XK_Insert: return LX_KEY_INSERT;
    case XK_Left: return LX_KEY_LEFT;
    case XK_Right: return LX_KEY_RIGHT;
    case XK_Up: return LX_KEY_UP;
    case XK_Down: return LX_KEY_DOWN;
    case XK_Home: return LX_KEY_HOME;
    case XK_End: return LX_KEY_END;
    case XK_Page_Up: return LX_KEY_PAGE_UP;
    case XK_Page_Down: return LX_KEY_PAGE_DOWN;
    case XK_Shift_L: case XK_Shift_R: return LX_KEY_SHIFT;
    case XK_Control_L: case XK_Control_R: return LX_KEY_CTRL;
    case XK_Alt_L: case XK_Alt_R: return LX_KEY_ALT;
    default: return LX_KEY_UNKNOWN;
    }
}

uint32_t lx_platform_surface_generation(void) { return 0; }
void *lx_platform_vk_loader(void) { return (void *)X.gipa; }

int lx_platform_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
    return 2;
}

bool lx_platform_vk_create_surface(void *instance, void *out_surface) {
    PFN_vkCreateXlibSurfaceKHR create = (PFN_vkCreateXlibSurfaceKHR)X.gipa((VkInstance)instance, "vkCreateXlibSurfaceKHR");
    if (!create) return false;
    VkXlibSurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    ci.dpy = X.dpy;
    ci.window = X.win;
    return create((VkInstance)instance, &ci, NULL, (VkSurfaceKHR *)out_surface) == VK_SUCCESS;
}

bool lx_platform_init(const LxWindowDesc *d) {
    XInitThreads();
    X.dpy = XOpenDisplay(NULL);
    if (!X.dpy) { LX_ERROR("Cannot open X display"); return false; }
    X.libvk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!X.libvk) X.libvk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!X.libvk) { LX_ERROR("Vulkan loader (libvulkan.so.1) not found - install your GPU's Vulkan driver"); return false; }
    X.gipa = (PFN_vkGetInstanceProcAddr)dlsym(X.libvk, "vkGetInstanceProcAddr");
    int scr = DefaultScreen(X.dpy);
    Visual *visual = DefaultVisual(X.dpy, scr);
    int depth = DefaultDepth(X.dpy, scr);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(X.dpy, RootWindow(X.dpy, scr), visual, AllocNone);
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask;
    int w = d->width > 0 ? d->width : 1600, h = d->height > 0 ? d->height : 900;
    X.win = XCreateWindow(X.dpy, RootWindow(X.dpy, scr), 0, 0, (unsigned)w, (unsigned)h, 0, depth,
                          InputOutput, visual, CWColormap | CWEventMask, &swa);
    X.wm_delete = XInternAtom(X.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(X.dpy, X.win, &X.wm_delete, 1);
    XStoreName(X.dpy, X.win, d->title ? d->title : "Luxora");
    X.net_wm_state = XInternAtom(X.dpy, "_NET_WM_STATE", False);
    X.net_wm_max_h = XInternAtom(X.dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    X.net_wm_max_v = XInternAtom(X.dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    X.net_wm_fullscreen = XInternAtom(X.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    X.net_wm_moveresize = XInternAtom(X.dpy, "_NET_WM_MOVERESIZE", False);
    X.motif_hints = XInternAtom(X.dpy, "_MOTIF_WM_HINTS", False);
    if (d->borderless && !d->fullscreen) {
        /* ask the window manager for no decorations: flags = decorations, decorations = none */
        long hints[5] = {2, 0, 0, 0, 0};
        XChangeProperty(X.dpy, X.win, X.motif_hints, X.motif_hints, 32, PropModeReplace, (unsigned char *)hints, 5);
    }
    if (d->maximized || d->fullscreen) {
        Atom atoms[2] = {X.net_wm_max_h, X.net_wm_max_v};
        if (d->fullscreen) atoms[0] = X.net_wm_fullscreen;
        XChangeProperty(X.dpy, X.win, X.net_wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)atoms,
                        d->fullscreen ? 1 : 2);
    }
    XMapWindow(X.dpy, X.win);

    X.im = XOpenIM(X.dpy, NULL, NULL, NULL);
    if (X.im) X.ic = XCreateIC(X.im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, X.win, NULL);

    X.cursors[0] = XCreateFontCursor(X.dpy, XC_left_ptr);
    X.cursors[1] = XCreateFontCursor(X.dpy, XC_xterm);
    X.cursors[2] = XCreateFontCursor(X.dpy, XC_sb_h_double_arrow);
    X.cursors[3] = XCreateFontCursor(X.dpy, XC_sb_v_double_arrow);
    X.cursors[4] = XCreateFontCursor(X.dpy, XC_hand2);
    X.cursors[5] = XCreateFontCursor(X.dpy, XC_bottom_right_corner);
    X.cursors[6] = XCreateFontCursor(X.dpy, XC_bottom_left_corner);

    g_lx_input.width = w;
    g_lx_input.height = h;
    const char *scale = getenv("LUXORA_UI_SCALE");
    g_lx_input.dpi_scale = scale ? (float)atof(scale) : 1.0f;
    if (g_lx_input.dpi_scale <= 0.25f) g_lx_input.dpi_scale = 1.0f;
    return true;
}

void lx_platform_shutdown(void) {
    if (!X.dpy) return;
    if (X.ic) XDestroyIC(X.ic);
    if (X.im) XCloseIM(X.im);
    XDestroyWindow(X.dpy, X.win);
    XCloseDisplay(X.dpy);
    X.dpy = NULL;
}

void lx_platform_set_title(const char *title) { XStoreName(X.dpy, X.win, title); }

void lx_platform_set_intercept_close(bool on) { X.intercept_close = on; }
bool lx_platform_take_close_request(void) {
    bool r = X.close_requested;
    X.close_requested = false;
    return r;
}

static void send_wm_state(long action, Atom a, Atom b) {
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = X.win;
    e.xclient.message_type = X.net_wm_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = action;   /* 0 remove, 1 add, 2 toggle */
    e.xclient.data.l[1] = (long)a;
    e.xclient.data.l[2] = (long)b;
    e.xclient.data.l[3] = 1;
    XSendEvent(X.dpy, DefaultRootWindow(X.dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(X.dpy);
}

bool lx_platform_window_maximized(void) {
    Atom type;
    int format;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    bool h = false, v = false;
    if (XGetWindowProperty(X.dpy, X.win, X.net_wm_state, 0, 64, False, XA_ATOM, &type, &format, &n, &after, &data) == Success && data) {
        Atom *a = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) {
            if (a[i] == X.net_wm_max_h) h = true;
            if (a[i] == X.net_wm_max_v) v = true;
        }
        XFree(data);
    }
    return h && v;
}
void lx_platform_window_toggle_maximize(void) { send_wm_state(lx_platform_window_maximized() ? 0 : 1, X.net_wm_max_h, X.net_wm_max_v); }
void lx_platform_window_minimize(void) {
    XIconifyWindow(X.dpy, X.win, DefaultScreen(X.dpy));
    XFlush(X.dpy);
}

/* hands the pointer to the window manager (_NET_WM_MOVERESIZE): it moves/resizes, snaps and tiles */
static void wm_move_resize(long direction) {
    Window root, child;
    int rx, ry, wx, wy;
    unsigned int mask;
    XQueryPointer(X.dpy, X.win, &root, &child, &rx, &ry, &wx, &wy, &mask);
    XUngrabPointer(X.dpy, CurrentTime);
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = X.win;
    e.xclient.message_type = X.net_wm_moveresize;
    e.xclient.format = 32;
    e.xclient.data.l[0] = rx;
    e.xclient.data.l[1] = ry;
    e.xclient.data.l[2] = direction;
    e.xclient.data.l[3] = 1;   /* left button */
    e.xclient.data.l[4] = 1;   /* normal application */
    XSendEvent(X.dpy, DefaultRootWindow(X.dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(X.dpy);
    /* the window manager now owns the drag and we will not see the button release */
    g_lx_input.mouse[LX_MOUSE_LEFT] = false;
}
void lx_platform_window_begin_move(void) { wm_move_resize(8); }
void lx_platform_window_begin_resize(int edges) {
    static const long dir[16] = {-1, 7, 3, -1, 1, 0, 2, -1, 5, 6, 4, -1, -1, -1, -1, -1};
    long d = dir[edges & 15];
    if (d >= 0) wm_move_resize(d);
}

void lx_platform_set_cursor(int c) {
    if (c < 0 || c > 6 || c == X.current_cursor) return;
    X.current_cursor = c;
    XDefineCursor(X.dpy, X.win, X.cursors[c]);
}

const char *lx_platform_clipboard_get(void) { return X.clipboard; }
void lx_platform_clipboard_set(const char *t) { lx_strcpy(X.clipboard, sizeof X.clipboard, t); }

bool lx_platform_pump(void) {
    lx_input_begin_frame();
    bool running = true;
    while (XPending(X.dpy)) {
        XEvent ev;
        XNextEvent(X.dpy, &ev);
        if (XFilterEvent(&ev, None)) continue;
        switch (ev.type) {
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == X.wm_delete) {
                if (X.intercept_close) X.close_requested = true;
                else running = false;
            }
            break;
        case ConfigureNotify:
            g_lx_input.width = ev.xconfigure.width;
            g_lx_input.height = ev.xconfigure.height;
            break;
        case KeyPress: case KeyRelease: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            LxKey k = map_keysym(ks);
            bool down = ev.type == KeyPress;
            if (!down && XEventsQueued(X.dpy, QueuedAfterReading)) { /* swallow auto-repeat release */
                XEvent nx;
                XPeekEvent(X.dpy, &nx);
                if (nx.type == KeyPress && nx.xkey.time == ev.xkey.time && nx.xkey.keycode == ev.xkey.keycode) break;
            }
            if (k) g_lx_input.keys[k] = down;
            if (down) {
                char buf[32];
                KeySym sym;
                Status st;
                int n = X.ic ? Xutf8LookupString(X.ic, &ev.xkey, buf, sizeof buf - 1, &sym, &st)
                             : XLookupString(&ev.xkey, buf, sizeof buf - 1, &sym, NULL);
                buf[n > 0 ? n : 0] = 0;
                /* decode utf-8 */
                for (int i = 0; i < n;) {
                    unsigned char c = (unsigned char)buf[i];
                    uint32_t cp; int len;
                    if (c < 0x80) { cp = c; len = 1; }
                    else if ((c >> 5) == 6) { cp = c & 0x1f; len = 2; }
                    else if ((c >> 4) == 14) { cp = c & 0x0f; len = 3; }
                    else { cp = c & 0x07; len = 4; }
                    for (int j = 1; j < len && i + j < n; j++) cp = (cp << 6) | ((unsigned char)buf[i + j] & 0x3f);
                    if (cp >= 32 && cp != 127) lx_input_push_char(cp);
                    i += len;
                }
            }
            break;
        }
        case ButtonPress: case ButtonRelease: {
            bool down = ev.type == ButtonPress;
            unsigned b = ev.xbutton.button;
            if (b == 1) g_lx_input.mouse[LX_MOUSE_LEFT] = down;
            else if (b == 2) g_lx_input.mouse[LX_MOUSE_MIDDLE] = down;
            else if (b == 3) g_lx_input.mouse[LX_MOUSE_RIGHT] = down;
            else if (down && b == 4) g_lx_input.wheel += 1.0f;
            else if (down && b == 5) g_lx_input.wheel -= 1.0f;
            break;
        }
        case MotionNotify:
            g_lx_input.mouse_pos = lx_v2((float)ev.xmotion.x, (float)ev.xmotion.y);
            break;
        case FocusOut:
            memset(g_lx_input.keys, 0, sizeof g_lx_input.keys);
            memset(g_lx_input.mouse, 0, sizeof g_lx_input.mouse);
            break;
        default: break;
        }
    }
    return running;
}


int lx_platform_run_process(const char *cmd, const char *workdir, void (*cb)(const char *, void *), void *u) {
    char full[4096];
    if (workdir && workdir[0]) snprintf(full, sizeof full, "cd \"%s\" && %s 2>&1", workdir, cmd);
    else snprintf(full, sizeof full, "%s 2>&1", cmd);
    FILE *p = popen(full, "r");
    if (!p) return -1;
    char line[1024];
    while (fgets(line, sizeof line, p)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (cb) cb(line, u);
    }
    int rc = pclose(p);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

void lx_platform_open_folder(const char *path) {
    char cmd[LX_PATH_MAX + 32];
    snprintf(cmd, sizeof cmd, "xdg-open \"%s\" >/dev/null 2>&1 &", path);
    if (system(cmd) != 0) LX_WARN("Could not open %s", path);
}
#endif
