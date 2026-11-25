/* Polish for OBS: the video filter.
 *
 * Each frame: capture the source, then run enabled passes on the GPU:
 *   smooth, brighten, foundation -> makeup (lips, teeth, blush, eyes, brows) -> face shape warp
 *   -> background -> soft light -> grade (look, tone, vignette, grain) -> sticker
 * A small copy of the frame goes to the Vision analyzer in the background (face landmarks,
 * person mask); the passes use its latest result, so analysis never blocks rendering.
 * Original work; MIT licence. */
#include <obs-module.h>
#include <graphics/matrix4.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "analyzer.h"
#include "image-load.h"

#define ANALYSIS_W 384

extern volatile bool polish_compare_held;
#define MASK_CAP (1024 * 1024)
#define BLUR_LEVELS 6
#define MOUTH_CAP (ANALYSIS_W * 1024 * 2)
#define EYES_CAP (ANALYSIS_W * 1024 * 4)

enum bg_mode { BG_OFF, BG_BLUR, BG_IMAGE, BG_COLOR };

/* grouped in the list by their labels: Portrait, Food, Landscape, Modern, then the basics */
static const char *LOOKS[] = {"none", "rosy", "fresh", "porcelain", "tasty", "lush", "sunset", "cinematic", "noir", "vintage",
			      "warm", "cool", "golden", "vivid", "pastel", "tealorange", "mono", "film", NULL};
/* Effects. id matches style_id in polish.effect; param is the label of the effect's own slider
 * (NULL: none); c1, c2 label its colours (NULL: none); blur > 0 asks for a blurred copy of the frame,
 * halved that many times, as blur_tex (-1: as many times as the slider says). */
struct style {
	const char *key;
	int id;
	const char *param, *c1, *c2;
	int blur;
};
static const struct style STYLES[] = {
	{"none", 0, NULL, NULL, NULL, 0},
	/* colour and tone */
	{"grayscale", 1, NULL, NULL, NULL, 0},
	{"sepia", 2, NULL, NULL, NULL, 0},
	{"tealorange", 3, NULL, NULL, NULL, 0},
	{"invert", 4, NULL, NULL, NULL, 0},
	{"tint", 5, NULL, "StyleTint", NULL, 0},
	{"duotone", 6, NULL, "StyleShadows", "StyleHighlights", 0},
	{"bleach", 7, NULL, NULL, NULL, 0},
	{"crossprocess", 8, NULL, NULL, NULL, 0},
	{"golden", 9, NULL, NULL, NULL, 0},
	{"moonlight", 10, NULL, NULL, NULL, 0},
	{"neon", 11, NULL, NULL, NULL, 0},
	{"colorpass", 12, "StyleRange", "StyleKeep", NULL, 0},
	/* light */
	{"hdr", 15, "StyleDetail", NULL, NULL, 5},
	{"bloom", 16, "StyleSpread", NULL, NULL, 4},
	{"solarize", 17, "StyleLevel", NULL, NULL, 0},
	{"posterize", 18, "StyleLevels", NULL, NULL, 0},
	{"threshold", 19, "StyleLevel", NULL, NULL, 0},
	{"lightleak", 20, "StyleSpeed", NULL, NULL, 0},
	/* blur and sharpen */
	{"blur", 21, "StyleSize", NULL, NULL, -1},
	{"motionblur", 22, "StyleAngle", NULL, NULL, 0},
	{"zoomblur", 23, "StyleReach", NULL, NULL, 0},
	{"tiltshift", 24, "StyleFocus", NULL, NULL, 4},
	{"boxblur", 25, "StyleSize", NULL, NULL, 0},
	{"lensblur", 26, "StyleSize", NULL, NULL, 0},
	{"unsharp", 27, "StyleBoost", NULL, NULL, 2},
	{"denoise", 28, "StyleRadius", NULL, NULL, 0},
	/* retro, analog and texture */
	{"filmgrain", 29, "StyleSize", NULL, NULL, 0},
	{"vhs", 30, "StyleSoftness", NULL, NULL, 0},
	{"crt", 31, "StyleLineSize", NULL, NULL, 0},
	{"super8", 32, "StyleGrain", NULL, NULL, 0},
	{"oldfilm", 33, "StyleDamage", NULL, NULL, 0},
	{"halftone", 34, "StyleSize", NULL, NULL, 0},
	{"rgbsplit", 35, "StyleDistance", NULL, NULL, 0},
	{"dither", 36, "StyleLevels", NULL, NULL, 0},
	{"lomo", 37, "StyleVignette", NULL, NULL, 0},
	/* distortion and stylisation */
	{"glitch", 38, "StyleSplit", NULL, NULL, 0},
	{"pixelate", 39, "StyleSize", NULL, NULL, 0},
	{"fisheye", 40, "StyleCurve", NULL, NULL, 0},
	{"mirror", 41, "StyleSlices", NULL, NULL, 0},
	{"wave", 42, "StyleWavelength", NULL, NULL, 0},
	{"twirl", 43, "StyleTurns", NULL, NULL, 0},
	{"emboss", 44, "StyleDepth", NULL, NULL, 0},
	{"edges", 45, "StyleLineWidth", "StyleLineColor", NULL, 0},
	{"cartoon", 46, "StyleOutlines", NULL, NULL, 1},
	{"oilpaint", 47, "StyleSize", NULL, NULL, 0},
	{"thermal", 48, "StyleSensitivity", NULL, NULL, 2},
	{"nightvision", 49, "StyleGain", NULL, NULL, 0},
	{"echo", 50, "StyleTrail", NULL, NULL, 0},
};
#define NSTYLES ((int)(sizeof(STYLES) / sizeof(STYLES[0])))
#define STYLE_ECHO 50

