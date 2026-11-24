/* Loads PNG, JPEG, BMP, GIF (first frame) and TGA files into textures with stb_image, so the
 * plugin does not depend on libobs's image-file API, which changed between OBS 30 and 33. */
#pragma once
#include <graphics/graphics.h>

struct polish_image {
	gs_texture_t *texture; /* straight (not premultiplied) alpha */
	uint32_t cx, cy;
};

/* Decodes off the graphics thread, then uploads. Returns false and leaves img empty on failure. */
bool polish_image_load(struct polish_image *img, const char *path);
void polish_image_free(struct polish_image *img);
