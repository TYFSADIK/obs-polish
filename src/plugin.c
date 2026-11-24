/* Polish for OBS: beauty, face shape, colour looks, background and stickers.
 * Original open-source work; MIT licence. */
#include <obs-module.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-polish", "en-US")

#define POLISH_VERSION "0.2.0"

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Polish: beauty, face shape, colour looks, background and stickers for camera sources.";
}

extern struct obs_source_info polish_filter;

/* Held: every Polish filter shows its untouched input (read in polish-filter.c). */
volatile bool polish_compare_held;
static obs_hotkey_id compare_hotkey = OBS_INVALID_HOTKEY_ID;

static void compare_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	polish_compare_held = pressed;
}

/* The frontend API is looked up at run time, so the plugin also loads where there is no OBS
 * window (the headless tests). */
typedef void (*frontend_cb)(void *);
static void (*fe_add_tools_item)(const char *, frontend_cb, void *);
static obs_source_t *(*fe_current_scene)(void);
static void (*fe_open_filters)(obs_source_t *);
static void (*fe_add_save_cb)(void (*)(obs_data_t *, bool, void *), void *);

/* keeps the hotkey binding with the scene collection, as OBS does for its own hotkeys */
static void save_cb(obs_data_t *data, bool saving, void *unused)
{
	if (saving) {
		obs_data_array_t *a = obs_hotkey_save(compare_hotkey);
		obs_data_set_array(data, "obs_polish_compare", a);
		obs_data_array_release(a);
	} else {
		obs_data_array_t *a = obs_data_get_array(data, "obs_polish_compare");
		obs_hotkey_load(compare_hotkey, a);
		obs_data_array_release(a);
	}
}

static bool is_camera(obs_source_t *s)
{
	const char *id = obs_source_get_unversioned_id(s);
	return id && (strstr(id, "av_capture") || strstr(id, "avcapture"));
}

struct pick {
	obs_source_t *selected, *camera;
};

static bool pick_item(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	struct pick *p = data;
	obs_source_t *s = obs_sceneitem_get_source(item);
	if (!(obs_source_get_output_flags(s) & OBS_SOURCE_VIDEO))
		return true;
	if (!p->selected && obs_sceneitem_selected(item))
		p->selected = s;
	if (!p->camera && is_camera(s))
		p->camera = s;
	return true;
}

static void find_filter(obs_source_t *parent, obs_source_t *child, void *data)
{
	obs_source_t **found = data;
	if (!*found && strcmp(obs_source_get_unversioned_id(child), "polish_filter") == 0)
		*found = child;
}

/* Tools > Polish: adds the filter to the selected source (or the first camera in the scene)
 * if it does not have one yet, then opens that source's filter window. */
static void tools_clicked(void *unused)
{
	obs_source_t *scene_src = fe_current_scene ? fe_current_scene() : NULL;
	if (!scene_src)
		return;
	struct pick p = {0};
	obs_scene_enum_items(obs_scene_from_source(scene_src), pick_item, &p);
	obs_source_t *target = p.selected ? p.selected : p.camera;
	if (target) {
		obs_source_t *found = NULL;
		obs_source_enum_filters(target, find_filter, &found);
		if (!found) {
			obs_source_t *f = obs_source_create("polish_filter", obs_module_text("Polish"), NULL, NULL);
			obs_source_filter_add(target, f);
			obs_source_release(f);
		}
		if (fe_open_filters)
			fe_open_filters(target);
	} else {
		blog(LOG_INFO, "[obs-polish] select a video source first");
	}
	obs_source_release(scene_src);
}

bool obs_module_load(void)
{
	obs_register_source(&polish_filter);
	compare_hotkey = obs_hotkey_register_frontend("obs_polish.compare", obs_module_text("CompareHotkey"), compare_pressed, NULL);
	blog(LOG_INFO, "[obs-polish] loaded, version %s", POLISH_VERSION);
	return true;
}

/* The frontend API lives in the OBS executable's frontend library; look it up at run time so the
 * plugin also loads where there is no OBS window (the headless tests, obs-websocket hosts). */
static void *frontend_sym(const char *name)
{
#ifdef _WIN32
	HMODULE m = GetModuleHandleA("obs-frontend-api.dll");
	return m ? (void *)GetProcAddress(m, name) : NULL;
#else
	return dlsym(RTLD_DEFAULT, name);
#endif
}

void obs_module_post_load(void)
{
	fe_add_tools_item = frontend_sym("obs_frontend_add_tools_menu_item");
	fe_current_scene = frontend_sym("obs_frontend_get_current_scene");
	fe_open_filters = frontend_sym("obs_frontend_open_source_filters");
	fe_add_save_cb = frontend_sym("obs_frontend_add_save_callback");
	if (fe_add_save_cb)
		fe_add_save_cb(save_cb, NULL);
	if (fe_add_tools_item)
		fe_add_tools_item(obs_module_text("ToolsMenu"), tools_clicked, NULL);
}
