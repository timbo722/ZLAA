/*
 * ui_internal.h - shared between ui.cpp (documents, input, settings) and ui_render.cpp (drawing).
 */
#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include <RmlUi/Core.h>
#include "render/render_ui.h"

Rml::RenderInterface *ui_render_interface(void);
int ui_render_init(void);
void ui_render_begin(const RenderUiTarget *t, int clear);   /* clear: start from black */
void ui_render_image(VkImageView view, float dim);           /* a window-sized image, darkened by dim (0..1) */
void ui_render_end(const char *shot);                        /* submit (shot: and save it as this PNG) */

#endif