static const struct style *find_style(const char *key)
{
	for (int i = 0; i < NSTYLES; i++)
		if (!strcmp(STYLES[i].key, key))
			return &STYLES[i];
	return &STYLES[0];
}

static const char *BG_PRESETS[] = {"custom", "studio", "gradient", "bokeh", "sunset", NULL};
static const char *STICKERS[] = {"none", "sunglasses", "starglasses", "crown", "flowercrown", "catears", "bunnyears",
				 "devilhorns", "halo", "hearts", "partyhat", NULL};

struct polish {
	obs_source_t *self;
	gs_effect_t *fx;
	gs_texrender_t *t_src, *t_a, *t_b, *t_small;
	gs_texrender_t *t_down[BLUR_LEVELS], *t_up[BLUR_LEVELS];
	gs_texrender_t *t_hist; /* the previous output, for echo */
	bool hist_valid;
	uint32_t hist_w, hist_h;
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
	uint8_t *eyes_buf;
	gs_texture_t *eyes_tex;
	uint64_t eyes_seq_uploaded;
	int eyes_w, eyes_h;
	uint64_t frame;

	float smooth, brighten, sharpen, slim, eyes;
	float foundation, soft_light, vline, chin, nose_slim, lip_fill;
	float lipstick, blush, teeth, eye_bright;
	float eyeshadow, eyeliner, brows, eye_color;
	uint32_t lip_color, blush_color, foundation_color, shadow_color, liner_color, brow_color, iris_color;
	float vignette, grain, contrast;
	const struct style *style;
	float style_amt, style_k;
	uint32_t style_c1, style_c2;
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

	struct polish_image lut_img, bg_img, sticker_img;
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
	p->foundation = pct(s, "foundation");
	p->foundation_color = (uint32_t)obs_data_get_int(s, "foundation_color");
	p->soft_light = pct(s, "soft_light");
	p->vline = pct(s, "vline");
	p->chin = pct(s, "chin");
	p->nose_slim = pct(s, "nose_slim");
	p->lip_fill = pct(s, "lip_fill");
	p->eyeshadow = pct(s, "eyeshadow");
	p->shadow_color = (uint32_t)obs_data_get_int(s, "eyeshadow_color");
	p->eyeliner = pct(s, "eyeliner");
	p->liner_color = (uint32_t)obs_data_get_int(s, "eyeliner_color");
	p->brows = pct(s, "brows");
	p->brow_color = (uint32_t)obs_data_get_int(s, "brow_color");
	p->eye_color = pct(s, "eye_color");
	p->iris_color = (uint32_t)obs_data_get_int(s, "iris_color");
	p->vignette = pct(s, "vignette");
	p->contrast = pct(s, "contrast");
	const struct style *st = find_style(obs_data_get_string(s, "style"));
	if (st != p->style)
		p->hist_valid = false; /* a new effect starts without old trails */
	p->style = st;
	p->style_amt = pct(s, "style_amount");
	p->style_k = pct(s, "style_param");
	p->style_c1 = (uint32_t)obs_data_get_int(s, "style_color");
	p->style_c2 = (uint32_t)obs_data_get_int(s, "style_color2");
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
	obs_data_set_default_int(s, "foundation_color", 0xFFA0BEE6); /* ABGR: a warm beige */
	obs_data_set_default_int(s, "eyeshadow_color", 0xFF6E5F96);  /* ABGR: a mauve brown */
	obs_data_set_default_int(s, "eyeliner_color", 0xFF141419);   /* ABGR: soft black */
	obs_data_set_default_int(s, "brow_color", 0xFF283246);       /* ABGR: dark brown */
	obs_data_set_default_int(s, "iris_color", 0xFFAA8246);       /* ABGR: blue-green */
	obs_data_set_default_string(s, "bg_preset", "studio");
	obs_data_set_default_string(s, "look", "none");
	obs_data_set_default_int(s, "look_strength", 80);
	obs_data_set_default_int(s, "bg_mode", BG_OFF);
	obs_data_set_default_int(s, "bg_blur", 60);
	obs_data_set_default_int(s, "bg_edge", 40);
	obs_data_set_default_int(s, "bg_color", 0xFF2A2A2A);
	obs_data_set_default_string(s, "sticker", "none");
	obs_data_set_default_int(s, "sticker_size", 100);
	obs_data_set_default_string(s, "style", "none");
	obs_data_set_default_int(s, "style_amount", 100);
	obs_data_set_default_int(s, "style_param", 50);
	obs_data_set_default_int(s, "style_color", 0xFF3C28E6);  /* ABGR: a strong red */
	obs_data_set_default_int(s, "style_color2", 0xFF78D2FF); /* ABGR: a warm yellow */
}

