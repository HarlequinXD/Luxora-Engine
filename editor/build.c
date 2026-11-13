/* Build pipeline: compiles the Luxora player with the project's scripts, stages the game data
 * and packages a runnable build for Windows, Linux or Android. Runs on a background thread. */
#include <ctype.h>
#include "editor.h"

typedef struct BuildJob {
    LxPlatform target;
    bool run;
    LxProject project;
    char engine[LX_PATH_MAX];
} BuildJob;

static BuildJob g_job;
static volatile long g_finished;   /* 0 running/idle, 1 success, 2 failure */
static char g_result[LX_PATH_MAX];

static void log_line(const char *line, void *u) {
    LX_UNUSED(u);
    if (line[0]) LX_INFO("  %s", line);
}

static int run(const char *cmd, const char *cwd) {
    LX_INFO("$ %s", cmd);
    return lx_platform_run_process(cmd, cwd, log_line, NULL);
}

static bool copy_file(const char *from, const char *to) {
    size_t n;
    char *data = lx_file_read(from, &n);
    if (!data) return false;
    bool ok = lx_file_write(to, data, n);
    lx_free(data);
    return ok;
}

static int copy_tree(const char *from, const char *to) {
    int count = 0;
    lx_mkdir_p(to);
    LxDirEntry *e;
    int n = lx_dir_list(from, &e);
    for (int i = 0; i < n; i++) {
        if (e[i].name[0] == '.') continue;
        char a[LX_PATH_MAX], b[LX_PATH_MAX];
        lx_path_join(a, sizeof a, from, e[i].name);
        lx_path_join(b, sizeof b, to, e[i].name);
        if (e[i].is_dir) count += copy_tree(a, b);
        else if (copy_file(a, b)) count++;
    }
    lx_free(e);
    return count;
}

/* Strips characters that would break a shell argument or a Windows resource string.
   file_name additionally keeps only characters that are safe in a file name. */
static void rc_safe(char *dst, size_t cap, const char *src, bool file_name) {
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)(src ? src : ""); *c && n + 1 < cap; c++) {
        bool ok = file_name ? (isalnum(*c) || *c == ' ' || *c == '-' || *c == '_' || *c == '.' || *c >= 0x80)
                            : (*c >= 0x20 && *c != '"' && *c != '\\' && *c != '`' && *c != '$' && *c != '%');
        if (ok) dst[n++] = (char)*c;
    }
    while (n && (dst[n - 1] == ' ' || dst[n - 1] == '.')) n--;
    dst[n] = 0;
}

static bool have_command(const char *probe) {
    char cmd[256];
#if LX_OS_WINDOWS
    snprintf(cmd, sizeof cmd, "where %s >NUL 2>&1", probe);
#else
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", probe);
#endif
    return lx_platform_run_process(cmd, NULL, NULL, NULL) == 0;
}

static void status(const char *fmt, const char *arg) {
    snprintf(ED.build_status, sizeof ED.build_status, fmt, arg);
}

/* game data shipped next to the executable (desktop) or inside the APK (Android) */
static bool stage_data(const LxProject *p, const char *data_dir) {
    lx_path_remove(data_dir);
    lx_mkdir_p(data_dir);
    char assets_dst[LX_PATH_MAX];
    lx_path_join(assets_dst, sizeof assets_dst, data_dir, "Assets");
    int files = copy_tree(p->assets, assets_dst);
    LxProject shipped = *p;
    lx_strcpy(shipped.root, sizeof shipped.root, data_dir);
    if (p->build.scene_count) lx_strcpy(shipped.startup_scene, sizeof shipped.startup_scene, p->build.scenes[0]);
    if (!lx_project_save(&shipped)) return false;
    char in_src[LX_PATH_MAX], in_dst[LX_PATH_MAX];
    lx_path_join(in_src, sizeof in_src, p->root, "input.lxinput");
    lx_path_join(in_dst, sizeof in_dst, data_dir, "input.lxinput");
    if (lx_file_exists(in_src)) copy_file(in_src, in_dst);
    else lx_input_map_save(lx_input_map(), in_dst);
    LX_INFO("Staged %d asset files", files);
    return true;
}

