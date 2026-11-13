/* Android platform layer: NativeActivity + Vulkan surface (libvulkan.so). */
#include "../lx_internal.h"
#if LX_OS_ANDROID
#include <android_native_app_glue.h>
#include <android/asset_manager.h>
#include <dlfcn.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

extern AAssetManager *g_lx_android_assets;
int lx_main(int argc, char **argv); /* provided by the game/player */

static struct {
    struct android_app *app;
    void *libvk;
    PFN_vkGetInstanceProcAddr gipa;
    bool has_window, focused, quit;
    uint32_t surface_gen;
    char clipboard[4096];
} A;

static void handle_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        A.has_window = app->window != NULL;
        A.surface_gen++;
        if (app->window) {
            g_lx_input.width = ANativeWindow_getWidth(app->window);
            g_lx_input.height = ANativeWindow_getHeight(app->window);
        }
        break;
    case APP_CMD_TERM_WINDOW: A.has_window = false; break;
    case APP_CMD_WINDOW_RESIZED: case APP_CMD_CONFIG_CHANGED:
        if (app->window) {
            g_lx_input.width = ANativeWindow_getWidth(app->window);
            g_lx_input.height = ANativeWindow_getHeight(app->window);
        }
        break;
    case APP_CMD_GAINED_FOCUS: A.focused = true; break;
    case APP_CMD_LOST_FOCUS: A.focused = false; break;
    case APP_CMD_DESTROY: A.quit = true; break;
    default: break;
    }
}

static int pad_slot(int32_t device) {
    static int32_t devices[LX_MAX_PADS] = {-1, -1, -1, -1};
    for (int i = 0; i < LX_MAX_PADS; i++) if (devices[i] == device) return i;
    for (int i = 0; i < LX_MAX_PADS; i++)
        if (devices[i] == -1) {
            devices[i] = device;
            g_lx_pads[i].connected = true;
            snprintf(g_lx_pads[i].name, sizeof g_lx_pads[i].name, "Android Controller %d", i + 1);
            LX_INFO("Gamepad connected: %s", g_lx_pads[i].name);
            return i;
        }
    return -1;
}

static int map_pad_key(int32_t code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return LX_PAD_A;
    case AKEYCODE_BUTTON_B: return LX_PAD_B;
    case AKEYCODE_BUTTON_X: return LX_PAD_X;
    case AKEYCODE_BUTTON_Y: return LX_PAD_Y;
    case AKEYCODE_BUTTON_L1: return LX_PAD_LB;
    case AKEYCODE_BUTTON_R1: return LX_PAD_RB;
    case AKEYCODE_BUTTON_SELECT: return LX_PAD_BACK;
    case AKEYCODE_BUTTON_START: return LX_PAD_START;
    case AKEYCODE_BUTTON_THUMBL: return LX_PAD_LSTICK;
    case AKEYCODE_BUTTON_THUMBR: return LX_PAD_RSTICK;
    case AKEYCODE_DPAD_UP: return LX_PAD_UP;
    case AKEYCODE_DPAD_DOWN: return LX_PAD_DOWN;
    case AKEYCODE_DPAD_LEFT: return LX_PAD_LEFT;
    case AKEYCODE_DPAD_RIGHT: return LX_PAD_RIGHT;
    case AKEYCODE_BUTTON_MODE: return LX_PAD_GUIDE;
    default: return -1;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *ev) {
    LX_UNUSED(app);
    int32_t source = AInputEvent_getSource(ev);
    bool pad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
    if (pad) {
        int slot = pad_slot(AInputEvent_getDeviceId(ev));
        if (slot < 0) return 0;
        LxPadState *p = &g_lx_pads[slot];
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
            int b = map_pad_key(AKeyEvent_getKeyCode(ev));
            if (b >= 0) { p->buttons[b] = AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN; return 1; }
            return 0;
        }
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
            p->axes[LX_PAD_LEFT_X] = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0);
            p->axes[LX_PAD_LEFT_Y] = -AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
            p->axes[LX_PAD_RIGHT_X] = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Z, 0);
            p->axes[LX_PAD_RIGHT_Y] = -AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RZ, 0);
            float lt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0), rt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0);
            if (lt == 0) lt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_BRAKE, 0);
            if (rt == 0) rt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_GAS, 0);
            p->axes[LX_PAD_LT] = lt;
            p->axes[LX_PAD_RT] = rt;
            float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
            p->buttons[LX_PAD_LEFT] = hx < -0.5f; p->buttons[LX_PAD_RIGHT] = hx > 0.5f;
            p->buttons[LX_PAD_UP] = hy < -0.5f; p->buttons[LX_PAD_DOWN] = hy > 0.5f;
            return 1;
        }
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
        size_t n = AMotionEvent_getPointerCount(ev);
        if (n > 10) n = 10;
        g_lx_input.touch_count = (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) ? 0 : (int)n;
        for (size_t i = 0; i < n; i++) g_lx_input.touches[i] = lx_v2(AMotionEvent_getX(ev, i), AMotionEvent_getY(ev, i));
        /* the first finger also drives the mouse so UI code works unchanged */
        if (n > 0) g_lx_input.mouse_pos = g_lx_input.touches[0];
        g_lx_input.mouse[LX_MOUSE_LEFT] = g_lx_input.touch_count > 0;
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(ev) == AKEYCODE_BACK) {
        g_lx_input.keys[LX_KEY_ESCAPE] = AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN;
        return 1;
    }
    return 0;
}

