/* Headless render test: runs an image through the Polish filter inside a real libobs
 * (the one in /Applications/OBS.app) and writes the result as a PNG.
 *
 *   render-test <plugin.plugin> <input image> <output.png> ['{"json":"settings"}'] [frames] [--json]
 *
 * With --json the filter's analysis state (faces found, landmark positions) is printed,
 * so tests can check face tracking as well as pixels. */
#import <AppKit/AppKit.h>
#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <stdio.h>
#include <unistd.h>
#include <util/platform.h>

#define OBS_APP "/Applications/OBS.app/Contents"

static bool load_module(const char *bundle, const char *name)
{
	char bin[1024], data[1024];
	snprintf(bin, sizeof bin, "%s/Contents/MacOS/%s", bundle, name);
	snprintf(data, sizeof data, "%s/Contents/Resources", bundle);
	obs_module_t *mod = NULL;
	int r = obs_open_module(&mod, bin, data);
	if (r != MODULE_SUCCESS) {
		fprintf(stderr, "open %s failed: %d\n", bin, r);
		return false;
	}
	return obs_init_module(mod);
}

static bool save_png(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t linesize, const char *path)
{
	NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
		bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
		bytesPerRow:w * 4 bitsPerPixel:32];
	for (uint32_t y = 0; y < h; y++)
		memcpy(rep.bitmapData + y * w * 4, rgba + y * linesize, w * 4);
	NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
	return [png writeToFile:@(path) atomically:YES];
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 4) {
			fprintf(stderr, "usage: render-test <plugin.plugin> <in> <out.png> [settings json] [frames] [--json]\n");
			return 2;
		}
		const char *plugin = argv[1], *in = argv[2], *out = argv[3];
		const char *json = argc > 4 ? argv[4] : "{}";
		int frames = argc > 5 ? atoi(argv[5]) : 30;
		bool dump = argc > 6 && strcmp(argv[6], "--json") == 0;

		NSImage *img = [[NSImage alloc] initWithContentsOfFile:@(in)];
		NSBitmapImageRep *src = (NSBitmapImageRep *)[NSBitmapImageRep imageRepWithData:img.TIFFRepresentation];
		uint32_t w = (uint32_t)src.pixelsWide, h = (uint32_t)src.pixelsHigh;

		if (!obs_startup("en-US", NULL, NULL))
			return 3;
		obs_add_data_path(OBS_APP "/Frameworks/libobs.framework/Resources/");
		struct obs_video_info ovi = {
			.graphics_module = OBS_APP "/Frameworks/libobs-opengl.dylib",
			.fps_num = 30, .fps_den = 1,
			.base_width = w, .base_height = h, .output_width = w, .output_height = h,
			.output_format = VIDEO_FORMAT_NV12, .adapter = 0, .gpu_conversion = true,
			.colorspace = VIDEO_CS_709, .range = VIDEO_RANGE_PARTIAL, .scale_type = OBS_SCALE_BICUBIC,
		};
		int vr = obs_reset_video(&ovi);
		if (vr != OBS_VIDEO_SUCCESS) {
			fprintf(stderr, "obs_reset_video failed: %d\n", vr);
			return 4;
		}
		if (!load_module(OBS_APP "/PlugIns/image-source.plugin", "image-source") || !load_module(plugin, "obs-polish"))
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
		obs_set_output_source(0, source);           /* makes the source active, so it ticks and renders */

		/* POLISH_BENCH=1: no sleep, and wait for the GPU every frame, so the time per frame is real */
		bool bench = getenv("POLISH_BENCH") != NULL;
		uint64_t t_start = 0;
		gs_texrender_t *tr = NULL;
		gs_stagesurf_t *stage = NULL;
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
				t_start = os_gettime_ns();      /* skip warm-up frames */
			if (!bench)
				usleep(40000);                  /* the video thread ticks; analysis runs in the background */
		}
		if (bench && frames > 10)
			printf("{\"ms_per_frame\":%.2f}\n", (os_gettime_ns() - t_start) / 1e6 / (frames - 11));
		obs_enter_graphics();
		gs_stage_texture(stage, gs_texrender_get_texture(tr));
		uint8_t *data;
		uint32_t linesize;
		bool ok = false;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			ok = save_png(data, w, h, linesize, out);
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
}
