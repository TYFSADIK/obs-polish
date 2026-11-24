#include <obs-module.h>
#include <util/platform.h>
#include "image-load.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_STDIO
#define STBI_MALLOC bmalloc
#define STBI_REALLOC brealloc
#define STBI_FREE bfree
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

void polish_image_free(struct polish_image *img)
{
	if (img->texture) {
		obs_enter_graphics();
		gs_texture_destroy(img->texture);
		obs_leave_graphics();
	}
	img->texture = NULL;
	img->cx = img->cy = 0;
}

bool polish_image_load(struct polish_image *img, const char *path)
{
	polish_image_free(img);
	if (!path || !*path)
		return false;
	/* read through libobs so UTF-8 paths work on Windows too */
	FILE *f = os_fopen(path, "rb");
	if (!f) {
		blog(LOG_WARNING, "[obs-polish] cannot open %s", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = size > 0 ? bmalloc((size_t)size) : NULL;
	bool read_ok = buf && fread(buf, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	int w = 0, h = 0, n = 0;
	uint8_t *px = read_ok ? stbi_load_from_memory(buf, (int)size, &w, &h, &n, 4) : NULL;
	bfree(buf);
	if (!px) {
		blog(LOG_WARNING, "[obs-polish] cannot decode %s: %s", path, stbi_failure_reason());
		return false;
	}
	const uint8_t *planes[1] = {px};
	obs_enter_graphics();
	img->texture = gs_texture_create((uint32_t)w, (uint32_t)h, GS_RGBA, 1, planes, 0);
	obs_leave_graphics();
	stbi_image_free(px);
	img->cx = (uint32_t)w;
	img->cy = (uint32_t)h;
	return img->texture != NULL;
}
