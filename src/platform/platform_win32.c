/* Windows platform layer: Win32 window + Vulkan surface (vulkan-1.dll loaded at runtime). */
#include "../lx_internal.h"
#if LX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

static struct {
    HWND hwnd;
    HMODULE libvk;
    PFN_vkGetInstanceProcAddr gipa;
    HCURSOR cursors[7];
    int cursor;
    bool quit;
    bool borderless, intercept_close, close_requested;
    WCHAR high_surrogate;
    char clipboard[8192];
} W;

static LxKey map_vk(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (LxKey)vk;
    if (vk >= '0' && vk <= '9') return (LxKey)vk;
    if (vk >= VK_F1 && vk <= VK_F12) return (LxKey)(LX_KEY_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_SPACE: return LX_KEY_SPACE;
    case VK_ESCAPE: return LX_KEY_ESCAPE;
    case VK_RETURN: return LX_KEY_ENTER;
    case VK_TAB: return LX_KEY_TAB;
    case VK_BACK: return LX_KEY_BACKSPACE;
    case VK_DELETE: return LX_KEY_DELETE;
    case VK_INSERT: return LX_KEY_INSERT;
    case VK_LEFT: return LX_KEY_LEFT;
    case VK_RIGHT: return LX_KEY_RIGHT;
    case VK_UP: return LX_KEY_UP;
    case VK_DOWN: return LX_KEY_DOWN;
    case VK_HOME: return LX_KEY_HOME;
    case VK_END: return LX_KEY_END;
    case VK_PRIOR: return LX_KEY_PAGE_UP;
    case VK_NEXT: return LX_KEY_PAGE_DOWN;
    case VK_SHIFT: return LX_KEY_SHIFT;
    case VK_CONTROL: return LX_KEY_CTRL;
    case VK_MENU: return LX_KEY_ALT;
    default: return LX_KEY_UNKNOWN;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        if (W.intercept_close) W.close_requested = true;
        else W.quit = true;
        return 0;
    case WM_NCCALCSIZE:
        /* borderless: the client area covers the whole window (the app draws the title bar), but the
           window keeps WS_THICKFRAME so Windows still snaps, animates and resizes it. When maximised,
           Windows positions it past the screen edge by the frame size; pull the client area back in. */
        if (W.borderless && wp) {
            if (IsZoomed(h)) {
                NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
                int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCACTIVATE:
        if (W.borderless) return DefWindowProcW(h, msg, wp, -1);   /* don't paint a frame on focus changes */
        break;
    case WM_SIZE:
        g_lx_input.width = LOWORD(lp);
        g_lx_input.height = HIWORD(lp);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        LxKey k = map_vk(wp);
        if (k) g_lx_input.keys[k] = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) {
            if (W.intercept_close) W.close_requested = true;
            else W.quit = true;
        }
        return 0;
    }
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        if (c >= 0xD800 && c <= 0xDBFF) { W.high_surrogate = c; return 0; }
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF && W.high_surrogate) {
            cp = 0x10000 + (((uint32_t)W.high_surrogate - 0xD800) << 10) + (c - 0xDC00);
            W.high_surrogate = 0;
        }
        if (cp >= 32 && cp != 127) lx_input_push_char(cp);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        g_lx_input.mouse[LX_MOUSE_LEFT] = msg == WM_LBUTTONDOWN;
        if (msg == WM_LBUTTONDOWN) SetCapture(h); else ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        g_lx_input.mouse[LX_MOUSE_RIGHT] = msg == WM_RBUTTONDOWN;
        return 0;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        g_lx_input.mouse[LX_MOUSE_MIDDLE] = msg == WM_MBUTTONDOWN;
        return 0;
    case WM_MOUSEWHEEL:
        g_lx_input.wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        return 0;
    case WM_MOUSEMOVE:
        g_lx_input.mouse_pos = lx_v2((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(W.cursors[W.cursor]); return TRUE; }
        break;
    case WM_KILLFOCUS:
        memset(g_lx_input.keys, 0, sizeof g_lx_input.keys);
        memset(g_lx_input.mouse, 0, sizeof g_lx_input.mouse);
        return 0;
    case WM_DPICHANGED:
        g_lx_input.dpi_scale = (float)HIWORD(wp) / 96.0f;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

uint32_t lx_platform_surface_generation(void) { return 0; }
void *lx_platform_vk_loader(void) { return (void *)W.gipa; }

int lx_platform_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    return 2;
}

bool lx_platform_vk_create_surface(void *instance, void *out_surface) {
    PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)W.gipa((VkInstance)instance, "vkCreateWin32SurfaceKHR");
    if (!create) return false;
    VkWin32SurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(NULL);
    ci.hwnd = W.hwnd;
    return create((VkInstance)instance, &ci, NULL, (VkSurfaceKHR *)out_surface) == VK_SUCCESS;
}

