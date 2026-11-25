/* Cross-platform backend: Google's MediaPipe models (Apache 2.0) on ONNX Runtime, CPU only.
 *
 *   face_detection_short_range  128x128 letterboxed frame -> face boxes and 6 keypoints
 *   face_landmark               192x192 rotated crop of one face -> 468 landmarks
 *   selfie_segmentation         256x256 stretched frame -> person probability
 *
 * Faces are found by the detector, then tracked from their own landmarks frame to frame; the
 * detector runs again when a face is lost and every DETECT_EVERY frames to pick up new ones.
 * ONNX Runtime is loaded at run time from a privately named copy next to the plugin, through
 * its single exported entry point, so it cannot clash with another plugin's copy. */
#ifdef __linux__
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "onnxruntime_c_api.h"
#include "analyzer.h"

#define DET_SIZE 128
#define MESH_SIZE 192
#define SEG_SIZE 256
#define NUM_ANCHORS 896
#define DETECT_EVERY 15
#define MIN_FACE_SCORE 0.5f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const OrtApi *ort;

struct model {
	OrtSession *session;
	char *in_name;
	char *out_names[4];
	size_t nout;
	bool nchw; /* input layout */
};

struct roi {
	float cx, cy, size, angle; /* pixels in the analysed frame; angle of the face's x axis */
};

struct onnx {
	OrtEnv *env;
	OrtSessionOptions *opts;
	OrtMemoryInfo *mem;
	struct model det, mesh, seg;
	float anchors[NUM_ANCHORS][2];
	struct roi track[PA_MAX_FACES];
	int ntrack;
	uint64_t frame;
	float *buf;
	size_t buf_cap;
};

const char *pa_backend_name(void) { return "onnx"; }

/* ------------------------------------------------------------------ loading ONNX Runtime */

static char *module_dir_file(const char *name)
{
	/* next to the plugin binary; POLISH_ORT_LIB overrides (tests, development builds) */
	const char *env = getenv("POLISH_ORT_LIB");
	if (env && *env)
		return bstrdup(env);
	const char *bin = obs_get_module_binary_path(obs_current_module());
	if (!bin)
		return NULL;
	struct dstr d = {0};
	dstr_copy(&d, bin);
	char *slash = strrchr(d.array, '/');
#ifdef _WIN32
	char *bs = strrchr(d.array, '\\');
	if (!slash || (bs && bs > slash))
		slash = bs;
#endif
	if (slash)
		dstr_resize(&d, (size_t)(slash - d.array) + 1);
	dstr_cat(&d, name);
	return d.array;
}

static bool load_runtime(const char **error)
{
	if (ort)
		return true;
#ifdef _WIN32
	char *path = module_dir_file("obs-polish-onnxruntime.dll");
	wchar_t *wpath = NULL;
	os_utf8_to_wcs_ptr(path, 0, &wpath);
	/* LOAD_WITH_ALTERED_SEARCH_PATH: its own dependencies are found next to it */
	HMODULE lib = wpath ? LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
	bfree(wpath);
	void *sym = lib ? (void *)GetProcAddress(lib, "OrtGetApiBase") : NULL;
#elif defined(__APPLE__)
	char *path = module_dir_file("../Frameworks/libobs-polish-onnxruntime.dylib");
	void *lib = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
	void *sym = lib ? dlsym(lib, "OrtGetApiBase") : NULL;
#else
	char *path = module_dir_file("libobs-polish-onnxruntime.so");
	void *lib = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
	void *sym = lib ? dlsym(lib, "OrtGetApiBase") : NULL;
#endif
	if (!sym) {
		blog(LOG_ERROR, "[obs-polish] cannot load ONNX Runtime from %s", path ? path : "(unknown)");
		bfree(path);
		*error = "ONNX Runtime is missing from the plugin folder: reinstall Polish";
		return false;
	}
	bfree(path);
	const OrtApiBase *base = ((const OrtApiBase *(*)(void))sym)();
	ort = base->GetApi(ORT_API_VERSION);
	if (!ort) {
		*error = "The bundled ONNX Runtime is older than this build expects: reinstall Polish";
		return false;
	}
	blog(LOG_INFO, "[obs-polish] ONNX Runtime %s", base->GetVersionString());
	return true;
}

