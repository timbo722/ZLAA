# LAARecomp: builds the recompiled game code + runtime.
#
#   python recompiler/recomp.py <exefs dir> -o ~/laar-build/gen --no-comments
#   cmake -S . -B ~/laar-build/host -G Ninja -DGAME_GEN_DIR=$HOME/laar-build/gen
#   cmake --build ~/laar-build/host
#
# GAME_GEN_DIR holds C generated from YOUR OWN dump. It must stay outside this repo.
# The game becomes its own library, libgame-<main build id>.so (target `game`), which the
# program (laa_host, or libmain.so on Android) opens at start-up (runtime/game_lib.c): no
# build of the program itself contains any of the game's code.
cmake_minimum_required(VERSION 3.20)
project(LAARecomp VERSION 0.1.0 LANGUAGES C)

set(GAME_GEN_DIR "" CACHE PATH "Folder written by recomp.py -o (chunked generated C)")
if(NOT EXISTS "${GAME_GEN_DIR}/sources.cmake")
    message(FATAL_ERROR "GAME_GEN_DIR must point to recomp.py's output folder (no sources.cmake in '${GAME_GEN_DIR}')")
endif()
include("${GAME_GEN_DIR}/sources.cmake")   # sets GAME_SOURCES, GAME_BUILD_ID; CMake re-runs when it changes
if(NOT GAME_BUILD_ID)
    message(FATAL_ERROR "${GAME_GEN_DIR} was made by an older recomp.py (no GAME_BUILD_ID): run recomp.py again")
endif()

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

# The ABI id: the headers the generated code is compiled against. The runtime refuses a game
# library built with other ones (game_lib.c); CMake re-runs when they change.
set(_abi_headers runtime/recomp_runtime.h runtime/recomp_simd.h)
set(_abi "")
foreach(h ${_abi_headers})
    file(SHA256 "${CMAKE_SOURCE_DIR}/${h}" _h)
    string(APPEND _abi "${_h}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${h}")
endforeach()
string(SHA256 RECOMP_ABI "${_abi}")
if(ANDROID)
    # The Thor's cores are ARMv9; armv8.2-a makes atomics single instructions (LSE) instead of
    # calls that check the CPU at run time
    add_compile_options(-march=armv8.2-a)
endif()

# No debug info for the game by default (-g0 below): it doubled the compile time, and crash
# reports name game functions from the library's own table (game_lib.c). ON: line numbers for a
# debugger.
option(LAA_GAME_DEBUG_INFO "debug info (-g) for the generated game code" OFF)

# Each chunk compile needs up to ~0.7 GB of RAM (16 jobs peaked at 11 GB). One job per core could
# run the machine out of memory, so game chunks get their own Ninja pool sized by RAM.
# (NDK clang, the largest chunk: 0.52 GB peak without debug info, 0.73 GB with.)
cmake_host_system_information(RESULT ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
cmake_host_system_information(RESULT cores QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR jobs_by_ram "${ram_mb} / 1000")
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    if(LAA_GAME_DEBUG_INFO)
        math(EXPR jobs_by_ram "${ram_mb} / 1500")
    else()
        math(EXPR jobs_by_ram "${ram_mb} / 750")
    endif()
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
            runtime/movie/movie.c runtime/trace_symbols.c runtime/game_lib.c)
target_include_directories(recomp_runtime PUBLIC runtime)
target_compile_definitions(recomp_runtime PRIVATE RECOMP_ABI="${RECOMP_ABI}")
target_link_libraries(recomp_runtime PUBLIC ${CMAKE_DL_LIBS})

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
# Movies: VP9 decoding with libvpx on desktop (optional; without it movies have no picture),
# Android's own decoders (MediaCodec, in libmediandk) on Android
find_path(VPX_INCLUDE vpx/vp8dx.h)
find_library(VPX_LIBRARY vpx)
if(ANDROID)
    target_link_libraries(recomp_runtime PRIVATE mediandk)
elseif(VPX_INCLUDE AND VPX_LIBRARY)
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