/* The game's custom icon from Build Settings, or NULL for the default. */
static uint8_t *load_game_icon(const LxProject *p, int *w, int *h) {
    if (!p->build.icon[0]) return NULL;
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, p->assets, p->build.icon);
    uint8_t *px = lx_image_load(path, w, h);
    if (!px) LX_WARN("Game icon %s could not be loaded - using the default icon", p->build.icon);
    else if (*w < 256 || *h < 256) LX_WARN("Game icon is %dx%d; 512x512 or larger looks best on high-DPI screens", *w, *h);
    return px;
}

static bool build_desktop(const BuildJob *j) {
    const LxProject *p = &j->project;
    const char *plat = lx_platform_name(j->target);
    char bdir[LX_PATH_MAX], out[LX_PATH_MAX], tmp[LX_PATH_MAX], cmd[4096];
    snprintf(tmp, sizeof tmp, "Library/Build/%s", plat);
    lx_path_join(bdir, sizeof bdir, p->root, tmp);
    snprintf(tmp, sizeof tmp, "%s/%s/%s", p->build.output_dir[0] ? p->build.output_dir : "Builds", plat, p->name);
    if (tmp[0] == '/' || (tmp[0] && tmp[1] == ':')) lx_strcpy(out, sizeof out, tmp);
    else lx_path_join(out, sizeof out, p->root, tmp);
    lx_mkdir_p(bdir);
    if (!have_command("cmake")) { LX_ERROR("CMake was not found on PATH. Install CMake 3.20+ to build games."); return false; }
    const char *toolchain = "";
    bool cross_win = j->target == LX_PLATFORM_WINDOWS && lx_platform_current() != LX_PLATFORM_WINDOWS;
    if (cross_win) {
        if (!have_command("x86_64-w64-mingw32-gcc")) { LX_ERROR("Cross-building for Windows needs mingw-w64 (x86_64-w64-mingw32-gcc)."); return false; }
        toolchain = " -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres";
    } else if (j->target != lx_platform_current()) {
        LX_ERROR("Building for %s from %s isn't supported here - use the GitHub Actions workflow in the engine repo.", plat,
                 lx_platform_name(lx_platform_current()));
        return false;
    }
    status("Configuring %s build...", plat);
    /* Windows stamps these into the .exe (Explorer > Properties > Details) */
    char gname[LX_NAME_MAX], gcompany[LX_NAME_MAX], gversion[32], gexe[LX_NAME_MAX], gicon[LX_PATH_MAX], icon_arg[LX_PATH_MAX + 32];
    rc_safe(gname, sizeof gname, p->name, false);
    rc_safe(gcompany, sizeof gcompany, p->company, false);
    rc_safe(gversion, sizeof gversion, p->version[0] ? p->version : "1.0.0", false);
    rc_safe(gexe, sizeof gexe, p->name, true);
    if (!gexe[0]) lx_strcpy(gexe, sizeof gexe, "Game");
    /* icon: Build Settings > Game Icon (converted to a multi-size .ico), else an Icon.ico in the
       project folder, else the default game-controller icon */
    icon_arg[0] = 0;
    int iw, ih;
    uint8_t *ipx = load_game_icon(p, &iw, &ih);
    if (ipx) {
        lx_path_join(gicon, sizeof gicon, bdir, "GameIcon.ico");
        if (lx_ico_write(gicon, ipx, iw, ih)) snprintf(icon_arg, sizeof icon_arg, " -DLUXORA_GAME_ICON=\"%s\"", gicon);
        lx_free(ipx);
    } else {
        lx_path_join(gicon, sizeof gicon, p->root, "Icon.ico");
        if (lx_file_exists(gicon)) snprintf(icon_arg, sizeof icon_arg, " -DLUXORA_GAME_ICON=\"%s\"", gicon);
    }
    snprintf(cmd, sizeof cmd, "cmake -S \"%s\" -B \"%s\" -DCMAKE_BUILD_TYPE=%s -DLUXORA_BUILD_EDITOR=OFF -DLUXORA_BUILD_TESTS=OFF -DLUXORA_GAME_PROJECT=\"%s\""
             " -DLUXORA_GAME_NAME=\"%s\" -DLUXORA_GAME_COMPANY=\"%s\" -DLUXORA_GAME_VERSION=\"%s\" -DLUXORA_GAME_EXE=\"%s\"%s%s",
             j->engine, bdir, p->build.development_build ? "RelWithDebInfo" : "Release", p->root, gname, gcompany, gversion, gexe,
             icon_arg, toolchain);
    if (run(cmd, NULL) != 0) { LX_ERROR("CMake configure failed"); return false; }
    status("Compiling %s player...", plat);
    snprintf(cmd, sizeof cmd, "cmake --build \"%s\" --config %s --target luxora_player --parallel", bdir,
             p->build.development_build ? "RelWithDebInfo" : "Release");
    if (run(cmd, NULL) != 0) { LX_ERROR("Compilation failed - check the errors above (your scripts are compiled too)"); return false; }
    status("Packaging %s...", plat);
    char exe_name[LX_NAME_MAX + 8];
    if (j->target == LX_PLATFORM_WINDOWS) snprintf(exe_name, sizeof exe_name, "%s.exe", gexe);
    else lx_strcpy(exe_name, sizeof exe_name, "luxora_player");
    char exe[LX_PATH_MAX];
    lx_path_join(exe, sizeof exe, bdir, exe_name);
    if (!lx_file_exists(exe)) {
        char cfg[LX_PATH_MAX];
        snprintf(cfg, sizeof cfg, "%s/%s", p->build.development_build ? "RelWithDebInfo" : "Release", exe_name);
        lx_path_join(exe, sizeof exe, bdir, cfg);
    }
    if (!lx_file_exists(exe)) { LX_ERROR("Built executable not found in %s", bdir); return false; }
    lx_path_remove(out);
    lx_mkdir_p(out);
    char dst[LX_PATH_MAX], fname[LX_NAME_MAX + 8];
    snprintf(fname, sizeof fname, "%s%s", j->target == LX_PLATFORM_WINDOWS ? gexe : p->name,
             j->target == LX_PLATFORM_WINDOWS ? ".exe" : "");
    lx_path_join(dst, sizeof dst, out, fname);
    if (!copy_file(exe, dst)) { LX_ERROR("Could not copy the executable to %s", dst); return false; }
#if !LX_OS_WINDOWS
    snprintf(cmd, sizeof cmd, "chmod +x \"%s\"", dst);
    run(cmd, NULL);
#endif
    char data[LX_PATH_MAX];
    lx_path_join(data, sizeof data, out, "Data");
    if (!stage_data(p, data)) { LX_ERROR("Could not stage game data"); return false; }
    lx_strcpy(g_result, sizeof g_result, dst);
    if (j->run && j->target == lx_platform_current()) {
        status("Launching %s...", p->name);
#if LX_OS_WINDOWS
        lx_platform_open_folder(dst);
#else
        snprintf(cmd, sizeof cmd, "\"%s\" >/dev/null 2>&1 &", dst);
        run(cmd, out);
#endif
    }
    return true;
}