static bool check(OrtStatus *st, const char *what)
{
	if (!st)
		return true;
	blog(LOG_ERROR, "[obs-polish] %s: %s", what, ort->GetErrorMessage(st));
	ort->ReleaseStatus(st);
	return false;
}

static bool load_model(struct onnx *o, struct model *m, const char *file)
{
	struct dstr rel = {0};
	dstr_printf(&rel, "models/%s", file);
	char *path = obs_module_file(rel.array);
	dstr_free(&rel);
	if (!path) {
		blog(LOG_ERROR, "[obs-polish] model %s not found", file);
		return false;
	}
#ifdef _WIN32
	wchar_t *wpath = NULL;
	os_utf8_to_wcs_ptr(path, 0, &wpath);
	bool ok = check(ort->CreateSession(o->env, wpath, o->opts, &m->session), file);
	bfree(wpath);
#else
	bool ok = check(ort->CreateSession(o->env, path, o->opts, &m->session), file);
#endif
	bfree(path);
	if (!ok)
		return false;
	OrtAllocator *alloc;
	if (!check(ort->GetAllocatorWithDefaultOptions(&alloc), "allocator"))
		return false;
	char *name;
	if (!check(ort->SessionGetInputName(m->session, 0, alloc, &name), "input name"))
		return false;
	m->in_name = bstrdup(name);
	alloc->Free(alloc, name);
	OrtTypeInfo *ti;
	if (check(ort->SessionGetInputTypeInfo(m->session, 0, &ti), "input type")) {
		const OrtTensorTypeAndShapeInfo *tsi;
		int64_t dims[4] = {0};
		size_t nd = 0;
		if (check(ort->CastTypeInfoToTensorInfo(ti, &tsi), "tensor info") &&
		    check(ort->GetDimensionsCount(tsi, &nd), "rank") && nd == 4 &&
		    check(ort->GetDimensions(tsi, dims, 4), "dims")) {
			m->nchw = dims[1] == 3;
		}
		ort->ReleaseTypeInfo(ti);
	}
	if (!check(ort->SessionGetOutputCount(m->session, &m->nout), "outputs"))
		return false;
	if (m->nout > 4)
		m->nout = 4;
	for (size_t i = 0; i < m->nout; i++) {
		if (!check(ort->SessionGetOutputName(m->session, i, alloc, &name), "output name"))
			return false;
		m->out_names[i] = bstrdup(name);
		alloc->Free(alloc, name);
	}
	return true;
}

static void free_model(struct model *m)
{
	if (m->session)
		ort->ReleaseSession(m->session);
	bfree(m->in_name);
	for (size_t i = 0; i < m->nout; i++)
		bfree(m->out_names[i]);
	memset(m, 0, sizeof *m);
}

/* SSD anchors for the short-range face detector: strides 8, 16, 16, 16 on a 128 px input,
 * two anchors per cell per layer, layers with the same stride merged, fixed anchor size. */
static void make_anchors(struct onnx *o)
{
	const int strides[4] = {8, 16, 16, 16};
	int n = 0, layer = 0;
	while (layer < 4) {
		int last = layer, per_cell = 0;
		while (last < 4 && strides[last] == strides[layer]) {
			per_cell += 2;
			last++;
		}
		int grid = DET_SIZE / strides[layer];
		for (int y = 0; y < grid; y++)
			for (int x = 0; x < grid; x++)
				for (int k = 0; k < per_cell && n < NUM_ANCHORS; k++) {
					o->anchors[n][0] = ((float)x + 0.5f) / (float)grid;
					o->anchors[n][1] = ((float)y + 0.5f) / (float)grid;
					n++;
				}
		layer = last;
	}
}

