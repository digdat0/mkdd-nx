# Nintendo Switch build (devkitA64 / libnx). Build with switch/build.sh.
# GEN_DIR: the C generated from your own disc by tools/gen_c.py (never part of this repository).
cmake_minimum_required(VERSION 3.13)
project(mkdd_nx C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
# 100+ objects: pass them to the linker in a response file
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS ON)
set(CMAKE_C_RESPONSE_FILE_LINK_FLAG "@")

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The app's version: the NACP's and the updater's (only update.c sees it, so a bump rebuilds one file)
set(MKDD_VERSION "1.4.1")
if(NOT GEN_DIR)
    message(FATAL_ERROR "set -DGEN_DIR=<output of tools/gen_c.py>")
endif()
message(STATUS "generated C: ${GEN_DIR}")

add_compile_options(-fPIE -ffp-contract=off -fwrapv -fno-strict-aliasing)
include_directories(${ROOT}/runtime ${GEN_DIR} ${DEVKITPRO}/portlibs/switch/include ${DEVKITPRO}/portlibs/switch/include/SDL2)
link_directories(${DEVKITPRO}/portlibs/switch/lib)

file(GLOB RT_SOURCES CONFIGURE_DEPENDS "${ROOT}/runtime/*.c")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/part_*.c")
set(SW_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main_switch.c ${CMAKE_CURRENT_SOURCE_DIR}/render_gl.c ${CMAKE_CURRENT_SOURCE_DIR}/tev_gen.c ${CMAKE_CURRENT_SOURCE_DIR}/prof_switch.c ${CMAKE_CURRENT_SOURCE_DIR}/menu.c ${CMAKE_CURRENT_SOURCE_DIR}/ui_font.c ${CMAKE_CURRENT_SOURCE_DIR}/pause_art.c ${CMAKE_CURRENT_SOURCE_DIR}/hdtex.c ${CMAKE_CURRENT_SOURCE_DIR}/update.c)

# Generated code: -O2 (big functions; -O3 mostly costs compile time). Runtime hot paths -O3.
set_source_files_properties(${GEN_SOURCES} ${GEN_DIR}/table.c PROPERTIES COMPILE_OPTIONS "-O2;-w")
set_source_files_properties(${RT_SOURCES} ${SW_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3;-Wall;-Wno-unused-function")
# pause_art_data.S: the pause screen's Switch picture and button badges (tools/make_pause_art.py)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/pause_art_data.S PROPERTIES COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}"
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pause_console.rgba;${CMAKE_CURRENT_SOURCE_DIR}/pause_badges.rgba")

add_executable(MKDD-NX ${SW_SOURCES} ${RT_SOURCES} ${GEN_SOURCES} ${GEN_DIR}/table.c ${CMAKE_CURRENT_SOURCE_DIR}/pause_art_data.S)
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/update.c APPEND PROPERTY COMPILE_DEFINITIONS MKDD_VERSION="${MKDD_VERSION}")
target_link_libraries(MKDD-NX SDL2 EGL GLESv2 glapi drm_nouveau curl z zstd lzma nx m)
# Mesa is C++: link with the C++ driver so libstdc++ / libsupc++ come in
set_target_properties(MKDD-NX PROPERTIES LINKER_LANGUAGE CXX)

nx_generate_nacp(MKDD-NX.nacp
    NAME "Mario Kart: Double Dash!! (recomp)"
    AUTHOR "digdat0"
    VERSION "${MKDD_VERSION}"
)
nx_create_nro(MKDD-NX
    NACP MKDD-NX.nacp
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg"
)

# SD card layout. Only the .nro is (re)written; the disc image and saves put there are left alone.
set(SD_DIR "${CMAKE_BINARY_DIR}/sdcard/switch/MKDD-NX")
add_custom_command(TARGET MKDD-NX_nro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/MKDD-NX.nro" "${SD_DIR}/"
    COMMENT "Assembling sdcard/switch/MKDD-NX/")