static bool build_android(const BuildJob *j) {
    const LxProject *p = &j->project;
    char gradle_dir[LX_PATH_MAX], bdir[LX_PATH_MAX], data[LX_PATH_MAX], out[LX_PATH_MAX], cmd[4096], tmp[LX_PATH_MAX];
    lx_path_join(gradle_dir, sizeof gradle_dir, j->engine, "android");
    lx_path_join(bdir, sizeof bdir, p->root, "Library/Build/Android");
    lx_path_join(data, sizeof data, bdir, "assets/Data");
    snprintf(tmp, sizeof tmp, "%s/Android", p->build.output_dir[0] ? p->build.output_dir : "Builds");
    lx_path_join(out, sizeof out, p->root, tmp);
    const char *sdk = getenv("ANDROID_HOME");
    if (!sdk || !sdk[0]) sdk = getenv("ANDROID_SDK_ROOT");
    if (!sdk || !sdk[0]) { LX_ERROR("Set ANDROID_HOME to your Android SDK (with the NDK and CMake installed via sdkmanager)."); return false; }
    const char *gradle = have_command("gradle") ? "gradle" : NULL;
    if (!gradle) { LX_ERROR("Gradle was not found on PATH (Gradle 8.x + JDK 17 are needed for Android builds)."); return false; }
    status("Staging %s data...", "Android");
    if (!stage_data(p, data)) return false;
    status("Building %s APK (Gradle)...", "Android");
    char app_name[LX_NAME_MAX * 2];
    lx_strcpy(app_name, sizeof app_name, p->name);
    for (char *c = app_name; *c; c++) if (*c == '"') *c = '\'';
    /* the assets path handed to gradle is the folder that contains Data/ */
    char assets_root[LX_PATH_MAX];
    lx_path_join(assets_root, sizeof assets_root, bdir, "assets");
    /* launcher icon: Build Settings > Game Icon as mipmaps, else the default in android/app/src/main/res */
    char res_arg[LX_PATH_MAX + 32] = "";
    int iw, ih;
    uint8_t *ipx = load_game_icon(p, &iw, &ih);
    if (ipx) {
        static const struct { const char *dir; int size; } dens[] = {{"mipmap-mdpi", 48}, {"mipmap-hdpi", 72}, {"mipmap-xhdpi", 96},
                                                                    {"mipmap-xxhdpi", 144}, {"mipmap-xxxhdpi", 192}};
        char res[LX_PATH_MAX];
        lx_path_join(res, sizeof res, bdir, "res");
        lx_path_remove(res);
        bool ok = true;
        for (int i = 0; i < 5; i++) {
            char d[LX_PATH_MAX], f[LX_PATH_MAX];
            lx_path_join(d, sizeof d, res, dens[i].dir);
            lx_mkdir_p(d);
            lx_path_join(f, sizeof f, d, "ic_launcher.png");
            uint8_t *sq = lx_image_square(ipx, iw, ih, dens[i].size);
            size_t n;
            uint8_t *png = lx_png_encode(sq, dens[i].size, dens[i].size, &n);
            ok = lx_file_write(f, png, n) && ok;
            lx_free(png);
            lx_free(sq);
        }
        lx_free(ipx);
        if (ok) snprintf(res_arg, sizeof res_arg, " -PluxoraRes=\"%s\"", res);
    }
    snprintf(cmd, sizeof cmd,
             "%s -p \"%s\" assembleDebug --no-daemon -PluxoraProject=\"%s\" -PluxoraAppId=%s -PluxoraAppName=\"%s\" -PluxoraMinSdk=%d "
             "-PluxoraOrientation=%s -PluxoraAssets=\"%s\" -PluxoraBuildDir=\"%s/gradle\"%s",
             gradle, gradle_dir, p->root, p->build.android_package, app_name, p->build.android_min_sdk,
             p->build.android_orientation ? "portrait" : "sensorLandscape", assets_root, bdir, res_arg);
    if (run(cmd, NULL) != 0) { LX_ERROR("Gradle build failed"); return false; }
    char apk[LX_PATH_MAX];
    lx_path_join(apk, sizeof apk, bdir, "gradle/outputs/apk/debug/app-debug.apk");
    if (!lx_file_exists(apk)) { LX_ERROR("APK not found at %s", apk); return false; }
    lx_mkdir_p(out);
    char dst[LX_PATH_MAX], fname[LX_NAME_MAX + 8];
    snprintf(fname, sizeof fname, "%s.apk", p->name);
    lx_path_join(dst, sizeof dst, out, fname);
    copy_file(apk, dst);
    lx_strcpy(g_result, sizeof g_result, dst);
    if (j->run) {
        if (!have_command("adb")) { LX_WARN("adb not found: install the APK manually (%s)", dst); return true; }
        status("Installing on device...%s", "");
        snprintf(cmd, sizeof cmd, "adb install -r \"%s\"", dst);
        if (run(cmd, NULL) != 0) { LX_ERROR("adb install failed - is a phone connected with USB debugging on?"); return false; }
        snprintf(cmd, sizeof cmd, "adb shell am start -n %s/android.app.NativeActivity", p->build.android_package);
        run(cmd, NULL);
    }
    return true;
}

