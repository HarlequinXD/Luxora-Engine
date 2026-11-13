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

static int32_t handle_input(struct android_app *app, AInputEvent *ev) {
    LX_UNUSED(app);
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
const char *lx_platform_clipboard_get(void) { return A.clipboard; }
void lx_platform_clipboard_set(const char *t) { lx_strcpy(A.clipboard, sizeof A.clipboard, t); }
int lx_platform_run_process(const char *c, const char *w, void (*cb)(const char *, void *), void *u) {
    LX_UNUSED(c); LX_UNUSED(w); LX_UNUSED(cb); LX_UNUSED(u);
    return -1;
}
void lx_platform_open_folder(const char *p) { LX_UNUSED(p); }
#endif
