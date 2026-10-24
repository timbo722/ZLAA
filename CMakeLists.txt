# LAARecomp: builds the recompiled game code + runtime.
#
#   python recompiler/recomp.py <exefs dir> -o ~/laar-build/gen --no-comments
#   cmake -S . -B ~/laar-build/host -G Ninja -DGAME_GEN_DIR=$HOME/laar-build/gen
#   cmake --build ~/laar-build/host
#
# GAME_GEN_DIR holds C generated from YOUR OWN dump. It must stay outside this repo.
cmake_minimum_required(VERSION 3.20)
project(LAARecomp C)

set(GAME_GEN_DIR "" CACHE PATH "Folder written by recomp.py -o (chunked generated C)")
if(NOT EXISTS "${GAME_GEN_DIR}/sources.cmake")
    message(FATAL_ERROR "GAME_GEN_DIR must point to recomp.py's output folder (no sources.cmake in '${GAME_GEN_DIR}')")
endif()
include("${GAME_GEN_DIR}/sources.cmake")   # sets GAME_SOURCES; CMake re-runs when it changes

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug or Release" FORCE)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")

# Generated C must keep ARM's exact float results: no fused a*b+c (GCC fuses by default!)
# and never -ffast-math. -fwrapv: signed overflow wraps, as it does on ARM.
if(CMAKE_C_FLAGS MATCHES "fast-math" OR CMAKE_C_FLAGS_RELEASE MATCHES "fast-math")
    message(FATAL_ERROR "-ffast-math changes float results; the generated code must not use it")
endif()
set(RECOMP_FLAGS -fwrapv -ffp-contract=off -fno-math-errno)

# Each chunk compile needs ~0.7 GB of RAM (16 jobs peaked at 11 GB). One job per core could
# run the machine out of memory, so game chunks get their own Ninja pool sized by RAM.
cmake_host_system_information(RESULT ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
cmake_host_system_information(RESULT cores QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR jobs_by_ram "${ram_mb} / 1000")
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    math(EXPR jobs_by_ram "${ram_mb} / 1500")    # clang needs ~1.5x gcc's memory per chunk
endif()
if(jobs_by_ram GREATER cores)
    set(jobs_by_ram ${cores})
endif()
if(jobs_by_ram LESS 1)
    set(jobs_by_ram 1)
endif()
set(GAME_JOBS ${jobs_by_ram} CACHE STRING "How many game chunks to compile at once")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS game_pool=${GAME_JOBS})
message(STATUS "Game chunks: ${GAME_JOBS} parallel jobs (${ram_mb} MB RAM, ${cores} cores)")

# Runtime: hand-written support code
add_library(recomp_runtime STATIC runtime/recomp_runtime.c runtime/recomp_crypto.c
            runtime/guest_memory.c runtime/nso_loader.c runtime/kernel.c runtime/kthread.c runtime/svc.c
            runtime/ipc.c runtime/services/sm.c runtime/services/lm.c
            runtime/services/applet.c runtime/services/apm.c
            runtime/services/misc.c runtime/services/nvdrv.c runtime/gpu/gpu.c
            runtime/services/time.c runtime/services/fs.c runtime/romfs.c
            runtime/services/acc.c runtime/services/set.c
            runtime/services/hid.c runtime/services/vi.c
            runtime/services/pl.c runtime/services/audio.c
            runtime/hle.c runtime/nvn/nvn.c runtime/nvn/device.c runtime/nvn/memory.c
            runtime/nvn/layout.c runtime/nvn/texture.c runtime/nvn/state.c runtime/nvn/cmdbuf.c runtime/nvn/exec.c runtime/render/png.c runtime/input.c
            runtime/movie/movie.c)
target_include_directories(recomp_runtime PUBLIC runtime)

# The shader translator (Maxwell -> SPIR-V), also built into a test tool
add_library(laa_shader STATIC runtime/render/shader/translate.c runtime/render/shader/alu.c
            runtime/render/shader/texture.c runtime/render/shader/spirv_builder.c runtime/render/shader/half.c)
target_include_directories(laa_shader PUBLIC runtime)
target_compile_options(laa_shader PRIVATE -Wall -Wextra)
add_executable(shader_tool tools/shader_tool.c)
target_link_libraries(shader_tool PRIVATE laa_shader)

