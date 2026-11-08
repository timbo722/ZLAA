/*
 * builder.h - building the game's code on the device (builder.c; Android): the start screen starts it and shows
 * its progress.
 */
#ifndef BUILDER_H
#define BUILDER_H

#ifdef __cplusplus
extern "C" {
#endif

enum { BUILD_IDLE, BUILD_PREPARING, BUILD_TRANSLATING, BUILD_COMPILING, BUILD_LINKING, BUILD_DONE, BUILD_FAILED };
enum { BUILD_COOL, BUILD_BALANCED, BUILD_FAST };      /* how warm the device may get (builder.c: speeds) */

typedef struct {
    int step;                /* BUILD_* */
    double fraction;         /* of the whole build, 0..1 */
    char detail[256];        /* what it is doing (or why it failed) */
    int cooling;             /* waiting for the device to cool down */
    double started, elapsed; /* seconds */
} BuildState;

/* Start building libgame-<id>.so for the dump's exefs into lib_dir (a thread; returns at once) at a speed
 * (BUILD_COOL / _BALANCED / _FAST). 0 = started or already running, -1 = can't (no game files) */
int builder_start(const char *exefs, const char *lib_dir, int speed);
void builder_state(BuildState *out);
void builder_cancel(void);

#ifdef __cplusplus
}
#endif
#endif
