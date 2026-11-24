/* Polish for OBS: the video filter.
 *
 * Each frame: capture the source, then run enabled passes on the GPU:
 *   smooth + brighten -> makeup -> face shape warp -> background -> grade (look, tone, vignette, grain) -> sticker
 * A small copy of the frame goes to the Vision analyzer in the background (face landmarks,
 * person mask); the passes use its latest result, so analysis never blocks rendering.
 * Original work; MIT licence. */
#include <obs-module.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "analyzer.h"

#define ANALYSIS_W 384

extern volatile bool polish_compare_held;
#define MASK_CAP (1024 * 1024)
#define MOUTH_CAP (ANALYSIS_W * 1024 * 2)

enum bg_mode { BG_OFF, BG_BLUR, BG_IMAGE, BG_COLOR };

/* grouped in the list by their labels: Portrait, Food, Landscape, Modern, then the basics */
static const char *LOOKS[] = {"none", "rosy", "fresh", "porcelain", "tasty", "lush", "sunset", "cinematic", "noir", "vintage",
			      "warm", "cool", "golden", "vivid", "pastel", "tealorange", "mono", "film", NULL};
static const char *BG_PRESETS[] = {"custom", "studio", "gradient", "bokeh", "sunset", NULL};
static const char *STICKERS[] = {"none", "sunglasses", "starglasses", "crown", "flowercrown", "catears", "bunnyears",
				 "devilhorns", "halo", "hearts", "partyhat", NULL};

struct polish {
	obs_source_t *self;
	gs_effect_t *fx;
	gs_texrender_t *t_src, *t_a, *t_b, *t_small, *t_blur1, *t_blur2;
	gs_stagesurf_t *stage;
	uint32_t stage_w, stage_h;
	bool stage_ready;
	void *analyzer;
	struct pa_result res;
	uint8_t *mask;
	gs_texture_t *mask_tex;
	uint64_t mask_seq_uploaded;
	int mask_w, mask_h;
	uint8_t *mouth;
	gs_texture_t *mouth_tex;
	uint64_t mouth_seq_uploaded;
	int mouth_w, mouth_h;
	uint64_t frame;

	float smooth, brighten, sharpen, slim, eyes;
	float lipstick, blush, teeth, eye_bright;
	uint32_t lip_color, blush_color;
	float vignette, grain;
	char bg_preset[32];
	char look[32];
	float look_amt, exposure, warmth, saturation;
	int bg_mode;
	float bg_blur, bg_edge;
	uint32_t bg_color;
	char *bg_path;
	char sticker[32];
	float sticker_size;
	bool compare;

	gs_image_file_ex_t lut_img, bg_img, sticker_img;
	bool lut_loaded, bg_loaded, sticker_loaded;
	char loaded_look[32], loaded_sticker[32];
	char *loaded_bg;
	bool reload;
};

/* ------------------------------------------------------------------ settings */

static float pct(obs_data_t *s, const char *k) { return (float)obs_data_get_int(s, k) / 100.0f; }

static void pf_update(void *data, obs_data_t *s)
{
	struct polish *p = data;
	p->smooth = pct(s, "smooth");
	p->brighten = pct(s, "brighten");
	p->sharpen = pct(s, "sharpen");
	p->slim = pct(s, "slim");
	p->eyes = pct(s, "eyes");
	p->lipstick = pct(s, "lipstick");
	p->lip_color = (uint32_t)obs_data_get_int(s, "lip_color");
	p->blush = pct(s, "blush");
	p->blush_color = (uint32_t)obs_data_get_int(s, "blush_color");
	p->teeth = pct(s, "teeth");
	p->eye_bright = pct(s, "eye_bright");
	p->vignette = pct(s, "vignette");
	p->grain = pct(s, "grain");
	snprintf(p->bg_preset, sizeof p->bg_preset, "%s", obs_data_get_string(s, "bg_preset"));
	snprintf(p->look, sizeof p->look, "%s", obs_data_get_string(s, "look"));
	p->look_amt = pct(s, "look_strength");
	p->exposure = pct(s, "exposure") * 0.5f;
	p->warmth = pct(s, "warmth");
	p->saturation = pct(s, "saturation");
	p->bg_mode = (int)obs_data_get_int(s, "bg_mode");
	p->bg_blur = pct(s, "bg_blur");
	p->bg_edge = pct(s, "bg_edge");
	p->bg_color = (uint32_t)obs_data_get_int(s, "bg_color");
	bfree(p->bg_path);
	p->bg_path = bstrdup(obs_data_get_string(s, "bg_image"));
	snprintf(p->sticker, sizeof p->sticker, "%s", obs_data_get_string(s, "sticker"));
	p->sticker_size = pct(s, "sticker_size");
	p->compare = obs_data_get_bool(s, "compare");
	p->reload = true;
}

