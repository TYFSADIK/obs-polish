/* The analyzer core: one worker thread per filter, the latest result, temporal smoothing and
 * the lips mask. Frames arrive small (a few hundred pixels wide) from the render thread; if the
 * worker is still busy, the frame is dropped, so analysis never blocks rendering. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <util/bmem.h>
#include <util/threading.h>
#include <util/platform.h>
#include "analyzer.h"

struct pa {
	void *backend;
	const char *error;

	pthread_t thread;
	bool thread_ok;
	os_sem_t *wake;
	volatile bool stop;
	volatile long busy; /* 1 while a frame is queued or being analysed */

	/* the frame handed to the worker */
	uint8_t *frame;
	int fw, fh, fstride, fwant, fcap;

	/* the latest result, guarded by a short spin lock */
	volatile long lock;
	struct pa_result res;
	uint8_t *mask, *mouth;
	int mask_cap, mouth_cap;
	bool have_prev;
};

static void lock(struct pa *p)
{
	while (os_atomic_set_long(&p->lock, 1) == 1)
		os_sleep_ms(0);
}
static void unlock(struct pa *p) { os_atomic_set_long(&p->lock, 0); }

/* ------------------------------------------------------------------ shared helpers */

void pa_resample(const pa_pt *in, int n, pa_pt *out, int m)
{
	if (n < 2) {
		for (int i = 0; i < m; i++)
			out[i] = n ? in[0] : (pa_pt){0, 0};
		return;
	}
	float len[512];
	if (n > 512)
		n = 512;
	len[0] = 0;
	for (int i = 1; i < n; i++)
		len[i] = len[i - 1] + hypotf(in[i].x - in[i - 1].x, in[i].y - in[i - 1].y);
	for (int k = 0; k < m; k++) {
		float t = len[n - 1] * (float)k / (float)(m - 1);
		int i = 1;
		while (i < n - 1 && len[i] < t)
			i++;
		float seg = len[i] - len[i - 1], u = seg > 0 ? (t - len[i - 1]) / seg : 0;
		out[k] = (pa_pt){in[i - 1].x + (in[i].x - in[i - 1].x) * u, in[i - 1].y + (in[i].y - in[i - 1].y) * u};
	}
}

void pa_finish_face(struct pa_face *f, float aspect)
{
	f->chin = f->contour[PA_CONTOUR / 2];
	pa_pt mid = {(f->eye_l.x + f->eye_r.x) / 2, (f->eye_l.y + f->eye_r.y) / 2};
	f->brow = (pa_pt){mid.x - (f->chin.x - mid.x) * 0.25f, mid.y - (f->chin.y - mid.y) * 0.25f};
	f->roll = atan2f((f->eye_r.y - f->eye_l.y) / aspect, f->eye_r.x - f->eye_l.x);
}

static void smooth_face(struct pa_face *f, const struct pa_face *prev, float a)
{
#define S(v) f->v = prev->v + (f->v - prev->v) * a
	S(x); S(y); S(w); S(h); S(eye_l.x); S(eye_l.y); S(eye_r.x); S(eye_r.y); S(eye_rl); S(eye_rr);
	S(nose.x); S(nose.y); S(chin.x); S(chin.y); S(brow.x); S(brow.y); S(roll);
	for (int i = 0; i < PA_CONTOUR; i++) {
		S(contour[i].x);
		S(contour[i].y);
	}
	if (f->nlip_outer == prev->nlip_outer && f->nlip_inner == prev->nlip_inner)
		for (int i = 0; i < PA_LIP_PTS; i++) {
			S(lip_outer[i].x);
			S(lip_outer[i].y);
			S(lip_inner[i].x);
			S(lip_inner[i].y);
		}
#undef S
}