void *pa_backend_create(const char **error)
{
	if (!load_runtime(error))
		return NULL;
	struct onnx *o = bzalloc(sizeof(*o));
	bool ok = check(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "obs-polish", &o->env), "env") &&
		  check(ort->CreateSessionOptions(&o->opts), "options") &&
		  check(ort->SetIntraOpNumThreads(o->opts, 2), "threads") &&
		  check(ort->SetInterOpNumThreads(o->opts, 1), "threads") &&
		  check(ort->SetSessionGraphOptimizationLevel(o->opts, ORT_ENABLE_ALL), "optimisation") &&
		  check(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &o->mem), "memory") &&
		  load_model(o, &o->det, "face_detection_short_range.onnx") &&
		  load_model(o, &o->mesh, "face_landmark.onnx") && load_model(o, &o->seg, "selfie_segmentation.onnx");
	if (!ok) {
		*error = "The face models could not be loaded: reinstall Polish";
		pa_backend_destroy(o);
		return NULL;
	}
	make_anchors(o);
	return o;
}

void pa_backend_destroy(void *ptr)
{
	struct onnx *o = ptr;
	if (!o)
		return;
	free_model(&o->det);
	free_model(&o->mesh);
	free_model(&o->seg);
	if (o->mem)
		ort->ReleaseMemoryInfo(o->mem);
	if (o->opts)
		ort->ReleaseSessionOptions(o->opts);
	if (o->env)
		ort->ReleaseEnv(o->env);
	bfree(o->buf);
	bfree(o);
}

/* ------------------------------------------------------------------ image helpers */

static float *scratch(struct onnx *o, size_t floats)
{
	if (floats > o->buf_cap) {
		o->buf = brealloc(o->buf, floats * sizeof(float));
		o->buf_cap = floats;
	}
	return o->buf;
}

/* bilinear sample of a BGRA frame at pixel coordinates; outside the frame reads as black */
static void sample(const uint8_t *img, int w, int h, int stride, float x, float y, float rgb[3])
{
	x -= 0.5f;
	y -= 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - (float)x0, fy = y - (float)y0;
	float acc[3] = {0, 0, 0};
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++) {
			int xx = x0 + i, yy = y0 + j;
			float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
			if (xx < 0 || yy < 0 || xx >= w || yy >= h || wgt == 0)
				continue;
			const uint8_t *p = img + (size_t)yy * stride + (size_t)xx * 4;
			acc[0] += wgt * p[2]; /* BGRA -> RGB */
			acc[1] += wgt * p[1];
			acc[2] += wgt * p[0];
		}
	rgb[0] = acc[0] / 255.0f;
	rgb[1] = acc[1] / 255.0f;
	rgb[2] = acc[2] / 255.0f;
}

static void put(float *dst, bool nchw, int size, int x, int y, const float rgb[3], float scale, float bias)
{
	for (int c = 0; c < 3; c++) {
		float v = rgb[c] * scale + bias;
		if (nchw)
			dst[(size_t)c * size * size + (size_t)y * size + x] = v;
		else
			dst[((size_t)y * size + x) * 3 + c] = v;
	}
}

/* Runs a model on a square float image; fills outs (caller releases). */
static bool run(struct onnx *o, struct model *m, float *input, int size, OrtValue **outs)
{
	int64_t shape[4] = {1, size, size, 3};
	if (m->nchw) {
		shape[1] = 3;
		shape[3] = size;
	}
	OrtValue *in = NULL;
	if (!check(ort->CreateTensorWithDataAsOrtValue(o->mem, input, sizeof(float) * 3 * size * size, shape, 4,
						      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in),
		   "input tensor"))
		return false;
	for (size_t i = 0; i < m->nout; i++)
		outs[i] = NULL;
	const char *in_names[1] = {m->in_name};
	bool ok = check(ort->Run(m->session, NULL, in_names, (const OrtValue *const *)&in, 1,
				 (const char *const *)m->out_names, m->nout, outs),
			"inference");
	ort->ReleaseValue(in);
	return ok;
}

