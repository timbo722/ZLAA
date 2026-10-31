/*
 * game_abi.c - built into the game library (not the runtime): which recomp_runtime.h the
 * generated code was compiled against. game_lib.c refuses a library whose id isn't its own.
 */
#include "recomp_runtime.h"

RECOMP_EXPORT const char recomp_game_abi[] = RECOMP_ABI;