/* Even-odd scanline fill of a closed polygon (normalised coordinates) into an 8-bit plane. */
static void fill_polygon(uint8_t *dst, int w, int h, const pa_pt *pts, int n, int stride, int channel)
{
	if (n < 3 || n > PA_LIP_PTS)
		return;
	float xs[64];
	for (int y = 0; y < h; y++) {
		float sy = ((float)y + 0.5f) / (float)h;
		int nx = 0;
		for (int i = 0; i < n && nx < 64; i++) {
			pa_pt a = pts[i], b = pts[(i + 1) % n];
			if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy))
				xs[nx++] = (a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x)) * (float)w;
		}
		for (int i = 1; i < nx; i++) /* insertion sort: a handful of crossings */
			for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
				float t = xs[j];
				xs[j] = xs[j - 1];
				xs[j - 1] = t;
			}
		for (int i = 0; i + 1 < nx; i += 2) {
			int x0 = (int)ceilf(xs[i] - 0.5f), x1 = (int)floorf(xs[i + 1] - 0.5f);
			if (x0 < 0)
				x0 = 0;
			if (x1 >= w)
				x1 = w - 1;
			for (int x = x0; x <= x1; x++)
				dst[(y * w + x) * stride + channel] ^= 0xFF;
		}
	}
}

/* Lips (outer minus inner, so an open mouth stays uncoloured) in R, mouth interior in G, with a
 * 3x3 box blur for soft edges. */
static uint8_t *mouth_mask(const struct pa_face *faces, int nfaces, int w, int h)
{
	uint8_t *raw = calloc((size_t)w * h * 2, 1);
	for (int i = 0; i < nfaces; i++) {
		const struct pa_face *f = &faces[i];
		uint8_t *one = calloc((size_t)w * h * 2, 1);
		fill_polygon(one, w, h, f->lip_outer, f->nlip_outer, 2, 0);
		fill_polygon(one, w, h, f->lip_inner, f->nlip_inner, 2, 0); /* even-odd: removes the opening */
		fill_polygon(one, w, h, f->lip_inner, f->nlip_inner, 2, 1);
		for (size_t k = 0; k < (size_t)w * h * 2; k++)
			raw[k] |= one[k];
		free(one);
	}
	uint8_t *out = malloc((size_t)w * h * 2);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			for (int c = 0; c < 2; c++) {
				int s = 0, cnt = 0;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						int xx = x + dx, yy = y + dy;
						if (xx < 0 || yy < 0 || xx >= w || yy >= h)
							continue;
						s += raw[(yy * w + xx) * 2 + c];
						cnt++;
					}
				out[(y * w + x) * 2 + c] = (uint8_t)(s / cnt);
			}
	free(raw);
	return out;
}

/* ------------------------------------------------------------------ the worker */

static void analyse_one(struct pa *p)
{
	struct pa_backend_out bo;
	memset(&bo, 0, sizeof bo);
	int w = p->fw, h = p->fh, want = p->fwant;
	bool ok = pa_backend_run(p->backend, p->frame, w, h, p->fstride, want, &bo);

	struct pa_result r;
	memset(&r, 0, sizeof r);
	r.aspect = (float)w / (float)h;
	r.backend = pa_backend_name();
	if (ok) {
		r.face_ms = bo.face_ms;
		r.mask_ms = bo.mask_ms;
		/* largest first */
		for (int i = 0; i < bo.nfaces && i < PA_MAX_FACES; i++)
			r.faces[r.nfaces++] = bo.faces[i];
		if (r.nfaces == 2 && r.faces[1].w * r.faces[1].h > r.faces[0].w * r.faces[0].h) {
			struct pa_face t = r.faces[0];
			r.faces[0] = r.faces[1];
			r.faces[1] = t;
		}
	}
	uint8_t *mouth = NULL;
	if (ok && (want & PA_WANT_MOUTH) && r.nfaces > 0)
		mouth = mouth_mask(r.faces, r.nfaces, w, h);

	lock(p);
	if (p->have_prev && r.nfaces == p->res.nfaces)
		for (int i = 0; i < r.nfaces; i++)
			smooth_face(&r.faces[i], &p->res.faces[i], 0.55f);
	r.seq = p->res.seq + 1;
	r.mask_seq = p->res.mask_seq;
	r.mouth_seq = p->res.mouth_seq;
	if (bo.mask) {
		int need = bo.mask_w * bo.mask_h;
		if (need > p->mask_cap) {
			p->mask = realloc(p->mask, need);
			p->mask_cap = need;
		}
		memcpy(p->mask, bo.mask, need);
		r.mask_w = bo.mask_w;
		r.mask_h = bo.mask_h;
		r.mask_seq++;
	} else if (want & PA_WANT_PERSON) {
		r.mask_w = p->res.mask_w; /* keep the last mask until a new one arrives */
		r.mask_h = p->res.mask_h;
	}
	if (mouth) {
		int need = w * h * 2;
		if (need > p->mouth_cap) {
			p->mouth = realloc(p->mouth, need);
			p->mouth_cap = need;
		}
		memcpy(p->mouth, mouth, need);
		r.mouth_w = w;
		r.mouth_h = h;
		r.mouth_seq++;
	}
	p->res = r;
	p->have_prev = true;
	unlock(p);
	free(bo.mask);
	free(mouth);
}

