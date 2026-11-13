cmake_minimum_required(VERSION 3.20)
project(Luxora VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LUXORA_BUILD_EDITOR "Build the Luxora editor" ON)
option(LUXORA_BUILD_TESTS "Build the headless test suite" ON)
option(LUXORA_BUILD_PLAYER "Build the game player runtime" ON)
set(LUXORA_GAME_PROJECT "" CACHE PATH "Project folder whose Assets/Scripts are compiled into the player")

if(ANDROID)
  set(LUXORA_BUILD_EDITOR OFF)
  set(LUXORA_BUILD_TESTS OFF)
endif()

if(MSVC)
  add_compile_options(/W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O2)
  endif()
endif()

# ------------------------------------------------------------------ engine library
set(LUXORA_SOURCES
  src/core/core.c src/core/math.c src/core/jobs.c src/core/kv.c
  src/gfx/mesh.c src/gfx/image.c src/gfx/assets.c
  src/gi/bvh.c src/gi/photon_gi.c
  src/audio/audio.c
  src/crowd/crowd.c
  src/scene/scene.c src/scene/serialize.c src/scene/behaviours.c
  src/project/project.c
  src/input/input.c
  src/engine.c)

if(WIN32)
  list(APPEND LUXORA_SOURCES src/platform/platform_win32.c)
