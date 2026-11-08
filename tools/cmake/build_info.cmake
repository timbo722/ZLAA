# build_info.cmake: run at every build (CMakeLists.txt, target laa_build_info): writes OUT (laa_build_info.h) with
# the source tree's commit, "-dirty" when it has uncommitted changes. Rewritten only when it changes, so nothing
# recompiles otherwise. The About page shows it.
execute_process(COMMAND git describe --always --dirty --exclude "*" WORKING_DIRECTORY "${SRC}"
                OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT commit)
    set(commit "unknown")
endif()
set(text "#define LAA_COMMIT \"${commit}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