static void *worker(void *data)
{
	struct pa *p = data;
	os_set_thread_name("obs-polish analysis");
	while (os_sem_wait(p->wake) == 0 && !p->stop) {
		if (p->backend)
			analyse_one(p);
		os_atomic_set_long(&p->busy, 0);
	}
	return NULL;
}

void *pa_create(void)
{
	struct pa *p = calloc(1, sizeof(*p));
	p->backend = pa_backend_create(&p->error);
	if (os_sem_init(&p->wake, 0) == 0 && pthread_create(&p->thread, NULL, worker, p) == 0)
		p->thread_ok = true;
	return p;
}

void pa_destroy(void *ptr)
{
	struct pa *p = ptr;
	if (!p)
		return;
	if (p->thread_ok) {
		p->stop = true;
		os_sem_post(p->wake);
		pthread_join(p->thread, NULL);
	}
	os_sem_destroy(p->wake);
	if (p->backend)
		pa_backend_destroy(p->backend);
	free(p->frame);
	free(p->mask);
	free(p->mouth);
	free(p);
}

const char *pa_error(void *ptr)
{
	struct pa *p = ptr;
	return p->backend ? NULL : (p->error ? p->error : "analysis unavailable");
}

bool pa_submit(void *ptr, const uint8_t *bgra, int w, int h, int stride, int want)
{
	struct pa *p = ptr;
	if (!p->thread_ok || !p->backend)
		return false;
	if (os_atomic_compare_swap_long(&p->busy, 0, 1) == false)
		return false;
	int need = w * h * 4;
	if (need > p->fcap) {
		p->frame = realloc(p->frame, need);
		p->fcap = need;
	}
	for (int y = 0; y < h; y++)
		memcpy(p->frame + (size_t)y * w * 4, bgra + (size_t)y * stride, (size_t)w * 4);
	p->fw = w;
	p->fh = h;
	p->fstride = w * 4;
	p->fwant = want;
	os_sem_post(p->wake);
	return true;
}

bool pa_get(void *ptr, struct pa_result *out, uint8_t *mask_buf, int cap, uint8_t *mouth_buf, int mouth_cap)
{
	struct pa *p = ptr;
	lock(p);
	*out = p->res;
	if (mask_buf && p->mask && out->mask_w * out->mask_h <= cap)
		memcpy(mask_buf, p->mask, (size_t)out->mask_w * out->mask_h);
	if (mouth_buf && p->mouth && out->mouth_w * out->mouth_h * 2 <= mouth_cap)
		memcpy(mouth_buf, p->mouth, (size_t)out->mouth_w * out->mouth_h * 2);
	unlock(p);
	return out->seq > 0;
}