# The menus (runtime/ui, C++): RmlUi + FreeType, static libraries made by tools/build_ui_deps.sh.
# Without them the program starts the game straight away, as before. Their files: res/ (LAA_RES_DIR).
find_package(RmlUi CONFIG QUIET)
if(SDL3_FOUND AND RmlUi_FOUND)
    enable_language(CXX)
    execute_process(COMMAND git describe --always --dirty --exclude "*" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE LAA_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)   # (as of configuring)
    add_library(laa_ui STATIC runtime/ui/ui.cpp runtime/ui/ui_render.cpp)
    target_include_directories(laa_ui PRIVATE runtime)
    target_link_libraries(laa_ui PRIVATE RmlUi::Core SDL3::SDL3)
    target_compile_definitions(laa_ui PRIVATE LAA_VERSION="${PROJECT_VERSION}" LAA_COMMIT="${LAA_COMMIT}")
    target_compile_options(laa_ui PRIVATE -Wall -fno-exceptions -fno-rtti)
    set(LAA_UI laa_ui)
    message(STATUS "Menus: RmlUi ${RmlUi_VERSION} (LAARecomp ${PROJECT_VERSION}, ${LAA_COMMIT})")
else()
    message(STATUS "Menus: off (no RmlUi: tools/build_ui_deps.sh)")
endif()

find_package(Threads REQUIRED)
target_link_libraries(recomp_runtime PUBLIC Threads::Threads)

# The game: ~290 generated chunks -> libgame-<build id>.so. Warnings are off (generated labels
# go unused, etc.). -fvisibility=hidden: only the tables are exported (recomp_runtime.h
# RECOMP_EXPORT), so the ~150K functions call each other directly and the library opens fast.
# It calls the runtime's recomp_* functions, which the program that opens it exports.
add_library(game SHARED ${GAME_SOURCES} runtime/game_abi.c)
set_target_properties(game PROPERTIES OUTPUT_NAME "game-${GAME_BUILD_ID}" JOB_POOL_COMPILE game_pool)
target_include_directories(game PRIVATE "${GAME_GEN_DIR}" runtime)
target_compile_definitions(game PRIVATE RECOMP_ABI="${RECOMP_ABI}")
target_compile_options(game PRIVATE ${RECOMP_FLAGS} -w -fvisibility=hidden)
if(NOT LAA_GAME_DEBUG_INFO)                     # (-g0 comes after the toolchain's -g)
    target_compile_options(game PRIVATE -g0)
endif()

if(ANDROID)
    # Android: the host is libmain.so, which SDL's Java activity loads and starts (SDL_main).
    # Build with the NDK's toolchain file and SDL3 built for Android (see tools/android/README.md).
    if(NOT SDL3_FOUND)
        message(FATAL_ERROR "Android needs SDL3 (built for Android; add its install prefix to CMAKE_PREFIX_PATH)")
    endif()
    add_library(main SHARED host/main.c)
    # the whole runtime, also what only the game library calls (recomp_bad_jump, recomp_aes, ...)
    target_link_libraries(main PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_runtime>" ${LAA_UI} m log android SDL3::SDL3)
    target_compile_definitions(main PRIVATE LAA_HAVE_SDL=1 $<$<BOOL:${LAA_UI}>:LAA_HAVE_UI=1>)
    target_compile_options(main PRIVATE -Wall)
    # Calls between the runtime's own functions go straight to them, not through the PLT: in a
    # shared library every global function could be replaced from outside, so each call took a
    # stub and a GOT load (~2-3% of the CPU). Still exported (the game library calls them).
    target_link_options(main PRIVATE -Wl,-Bsymbolic-functions)
    # The game library needs libmain.so (DT_NEEDED): Android's loader finds the runtime's
    # functions only in a library's own dependencies, and libmain.so is already loaded.
    # (no relink when only libmain.so changed: the game library needs only its symbol names)
    target_link_libraries(game PRIVATE main)
    set_target_properties(game PROPERTIES LINK_DEPENDS_NO_SHARED ON)
else()
    # Desktop host program: loads the player's ExeFS and runs the game.
    # ENABLE_EXPORTS (-rdynamic): the game library finds the runtime's functions in laa_host.
    add_executable(laa_host host/main.c)
    target_link_libraries(laa_host PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_runtime>" m)
    add_dependencies(laa_host game)              # (built together; opened from next to laa_host)
    if(SDL3_FOUND)
        target_link_libraries(laa_host PRIVATE SDL3::SDL3 ${LAA_UI})
        target_compile_definitions(laa_host PRIVATE LAA_HAVE_SDL=1 $<$<BOOL:${LAA_UI}>:LAA_HAVE_UI=1>
                                   LAA_RES_DIR="${CMAKE_SOURCE_DIR}/res")
    endif()
    target_compile_options(laa_host PRIVATE -Wall)
    set_property(TARGET laa_host PROPERTY ENABLE_EXPORTS ON)
endif()