static int build_main(void *u) {
    LX_UNUSED(u);
    double t0 = lx_time();
    LX_INFO("==== Building %s for %s ====", g_job.project.name, lx_platform_name(g_job.target));
    bool ok = g_job.target == LX_PLATFORM_ANDROID ? build_android(&g_job) : build_desktop(&g_job);
    if (ok) LX_INFO("==== Build succeeded in %.1f s: %s ====", lx_time() - t0, g_result);
    else LX_ERROR("==== Build failed ====");
    lx_atomic_store(&g_finished, ok ? 1 : 2);
    return 0;
}

static LxThread *g_thread;

void ed_build(LxPlatform target, bool run_after) {
    if (lx_atomic_load(&ED.building)) { ui_notify(ED.ui, "A build is already running"); return; }
    if (ED.play != ED_EDIT) ed_stop();
    if (ED.dirty && ED.scene_ref[0]) ed_save_scene();
    if (!ED.project.build.scene_count) {
        if (!ED.scene_ref[0]) { ui_notify(ED.ui, "Save your scene first so it can be included in the build"); return; }
        lx_strcpy(ED.project.build.scenes[0], LX_REF_MAX, ED.scene_ref);
        ED.project.build.scene_count = 1;
    }
    ED.project.graphics = *lx_graphics_settings();
    ED.project.audio = *lx_audio_settings();
    lx_project_save(&ED.project);
    char in[LX_PATH_MAX];
    lx_path_join(in, sizeof in, ED.project.root, "input.lxinput");
    lx_input_map_save(lx_input_map(), in);
    const char *engine = ed_engine_root();
    char probe[LX_PATH_MAX];
    lx_path_join(probe, sizeof probe, engine, "CMakeLists.txt");
    if (!engine[0] || !lx_file_exists(probe)) {
        ui_notify(ED.ui, "Engine source not found - set LUXORA_ENGINE_ROOT");
        LX_ERROR("Engine source folder not found (%s). Set the LUXORA_ENGINE_ROOT environment variable to the Luxora repo.", engine);
        return;
    }
    if (g_thread) { lx_thread_join(g_thread); g_thread = NULL; }
    memset(&g_job, 0, sizeof g_job);
    g_job.target = target;
    g_job.run = run_after;
    g_job.project = ED.project;
    lx_strcpy(g_job.engine, sizeof g_job.engine, engine);
    g_result[0] = 0;
    lx_atomic_store(&g_finished, 0);
    lx_atomic_store(&ED.building, 1);
    snprintf(ED.build_status, sizeof ED.build_status, "Building for %s...", lx_platform_name(target));
    if (ED.bottom_tabs) ED.bottom_tabs->tab_index = 1; /* show the console */
    g_thread = lx_thread_create(build_main, NULL);
}

int ed_last_build_result; /* 0 none, 1 success, 2 failure */

void ed_build_poll(void) {
    long f = lx_atomic_load(&g_finished);
    if (!f || !lx_atomic_load(&ED.building)) return;
    ed_last_build_result = (int)f;
    lx_atomic_store(&ED.building, 0);
    lx_atomic_store(&g_finished, 0);
    if (g_thread) { lx_thread_join(g_thread); g_thread = NULL; }
    if (f == 1) ui_notify(ED.ui, "Build succeeded: %s", lx_path_filename(g_result));
    else ui_notify(ED.ui, "Build failed - see the Console");
}