static size_t elements(OrtValue *v)
{
	OrtTensorTypeAndShapeInfo *info;
	size_t n = 0;
	if (check(ort->GetTensorTypeAndShape(v, &info), "shape")) {
		if (!check(ort->GetTensorShapeElementCount(info, &n), "count"))
			n = 0;
		ort->ReleaseTensorTypeAndShapeInfo(info);
	}
	return n;
}

static float *data_of(OrtValue *v)
{
	float *p = NULL;
	if (!check(ort->GetTensorMutableData(v, (void **)&p), "data"))
		return NULL;
	return p;
}

static void release_all(OrtValue **v, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (v[i])
			ort->ReleaseValue(v[i]);
}

static float sigmoid(float x)
{
	if (x < -100)
		x = -100;
	if (x > 100)
		x = 100;
	return 1.0f / (1.0f + expf(-x));
}

/* ------------------------------------------------------------------ detection */

struct det {
	float cx, cy, w, h; /* pixels */
	float kx[6], ky[6];
	float score;
};

static float iou(const struct det *a, const struct det *b)
{
	float x0 = fmaxf(a->cx - a->w / 2, b->cx - b->w / 2), x1 = fminf(a->cx + a->w / 2, b->cx + b->w / 2);
	float y0 = fmaxf(a->cy - a->h / 2, b->cy - b->h / 2), y1 = fminf(a->cy + a->h / 2, b->cy + b->h / 2);
	float inter = fmaxf(0, x1 - x0) * fmaxf(0, y1 - y0);
	float uni = a->w * a->h + b->w * b->h - inter;
	return uni > 0 ? inter / uni : 0;
}

static int detect(struct onnx *o, const uint8_t *img, int w, int h, int stride, struct det *out, int max)
{
	/* letterbox into 128 x 128 */
	float scale = (float)DET_SIZE / (float)(w > h ? w : h);
	float padx = ((float)DET_SIZE - w * scale) / 2, pady = ((float)DET_SIZE - h * scale) / 2;
	float *in = scratch(o, 3 * DET_SIZE * DET_SIZE);
	for (int y = 0; y < DET_SIZE; y++)
		for (int x = 0; x < DET_SIZE; x++) {
			float rgb[3];
			sample(img, w, h, stride, ((float)x + 0.5f - padx) / scale, ((float)y + 0.5f - pady) / scale, rgb);
			put(in, o->det.nchw, DET_SIZE, x, y, rgb, 2.0f, -1.0f); /* [-1, 1] */
		}
	OrtValue *outs[4];
	if (!run(o, &o->det, in, DET_SIZE, outs))
		return 0;
	float *reg = NULL, *cls = NULL;
	for (size_t i = 0; i < o->det.nout; i++) {
		size_t n = elements(outs[i]);
		if (n == NUM_ANCHORS * 16)
			reg = data_of(outs[i]);
		else if (n == NUM_ANCHORS)
			cls = data_of(outs[i]);
	}
	struct det cand[64];
	int nc = 0;
	for (int a = 0; reg && cls && a < NUM_ANCHORS && nc < 64; a++) {
		float score = sigmoid(cls[a]);
		if (score < MIN_FACE_SCORE)
			continue;
		const float *r = reg + a * 16;
		float ax = o->anchors[a][0], ay = o->anchors[a][1];
		struct det d;
		/* decode in the 128 px letterbox, then back to frame pixels */
		float cx = r[0] / DET_SIZE + ax, cy = r[1] / DET_SIZE + ay;
		d.cx = (cx * DET_SIZE - padx) / scale;
		d.cy = (cy * DET_SIZE - pady) / scale;
		d.w = r[2] / scale;
		d.h = r[3] / scale;
		for (int k = 0; k < 6; k++) {
			d.kx[k] = ((r[4 + 2 * k] / DET_SIZE + ax) * DET_SIZE - padx) / scale;
			d.ky[k] = ((r[5 + 2 * k] / DET_SIZE + ay) * DET_SIZE - pady) / scale;
		}
		d.score = score;
		cand[nc++] = d;
	}
	release_all(outs, o->det.nout);

	/* weighted non-maximum suppression, best first */
	int n = 0;
	bool used[64] = {0};
	while (n < max) {
		int best = -1;
		for (int i = 0; i < nc; i++)
			if (!used[i] && (best < 0 || cand[i].score > cand[best].score))
				best = i;
		if (best < 0)
			break;
		struct det acc = {0};
		float wsum = 0;
		for (int i = 0; i < nc; i++) {
			if (used[i] || iou(&cand[i], &cand[best]) < 0.3f)
				continue;
			used[i] = true;
			float s = cand[i].score;
			acc.cx += s * cand[i].cx;
			acc.cy += s * cand[i].cy;
			acc.w += s * cand[i].w;
			acc.h += s * cand[i].h;
			for (int k = 0; k < 6; k++) {
				acc.kx[k] += s * cand[i].kx[k];
				acc.ky[k] += s * cand[i].ky[k];
			}
			wsum += s;
		}
		acc.cx /= wsum;
		acc.cy /= wsum;
		acc.w /= wsum;
		acc.h /= wsum;
		for (int k = 0; k < 6; k++) {
			acc.kx[k] /= wsum;
			acc.ky[k] /= wsum;
		}
		acc.score = cand[best].score;
		out[n++] = acc;
	}
	return n;
}