static void pf_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "smooth", 45);
	obs_data_set_default_int(s, "brighten", 20);
	obs_data_set_default_int(s, "sharpen", 15);
	obs_data_set_default_int(s, "slim", 0);
	obs_data_set_default_int(s, "eyes", 0);
	obs_data_set_default_int(s, "lip_color", 0xFF5A3CC8);   /* ABGR: a rose red */
	obs_data_set_default_int(s, "blush_color", 0xFF8C78F0); /* ABGR: a soft pink */
	obs_data_set_default_string(s, "bg_preset", "studio");
	obs_data_set_default_string(s, "look", "none");
	obs_data_set_default_int(s, "look_strength", 80);
	obs_data_set_default_int(s, "bg_mode", BG_OFF);
	obs_data_set_default_int(s, "bg_blur", 60);
	obs_data_set_default_int(s, "bg_edge", 40);
	obs_data_set_default_int(s, "bg_color", 0xFF2A2A2A);
	obs_data_set_default_string(s, "sticker", "none");
	obs_data_set_default_int(s, "sticker_size", 100);
}

static bool bg_mode_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *s)
{
	int m = (int)obs_data_get_int(s, "bg_mode");
	obs_property_set_visible(obs_properties_get(props, "bg_blur"), m == BG_BLUR);
	bool custom = strcmp(obs_data_get_string(s, "bg_preset"), "custom") == 0;
	obs_property_set_visible(obs_properties_get(props, "bg_preset"), m == BG_IMAGE);
	obs_property_set_visible(obs_properties_get(props, "bg_image"), m == BG_IMAGE && custom);
	obs_property_set_visible(obs_properties_get(props, "bg_color"), m == BG_COLOR);
	obs_property_set_visible(obs_properties_get(props, "bg_edge"), m != BG_OFF);
	return true;
}

static bool reset_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	struct polish *p = data;
	obs_data_t *s = obs_source_get_settings(p->self);
	obs_data_clear(s);
	obs_source_update(p->self, s);
	obs_data_release(s);
	return true;
}

