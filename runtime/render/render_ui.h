/*
 * render_ui.h - what the menus (runtime/ui) use of the renderer: they draw whole frames into the
 * window's images themselves, on their own command buffers (submitted under render_queue_lock),
 * before the game starts or while it is paused. (Kept apart from render.h, which C++ can't read.)
 */
#ifndef RENDER_UI_H
#define RENDER_UI_H

#include <pthread.h>
#include "vk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* render_ui_begin waits until the display took the previous frame, then gives a window-sized image
 * (layout GENERAL; view: UNORM) and the game's last frame (last_view, or VK_NULL_HANDLE), which
 * stays untouched; render_ui_end (after submitting) shows it. */
typedef struct { int slot, last; VkImage image; VkImageView view, last_view; uint32_t width, height; } RenderUiTarget;
VkFormat render_ui_format(void);
int render_ui_begin(RenderUiTarget *t);
void render_ui_end(const RenderUiTarget *t);
extern pthread_mutex_t render_queue_lock;

#ifdef __cplusplus
}
#endif
#endif