/* ------------------------------------------------------------------ face mesh */

/* MediaPipe face mesh indices */
static const int JAW[] = {234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454};
static const int EYE_A[] = {33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246};
static const int EYE_B[] = {263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466};
static const int LIPS_OUT[PA_LIP_PTS] = {61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270, 269, 267, 0, 37, 39, 40, 185};
static const int LIPS_IN[PA_LIP_PTS] = {78, 95, 88, 178, 87, 14, 317, 402, 318, 324, 308, 415, 310, 311, 312, 13, 82, 81, 80, 191};
#define NOSE_TIP 1
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Crop the ROI to 192 x 192, run the mesh, map the 468 landmarks back to frame pixels. */
static bool mesh(struct onnx *o, const uint8_t *img, int w, int h, int stride, const struct roi *r, float *lx, float *ly)
{
	float cs = cosf(r->angle), sn = sinf(r->angle);
	float *in = scratch(o, 3 * MESH_SIZE * MESH_SIZE);
	for (int y = 0; y < MESH_SIZE; y++)
		for (int x = 0; x < MESH_SIZE; x++) {
			float u = (((float)x + 0.5f) / MESH_SIZE - 0.5f) * r->size;
			float v = (((float)y + 0.5f) / MESH_SIZE - 0.5f) * r->size;
			float rgb[3];
			sample(img, w, h, stride, r->cx + u * cs - v * sn, r->cy + u * sn + v * cs, rgb);
			put(in, o->mesh.nchw, MESH_SIZE, x, y, rgb, 1.0f, 0.0f); /* [0, 1] */
		}
	OrtValue *outs[4];
	if (!run(o, &o->mesh, in, MESH_SIZE, outs))
		return false;
	float *lm = NULL, *score = NULL;
	for (size_t i = 0; i < o->mesh.nout; i++) {
		size_t n = elements(outs[i]);
		if (n == 468 * 3)
			lm = data_of(outs[i]);
		else if (n == 1)
			score = data_of(outs[i]);
	}
	bool ok = lm && score && sigmoid(score[0]) >= MIN_FACE_SCORE;
	for (int i = 0; ok && i < 468; i++) {
		float u = (lm[i * 3] / MESH_SIZE - 0.5f) * r->size, v = (lm[i * 3 + 1] / MESH_SIZE - 0.5f) * r->size;
		lx[i] = r->cx + u * cs - v * sn;
		ly[i] = r->cy + u * sn + v * cs;
	}
	release_all(outs, o->mesh.nout);
	return ok;
}