static void poll_events(int timeout) {
    int events;
    struct android_poll_source *src;
    while (ALooper_pollOnce(timeout, NULL, &events, (void **)&src) >= 0) {
        if (src) src->process(A.app, src);
        if (A.app->destroyRequested) { A.quit = true; break; }
        timeout = 0;
    }
}

void android_main(struct android_app *app) {
    memset(&A, 0, sizeof A);
    A.app = app;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    g_lx_android_assets = app->activity->assetManager;
    lx_main(0, NULL);
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) poll_events(-1);
}

void *lx_platform_vk_loader(void) { return (void *)A.gipa; }
uint32_t lx_platform_surface_generation(void) { return A.surface_gen; }

int lx_platform_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    return 2;
}

bool lx_platform_vk_create_surface(void *instance, void *out_surface) {
    PFN_vkCreateAndroidSurfaceKHR create =
        (PFN_vkCreateAndroidSurfaceKHR)A.gipa((VkInstance)instance, "vkCreateAndroidSurfaceKHR");
    if (!create || !A.app->window) return false;
    VkAndroidSurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = A.app->window;
    return create((VkInstance)instance, &ci, NULL, (VkSurfaceKHR *)out_surface) == VK_SUCCESS;
}

bool lx_platform_init(const LxWindowDesc *d) {
    LX_UNUSED(d);
    A.libvk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!A.libvk) { LX_ERROR("libvulkan.so missing"); return false; }
    A.gipa = (PFN_vkGetInstanceProcAddr)dlsym(A.libvk, "vkGetInstanceProcAddr");
    while (!A.has_window && !A.quit) poll_events(-1);
    g_lx_input.dpi_scale = (float)AConfiguration_getDensity(A.app->config) / 160.0f;
    return !A.quit;
}

void lx_platform_shutdown(void) {}

bool lx_platform_pump(void) {
    lx_input_begin_frame();
    poll_events(0);
    /* while backgrounded there is no window: block instead of burning battery */
    while (!A.quit && !A.has_window) poll_events(-1);
    return !A.quit;
}

void lx_platform_set_title(const char *t) { LX_UNUSED(t); }
void lx_platform_set_cursor(int c) { LX_UNUSED(c); }
void lx_platform_set_intercept_close(bool on) { LX_UNUSED(on); }
void lx_platform_set_icon(const uint8_t *rgba, int w, int h) { LX_UNUSED(rgba); LX_UNUSED(w); LX_UNUSED(h); }
bool lx_platform_take_close_request(void) { return false; }
void lx_platform_window_minimize(void) {}
void lx_platform_window_toggle_maximize(void) {}
bool lx_platform_window_maximized(void) { return true; }
void lx_platform_window_begin_move(void) {}
void lx_platform_window_begin_resize(int edges) { LX_UNUSED(edges); }
const char *lx_platform_clipboard_get(void) { return A.clipboard; }
void lx_platform_clipboard_set(const char *t) { lx_strcpy(A.clipboard, sizeof A.clipboard, t); }
int lx_platform_run_process(const char *c, const char *w, void (*cb)(const char *, void *), void *u) {
    LX_UNUSED(c); LX_UNUSED(w); LX_UNUSED(cb); LX_UNUSED(u);
    return -1;
}
void lx_platform_open_folder(const char *p) { LX_UNUSED(p); }
#endif