elseif(ANDROID)
  list(APPEND LUXORA_SOURCES src/platform/platform_android.c
       ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
else()
  list(APPEND LUXORA_SOURCES src/platform/platform_x11.c)
endif()

set(LUXORA_GPU_SOURCES src/gfx/vk_loader.c src/gfx/renderer.c)
add_library(luxora STATIC ${LUXORA_SOURCES} ${LUXORA_GPU_SOURCES})
target_include_directories(luxora PUBLIC include third_party/vulkan PRIVATE src)
if(WIN32)
  target_link_libraries(luxora PUBLIC winmm user32 shell32 gdi32)
elseif(ANDROID)
  target_include_directories(luxora PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
  target_link_libraries(luxora PUBLIC android log aaudio dl m)
else()
  find_package(X11 REQUIRED)
  find_package(Threads REQUIRED)
  target_link_libraries(luxora PUBLIC ${X11_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS} m)
  target_include_directories(luxora PUBLIC ${X11_INCLUDE_DIR})
endif()

# ------------------------------------------------------------------ game scripts registration
function(luxora_add_game_scripts target project_dir)
  set(scripts "")
  if(project_dir AND EXISTS "${project_dir}/Assets/Scripts")
    file(GLOB scripts "${project_dir}/Assets/Scripts/*.c")
  endif()
  set(decls "")
  set(calls "")
  foreach(s ${scripts})
    get_filename_component(stem ${s} NAME_WE)
    string(MAKE_C_IDENTIFIER ${stem} ident)
    string(APPEND decls "void lx_script_${ident}(void);\n")
    string(APPEND calls "    lx_script_${ident}();\n")
  endforeach()
  set(gen "${CMAKE_CURRENT_BINARY_DIR}/${target}_scripts.c")
  file(WRITE ${gen} "/* generated */\n${decls}void lx_game_register_scripts(void) {\n${calls}}\n")
  target_sources(${target} PRIVATE ${gen} ${scripts})
endfunction()

# ------------------------------------------------------------------ Windows app resources
# Gives a Windows .exe its icon, version information (product name, publisher,
# version - what Explorer's Properties > Details shows) and an application
# manifest (Windows 10/11, per-monitor DPI, never asks for admin). Executables
# without these look like anonymous tools to SmartScreen and antivirus scanners.
if(WIN32)
  enable_language(RC)
endif()
function(luxora_windows_app target exe_name product description company version icon)
  if(NOT WIN32)
    return()
  endif()
  foreach(v product description company)
    string(REGEX REPLACE "[\"\\\\]" "" ${v} "${${v}}")
  endforeach()
  if(company STREQUAL "")
    set(company "${product}")
  endif()
  string(REGEX MATCHALL "[0-9]+" parts "${version}")
  list(APPEND parts 0 0 0)
  list(GET parts 0 LX_RC_V1)
  list(GET parts 1 LX_RC_V2)
  list(GET parts 2 LX_RC_V3)
  set(LX_RC_VERSION "${LX_RC_V1}.${LX_RC_V2}.${LX_RC_V3}")
  set(LX_RC_NAME "${product}")
  set(LX_RC_DESC "${description}")
  set(LX_RC_COMPANY "${company}")
  set(LX_RC_EXE "${exe_name}")
  file(TO_CMAKE_PATH "${icon}" LX_RC_ICON)
  set(LX_RC_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/luxora.manifest")
  set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_app.rc")
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/app.rc.in ${rc} @ONLY)
  target_sources(${target} PRIVATE ${rc})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME "${exe_name}")
  if(MSVC)
    # MSVC merges the manifest itself (an RT_MANIFEST resource would clash with the linker's).
    target_sources(${target} PRIVATE ${LX_RC_MANIFEST})
  else()
    set_source_files_properties(${rc} PROPERTIES COMPILE_DEFINITIONS LX_EMBED_MANIFEST)
  endif()
endfunction()

# ------------------------------------------------------------------ player
set(LUXORA_GAME_NAME "Luxora Game" CACHE STRING "Product name stamped into the game executable")
set(LUXORA_GAME_COMPANY "" CACHE STRING "Publisher stamped into the game executable")
set(LUXORA_GAME_VERSION "1.0.0" CACHE STRING "Version stamped into the game executable")
set(LUXORA_GAME_EXE "luxora_player" CACHE STRING "File name of the game executable (without .exe)")
set(LUXORA_GAME_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/luxora_game.ico" CACHE FILEPATH "Icon (.ico) for the game executable")
if(LUXORA_BUILD_PLAYER)
  if(ANDROID)
    add_library(luxora_player SHARED player/main.c)
    target_link_options(luxora_player PRIVATE "-u" "ANativeActivity_onCreate")
  else()
    add_executable(luxora_player player/main.c)
    if(WIN32)
      set_target_properties(luxora_player PROPERTIES WIN32_EXECUTABLE TRUE)
      target_link_options(luxora_player PRIVATE $<$<C_COMPILER_ID:MSVC>:/ENTRY:mainCRTStartup>)
      if(MINGW)
        target_link_options(luxora_player PRIVATE -mwindows)
      endif()
      luxora_windows_app(luxora_player "${LUXORA_GAME_EXE}" "${LUXORA_GAME_NAME}" "${LUXORA_GAME_NAME}"
                         "${LUXORA_GAME_COMPANY}" "${LUXORA_GAME_VERSION}" "${LUXORA_GAME_ICON}")
    endif()
  endif()
  target_link_libraries(luxora_player PRIVATE luxora)
  luxora_add_game_scripts(luxora_player "${LUXORA_GAME_PROJECT}")
endif()

# ------------------------------------------------------------------ editor
if(LUXORA_BUILD_EDITOR AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/editor/main.c)
  file(GLOB EDITOR_SOURCES editor/*.c)
  add_executable(luxora_editor ${EDITOR_SOURCES})
  target_include_directories(luxora_editor PRIVATE src editor)
  target_link_libraries(luxora_editor PRIVATE luxora)
  target_compile_definitions(luxora_editor PRIVATE LUXORA_ENGINE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
  if(WIN32)
    set_target_properties(luxora_editor PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(luxora_editor PRIVATE $<$<C_COMPILER_ID:MSVC>:/ENTRY:mainCRTStartup>)
    luxora_windows_app(luxora_editor "LuxoraEditor" "Luxora Editor" "Luxora Engine Editor" "Luxora Engine"
                       "${PROJECT_VERSION}" "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/luxora_editor.ico")
  endif()
  luxora_add_game_scripts(luxora_editor "")
endif()

# ------------------------------------------------------------------ tests
if(LUXORA_BUILD_TESTS)
  enable_testing()
  add_executable(luxora_tests tests/test_main.c)
  target_include_directories(luxora_tests PRIVATE src)
  target_link_libraries(luxora_tests PRIVATE luxora)
  add_test(NAME luxora_tests COMMAND luxora_tests)

  # The editor linked against a no-op renderer: runs its UI and logic without a GPU
  # (used for smoke tests on machines without Vulkan, e.g. under Xvfb).
  if(LUXORA_BUILD_EDITOR AND NOT WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/editor/main.c)
    add_library(luxora_nogpu STATIC ${LUXORA_SOURCES} tests/stub_renderer.c)
    target_include_directories(luxora_nogpu PUBLIC include third_party/vulkan PRIVATE src)
    target_link_libraries(luxora_nogpu PUBLIC ${X11_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS} m)
    add_executable(luxora_editor_nogpu ${EDITOR_SOURCES})
    target_include_directories(luxora_editor_nogpu PRIVATE src editor)
    target_link_libraries(luxora_editor_nogpu PRIVATE luxora_nogpu)
    target_compile_definitions(luxora_editor_nogpu PRIVATE LUXORA_ENGINE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    luxora_add_game_scripts(luxora_editor_nogpu "")
  endif()
endif()
