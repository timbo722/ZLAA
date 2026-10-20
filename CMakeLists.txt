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

# Each compile of a chunk needs up to ~1.5 GB of RAM. Running one job per core could run the
# machine out of memory, so game chunks get their own Ninja pool sized by RAM.
cmake_host_system_information(RESULT ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
cmake_host_system_information(RESULT cores QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR jobs_by_ram "${ram_mb} / 1500")
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
add_library(recomp_runtime STATIC runtime/recomp_runtime.c runtime/recomp_crypto.c)
target_include_directories(recomp_runtime PUBLIC runtime)
target_compile_options(recomp_runtime PRIVATE ${RECOMP_FLAGS} -Wall)

# The game: ~290 generated chunks. Warnings are off (generated labels go unused, etc.)
add_library(game_code STATIC ${GAME_SOURCES})
target_include_directories(game_code PRIVATE "${GAME_GEN_DIR}")
target_link_libraries(game_code PUBLIC recomp_runtime)
target_compile_options(game_code PRIVATE ${RECOMP_FLAGS} -w)
set_property(TARGET game_code PROPERTY JOB_POOL_COMPILE game_pool)

# Desktop host program. For now it only proves that everything links.
add_executable(laa_host host/main.c)
target_link_libraries(laa_host PRIVATE game_code m)
target_compile_options(laa_host PRIVATE -Wall)
