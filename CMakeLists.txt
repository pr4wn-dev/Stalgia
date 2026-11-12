cmake_minimum_required(VERSION 3.16)
project(Stalgia VERSION 1.12.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(STALGIA_SOURCES
    src/main.cpp
    src/core/Log.cpp
    src/core/Util.cpp
    src/core/Config.cpp
    src/effect/EffectRegistry.cpp
    src/gfx/GlyphAtlas.cpp
    src/gfx/Plates.cpp
    src/gfx/Renderer.cpp
    src/effects/MatrixRain.cpp
    src/effects/Starfield.cpp
    src/effects/Aurora.cpp
    src/effects/LavaLamp.cpp
    src/effects/LavaField.cpp
    src/effects/RainReveal.cpp
    src/effects/ImageMask.cpp
    src/effects/Fireflies.cpp
    src/effects/Snow.cpp
    src/effect/Presets.cpp
    src/host/WallpaperHost.cpp
    src/host/DesktopDiag.cpp
    src/host/PauseMonitor.cpp
    src/host/TrayIcon.cpp
    src/host/SettingsPanel.cpp
    src/host/DesktopWidget.cpp
    src/host/ThumbnailStage.cpp
    src/host/WallpaperGallery.cpp
    src/host/SysMonitor.cpp
    src/host/Autostart.cpp
    src/host/Install.cpp
    src/host/DesktopIcons.cpp
    src/host/CrashGuard.cpp
    src/host/Application.cpp
    resources/app.rc
)

# Extra effect sources live outside the repo. Off by default, so the public
# build does not include them. The directory must define registerPrivateEffects
# in register.cpp. Pass -DSTALGIA_PRIVATE_EFFECTS_DIR=<path> to turn it on.
# The cache value is cleared at the end of this file so a later configure
# without -D does not keep a private folder attached. To drop it immediately:
#   cmake -USTALGIA_PRIVATE_EFFECTS_DIR
set(STALGIA_PRIVATE_EFFECTS_DIR "" CACHE PATH
    "Directory of extra effect sources compiled into this build. Empty skips them. Not sticky: cleared after configure.")
if(STALGIA_PRIVATE_EFFECTS_DIR)
    get_filename_component(_stalgia_private "${STALGIA_PRIVATE_EFFECTS_DIR}" ABSOLUTE)
    file(RELATIVE_PATH _stalgia_private_rel "${CMAKE_SOURCE_DIR}" "${_stalgia_private}")
    if(NOT _stalgia_private_rel MATCHES "^\\.\\./")
        message(FATAL_ERROR
            "STALGIA_PRIVATE_EFFECTS_DIR must be outside the source tree (refusing ${_stalgia_private}). "
            "Clear a sticky cache value with -USTALGIA_PRIVATE_EFFECTS_DIR.")
    endif()
    if(NOT EXISTS "${_stalgia_private}/register.cpp")
        message(FATAL_ERROR
            "STALGIA_PRIVATE_EFFECTS_DIR has no register.cpp (expected ${_stalgia_private}/register.cpp). "
            "That file must define registerPrivateEffects.")
    endif()
    file(GLOB STALGIA_PRIVATE_SOURCES CONFIGURE_DEPENDS
        "${_stalgia_private}/*.cpp"
        "${_stalgia_private}/effects/*.cpp")
    if(NOT STALGIA_PRIVATE_SOURCES)
        message(FATAL_ERROR "STALGIA_PRIVATE_EFFECTS_DIR is set but contains no .cpp files: ${_stalgia_private}")
    endif()
    list(APPEND STALGIA_SOURCES ${STALGIA_PRIVATE_SOURCES})
    message(STATUS "Private effects: ${_stalgia_private}")
else()
    list(APPEND STALGIA_SOURCES src/effect/PrivateEffects.cpp)
    set(_stalgia_private "")
    message(STATUS "Private effects: off")
endif()
# Drop the cache entry so the next configure is public unless -D is passed again.
unset(STALGIA_PRIVATE_EFFECTS_DIR CACHE)

set(SEH_OBJ ${CMAKE_CURRENT_BINARY_DIR}/SehGuard.obj)
if(MSVC)
    list(APPEND STALGIA_SOURCES src/host/SehGuard.c)
else()
    find_program(CLANG_SEH clang)
    if(NOT CLANG_SEH)
        message(FATAL_ERROR "clang is required to compile src/host/SehGuard.c (MinGW g++ has no __try)")
    endif()
    add_custom_command(
        OUTPUT ${SEH_OBJ}
        COMMAND ${CLANG_SEH} --target=x86_64-w64-windows-gnu -fms-extensions -O2
                -c ${CMAKE_SOURCE_DIR}/src/host/SehGuard.c -o ${SEH_OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/src/host/SehGuard.c
        VERBATIM
    )
    set_source_files_properties(${SEH_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND STALGIA_SOURCES ${SEH_OBJ})
endif()

add_executable(Stalgia WIN32 ${STALGIA_SOURCES})

target_include_directories(Stalgia PRIVATE src resources)
if(_stalgia_private)
    target_include_directories(Stalgia PRIVATE "${_stalgia_private}")
endif()

target_compile_definitions(Stalgia PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
    STALGIA_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(Stalgia PRIVATE /W4 /permissive- /utf-8 /MP)
    target_link_options(Stalgia PRIVATE /SUBSYSTEM:WINDOWS)
else()
    target_compile_options(Stalgia PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_options(Stalgia PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -municode
        -mwindows
    )
endif()

target_link_libraries(Stalgia PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    ole32
    uuid
    shell32
    gdi32
    user32
    advapi32
    comctl32
    comdlg32
    wtsapi32
    dcomp
    d2d1
    dwrite
    windowscodecs
    pdh
    psapi
    powrprof
)

set_target_properties(Stalgia PROPERTIES
    OUTPUT_NAME "Stalgia"
    WIN32_EXECUTABLE TRUE
)

install(TARGETS Stalgia RUNTIME DESTINATION .)
install(FILES README.md config.example.json DESTINATION .)
