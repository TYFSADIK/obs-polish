/* Headless render test: runs an image through the Polish filter inside a real libobs and writes
 * the result as a PNG. Works on macOS, Windows and Linux.
 *
 *   render-test <plugin binary> <plugin data dir> <input image> <output.png> ['{"json":"settings"}'] [frames] [--json]
 *
 * Where OBS's own pieces are comes from the environment, with defaults for each platform:
 *   POLISH_OBS_DATA            libobs data directory (effects)
 *   POLISH_GRAPHICS            graphics module (libobs-opengl, libobs-d3d11, or a full path)
 *   POLISH_IMAGE_SOURCE_BIN    OBS's image-source plugin binary
 *   POLISH_IMAGE_SOURCE_DATA   its data directory
 *   POLISH_BENCH=1             no sleeping, wait for the GPU every frame, print ms per frame
 * With --json the filter's analysis state (faces found, landmark positions) is printed. */
#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <obs-nix-platform.h>
#include <dlfcn.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const char *env_or(const char *name, const char *def)
{
	const char *v = getenv(name);
	return v && *v ? v : def;
}

#if defined(__APPLE__)
#define OBS_APP "/Applications/OBS.app/Contents"
#define DEF_DATA OBS_APP "/Frameworks/libobs.framework/Resources/"
#define DEF_GRAPHICS OBS_APP "/Frameworks/libobs-opengl.dylib"
#define DEF_IMG_BIN OBS_APP "/PlugIns/image-source.plugin/Contents/MacOS/image-source"
#define DEF_IMG_DATA OBS_APP "/PlugIns/image-source.plugin/Contents/Resources"
#elif defined(_WIN32)
#define DEF_DATA "data/libobs/"
#define DEF_GRAPHICS "libobs-d3d11"
#define DEF_IMG_BIN "obs-plugins/64bit/image-source.dll"
#define DEF_IMG_DATA "data/obs-plugins/image-source"
#else
#define DEF_DATA "/usr/share/obs/libobs/"
#define DEF_GRAPHICS "libobs-opengl"
#define DEF_IMG_BIN "/usr/lib/x86_64-linux-gnu/obs-plugins/image-source.so"
#define DEF_IMG_DATA "/usr/share/obs/obs-plugins/image-source"
#endif

static bool load_module(const char *bin, const char *data)
{
	obs_module_t *mod = NULL;
	int r = obs_open_module(&mod, bin, data);
	if (r != MODULE_SUCCESS) {
		fprintf(stderr, "open %s failed: %d\n", bin, r);
		return false;
	}
	return obs_init_module(mod);
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: render-test <plugin bin> <plugin data> <in> <out.png> [settings json] [frames] [--json]\n");
		return 2;
	}
	const char *plugin_bin = argv[1], *plugin_data = argv[2], *in = argv[3], *out = argv[4];
	const char *json = argc > 5 ? argv[5] : "{}";
	int frames = argc > 6 ? atoi(argv[6]) : 30;
	bool dump = argc > 7 && strcmp(argv[7], "--json") == 0;
	bool bench = getenv("POLISH_BENCH") != NULL;

	int iw = 0, ih = 0, n = 0;
	if (!stbi_info(in, &iw, &ih, &n)) {
		fprintf(stderr, "cannot read %s\n", in);
		return 2;
	}
	uint32_t w = (uint32_t)iw, h = (uint32_t)ih;

#ifdef __linux__
	/* EGL on the X display (Xvfb in CI); Xlib is opened at run time so the tool needs no -dev package */
	void *x11 = dlopen("libX11.so.6", RTLD_NOW);
	void *(*xopen)(const char *) = x11 ? (void *(*)(const char *))dlsym(x11, "XOpenDisplay") : NULL;
	void *display = xopen ? xopen(NULL) : NULL;
	if (!display) {
		fprintf(stderr, "no X display (run under xvfb-run)\n");
		return 3;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);
#endif
	if (!obs_startup("en-US", NULL, NULL))
		return 3;
	obs_add_data_path(env_or("POLISH_OBS_DATA", DEF_DATA));
	struct obs_video_info ovi = {
		.graphics_module = env_or("POLISH_GRAPHICS", DEF_GRAPHICS),
		.fps_num = 30,
		.fps_den = 1,
		.base_width = w,
		.base_height = h,
		.output_width = w,
		.output_height = h,
		.output_format = VIDEO_FORMAT_NV12,
		.adapter = 0,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	int vr = obs_reset_video(&ovi);
	if (vr != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed: %d\n", vr);
		return 4;
	}
	if (!load_module(env_or("POLISH_IMAGE_SOURCE_BIN", DEF_IMG_BIN), env_or("POLISH_IMAGE_SOURCE_DATA", DEF_IMG_DATA)) ||
	    !load_module(plugin_bin, plugin_data))
		return 5;
	obs_post_load_modules();

	obs_data_t *is = obs_data_create();
	obs_data_set_string(is, "file", in);
	obs_source_t *source = obs_source_create("image_source", "photo", is, NULL);
	obs_data_release(is);
	obs_data_t *fs = obs_data_create_from_json(json);
	obs_source_t *filter = obs_source_create("polish_filter", "polish", fs, NULL);
	obs_data_release(fs);
	if (!filter) {
		fprintf(stderr, "filter not registered\n");
		return 6;
	}
	obs_source_filter_add(source, filter);
	obs_set_output_source(0, source); /* makes the source active, so it ticks and renders */

	gs_texrender_t *tr = NULL;
	gs_stagesurf_t *stage = NULL;
	uint64_t t_start = 0;
	for (int f = 0; f < frames; f++) {
		obs_enter_graphics();
		if (!tr) {
			tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			stage = gs_stagesurface_create(w, h, GS_RGBA);
		}
		gs_texrender_reset(tr);
		if (gs_texrender_begin(tr, w, h)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			obs_source_video_render(source);
			gs_texrender_end(tr);
		}
		if (bench) {
			gs_stage_texture(stage, gs_texrender_get_texture(tr));
			uint8_t *d;
			uint32_t ls;
			if (gs_stagesurface_map(stage, &d, &ls))
				gs_stagesurface_unmap(stage);
		}
		obs_leave_graphics();
		if (bench && f == 10)
			t_start = os_gettime_ns(); /* skip warm-up frames */
		if (!bench)
			os_sleep_ms(40); /* the video thread ticks; analysis runs in the background */
	}
	if (bench && frames > 11)
		printf("{\"ms_per_frame\":%.2f}\n", (double)(os_gettime_ns() - t_start) / 1e6 / (frames - 11));

	obs_enter_graphics();
	gs_stage_texture(stage, gs_texrender_get_texture(tr));
	uint8_t *data;
	uint32_t linesize;
	bool ok = false;
	if (gs_stagesurface_map(stage, &data, &linesize)) {
		ok = stbi_write_png(out, (int)w, (int)h, 4, data, (int)linesize) != 0;
		gs_stagesurface_unmap(stage);
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);
	obs_leave_graphics();

	if (dump) {
		proc_handler_t *ph = obs_source_get_proc_handler(filter);
		calldata_t cd = {0};
		if (proc_handler_call(ph, "analysis_json", &cd))
			printf("%s\n", calldata_string(&cd, "json"));
		calldata_free(&cd);
	}
	obs_set_output_source(0, NULL);
	obs_source_release(filter);
	obs_source_release(source);
	obs_shutdown();
	return ok ? 0 : 7;
}