# Window, input and sound: SDL3 (also finds the Vulkan loader for us). Without it the game
# still runs, with no window. LAA_DEPS_ROOT: a folder with unpacked SDL3 + Vulkan headers
# (and the libraries SDL3 needs), for systems where they can't be installed.
if(LAA_DEPS_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${LAA_DEPS_ROOT}/usr")
    file(GLOB _dep_lib_dirs LIST_DIRECTORIES true "${LAA_DEPS_ROOT}/usr/lib/*-linux-gnu" "${LAA_DEPS_ROOT}/usr/lib/*-linux-gnu/pulseaudio")
    set(CMAKE_BUILD_RPATH ${_dep_lib_dirs})
    add_link_options(-Wl,--disable-new-dtags)     # RPATH, which the libraries' own dependencies also use
endif()
# Movies: VP9 decoding with libvpx (optional; without it movies have no picture)
find_path(VPX_INCLUDE vpx/vp8dx.h)
find_library(VPX_LIBRARY vpx)
if(VPX_INCLUDE AND VPX_LIBRARY)
    target_include_directories(recomp_runtime PRIVATE ${VPX_INCLUDE})
    target_link_libraries(recomp_runtime PRIVATE ${VPX_LIBRARY})
    target_compile_definitions(recomp_runtime PRIVATE LAA_HAVE_VPX=1)
endif()
find_package(SDL3 CONFIG)
if(SDL3_FOUND)
    target_sources(recomp_runtime PRIVATE runtime/render/vk.c runtime/render/render.c runtime/render/image.c
                                          runtime/render/submit.c runtime/render/draw.c runtime/render/buffer.c
                                          runtime/render/pipeline.c runtime/render/astc.c)
    target_link_libraries(recomp_runtime PRIVATE laa_shader)
    # PRIVATE: the game's generated code needn't see SDL (its compile flags stay the same)
    target_link_libraries(recomp_runtime PRIVATE SDL3::SDL3)
    target_compile_definitions(recomp_runtime PRIVATE LAA_HAVE_SDL=1)
endif()
target_compile_options(recomp_runtime PRIVATE ${RECOMP_FLAGS} -Wall -Wextra)

# The game: ~290 generated chunks. Warnings are off (generated labels go unused, etc.)
add_library(game_code STATIC ${GAME_SOURCES})
target_include_directories(game_code PRIVATE "${GAME_GEN_DIR}")
target_link_libraries(game_code PUBLIC recomp_runtime)
find_package(Threads REQUIRED)
target_link_libraries(recomp_runtime PUBLIC Threads::Threads)
target_compile_options(game_code PRIVATE ${RECOMP_FLAGS} -w)
set_property(TARGET game_code PROPERTY JOB_POOL_COMPILE game_pool)

if(ANDROID)
    # Android: the host is libmain.so, which SDL's Java activity loads and starts (SDL_main).
    # Build with the NDK's toolchain file and SDL3 built for Android (see tools/android/README.md).
    if(NOT SDL3_FOUND)
        message(FATAL_ERROR "Android needs SDL3 (built for Android; add its install prefix to CMAKE_PREFIX_PATH)")
    endif()
    add_library(main SHARED host/main.c)
    target_link_libraries(main PRIVATE game_code m log android SDL3::SDL3)
    target_compile_definitions(main PRIVATE LAA_HAVE_SDL=1)
    target_compile_options(main PRIVATE -Wall)
else()
    # Desktop host program: loads the player's ExeFS and runs the game.
    # ENABLE_EXPORTS (-rdynamic) keeps function names, so crash backtraces show f_<guest address>.
    add_executable(laa_host host/main.c)
    target_link_libraries(laa_host PRIVATE game_code m)
    if(SDL3_FOUND)
        target_link_libraries(laa_host PRIVATE SDL3::SDL3)
        target_compile_definitions(laa_host PRIVATE LAA_HAVE_SDL=1)
    endif()
    target_compile_options(laa_host PRIVATE -Wall)
    set_property(TARGET laa_host PROPERTY ENABLE_EXPORTS ON)
endif()