/* Next frame's ROI from this frame's landmarks: rotated bounding box, 1.5 times its size. */
static struct roi roi_from_landmarks(const float *lx, const float *ly)
{
	struct roi r;
	r.angle = atan2f(ly[263] - ly[33], lx[263] - lx[33]);
	float cs = cosf(r.angle), sn = sinf(r.angle);
	float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
	for (int i = 0; i < 468; i++) {
		float u = lx[i] * cs + ly[i] * sn, v = -lx[i] * sn + ly[i] * cs;
		u0 = fminf(u0, u);
		u1 = fmaxf(u1, u);
		v0 = fminf(v0, v);
		v1 = fmaxf(v1, v);
	}
	float uc = (u0 + u1) / 2, vc = (v0 + v1) / 2;
	r.cx = uc * cs - vc * sn;
	r.cy = uc * sn + vc * cs;
	r.size = fmaxf(u1 - u0, v1 - v0) * 1.5f;
	return r;
}

static struct roi roi_from_detection(const struct det *d)
{
	struct roi r;
	r.angle = atan2f(d->ky[1] - d->ky[0], d->kx[1] - d->kx[0]); /* eye to eye */
	r.cx = d->cx;
	r.cy = d->cy;
	r.size = fmaxf(d->w, d->h) * 1.5f;
	return r;
}

static void to_face(const float *lx, const float *ly, int w, int h, struct pa_face *f)
{
	float fw = (float)w, fh = (float)h;
	float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
	for (int i = 0; i < 468; i++) {
		x0 = fminf(x0, lx[i]);
		x1 = fmaxf(x1, lx[i]);
		y0 = fminf(y0, ly[i]);
		y1 = fmaxf(y1, ly[i]);
	}
	f->x = x0 / fw;
	f->y = y0 / fh;
	f->w = (x1 - x0) / fw;
	f->h = (y1 - y0) / fh;
	pa_pt eyes[2];
	float rad[2];
	const int *sets[2] = {EYE_A, EYE_B};
	for (int e = 0; e < 2; e++) {
		float sx = 0, sy = 0, m = 0;
		for (int i = 0; i < COUNT(EYE_A); i++) {
			sx += lx[sets[e][i]];
			sy += ly[sets[e][i]];
		}
		sx /= COUNT(EYE_A);
		sy /= COUNT(EYE_A);
		for (int i = 0; i < COUNT(EYE_A); i++)
			m = fmaxf(m, fabsf(lx[sets[e][i]] - sx));
		eyes[e] = (pa_pt){sx / fw, sy / fh};
		rad[e] = m / fw;
	}
	bool swap = eyes[0].x > eyes[1].x; /* store by position in the image */
	f->eye_l = eyes[swap ? 1 : 0];
	f->eye_r = eyes[swap ? 0 : 1];
	f->eye_rl = rad[swap ? 1 : 0];
	f->eye_rr = rad[swap ? 0 : 1];
	pa_pt jaw[COUNT(JAW)];
	for (int i = 0; i < COUNT(JAW); i++)
		jaw[i] = (pa_pt){lx[JAW[i]] / fw, ly[JAW[i]] / fh};
	if (jaw[0].x > jaw[COUNT(JAW) - 1].x)
		for (int i = 0; i < COUNT(JAW) / 2; i++) {
			pa_pt t = jaw[i];
			jaw[i] = jaw[COUNT(JAW) - 1 - i];
			jaw[COUNT(JAW) - 1 - i] = t;
		}
	pa_resample(jaw, COUNT(JAW), f->contour, PA_CONTOUR);
	f->ncontour = PA_CONTOUR;
	f->nose = (pa_pt){lx[NOSE_TIP] / fw, ly[NOSE_TIP] / fh};
	for (int i = 0; i < PA_LIP_PTS; i++) {
		f->lip_outer[i] = (pa_pt){lx[LIPS_OUT[i]] / fw, ly[LIPS_OUT[i]] / fh};
		f->lip_inner[i] = (pa_pt){lx[LIPS_IN[i]] / fw, ly[LIPS_IN[i]] / fh};
	}
	f->nlip_outer = f->nlip_inner = PA_LIP_PTS;
	pa_finish_face(f, fw / fh);
}