bool lx_platform_init(const LxWindowDesc *d) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * SetDpiCtx)(HANDLE);
    SetDpiCtx set_ctx = (SetDpiCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (set_ctx) set_ctx((HANDLE)-4); /* PER_MONITOR_AWARE_V2 */
    else SetProcessDPIAware();

    W.libvk = LoadLibraryW(L"vulkan-1.dll");
    if (!W.libvk) { LX_ERROR("vulkan-1.dll not found - install a Vulkan capable GPU driver"); return false; }
    W.gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(W.libvk, "vkGetInstanceProcAddr");
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    /* icon resource 1 is embedded by CMake (editor: Luxora icon, games: controller or the project's Icon.ico) */
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.lpszClassName = L"LuxoraWindow";
    RegisterClassExW(&wc);

    wchar_t title[256];
    MultiByteToWideChar(CP_UTF8, 0, d->title ? d->title : "Luxora", -1, title, 256);
    int w = d->width > 0 ? d->width : 1600, h = d->height > 0 ? d->height : 900;
    W.borderless = d->borderless && !d->fullscreen;
    DWORD style = d->fullscreen ? (WS_POPUP | WS_VISIBLE)
                : W.borderless ? (WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
                : WS_OVERLAPPEDWINDOW;
    RECT r = {0, 0, w, h};
    if (!W.borderless) AdjustWindowRect(&r, style, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (d->fullscreen) { x = 0; y = 0; r.right = GetSystemMetrics(SM_CXSCREEN); r.left = 0; r.bottom = GetSystemMetrics(SM_CYSCREEN); r.top = 0; }
    W.hwnd = CreateWindowExW(0, wc.lpszClassName, title, style, x, y, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!W.hwnd) { LX_ERROR("CreateWindow failed"); return false; }
    W.cursors[0] = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    W.cursors[1] = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
    W.cursors[2] = LoadCursorW(NULL, (LPCWSTR)IDC_SIZEWE);
    W.cursors[3] = LoadCursorW(NULL, (LPCWSTR)IDC_SIZENS);
    W.cursors[4] = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    W.cursors[5] = LoadCursorW(NULL, (LPCWSTR)IDC_SIZENWSE);
    W.cursors[6] = LoadCursorW(NULL, (LPCWSTR)IDC_SIZENESW);
    if (W.borderless) /* apply the frame change so WM_NCCALCSIZE removes the caption */
        SetWindowPos(W.hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    GetDpiForWindowFn gdfw = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
    g_lx_input.dpi_scale = gdfw ? (float)gdfw(W.hwnd) / 96.0f : 1.0f;
    ShowWindow(W.hwnd, d->maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    RECT cr;
    GetClientRect(W.hwnd, &cr);
    g_lx_input.width = cr.right - cr.left;
    g_lx_input.height = cr.bottom - cr.top;
    return true;
}

void lx_platform_shutdown(void) {
    if (W.hwnd) DestroyWindow(W.hwnd);
    memset(&W, 0, sizeof W);
}

void lx_platform_set_title(const char *title) {
    wchar_t t[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, t, 256);
    SetWindowTextW(W.hwnd, t);
}

void lx_platform_set_cursor(int c) {
    if (c >= 0 && c < 7 && c != W.cursor) { W.cursor = c; SetCursor(W.cursors[c]); }
}

void lx_platform_set_intercept_close(bool on) { W.intercept_close = on; }
void lx_platform_set_icon(const uint8_t *rgba, int w, int h) { LX_UNUSED(rgba); LX_UNUSED(w); LX_UNUSED(h); } /* from the .exe resource */
bool lx_platform_take_close_request(void) {
    bool r = W.close_requested;
    W.close_requested = false;
    return r;
}
void lx_platform_window_minimize(void) { ShowWindow(W.hwnd, SW_MINIMIZE); }
bool lx_platform_window_maximized(void) { return IsZoomed(W.hwnd) != 0; }
void lx_platform_window_toggle_maximize(void) { ShowWindow(W.hwnd, IsZoomed(W.hwnd) ? SW_RESTORE : SW_MAXIMIZE); }
/* Windows' own move/size loop: Aero snap, shake, multi-monitor all work. It swallows the button
   release, so clear the button state here. */
void lx_platform_window_begin_move(void) {
    ReleaseCapture();
    g_lx_input.mouse[LX_MOUSE_LEFT] = false;
    SendMessageW(W.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}
void lx_platform_window_begin_resize(int edges) {
    static const WPARAM ht[16] = {0, HTLEFT, HTRIGHT, 0, HTTOP, HTTOPLEFT, HTTOPRIGHT, 0, HTBOTTOM, HTBOTTOMLEFT, HTBOTTOMRIGHT, 0, 0, 0, 0, 0};
    WPARAM h = ht[edges & 15];
    if (!h) return;
    ReleaseCapture();
    g_lx_input.mouse[LX_MOUSE_LEFT] = false;
    SendMessageW(W.hwnd, WM_NCLBUTTONDOWN, h, 0);
}

const char *lx_platform_clipboard_get(void) {
    if (!OpenClipboard(W.hwnd)) return W.clipboard;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *ws = GlobalLock(h);
        if (ws) { WideCharToMultiByte(CP_UTF8, 0, ws, -1, W.clipboard, sizeof W.clipboard, NULL, NULL); GlobalUnlock(h); }
    }
    CloseClipboard();
    return W.clipboard;
}

void lx_platform_clipboard_set(const char *text) {
    lx_strcpy(W.clipboard, sizeof W.clipboard, text);
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!mem) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, GlobalLock(mem), n);
    GlobalUnlock(mem);
    if (OpenClipboard(W.hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
    } else GlobalFree(mem);
}

bool lx_platform_pump(void) {
    lx_input_begin_frame();
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) W.quit = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !W.quit;
}


int lx_platform_run_process(const char *cmd, const char *workdir, void (*cb)(const char *, void *), void *u) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    wchar_t wcmd[4096], wdir[LX_PATH_MAX];
    char full[4096];
    snprintf(full, sizeof full, "cmd.exe /C \"%s\"", cmd);
    MultiByteToWideChar(CP_UTF8, 0, full, -1, wcmd, 4096);
    if (workdir) MultiByteToWideChar(CP_UTF8, 0, workdir, -1, wdir, LX_PATH_MAX);
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, workdir ? wdir : NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    char buf[1024], line[2048];
    int ln = 0;
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got > 0) {
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || ln == (int)sizeof line - 1) {
                line[ln] = 0;
                if (ln && line[ln - 1] == '\r') line[ln - 1] = 0;
                if (cb) cb(line, u);
                ln = 0;
            } else line[ln++] = c;
        }
    }
    if (ln) { line[ln] = 0; if (cb) cb(line, u); }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return (int)code;
}

void lx_platform_open_folder(const char *path) {
    wchar_t w[LX_PATH_MAX];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, LX_PATH_MAX);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}
#endif