/* Show the effect's own controls, labelled for what they do in that effect. */
static bool style_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *s)
{
	const struct style *st = find_style(obs_data_get_string(s, "style"));
	bool on = st->id != 0;
	obs_property_set_visible(obs_properties_get(props, "style_amount"), on);
	obs_property_t *k = obs_properties_get(props, "style_param");
	obs_property_set_visible(k, st->param != NULL);
	if (st->param)
		obs_property_set_description(k, obs_module_text(st->param));
	obs_property_t *c1 = obs_properties_get(props, "style_color"), *c2 = obs_properties_get(props, "style_color2");
	obs_property_set_visible(c1, st->c1 != NULL);
	obs_property_set_visible(c2, st->c2 != NULL);
	if (st->c1)
		obs_property_set_description(c1, obs_module_text(st->c1));
	if (st->c2)
		obs_property_set_description(c2, obs_module_text(st->c2));
	return true;
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
	obs_properties_add_int_slider(g, "foundation", obs_module_text("Foundation"), 0, 100, 1);
	obs_properties_add_color(g, "foundation_color", obs_module_text("FoundationColor"));
	obs_properties_add_int_slider(g, "soft_light", obs_module_text("SoftLight"), 0, 100, 1);
	obs_properties_add_group(pr, "g_beauty", obs_module_text("Beauty"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_int_slider(g, "slim", obs_module_text("Slim"), 0, 100, 1);
	obs_properties_add_int_slider(g, "vline", obs_module_text("VLine"), 0, 100, 1);
	obs_properties_add_int_slider(g, "chin", obs_module_text("Chin"), -50, 50, 1);
	obs_properties_add_int_slider(g, "nose_slim", obs_module_text("NoseSlim"), 0, 100, 1);
	obs_properties_add_int_slider(g, "eyes", obs_module_text("Eyes"), 0, 100, 1);
	obs_properties_add_int_slider(g, "lip_fill", obs_module_text("LipFill"), 0, 100, 1);
	obs_properties_add_group(pr, "g_shape", obs_module_text("Shape"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	obs_properties_add_int_slider(g, "lipstick", obs_module_text("Lipstick"), 0, 100, 1);
	obs_properties_add_color(g, "lip_color", obs_module_text("LipColor"));
	obs_properties_add_int_slider(g, "blush", obs_module_text("Blush"), 0, 100, 1);
	obs_properties_add_color(g, "blush_color", obs_module_text("BlushColor"));
	obs_properties_add_int_slider(g, "eyeshadow", obs_module_text("Eyeshadow"), 0, 100, 1);
	obs_properties_add_color(g, "eyeshadow_color", obs_module_text("EyeshadowColor"));
	obs_properties_add_int_slider(g, "eyeliner", obs_module_text("Eyeliner"), 0, 100, 1);
	obs_properties_add_color(g, "eyeliner_color", obs_module_text("EyelinerColor"));
	obs_properties_add_int_slider(g, "brows", obs_module_text("Brows"), 0, 100, 1);
	obs_properties_add_color(g, "brow_color", obs_module_text("BrowColor"));
	obs_properties_add_int_slider(g, "eye_color", obs_module_text("EyeColor"), 0, 100, 1);
	obs_properties_add_color(g, "iris_color", obs_module_text("IrisColor"));
	obs_properties_add_int_slider(g, "teeth", obs_module_text("Teeth"), 0, 100, 1);
	obs_properties_add_int_slider(g, "eye_bright", obs_module_text("EyeBright"), 0, 100, 1);
	obs_properties_add_group(pr, "g_makeup", obs_module_text("Makeup"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	add_list(g, "look", "Look", "Look", LOOKS);
	obs_properties_add_int_slider(g, "look_strength", obs_module_text("LookStrength"), 0, 100, 1);
	obs_properties_add_int_slider(g, "exposure", obs_module_text("Exposure"), -50, 50, 1);
	obs_properties_add_int_slider(g, "warmth", obs_module_text("Warmth"), -50, 50, 1);
	obs_properties_add_int_slider(g, "saturation", obs_module_text("Saturation"), -50, 50, 1);
	obs_properties_add_int_slider(g, "contrast", obs_module_text("Contrast"), -50, 50, 1);
	obs_properties_add_int_slider(g, "vignette", obs_module_text("Vignette"), -100, 100, 1);
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

	g = obs_properties_create();
	obs_property_t *sl = obs_properties_add_list(g, "style", obs_module_text("Style"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (int i = 0; i < NSTYLES; i++) {
		struct dstr k = {0};
		dstr_printf(&k, "Style.%s", STYLES[i].key);
		obs_property_list_add_string(sl, obs_module_text(k.array), STYLES[i].key);
		dstr_free(&k);
	}
	obs_property_set_modified_callback(sl, style_changed);
	obs_properties_add_int_slider(g, "style_amount", obs_module_text("StyleStrength"), 0, 100, 1);
	obs_properties_add_int_slider(g, "style_param", obs_module_text("StyleSize"), 0, 100, 1);
	obs_properties_add_color(g, "style_color", obs_module_text("StyleTint"));
	obs_properties_add_color(g, "style_color2", obs_module_text("StyleHighlights"));
	obs_properties_add_group(pr, "g_style", obs_module_text("Effects"), OBS_GROUP_NORMAL, g);

	const char *err = data ? pa_error(((struct polish *)data)->analyzer) : NULL;
	if (err) {
		obs_property_t *warn = obs_properties_add_text(pr, "analysis_error", err, OBS_TEXT_INFO);
		obs_property_text_set_info_type(warn, OBS_TEXT_INFO_ERROR);
	}
	obs_properties_add_button2(pr, "reset", obs_module_text("Reset"), reset_clicked, data);
	return pr;
}

/* ------------------------------------------------------------------ resources */

static void load_image(struct polish_image *img, bool *loaded, const char *path)
{
	*loaded = polish_image_load(img, path);
	if (*loaded)
		blog(LOG_DEBUG, "[obs-polish] loaded %s (%ux%u)", path, img->cx, img->cy);
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

/* Direct3D 11 in OBS refuses to draw when a shader uses a value that was never set (OpenGL does
 * not mind), and not every pass sets every value in every mode. Give them all zero to start. */
static void init_params(gs_effect_t *fx)
{
	size_t n = gs_effect_get_num_params(fx);
	for (size_t i = 0; i < n; i++) {
		gs_eparam_t *prm = gs_effect_get_param_by_idx(fx, i);
		struct gs_effect_param_info info;
		gs_effect_get_param_info(prm, &info);
		const float zero[16] = {0};
		switch (info.type) {
		case GS_SHADER_PARAM_FLOAT: gs_effect_set_float(prm, 0.0f); break;
		case GS_SHADER_PARAM_INT: gs_effect_set_int(prm, 0); break;
		case GS_SHADER_PARAM_BOOL: gs_effect_set_bool(prm, false); break;
		case GS_SHADER_PARAM_VEC2: gs_effect_set_val(prm, zero, sizeof(float) * 2); break;
		case GS_SHADER_PARAM_VEC3: gs_effect_set_val(prm, zero, sizeof(float) * 3); break;
		case GS_SHADER_PARAM_VEC4: gs_effect_set_val(prm, zero, sizeof(float) * 4); break;
		case GS_SHADER_PARAM_MATRIX4X4: gs_effect_set_val(prm, zero, sizeof(float) * 16); break;
		default: break; /* textures: an unbound texture reads as black, which D3D accepts */
		}
	}
}

static void analysis_json(void *data, calldata_t *cd);

static void *pf_create(obs_data_t *s, obs_source_t *src)
{
	struct polish *p = bzalloc(sizeof(*p));
	p->self = src;
	char *fx = obs_module_file("effects/polish.effect");
	char *err = NULL;
	obs_enter_graphics();
	p->fx = gs_effect_create_from_file(fx, &err);
	if (p->fx)
		init_params(p->fx);
	p->t_src = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	p->t_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	p->t_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	p->t_small = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	p->t_hist = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (int i = 0; i < BLUR_LEVELS; i++) {
		p->t_down[i] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		p->t_up[i] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	}
	obs_leave_graphics();
	if (!p->fx)
		blog(LOG_ERROR, "[obs-polish] effect failed to compile: %s", err ? err : "(no message)");
	bfree(err);
	bfree(fx);
	p->analyzer = pa_create();
	p->mask = bmalloc(MASK_CAP);
	p->mouth = bmalloc(MOUTH_CAP);
	p->eyes_buf = bmalloc(EYES_CAP);
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
	gs_texrender_destroy(p->t_hist);
	for (int i = 0; i < BLUR_LEVELS; i++) {
		gs_texrender_destroy(p->t_down[i]);
		gs_texrender_destroy(p->t_up[i]);
	}
	gs_stagesurface_destroy(p->stage);
	gs_texture_destroy(p->mask_tex);
	gs_texture_destroy(p->mouth_tex);
	gs_texture_destroy(p->eyes_tex);
	obs_leave_graphics();
	polish_image_free(&p->lut_img);
	polish_image_free(&p->bg_img);
	polish_image_free(&p->sticker_img);
	bfree(p->mask);
	bfree(p->mouth);
	bfree(p->eyes_buf);
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
				   (p->lipstick > 0 || p->teeth > 0 ? PA_WANT_MOUTH : 0) |
				   (p->eyeshadow > 0 || p->eyeliner > 0 || p->brows > 0 || p->eye_color > 0 ? PA_WANT_EYES : 0);
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

static void upload_eyes(struct polish *p)
{
	if (p->res.eyes_w <= 0 || p->res.eyes_seq == p->eyes_seq_uploaded)
		return;
	if (!p->eyes_tex || p->eyes_w != p->res.eyes_w || p->eyes_h != p->res.eyes_h) {
		gs_texture_destroy(p->eyes_tex);
		p->eyes_tex = gs_texture_create(p->res.eyes_w, p->res.eyes_h, GS_RGBA, 1, NULL, GS_DYNAMIC);
		p->eyes_w = p->res.eyes_w;
		p->eyes_h = p->res.eyes_h;
	}
	gs_texture_set_image(p->eyes_tex, p->eyes_buf, p->eyes_w * 4, false);
	p->eyes_seq_uploaded = p->res.eyes_seq;
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

/* libobs clears every effect parameter when a technique ends, so values a pass reads must be set
 * before that pass, every time. These are the ones several passes share. */
struct pass_common {
	float fw, fh;
	float e0[4], e1[4], roll0, roll1;
	int nfaces;
};

static void pass_common(gs_effect_t *fx, const struct pass_common *c)
{
	set_v2(fx, "size", c->fw, c->fh);
	set_v2(fx, "texel", 1.0f / c->fw, 1.0f / c->fh);
	set_f(fx, "face_count", (float)c->nfaces);
	set_v4(fx, "face0", c->e0[0], c->e0[1], c->e0[2], c->e0[3]);
	set_v4(fx, "face1", c->e1[0], c->e1[1], c->e1[2], c->e1[3]);
	set_f(fx, "face0_roll", c->roll0);
	set_f(fx, "face1_roll", c->roll1);
}

/* A blurred copy: halve the picture `levels` times, then grow it back to 1 / 2^keep of the frame
 * with bilinear filtering (a "dual filter" blur). Only OBS's standard drawing, so it looks the
 * same on Direct3D, OpenGL and Metal. */
static gs_texture_t *blur_chain(struct polish *p, gs_texture_t *src, uint32_t w, uint32_t h, int levels, int keep)
{
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (levels > BLUR_LEVELS)
		levels = BLUR_LEVELS;
	if (levels < 1)
		levels = 1;
	gs_texture_t *b = src;
	for (int i = 0; i < levels; i++) {
		uint32_t lw = w >> (i + 1), lh = h >> (i + 1);
		draw_into(p->t_down[i], lw ? lw : 1, lh ? lh : 1, def, "Draw", b);
		b = gs_texrender_get_texture(p->t_down[i]);
	}
	for (int i = levels - 1; i >= keep && i >= 1; i--) {
		uint32_t lw = w >> i, lh = h >> i;
		draw_into(p->t_up[i], lw ? lw : 1, lh ? lh : 1, def, "Draw", b);
		b = gs_texrender_get_texture(p->t_up[i]);
	}
	return b;
}

/* The sticker on the largest face, drawn with the current matrices. */
static void draw_sticker(struct polish *p, const struct pa_face *f, float fw, float fh)
{
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
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

	/* Nothing to do (every effect off, or compare held): step aside, so the picture is exactly
	 * what OBS would show without the filter, at no cost. */
	bool eye_makeup = p->eyeshadow > 0 || p->eyeliner > 0 || p->brows > 0 || p->eye_color > 0;
	bool shape = p->slim > 0 || p->eyes > 0 || p->vline > 0 || p->chin != 0 || p->nose_slim > 0 || p->lip_fill > 0;
	bool any = p->smooth > 0 || p->brighten > 0 || p->sharpen > 0 || shape || p->foundation > 0 || p->soft_light > 0 ||
		   eye_makeup || p->lipstick > 0 || p->blush > 0 || p->teeth > 0 || p->eye_bright > 0 || p->bg_mode != BG_OFF ||
		   (p->lut_loaded && p->look_amt > 0) || p->exposure != 0 || p->warmth != 0 || p->saturation != 0 ||
		   p->vignette != 0 || p->grain > 0 || p->contrast != 0 || strcmp(p->sticker, "none") != 0 ||
		   (p->style->id != 0 && p->style_amt > 0);
	if (!any || p->compare || polish_compare_held) {
		obs_source_skip_video_filter(p->self);
		return;
	}

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
	bool makeup = p->lipstick > 0 || p->blush > 0 || p->teeth > 0 || p->eye_bright > 0 || eye_makeup;
	bool need_faces = p->smooth > 0 || p->brighten > 0 || p->foundation > 0 || p->soft_light > 0 || shape || makeup ||
			  strcmp(p->sticker, "none") != 0;
	p->frame++;
	if (need_faces || p->bg_mode != BG_OFF)
		analyse(p, src, w, h);
	pa_get(p->analyzer, &p->res, p->mask, MASK_CAP, p->mouth, MOUTH_CAP, p->eyes_buf, EYES_CAP);
	upload_mask(p);
	upload_mouth(p);
	upload_eyes(p);
	const struct pa_result *r = &p->res;

	gs_texrender_t *ping = p->t_a, *pong = p->t_b;
	bool compare = p->compare || polish_compare_held;
	bool sticker_done = false; /* drawn into the frame already, under an effect */
	if (compare)
		goto output;

	/* frame size and face regions, set again before each pass */
	struct pass_common pc = {.fw = fw, .fh = fh, .nfaces = r->nfaces};
	if (r->nfaces > 0) {
		face_ellipse(&r->faces[0], pc.e0);
		pc.roll0 = r->faces[0].roll;
	}
	if (r->nfaces > 1) {
		face_ellipse(&r->faces[1], pc.e1);
		pc.roll1 = r->faces[1].roll;
	}

	/* 3. smooth, brighten, foundation */
	if (p->smooth > 0 || p->brighten > 0 || p->foundation > 0) {
		set_f(fx, "smooth_amt", p->smooth);
		set_f(fx, "smooth_radius", fmaxf(2.0f, fh * 0.0085f));
		set_f(fx, "brighten", p->brighten);
		color_v4(fx, "foundation", p->foundation_color, p->foundation);
		/* until the first analysis arrives, smooth skin everywhere at a lower level */
		set_f(fx, "no_face_level", r->seq > 0 ? 0.0f : 0.6f);
		pass_common(fx, &pc);
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
		bool eyes_ok = p->eyes_tex && r->eyes_w > 0;
		color_v4(fx, "shadow_color", p->shadow_color, eyes_ok ? p->eyeshadow * 0.85f : 0.0f);
		color_v4(fx, "liner_color", p->liner_color, eyes_ok ? p->eyeliner * 0.9f : 0.0f);
		color_v4(fx, "brow_color", p->brow_color, eyes_ok ? p->brows * 0.7f : 0.0f);
		color_v4(fx, "iris_color", p->iris_color, eyes_ok ? p->eye_color * 0.85f : 0.0f);
		/* always bind something: Metal stops on an unbound texture (the amounts are 0 without one) */
		gs_effect_set_texture(gs_effect_get_param_by_name(fx, "eyes_tex"), eyes_ok ? p->eyes_tex : cur);
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
		pass_common(fx, &pc);
		draw_into(ping, w, h, fx, "Makeup", cur);
		cur = gs_texrender_get_texture(ping);
		SWAP_PASS();
	}

	/* 4. face shape (largest face): jaw, chin, nose, eyes and lips */
	if (shape && r->nfaces > 0) {
		const struct pa_face *f = &r->faces[0];
		float cx = (f->eye_l.x + f->eye_r.x) * 0.5f * fw, cy = f->nose.y * fh;
		float facew = f->w * fw, faceh = f->h * fh;
		float cs = cosf(f->roll), sn = sinf(f->roll);
		float downx = -sn, downy = cs; /* the face's down, rotated with the head */
		/* jaw points cheek to cheek, through the chin (contour index 8) */
		const int idx[9] = {2, 4, 6, 7, 8, 9, 10, 12, 14};
		const char *names[9] = {"ctl0", "ctl1", "ctl2", "ctl3", "ctl4", "ctl5", "ctl6", "ctl7", "ctl8"};
		float chinx = f->chin.x * fw;
		for (int i = 0; i < 9; i++) {
			int k = idx[i];
			float px = f->contour[k].x * fw, py = f->contour[k].y * fh;
			float dxs = 0, dys = 0;
			if (k != 7 && k != 8 && k != 9) { /* slim: mostly sideways, narrow the jaw without lifting it */
				float dx = cx - px, dy = (cy - py) * 0.2f, len = sqrtf(dx * dx + dy * dy) + 1e-3f, amt = p->slim * facew * 0.05f;
				dxs += dx / len * amt;
				dys += dy / len * amt;
			}
			if (k >= 6 && k <= 10 && k != 8) { /* V-shape: the lower jaw in towards the chin, and up a little */
				float side = chinx > px ? 1.0f : -1.0f, amt = p->vline * facew * (k == 7 || k == 9 ? 0.035f : 0.045f);
				dxs += side * amt * cs - downx * amt * 0.3f;
				dys += side * amt * sn - downy * amt * 0.3f;
			}
			if (k >= 7 && k <= 9) { /* chin: longer or shorter along the face */
				float amt = p->chin * faceh * 0.12f * (k == 8 ? 1.0f : 0.55f);
				dxs += downx * amt;
				dys += downy * amt;
			}
			set_v4(fx, names[i], px, py, dxs, dys);
		}
		set_f(fx, "ctl_radius", facew * 0.32f);
		float s = p->eyes * 0.28f;
		set_v4(fx, "eye0", f->eye_l.x * fw, f->eye_l.y * fh, fmaxf(f->eye_rl * fw * 2.4f, 4.0f), s);
		set_v4(fx, "eye1", f->eye_r.x * fw, f->eye_r.y * fh, fmaxf(f->eye_rr * fw * 2.4f, 4.0f), s);
		/* nose: narrow the wings towards the nose's centre line */
		float nlx = f->nose_l.x * fw, nly = f->nose_l.y * fh, nrx = f->nose_r.x * fw, nry = f->nose_r.y * fh;
		float nwidth = hypotf(nrx - nlx, nry - nly);
		float ntx = f->nose_top.x * fw, nty = f->nose_top.y * fh, nbx = f->nose.x * fw, nby = f->nose.y * fh;
		float nlen = hypotf(nbx - ntx, nby - nty);
		set_v4(fx, "nose_area", (nlx + nrx) * 0.5f * 0.6f + (ntx + (nbx - ntx) * 0.7f) * 0.4f,
		       (nly + nry) * 0.5f * 0.6f + (nty + (nby - nty) * 0.7f) * 0.4f, fmaxf(nwidth * 1.05f, 4.0f), fmaxf(nlen * 0.75f, 4.0f));
		set_f(fx, "nose_amt", p->nose_slim * 0.55f);
		/* lips: a gentle zoom on the mouth, wider than tall */
		float lx0 = 1e9f, lx1 = -1e9f, ly0 = 1e9f, ly1 = -1e9f;
		for (int i = 0; i < f->nlip_outer; i++) {
			lx0 = fminf(lx0, f->lip_outer[i].x * fw);
			lx1 = fmaxf(lx1, f->lip_outer[i].x * fw);
			ly0 = fminf(ly0, f->lip_outer[i].y * fh);
			ly1 = fmaxf(ly1, f->lip_outer[i].y * fh);
		}
		bool lips = f->nlip_outer > 0 && p->lip_fill > 0;
		set_v4(fx, "lip_area", lips ? (lx0 + lx1) * 0.5f : 0.0f, lips ? (ly0 + ly1) * 0.5f : 0.0f,
		       lips ? fmaxf((lx1 - lx0) * 0.5f * 1.35f, 4.0f) : 4.0f, lips ? fmaxf((ly1 - ly0) * 0.5f * 2.0f, 4.0f) : 4.0f);
		set_f(fx, "lip_amt", lips ? p->lip_fill * 0.22f : 0.0f);
		set_f(fx, "shape_roll", f->roll);
		pass_common(fx, &pc);
		draw_into(ping, w, h, fx, "Warp", cur);
		cur = gs_texrender_get_texture(ping);
		SWAP_PASS();
	}

	/* 5. background */
	if (p->bg_mode != BG_OFF && p->mask_tex) {
		gs_texture_t *bg = NULL;
		if (p->bg_mode == BG_BLUR) {
			/* Blur by halving the picture several times and growing it back with bilinear
			 * filtering (a "dual filter" blur). It uses only OBS's standard drawing, which behaves
			 * the same on Direct3D, OpenGL and Metal. More levels, more blur. */
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			int levels = 2 + (int)ceilf(p->bg_blur * (BLUR_LEVELS - 2));
			uint32_t lw[BLUR_LEVELS + 1], lh[BLUR_LEVELS + 1];
			lw[0] = w;
			lh[0] = h;
			gs_texture_t *b = cur;
			for (int i = 0; i < levels; i++) {
				lw[i + 1] = lw[i] / 2 ? lw[i] / 2 : 1;
				lh[i + 1] = lh[i] / 2 ? lh[i] / 2 : 1;
				draw_into(p->t_down[i], lw[i + 1], lh[i + 1], def, "Draw", b);
				b = gs_texrender_get_texture(p->t_down[i]);
			}
			for (int i = levels - 1; i >= 2; i--) { /* back up to quarter size; the composite stretches it */
				draw_into(p->t_up[i], lw[i], lh[i], def, "Draw", b);
				b = gs_texrender_get_texture(p->t_up[i]);
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
			pass_common(fx, &pc);
			draw_into(ping, w, h, fx, "Composite", cur);
			cur = gs_texrender_get_texture(ping);
			SWAP_PASS();
		}
	}

	/* 5b. soft light: a glow from a blurred copy, and a gentle light on the face */
	if (p->soft_light > 0) {
		gs_texture_t *b = blur_chain(p, cur, w, h, 4, 2);
		set_f(fx, "glow_amt", p->soft_light);
		gs_effect_set_texture(gs_effect_get_param_by_name(fx, "glow_tex"), b);
		pass_common(fx, &pc);
		draw_into(ping, w, h, fx, "Glow", cur);
		cur = gs_texrender_get_texture(ping);
		SWAP_PASS();
	}

	/* 6. grade */
	bool look = p->lut_loaded && p->look_amt > 0;
	if (look || p->exposure != 0 || p->warmth != 0 || p->saturation != 0 || p->sharpen > 0 || p->vignette != 0 ||
	    p->grain > 0 || p->contrast != 0) {
		set_f(fx, "vignette", p->vignette);
		set_f(fx, "grain", p->grain);
		set_f(fx, "grain_seed", (float)(p->frame % 997));
		set_f(fx, "lut_amt", look ? p->look_amt : 0.0f);
		/* always bind something: Metal stops on an unbound texture (lut_amt is 0 without a look) */
		gs_effect_set_texture(gs_effect_get_param_by_name(fx, "lut_tex"), look ? p->lut_img.texture : cur);
		set_f(fx, "exposure", p->exposure);
		set_f(fx, "warmth", p->warmth);
		set_f(fx, "saturation", p->saturation);
		set_f(fx, "sharpen", p->sharpen);
		set_f(fx, "contrast", p->contrast);
		pass_common(fx, &pc);
		draw_into(ping, w, h, fx, "Grade", cur);
		cur = gs_texrender_get_texture(ping);
		SWAP_PASS();
	}

	/* 7. effects: on everything above, the sticker included */
	if (p->style->id != 0 && p->style_amt > 0) {
		if (p->sticker_loaded && r->nfaces > 0) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_texrender_reset(ping);
			if (gs_texrender_begin(ping, w, h)) {
				struct vec4 clear;
				vec4_zero(&clear);
				gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
				gs_ortho(0.0f, fw, 0.0f, fh, -100.0f, 100.0f);
				gs_blend_state_push();
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
				gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), cur);
				while (gs_effect_loop(def, "Draw"))
					gs_draw_sprite(cur, 0, w, h);
				gs_blend_state_pop();
				draw_sticker(p, &r->faces[0], fw, fh);
				gs_texrender_end(ping);
				cur = gs_texrender_get_texture(ping);
				SWAP_PASS();
				sticker_done = true;
			}
		}
		const struct style *st = p->style;
		set_f(fx, "style_id", (float)st->id);
		set_f(fx, "style_amt", p->style_amt);
		set_f(fx, "style_k", p->style_k);
		set_f(fx, "style_time", (float)p->frame / 30.0f);
		set_f(fx, "style_seed", (float)((p->frame / 2) % 9973));
		color_v4(fx, "style_c1", p->style_c1, 1.0f);
		color_v4(fx, "style_c2", p->style_c2, 1.0f);
		if (st->id == STYLE_ECHO) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			if (!p->hist_valid || p->hist_w != w || p->hist_h != h) {
				draw_into(p->t_hist, w, h, def, "Draw", cur);
				p->hist_w = w;
				p->hist_h = h;
				p->hist_valid = true;
			}
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "hist_tex"), gs_texrender_get_texture(p->t_hist));
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "blur_tex"), cur);
			pass_common(fx, &pc);
			draw_into(ping, w, h, fx, "Echo", cur);
			cur = gs_texrender_get_texture(ping);
			SWAP_PASS();
			draw_into(p->t_hist, w, h, def, "Draw", cur); /* this output is the next frame's trail */
		} else {
			int levels = st->blur < 0 ? 1 + (int)roundf(p->style_k * 5.0f) : st->blur;
			gs_texture_t *b = levels > 0 ? blur_chain(p, cur, w, h, levels, levels >= 4 ? 2 : 1) : cur;
			/* always bind both: Metal stops on an unbound texture */
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "blur_tex"), b);
			gs_effect_set_texture(gs_effect_get_param_by_name(fx, "hist_tex"), cur);
			pass_common(fx, &pc);
			draw_into(ping, w, h, fx, "Style", cur);
			cur = gs_texrender_get_texture(ping);
			SWAP_PASS();
		}
	} else {
		p->hist_valid = false;
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

	if (!compare && p->sticker_loaded && r->nfaces > 0 && !sticker_done)
		draw_sticker(p, &r->faces[0], fw, fh);
}

/* ------------------------------------------------------------------ status for tests */

static void analysis_json(void *data, calldata_t *cd)
{
	struct polish *p = data;
	struct pa_result r = p->res;
	struct dstr s = {0};
	const char *err = pa_error(p->analyzer);
	dstr_printf(&s, "{\"backend\":\"%s\",\"error\":\"%s\",\"seq\":%llu,\"faces\":%d,\"face_ms\":%.1f,\"mask_ms\":%.1f,\"mask\":[%d,%d],\"mouth\":[%d,%d],\"eyes\":[%d,%d],\"face\":[",
		    pa_backend_name(), err ? err : "", (unsigned long long)r.seq, r.nfaces, r.face_ms, r.mask_ms, r.mask_w, r.mask_h, r.mouth_w, r.mouth_h,
		    r.eyes_w, r.eyes_h);
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
	dstr_cat(&s, "]");
	if (r.nfaces > 0) {
		const struct pa_face *f = &r.faces[0];
		dstr_catf(&s, ",\"iris\":[[%.4f,%.4f,%.4f],[%.4f,%.4f,%.4f]],\"nose\":[[%.4f,%.4f],[%.4f,%.4f],[%.4f,%.4f]],\"brows\":%d,\"eye_outline\":%d",
			  f->iris[0].x, f->iris[0].y, f->iris_r[0], f->iris[1].x, f->iris[1].y, f->iris_r[1], f->nose_l.x, f->nose_l.y,
			  f->nose_r.x, f->nose_r.y, f->nose_top.x, f->nose_top.y, f->nbrow, f->neye);
		dstr_cat(&s, ",\"brow\":[");
		for (int e = 0; e < 2 && f->nbrow; e++)
			for (int i = 0; i < f->nbrow; i++)
				dstr_catf(&s, "%s[%.4f,%.4f]", e || i ? "," : "", f->brow_line[e][i].x, f->brow_line[e][i].y);
		dstr_cat(&s, "]");
	}
	dstr_cat(&s, "}");
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