/* ------------------------------------------------------------------ segmentation */

static uint8_t *segment(struct onnx *o, const uint8_t *img, int w, int h, int stride)
{
	float *in = scratch(o, 3 * SEG_SIZE * SEG_SIZE);
	for (int y = 0; y < SEG_SIZE; y++)
		for (int x = 0; x < SEG_SIZE; x++) {
			float rgb[3];
			sample(img, w, h, stride, ((float)x + 0.5f) * w / SEG_SIZE, ((float)y + 0.5f) * h / SEG_SIZE, rgb);
			put(in, o->seg.nchw, SEG_SIZE, x, y, rgb, 1.0f, 0.0f);
		}
	OrtValue *outs[4];
	if (!run(o, &o->seg, in, SEG_SIZE, outs))
		return NULL;
	uint8_t *mask = NULL;
	if (o->seg.nout > 0 && elements(outs[0]) == SEG_SIZE * SEG_SIZE) {
		const float *p = data_of(outs[0]);
		mask = p ? malloc(SEG_SIZE * SEG_SIZE) : NULL;
		for (int i = 0; mask && i < SEG_SIZE * SEG_SIZE; i++) {
			float v = p[i] < 0 ? 0 : p[i] > 1 ? 1 : p[i];
			mask[i] = (uint8_t)(v * 255.0f + 0.5f);
		}
	}
	release_all(outs, o->seg.nout);
	return mask;
}

/* ------------------------------------------------------------------ one frame */

bool pa_backend_run(void *ptr, const uint8_t *img, int w, int h, int stride, int want, struct pa_backend_out *out)
{
	struct onnx *o = ptr;
	uint64_t t0 = os_gettime_ns();
	o->frame++;

	/* detect when nothing is tracked, and now and then to pick up new faces */
	if (o->ntrack == 0 || o->frame % DETECT_EVERY == 0) {
		struct det d[PA_MAX_FACES];
		int nd = detect(o, img, w, h, stride, d, PA_MAX_FACES);
		for (int i = 0; i < nd; i++) {
			struct roi r = roi_from_detection(&d[i]);
			bool known = false; /* already tracked? */
			for (int k = 0; k < o->ntrack; k++)
				if (hypotf(o->track[k].cx - r.cx, o->track[k].cy - r.cy) < o->track[k].size * 0.3f)
					known = true;
			if (!known && o->ntrack < PA_MAX_FACES)
				o->track[o->ntrack++] = r;
		}
	}

	static float lx[468], ly[468]; /* worker thread only */
	int kept = 0;
	for (int i = 0; i < o->ntrack; i++) {
		if (!mesh(o, img, w, h, stride, &o->track[i], lx, ly))
			continue; /* lost: dropped, re-detected on a later frame */
		to_face(lx, ly, w, h, &out->faces[out->nfaces++]);
		o->track[kept++] = roi_from_landmarks(lx, ly);
	}
	o->ntrack = kept;
	out->face_ms = (double)(os_gettime_ns() - t0) / 1e6;

	if (want & PA_WANT_PERSON) {
		uint64_t t1 = os_gettime_ns();
		out->mask = segment(o, img, w, h, stride);
		if (out->mask)
			out->mask_w = out->mask_h = SEG_SIZE;
		out->mask_ms = (double)(os_gettime_ns() - t1) / 1e6;
	}
	return true;
}