static void add_list(obs_properties_t *g, const char *key, const char *label, const char *prefix, const char **items)
{
	obs_property_t *l = obs_properties_add_list(g, key, obs_module_text(label), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (int i = 0; items[i]; i++) {
		struct dstr k = {0};
		dstr_printf(&k, "%s.%s", prefix, items[i]);
		obs_property_list_add_string(l, obs_module_text(k.array), items[i]);
		dstr_free(&k);
	}
}

static obs_properties_t *pf_properties(void *data)
{
	obs_properties_t *pr = obs_properties_create();
	obs_properties_add_bool(pr, "compare", obs_module_text("Compare"));

	obs_properties_t *g = obs_properties_create();
	obs_properties_add_int_slider(g, "smooth", obs_module_text("Smooth"), 0, 100, 1);
	obs_properties_add_int_slider(g, "brighten", obs_module_text("Brighten"), 0, 100, 1);
	obs_properties_add_int_slider(g, "sharpen", obs_module_text("Sharpen"), 0, 100, 1);
	obs_properties_add_group(pr, "g_beauty", obs_module_text("Beauty"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_int_slider(g, "slim", obs_module_text("Slim"), 0, 100, 1);
	obs_properties_add_int_slider(g, "eyes", obs_module_text("Eyes"), 0, 100, 1);
	obs_properties_add_group(pr, "g_shape", obs_module_text("Shape"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_int_slider(g, "lipstick", obs_module_text("Lipstick"), 0, 100, 1);
	obs_properties_add_color(g, "lip_color", obs_module_text("LipColor"));
	obs_properties_add_int_slider(g, "blush", obs_module_text("Blush"), 0, 100, 1);
	obs_properties_add_color(g, "blush_color", obs_module_text("BlushColor"));
	obs_properties_add_int_slider(g, "teeth", obs_module_text("Teeth"), 0, 100, 1);
	obs_properties_add_int_slider(g, "eye_bright", obs_module_text("EyeBright"), 0, 100, 1);
	obs_properties_add_group(pr, "g_makeup", obs_module_text("Makeup"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	add_list(g, "look", "Look", "Look", LOOKS);
	obs_properties_add_int_slider(g, "look_strength", obs_module_text("LookStrength"), 0, 100, 1);
	obs_properties_add_int_slider(g, "exposure", obs_module_text("Exposure"), -50, 50, 1);
	obs_properties_add_int_slider(g, "warmth", obs_module_text("Warmth"), -50, 50, 1);
	obs_properties_add_int_slider(g, "saturation", obs_module_text("Saturation"), -50, 50, 1);
	obs_properties_add_int_slider(g, "vignette", obs_module_text("Vignette"), 0, 100, 1);
	obs_properties_add_int_slider(g, "grain", obs_module_text("Grain"), 0, 100, 1);
	obs_properties_add_group(pr, "g_color", obs_module_text("Colour"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_property_t *m = obs_properties_add_list(g, "bg_mode", obs_module_text("Background"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(m, obs_module_text("Bg.Off"), BG_OFF);
	obs_property_list_add_int(m, obs_module_text("Bg.Blur"), BG_BLUR);
	obs_property_list_add_int(m, obs_module_text("Bg.Image"), BG_IMAGE);
	obs_property_list_add_int(m, obs_module_text("Bg.Color"), BG_COLOR);
	obs_property_set_modified_callback(m, bg_mode_changed);
	obs_properties_add_int_slider(g, "bg_blur", obs_module_text("BgBlur"), 0, 100, 1);
	obs_property_t *bp = obs_properties_add_list(g, "bg_preset", obs_module_text("BgPreset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (int i = 0; BG_PRESETS[i]; i++) {
		struct dstr k = {0};
		dstr_printf(&k, "BgPreset.%s", BG_PRESETS[i]);
		obs_property_list_add_string(bp, obs_module_text(k.array), BG_PRESETS[i]);
		dstr_free(&k);
	}
	obs_property_set_modified_callback(bp, bg_mode_changed);
	obs_properties_add_path(g, "bg_image", obs_module_text("BgImage"), OBS_PATH_FILE,
				"Images (*.png *.jpg *.jpeg *.bmp *.gif)", NULL);
	obs_properties_add_color(g, "bg_color", obs_module_text("BgColor"));
	obs_properties_add_int_slider(g, "bg_edge", obs_module_text("BgEdge"), 0, 100, 1);
	obs_properties_add_group(pr, "g_bg", obs_module_text("BackgroundGroup"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	add_list(g, "sticker", "Sticker", "Sticker", STICKERS);
	obs_properties_add_int_slider(g, "sticker_size", obs_module_text("StickerSize"), 50, 200, 1);
	obs_properties_add_group(pr, "g_sticker", obs_module_text("Stickers"), OBS_GROUP_NORMAL, g);

	obs_properties_add_button2(pr, "reset", obs_module_text("Reset"), reset_clicked, data);
	return pr;
}

/* ------------------------------------------------------------------ resources */

static void load_image(gs_image_file_ex_t *img, bool *loaded, const char *path)
{
	obs_enter_graphics();
	gs_image_file_ex_free(img);
	obs_leave_graphics();
	*loaded = false;
	if (!path || !*path)
		return;
	gs_image_file_ex_init(img, path, GS_IMAGE_ALPHA_PREMULTIPLY);
	obs_enter_graphics();
	gs_image_file_ex_init_texture(img);
	obs_leave_graphics();
	*loaded = img->texture != NULL;
	blog(LOG_DEBUG, "[obs-polish] loaded %s: %s (%ux%u)", path, *loaded ? "ok" : "FAILED", img->cx, img->cy);
	if (!*loaded)
		blog(LOG_WARNING, "[obs-polish] could not load %s", path);
}

static char *module_asset(const char *dir, const char *name, const char *ext)
{
	if (!strcmp(name, "none") || !*name)
		return NULL;
	struct dstr f = {0};
	dstr_printf(&f, "%s/%s.%s", dir, name, ext);
	char *path = obs_module_file(f.array);
	dstr_free(&f);
	return path;
}

static void reload_resources(struct polish *p)
{
	p->reload = false;
	if (strcmp(p->loaded_look, p->look) != 0) {
		char *path = module_asset("luts", p->look, "png");
		load_image(&p->lut_img, &p->lut_loaded, path);
		bfree(path);
		snprintf(p->loaded_look, sizeof p->loaded_look, "%s", p->look);
	}
	if (strcmp(p->loaded_sticker, p->sticker) != 0) {
		char *path = module_asset("stickers", p->sticker, "png");
		load_image(&p->sticker_img, &p->sticker_loaded, path);
		bfree(path);
		snprintf(p->loaded_sticker, sizeof p->loaded_sticker, "%s", p->sticker);
	}
	char *preset = NULL;
	if (p->bg_mode == BG_IMAGE && strcmp(p->bg_preset, "custom") != 0)
		preset = module_asset("backgrounds", p->bg_preset, "jpg");
	const char *bg = p->bg_mode != BG_IMAGE ? "" : preset ? preset : p->bg_path ? p->bg_path : "";
	if (!p->loaded_bg || strcmp(p->loaded_bg, bg) != 0) {
		load_image(&p->bg_img, &p->bg_loaded, bg);
		bfree(p->loaded_bg);
		p->loaded_bg = bstrdup(bg);
	}
	bfree(preset);
}

/* ------------------------------------------------------------------ lifecycle */

static const char *pf_name(void *u) { return obs_module_text("Polish"); }

static void analysis_json(void *data, calldata_t *cd);

static void *pf_create(obs_data_t *s, obs_source_t *src)
{
	struct polish *p = bzalloc(sizeof(*p));
	p->self = src;
	char *fx = obs_module_file("effects/polish.effect");
	char *err = NULL;
	obs_enter_graphics();
	p->fx = gs_effect_create_from_file(fx, &err);
	p->t_src = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	p->t_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	p->t_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	p->t_small = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	p->t_blur1 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	p->t_blur2 = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	if (!p->fx)
		blog(LOG_ERROR, "[obs-polish] effect failed to compile: %s", err ? err : "(no message)");
	bfree(err);
	bfree(fx);
	p->analyzer = pa_create();
	p->mask = bmalloc(MASK_CAP);
	p->mouth = bmalloc(MOUTH_CAP);
	proc_handler_add(obs_source_get_proc_handler(src), "void analysis_json(out string json)", analysis_json, p);
	pf_update(p, s);
	return p;
}

static void pf_destroy(void *data)
{
	struct polish *p = data;
	pa_destroy(p->analyzer);
	obs_enter_graphics();
	gs_effect_destroy(p->fx);
	gs_texrender_destroy(p->t_src);
	gs_texrender_destroy(p->t_a);
	gs_texrender_destroy(p->t_b);
	gs_texrender_destroy(p->t_small);
	gs_texrender_destroy(p->t_blur1);
	gs_texrender_destroy(p->t_blur2);
	gs_stagesurface_destroy(p->stage);
	gs_texture_destroy(p->mask_tex);
	gs_texture_destroy(p->mouth_tex);
	gs_image_file_ex_free(&p->lut_img);
	gs_image_file_ex_free(&p->bg_img);
	gs_image_file_ex_free(&p->sticker_img);
	obs_leave_graphics();
	bfree(p->mask);
	bfree(p->mouth);
	bfree(p->bg_path);
	bfree(p->loaded_bg);
	bfree(p);
}

/* ------------------------------------------------------------------ helpers */

static void draw_into(gs_texrender_t *tr, uint32_t w, uint32_t h, gs_effect_t *fx, const char *tech, gs_texture_t *in)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_set_texture(gs_effect_get_param_by_name(fx, "image"), in);
	while (gs_effect_loop(fx, tech))
		gs_draw_sprite(in, 0, w, h);
	gs_blend_state_pop();
	gs_texrender_end(tr);
}

static void set_f(gs_effect_t *fx, const char *n, float v) { gs_effect_set_float(gs_effect_get_param_by_name(fx, n), v); }
static void set_v2(gs_effect_t *fx, const char *n, float x, float y)
{
	struct vec2 v = {.x = x, .y = y};
	gs_effect_set_vec2(gs_effect_get_param_by_name(fx, n), &v);
}
static void set_v4(gs_effect_t *fx, const char *n, float x, float y, float z, float w)
{
	struct vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(gs_effect_get_param_by_name(fx, n), &v);
}

/* Read back the previous small frame (one frame late, so the GPU never stalls) and send it to Vision. */
static void analyse(struct polish *p, gs_texture_t *src, uint32_t w, uint32_t h)
{
	uint32_t sw = ANALYSIS_W, sh = (uint32_t)((float)ANALYSIS_W * h / w + 0.5f);
	if (sw > w) {
		sw = w;
		sh = h;
	}
	if (p->stage && (p->stage_w != sw || p->stage_h != sh)) {
		gs_stagesurface_destroy(p->stage);
		p->stage = NULL;
		p->stage_ready = false;
	}
	if (!p->stage) {
		p->stage = gs_stagesurface_create(sw, sh, GS_BGRA);
		p->stage_w = sw;
		p->stage_h = sh;
	}
	if (p->stage_ready) {
		uint8_t *data;
		uint32_t ls;
		if (gs_stagesurface_map(p->stage, &data, &ls)) {
			int want = (p->bg_mode != BG_OFF ? PA_WANT_PERSON : 0) |
				   (p->lipstick > 0 || p->teeth > 0 ? PA_WANT_MOUTH : 0);
			pa_submit(p->analyzer, data, (int)sw, (int)sh, (int)ls, want);
			gs_stagesurface_unmap(p->stage);
		}
	}
	draw_into(p->t_small, sw, sh, obs_get_base_effect(OBS_EFFECT_DEFAULT), "Draw", src);
	gs_stage_texture(p->stage, gs_texrender_get_texture(p->t_small));
	p->stage_ready = true;
}

static void upload_mask(struct polish *p)
{
	if (p->res.mask_w <= 0 || p->res.mask_seq == p->mask_seq_uploaded)
		return;
	if (!p->mask_tex || p->mask_w != p->res.mask_w || p->mask_h != p->res.mask_h) {
		gs_texture_destroy(p->mask_tex);
		p->mask_tex = gs_texture_create(p->res.mask_w, p->res.mask_h, GS_R8, 1, NULL, GS_DYNAMIC);
		p->mask_w = p->res.mask_w;
		p->mask_h = p->res.mask_h;
	}
	gs_texture_set_image(p->mask_tex, p->mask, p->mask_w, false);
	p->mask_seq_uploaded = p->res.mask_seq;
}

static void upload_mouth(struct polish *p)
{
	if (p->res.mouth_w <= 0 || p->res.mouth_seq == p->mouth_seq_uploaded)
		return;
	if (!p->mouth_tex || p->mouth_w != p->res.mouth_w || p->mouth_h != p->res.mouth_h) {
		gs_texture_destroy(p->mouth_tex);
		p->mouth_tex = gs_texture_create(p->res.mouth_w, p->res.mouth_h, GS_R8G8, 1, NULL, GS_DYNAMIC);
		p->mouth_w = p->res.mouth_w;
		p->mouth_h = p->res.mouth_h;
	}
	gs_texture_set_image(p->mouth_tex, p->mouth, p->mouth_w * 2, false);
	p->mouth_seq_uploaded = p->res.mouth_seq;
}

static void color_v4(gs_effect_t *fx, const char *n, uint32_t c, float a)
{
	set_v4(fx, n, (float)(c & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f, (float)((c >> 16) & 0xFF) / 255.0f, a);
}

/* Face ellipse for the beauty region: centred between eyes and chin, sized from the face box. */
static void face_ellipse(const struct pa_face *f, float out[4])
{
	float cx = (f->eye_l.x + f->eye_r.x) * 0.5f, ey = (f->eye_l.y + f->eye_r.y) * 0.5f;
	out[0] = cx;
	out[1] = ey + (f->chin.y - ey) * 0.20f;
	out[2] = f->w * 0.62f;
	out[3] = f->h * 0.68f;
}

#define SWAP_PASS()                    \
	do {                           \
		gs_texrender_t *t_ = ping; \
		ping = pong;           \
		pong = t_;             \
	} while (0)

/* ------------------------------------------------------------------ render */

static void pf_render(void *data, gs_effect_t *unused)
{
	struct polish *p = data;
	obs_source_t *target = obs_filter_get_target(p->self);
	uint32_t w = target ? obs_source_get_base_width(target) : 0, h = target ? obs_source_get_base_height(target) : 0;
	if (!p->fx || !w || !h) {
		obs_source_skip_video_filter(p->self);
		return;
	}
	if (p->reload)
		reload_resources(p);

	/* 1. capture the source */
	if (!obs_source_process_filter_begin(p->self, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;
	gs_texrender_reset(p->t_src);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin(p->t_src, w, h)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		obs_source_process_filter_end(p->self, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
		gs_texrender_end(p->t_src);
	}
	gs_blend_state_pop();
	gs_texture_t *src = gs_texrender_get_texture(p->t_src);
	gs_texture_t *cur = src;
	gs_effect_t *fx = p->fx;
	float fw = (float)w, fh = (float)h, aspect = fw / fh;

	/* 2. analysis */
	bool makeup = p->lipstick > 0 || p->blush > 0 || p->teeth > 0 || p->eye_bright > 0;
	bool need_faces = p->smooth > 0 || p->brighten > 0 || p->slim > 0 || p->eyes > 0 || makeup ||
			  strcmp(p->sticker, "none") != 0;
	p->frame++;
	if (need_faces || p->bg_mode != BG_OFF)
		analyse(p, src, w, h);
	pa_get(p->analyzer, &p->res, p->mask, MASK_CAP, p->mouth, MOUTH_CAP);
	upload_mask(p);
	upload_mouth(p);
	const struct pa_result *r = &p->res;

	gs_texrender_t *ping = p->t_a, *pong = p->t_b;
	bool compare = p->compare || polish_compare_held;
	if (compare)
		goto output;

	set_v2(fx, "size", fw, fh);
	set_v2(fx, "texel", 1.0f / fw, 1.0f / fh);

	/* 3. smooth + brighten */
	if (p->smooth > 0 || p->brighten > 0) {
		float e0[4] = {0}, e1[4] = {0};
		if (r->nfaces > 0)
			face_ellipse(&r->faces[0], e0);
		if (r->nfaces > 1)
			face_ellipse(&r->faces[1], e1);
		set_f(fx, "smooth_amt", p->smooth);
		set_f(fx, "smooth_radius", fmaxf(2.0f, fh * 0.0085f));
		set_f(fx, "brighten", p->brighten);
		set_f(fx, "face_count", (float)r->nfaces);
		set_v4(fx, "face0", e0[0], e0[1], e0[2], e0[3]);
		set_v4(fx, "face1", e1[0], e1[1], e1[2], e1[3]);
		set_f(fx, "face0_roll", r->nfaces > 0 ? r->faces[0].roll : 0.0f);
		set_f(fx, "face1_roll", r->nfaces > 1 ? r->faces[1].roll : 0.0f);
		/* until the first analysis arrives, smooth skin everywhere at a lower level */
		set_f(fx, "no_face_level", r->seq > 0 ? 0.0f : 0.6f);
		draw_into(ping, w, h, fx, "Smooth", cur);
		cur = gs_texrender_get_texture(ping);
		SWAP_PASS();
	}

	/* 3b. makeup: lips and teeth from the mouth mask, blush and eyes from the landmarks */
	if (makeup && r->nfaces > 0) {
		bool mouth = p->mouth_tex && r->mouth_w > 0;
		color_v4(fx, "lip_color", p->lip_color, mouth ? p->lipstick * 0.85f : 0.0f);
		set_f(fx, "teeth_amt", mouth ? p->teeth : 0.0f);
		if (mouth)
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "mouth_tex"), p->mouth_tex);
		else
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "mouth_tex"), cur); /* unused: amounts are 0 */
		color_v4(fx, "blush_color", p->blush_color, p->blush * 0.55f);
		const char *cheeks[2] = {"cheek0", "cheek1"}, *eyesb[2] = {"eyeb0", "eyeb1"};
		const struct pa_face *f = &r->faces[0];
		float facew = f->w * fw;
		for (int i = 0; i < 2; i++) {
			pa_pt eye = i ? f->eye_r : f->eye_l;
			pa_pt jaw = f->contour[i ? 13 : 3];
			float cx = (eye.x + (jaw.x - eye.x) * 0.42f) * fw;
			float cy = (eye.y + (f->nose.y - eye.y) * 0.9f) * fh;
			set_v4(fx, cheeks[i], cx, cy, facew * 0.15f, facew * 0.11f);
			float er = (i ? f->eye_rr : f->eye_rl) * fw * 1.7f;
			set_v4(fx, eyesb[i], eye.x * fw, eye.y * fh, fmaxf(er, 3.0f), p->eye_bright * 0.8f);
		}
		draw_into(ping, w, h, fx, "Makeup", cur);
		cur = gs_texrender_get_texture(ping);
		SWAP_PASS();
	}

	/* 4. face shape (largest face) */
	if ((p->slim > 0 || p->eyes > 0) && r->nfaces > 0) {
		const struct pa_face *f = &r->faces[0];
		float cx = (f->eye_l.x + f->eye_r.x) * 0.5f * fw, cy = f->nose.y * fh;
		float facew = f->w * fw;
		const int idx[6] = {2, 4, 6, 10, 12, 14}; /* jaw points, cheek to cheek, chin excluded */
		const char *names[6] = {"ctl0", "ctl1", "ctl2", "ctl3", "ctl4", "ctl5"};
		for (int i = 0; i < 6; i++) {
			float px = f->contour[idx[i]].x * fw, py = f->contour[idx[i]].y * fh;
			float dx = cx - px, dy = (cy - py) * 0.2f; /* mostly sideways: narrow the jaw, don't lift it */
			float len = sqrtf(dx * dx + dy * dy) + 1e-3f;
			float amt = p->slim * facew * 0.05f;
			set_v4(fx, names[i], px, py, dx / len * amt, dy / len * amt);
		}
		set_f(fx, "ctl_radius", facew * 0.32f);
		float s = p->eyes * 0.28f;
		set_v4(fx, "eye0", f->eye_l.x * fw, f->eye_l.y * fh, fmaxf(f->eye_rl * fw * 2.4f, 4.0f), s);
		set_v4(fx, "eye1", f->eye_r.x * fw, f->eye_r.y * fh, fmaxf(f->eye_rr * fw * 2.4f, 4.0f), s);
		draw_into(ping, w, h, fx, "Warp", cur);
		cur = gs_texrender_get_texture(ping);
		SWAP_PASS();
	}

	/* 5. background */
	if (p->bg_mode != BG_OFF && p->mask_tex) {
		gs_texture_t *bg = NULL;
		if (p->bg_mode == BG_BLUR) {
			uint32_t bw = w / 4 ? w / 4 : 1, bh = h / 4 ? h / 4 : 1;
			draw_into(p->t_blur1, bw, bh, obs_get_base_effect(OBS_EFFECT_DEFAULT), "Draw", cur);
			gs_texture_t *b = gs_texrender_get_texture(p->t_blur1);
			int iters = 1 + (int)(p->bg_blur * 3.0f);
			float spread = 0.6f + p->bg_blur * 1.6f;
			for (int i = 0; i < iters; i++) {
				set_v2(fx, "blur_step", spread / (float)bw, 0.0f);
				draw_into(p->t_blur2, bw, bh, fx, "Blur", b);
				set_v2(fx, "blur_step", 0.0f, spread / (float)bh);
				draw_into(p->t_blur1, bw, bh, fx, "Blur", gs_texrender_get_texture(p->t_blur2));
				b = gs_texrender_get_texture(p->t_blur1);
			}
			bg = b;
		} else if (p->bg_mode == BG_IMAGE && p->bg_loaded) {
			bg = p->bg_img.texture;
			float ia = (float)p->bg_img.cx / (float)p->bg_img.cy;
			float sx = 1.0f, sy = 1.0f;
			if (ia > aspect)
				sx = aspect / ia;
			else
				sy = ia / aspect;
			set_v4(fx, "bg_uv", sx, sy, (1.0f - sx) * 0.5f, (1.0f - sy) * 0.5f);
		}
		if (p->bg_mode == BG_COLOR || bg) {
			uint32_t c = p->bg_color;
			set_f(fx, "bg_mode", (float)p->bg_mode);
			set_v4(fx, "bg_color", (float)(c & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f,
			       (float)((c >> 16) & 0xFF) / 255.0f, 1.0f);
			float soft = 0.05f + p->bg_edge * 0.45f;
			set_f(fx, "edge_lo", 0.5f - soft * 0.5f);
			set_f(fx, "edge_hi", 0.5f + soft * 0.5f);
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "mask_tex"), p->mask_tex);
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "bg_tex"), bg ? bg : cur);
			draw_into(ping, w, h, fx, "Composite", cur);
			cur = gs_texrender_get_texture(ping);
			SWAP_PASS();
		}
	}

	/* 6. grade */
	bool look = p->lut_loaded && p->look_amt > 0;
	if (look || p->exposure != 0 || p->warmth != 0 || p->saturation != 0 || p->sharpen > 0 || p->vignette > 0 ||
	    p->grain > 0) {
		set_f(fx, "vignette", p->vignette);
		set_f(fx, "grain", p->grain);
		set_f(fx, "grain_seed", (float)(p->frame % 997));
		set_f(fx, "lut_amt", look ? p->look_amt : 0.0f);
		if (look)
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "lut_tex"), p->lut_img.texture);
		set_f(fx, "exposure", p->exposure);
		set_f(fx, "warmth", p->warmth);
		set_f(fx, "saturation", p->saturation);
		set_f(fx, "sharpen", p->sharpen);
		draw_into(ping, w, h, fx, "Grade", cur);
		cur = gs_texrender_get_texture(ping);
		SWAP_PASS();
	}

output:;
	/* 7. output, then the sticker on top */
	gs_texture_t *out = compare ? src : cur;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), out);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(out, 0, w, h);
	gs_blend_state_pop();

	if (!compare && p->sticker_loaded && r->nfaces > 0) {
		const struct pa_face *f = &r->faces[0];
		float ex = (f->eye_r.x - f->eye_l.x) * fw, ey = (f->eye_r.y - f->eye_l.y) * fh;
		float eyedist = sqrtf(ex * ex + ey * ey);
		float ax = (f->eye_l.x + f->eye_r.x) * 0.5f * fw, ay = (f->eye_l.y + f->eye_r.y) * 0.5f * fh;
		float width = eyedist * 2.4f, oy = 0.0f; /* glasses sit on the eyes */
		if (!strcmp(p->sticker, "crown") || !strcmp(p->sticker, "partyhat")) {
			width = eyedist * 2.6f;
			oy = -eyedist * 1.75f;
		} else if (!strcmp(p->sticker, "halo")) {
			width = eyedist * 2.6f;
			oy = -eyedist * 2.05f;
		} else if (!strcmp(p->sticker, "flowercrown")) {
			width = eyedist * 3.3f;
			oy = -eyedist * 1.8f;
		} else if (!strcmp(p->sticker, "devilhorns")) {
			width = eyedist * 3.0f;
			oy = -eyedist * 1.75f;
		} else if (!strcmp(p->sticker, "bunnyears")) {
			width = eyedist * 2.6f;
			oy = -eyedist * 2.0f;
		} else if (!strcmp(p->sticker, "catears")) {
			width = eyedist * 3.4f;
			oy = -eyedist * 1.55f;
		} else if (!strcmp(p->sticker, "hearts")) {
			width = eyedist * 4.4f;
			oy = -eyedist * 1.45f;
		}
		width *= p->sticker_size;
		float height = width * (float)p->sticker_img.cy / (float)p->sticker_img.cx;
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
		gs_matrix_push();
		gs_matrix_translate3f(ax, ay, 0.0f);
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, f->roll);
		gs_matrix_translate3f(-width * 0.5f, oy * p->sticker_size - height * 0.5f, 0.0f);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), p->sticker_img.texture);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(p->sticker_img.texture, 0, (uint32_t)width, (uint32_t)height);
		gs_matrix_pop();
		gs_blend_state_pop();
	}
}

/* ------------------------------------------------------------------ status for tests */

static void analysis_json(void *data, calldata_t *cd)
{
	struct polish *p = data;
	struct pa_result r = p->res;
	struct dstr s = {0};
	dstr_printf(&s, "{\"seq\":%llu,\"faces\":%d,\"face_ms\":%.1f,\"mask_ms\":%.1f,\"mask\":[%d,%d],\"mouth\":[%d,%d],\"face\":[",
		    (unsigned long long)r.seq, r.nfaces, r.face_ms, r.mask_ms, r.mask_w, r.mask_h, r.mouth_w, r.mouth_h);
	for (int i = 0; i < r.nfaces; i++) {
		const struct pa_face *f = &r.faces[i];
		dstr_catf(&s,
			  "%s{\"box\":[%.4f,%.4f,%.4f,%.4f],\"eye_l\":[%.4f,%.4f],\"eye_r\":[%.4f,%.4f],\"chin\":[%.4f,%.4f],\"roll\":%.4f}",
			  i ? "," : "", f->x, f->y, f->w, f->h, f->eye_l.x, f->eye_l.y, f->eye_r.x, f->eye_r.y, f->chin.x,
			  f->chin.y, f->roll);
	}
	dstr_cat(&s, "],\"contour\":[");
	for (int i = 0; r.nfaces > 0 && i < r.faces[0].ncontour; i++)
		dstr_catf(&s, "%s[%.4f,%.4f]", i ? "," : "", r.faces[0].contour[i].x, r.faces[0].contour[i].y);
	dstr_cat(&s, "]}");
	calldata_set_string(cd, "json", s.array);
	dstr_free(&s);
}

struct obs_source_info polish_filter = {
	.id = "polish_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = pf_name,
	.create = pf_create,
	.destroy = pf_destroy,
	.update = pf_update,
	.get_defaults = pf_defaults,
	.get_properties = pf_properties,
	.video_render = pf_render,
};
