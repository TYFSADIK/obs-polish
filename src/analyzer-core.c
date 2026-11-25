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
	uint8_t *mask, *mouth, *eyes;
	int mask_cap, mouth_cap, eyes_cap;
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
	/* backends without nose wings or bridge: place them from the eyes and the nose tip */
	float ed = hypotf(f->eye_r.x - f->eye_l.x, f->eye_r.y - f->eye_l.y);
	if (f->nose_l.x == 0 && f->nose_r.x == 0) {
		f->nose_l = (pa_pt){f->nose.x - ed * 0.2f, f->nose.y - (f->nose.y - mid.y) * 0.08f};
		f->nose_r = (pa_pt){f->nose.x + ed * 0.2f, f->nose.y - (f->nose.y - mid.y) * 0.08f};
	}
	if (f->nose_top.x == 0 && f->nose_top.y == 0)
		f->nose_top = mid;
	for (int e = 0; e < 2; e++) /* until the frame is looked at: the eye centre */
		if (f->iris_r[e] == 0) {
			f->iris[e] = e ? f->eye_r : f->eye_l;
			f->iris_r[e] = (e ? f->eye_rr : f->eye_rl) * 0.45f;
		}
}

/* Eyebrow centre line from its points, whether the backend gives an outline (MediaPipe: upper
 * edge then lower edge) or a single line (Vision). Work in the face's own frame (roll removed),
 * cut the brow into PA_BROW_PTS slices across its length, and take the middle and the spread of
 * the outline at each slice. Output runs from the image-left end to the image-right end. */
void pa_brow_from_points(const pa_pt *pts, int n, float roll, float aspect, pa_pt *line, float *thick)
{
	*thick = 0;
	if (n < 2) {
		for (int i = 0; i < PA_BROW_PTS; i++)
			line[i] = n ? pts[0] : (pa_pt){0, 0};
		return;
	}
	if (n > 64)
		n = 64;
	float cs = cosf(roll), sn = sinf(roll), cx = 0, cy = 0;
	for (int i = 0; i < n; i++) {
		cx += pts[i].x * aspect;
		cy += pts[i].y;
	}
	cx /= n;
	cy /= n;
	float u[64], v[64], u0 = 1e9f, u1 = -1e9f;
	for (int i = 0; i < n; i++) { /* isotropic units (image height = 1), rotated by -roll */
		float x = pts[i].x * aspect - cx, y = pts[i].y - cy;
		u[i] = cs * x + sn * y;
		v[i] = -sn * x + cs * y;
		u0 = fminf(u0, u[i]);
		u1 = fmaxf(u1, u[i]);
	}
	/* closed if the ends meet (an outline), open if they are the brow's two ends (a line) */
	bool closed = hypotf(u[n - 1] - u[0], v[n - 1] - v[0]) < 0.35f * (u1 - u0);
	int nseg = closed ? n : n - 1;
	float spread[PA_BROW_PTS], mv[PA_BROW_PTS];
	bool got[PA_BROW_PTS];
	for (int k = 0; k < PA_BROW_PTS; k++) {
		float uk = u0 + (u1 - u0) * (0.04f + 0.92f * (float)k / (PA_BROW_PTS - 1));
		float lo = 1e9f, hi = -1e9f;
		for (int i = 0; i < nseg; i++) {
			int j = (i + 1) % n;
			if ((u[i] <= uk && u[j] >= uk) || (u[j] <= uk && u[i] >= uk)) {
				float du = u[j] - u[i];
				float vk = fabsf(du) > 1e-7f ? v[i] + (uk - u[i]) / du * (v[j] - v[i]) : (v[i] + v[j]) * 0.5f;
				lo = fminf(lo, vk);
				hi = fmaxf(hi, vk);
			}
		}
		got[k] = lo <= hi;
		mv[k] = got[k] ? (lo + hi) * 0.5f : 0;
		spread[k] = got[k] ? hi - lo : 0;
		float back_x = cs * uk - sn * mv[k], back_y = sn * uk + cs * mv[k];
		line[k] = (pa_pt){(back_x + cx) / aspect, back_y + cy};
	}
	/* thickness: the median spread, ignoring the tapered ends */
	float sp[PA_BROW_PTS];
	int m = 0;
	for (int k = 1; k < PA_BROW_PTS - 1; k++)
		if (got[k])
			sp[m++] = spread[k];
	for (int i = 1; i < m; i++)
		for (int j = i; j > 0 && sp[j - 1] > sp[j]; j--) {
			float t = sp[j];
			sp[j] = sp[j - 1];
			sp[j - 1] = t;
		}
	*thick = m ? sp[m / 2] : 0;
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
	S(nose_l.x); S(nose_l.y); S(nose_r.x); S(nose_r.y); S(nose_top.x); S(nose_top.y);
	for (int e = 0; e < 2; e++) {
		S(iris[e].x);
		S(iris[e].y);
		S(iris_r[e]);
		S(brow_thick[e]);
		if (f->neye == prev->neye)
			for (int i = 0; i < f->neye; i++) {
				S(eye_poly[e][i].x);
				S(eye_poly[e][i].y);
			}
		if (f->nbrow == prev->nbrow)
			for (int i = 0; i < f->nbrow; i++) {
				S(brow_line[e][i].x);
				S(brow_line[e][i].y);
			}
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

/* ------------------------------------------------------------------ eyes */

typedef struct {
	float x, y;
} v2;

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float sstep(float a, float b, float x)
{
	float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

/* Distance from p to a polyline, and where along it (0 at the first point, 1 at the last). */
static float polyline_dist(v2 p, const v2 *pts, int n, float *along)
{
	float best = 1e9f, total = 0, at = 0, run = 0;
	for (int i = 0; i + 1 < n; i++)
		total += hypotf(pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y);
	for (int i = 0; i + 1 < n; i++) {
		v2 a = pts[i], b = pts[i + 1];
		float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
		float t = len2 > 0 ? clampf(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f) : 0;
		float qx = a.x + dx * t - p.x, qy = a.y + dy * t - p.y, d = sqrtf(qx * qx + qy * qy), seg = sqrtf(len2);
		if (d < best) {
			best = d;
			at = run + seg * t;
		}
		run += seg;
	}
	*along = total > 0 ? at / total : 0;
	return best;
}

/* Signed distance to a closed polygon: positive inside. */
static float polygon_sdist(v2 p, const v2 *pts, int n)
{
	bool in = false;
	float best = 1e9f;
	for (int i = 0, j = n - 1; i < n; j = i++) {
		v2 a = pts[j], b = pts[i];
		if ((b.y > p.y) != (a.y > p.y) && p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x)
			in = !in;
		float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
		float t = len2 > 0 ? clampf(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f) : 0;
		best = fminf(best, hypotf(a.x + dx * t - p.x, a.y + dy * t - p.y));
	}
	return in ? best : -best;
}

/* The two ends of an eye outline along the face's horizontal, and the upper lid between them,
 * from the inner corner (nearest the nose) to the outer. In pixels. Returns the lid's point count. */
static int upper_lid(const pa_pt *poly, int n, float roll, bool image_left, int w, int h, v2 *lid)
{
	float cs = cosf(roll), sn = sinf(roll);
	int a = 0, b = 0;
	float umin = 1e9f, umax = -1e9f;
	for (int i = 0; i < n; i++) {
		float u = cs * poly[i].x * w + sn * poly[i].y * h;
		if (u < umin) {
			umin = u;
			a = i;
		}
		if (u > umax) {
			umax = u;
			b = i;
		}
	}
	/* the two arcs from a to b; the upper one has the smaller face-frame y */
	float m1 = 0, m2 = 0;
	int c1 = 0, c2 = 0;
	for (int i = a; i != b; i = (i + 1) % n, c1++)
		m1 += -sn * poly[i].x * w + cs * poly[i].y * h;
	for (int i = b; i != a; i = (i + 1) % n, c2++)
		m2 += -sn * poly[i].x * w + cs * poly[i].y * h;
	bool first_upper = c1 && c2 ? m1 / c1 < m2 / c2 : true;
	int k = 0;
	v2 arc[PA_EYE_PTS + 1];
	if (first_upper)
		for (int i = a;; i = (i + 1) % n) {
			arc[k++] = (v2){poly[i].x * w, poly[i].y * h};
			if (i == b || k > PA_EYE_PTS)
				break;
		}
	else
		for (int i = a;; i = (i - 1 + n) % n) {
			arc[k++] = (v2){poly[i].x * w, poly[i].y * h};
			if (i == b || k > PA_EYE_PTS)
				break;
		}
	/* arc runs from the face-frame left corner (a) to the right corner (b). For the image-left
	 * eye the inner corner is on the right, so reverse it. */
	for (int i = 0; i < k; i++)
		lid[i] = image_left ? arc[k - 1 - i] : arc[i];
	return k;
}

/* Height of the upper lid above the inner corner at a point along the eye; past the outer corner
 * it keeps rising gently, so eyeshadow and liner sweep up at the outer end. */
static float lid_height(const float *la, const float *lh, int n, float along, float ew)
{
	if (along <= la[0])
		return lh[0];
	if (along >= la[n - 1])
		return lh[n - 1] + (along - la[n - 1]) * ew * 0.25f;
	for (int i = 1; i < n; i++)
		if (along <= la[i]) {
			float d = la[i] - la[i - 1];
			return d > 1e-6f ? lh[i - 1] + (along - la[i - 1]) / d * (lh[i] - lh[i - 1]) : lh[i];
		}
	return lh[n - 1];
}

/* Iris: the centre of the dark part of each eye, weighted by how much darker than the eye's
 * average each pixel is. Radius from the eye's width (an iris is about 40% of it across). */
static void find_iris(struct pa_face *f, const uint8_t *bgra, int w, int h)
{
	for (int e = 0; e < 2 && f->neye >= 3; e++) {
		v2 poly[PA_EYE_PTS];
		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
		for (int i = 0; i < f->neye; i++) {
			poly[i] = (v2){f->eye_poly[e][i].x * w, f->eye_poly[e][i].y * h};
			x0 = fminf(x0, poly[i].x);
			x1 = fmaxf(x1, poly[i].x);
			y0 = fminf(y0, poly[i].y);
			y1 = fmaxf(y1, poly[i].y);
		}
		float ew = 0;
		for (int i = 0; i < f->neye; i++)
			for (int j = i + 1; j < f->neye; j++)
				ew = fmaxf(ew, hypotf(poly[i].x - poly[j].x, poly[i].y - poly[j].y));
		int ix0 = (int)fmaxf(0, floorf(x0)), ix1 = (int)fminf((float)w - 1, ceilf(x1));
		int iy0 = (int)fmaxf(0, floorf(y0)), iy1 = (int)fminf((float)h - 1, ceilf(y1));
		float sum = 0, cnt = 0;
		for (int y = iy0; y <= iy1; y++)
			for (int x = ix0; x <= ix1; x++)
				if (polygon_sdist((v2){x + 0.5f, y + 0.5f}, poly, f->neye) > 0.5f) {
					const uint8_t *px = bgra + ((size_t)y * w + x) * 4;
					sum += 0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2];
					cnt++;
				}
		if (cnt < 4)
			continue;
		float mean = sum / cnt, wx = 0, wy = 0, ws = 0;
		for (int y = iy0; y <= iy1; y++)
			for (int x = ix0; x <= ix1; x++)
				if (polygon_sdist((v2){x + 0.5f, y + 0.5f}, poly, f->neye) > 0.5f) {
					const uint8_t *px = bgra + ((size_t)y * w + x) * 4;
					float d = mean - (0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2]);
					if (d > 0) {
						wx += (x + 0.5f) * d * d;
						wy += (y + 0.5f) * d * d;
						ws += d * d;
					}
				}
		if (ws <= 0)
			continue;
		f->iris[e] = (pa_pt){wx / ws / w, wy / ws / h};
		f->iris_r[e] = ew * 0.2f / w;
	}
}

/* Eye makeup mask for every face: brows (R), eyeshadow (G), eyeliner (B), iris (A). Soft edges
 * from distances, so the mask stays smooth when it is stretched up to the frame. */
static uint8_t *eyes_mask(const struct pa_face *faces, int nfaces, int w, int h)
{
	uint8_t *out = calloc((size_t)w * h * 4, 1);
	for (int fi = 0; fi < nfaces; fi++) {
		const struct pa_face *f = &faces[fi];
		if (f->neye < 3)
			continue;
		float cs = cosf(f->roll), sn = sinf(f->roll);
		for (int e = 0; e < 2; e++) {
			v2 poly[PA_EYE_PTS], lid[PA_EYE_PTS + 1];
			for (int i = 0; i < f->neye; i++)
				poly[i] = (v2){f->eye_poly[e][i].x * w, f->eye_poly[e][i].y * h};
			int nl = upper_lid(f->eye_poly[e], f->neye, f->roll, e == 0, w, h, lid);
			if (nl < 2)
				continue;
			v2 inner = lid[0], outer = lid[nl - 1];
			float ew = hypotf(outer.x - inner.x, outer.y - inner.y);
			if (ew < 2.0f)
				continue;
			/* face-frame axes in pixels: along = towards the outer corner, up = towards the brow */
			float ax = (outer.x - inner.x) / ew, ay = (outer.y - inner.y) / ew;
			float upx = sn, upy = -cs; /* face "up", rotated with the head */
			/* eyeliner: the upper lid raised a little onto the lashes, plus a short wing */
			float lt = fmaxf(ew * 0.085f, 1.4f);
			v2 liner[PA_EYE_PTS + 2];
			for (int i = 0; i < nl; i++)
				liner[i] = (v2){lid[i].x + upx * lt * 0.35f, lid[i].y + upy * lt * 0.35f};
			liner[nl] = (v2){outer.x + ax * ew * 0.2f + upx * ew * 0.1f, outer.y + ay * ew * 0.2f + upy * ew * 0.1f};
			int nliner = nl + 1;
			float wing_at = 0; /* where the wing starts, as a fraction of the liner's length */
			{
				float tot = 0, lidlen = 0;
				for (int i = 0; i + 1 < nliner; i++) {
					float s = hypotf(liner[i + 1].x - liner[i].x, liner[i + 1].y - liner[i].y);
					tot += s;
					if (i + 1 < nl)
						lidlen += s;
				}
				wing_at = tot > 0 ? lidlen / tot : 1;
			}
			/* brow (pixels), inner end first */
			v2 brow[PA_BROW_PTS];
			bool has_brow = f->nbrow >= 2;
			float bt = fmaxf(f->brow_thick[e] * h, ew * 0.16f);
			for (int i = 0; has_brow && i < f->nbrow; i++)
				brow[i] = (v2){f->brow_line[e][i].x * w, f->brow_line[e][i].y * h};
			/* lid-to-brow distance at the eye's middle, for the eyeshadow */
			v2 mid = lid[nl / 2];
			float gap = ew * 0.75f;
			if (has_brow) {
				float along;
				gap = polyline_dist(mid, brow, f->nbrow, &along) - bt * 0.5f;
				gap = clampf(gap, ew * 0.45f, ew * 1.1f);
			}
			float la[PA_EYE_PTS + 1], lh[PA_EYE_PTS + 1]; /* each lid point: along the eye, and height */
			for (int i = 0; i < nl; i++) {
				float rx = lid[i].x - inner.x, ry = lid[i].y - inner.y;
				la[i] = (rx * ax + ry * ay) / ew;
				lh[i] = rx * upx + ry * upy;
			}
			v2 iris = {f->iris[e].x * w, f->iris[e].y * h};
			float ir = f->iris_r[e] * w;
			/* work area: around the eye and its brow */
			float x0 = fminf(inner.x, outer.x) - ew * 0.6f, x1 = fmaxf(inner.x, outer.x) + ew * 0.6f;
			float y0 = fminf(inner.y, outer.y) - ew * 1.4f, y1 = fmaxf(inner.y, outer.y) + ew * 0.5f;
			for (int i = 0; has_brow && i < f->nbrow; i++) {
				x0 = fminf(x0, brow[i].x - bt);
				x1 = fmaxf(x1, brow[i].x + bt);
				y0 = fminf(y0, brow[i].y - bt);
			}
			int ix0 = (int)fmaxf(0, x0), ix1 = (int)fminf((float)w - 1, x1);
			int iy0 = (int)fmaxf(0, y0), iy1 = (int)fminf((float)h - 1, y1);
			for (int y = iy0; y <= iy1; y++)
				for (int x = ix0; x <= ix1; x++) {
					v2 p = {x + 0.5f, y + 0.5f};
					uint8_t *o = out + ((size_t)y * w + x) * 4;
					float t;
					/* R: eyebrow, thicker at the inner end, tapering to the tail */
					if (has_brow) {
						float d = polyline_dist(p, brow, f->nbrow, &t);
						float half = bt * 0.5f * (1.0f - 0.35f * t);
						float v = (1.0f - sstep(half - 0.8f, half + 0.8f, d)) * (1.0f - 0.6f * sstep(0.82f, 1.0f, t));
						o[0] = (uint8_t)fmaxf(o[0], v * 255.0f);
					}
					/* G: eyeshadow, strongest on the lid crease, fading towards the brow */
					{
						float rx = p.x - inner.x, ry = p.y - inner.y;
						float along = (rx * ax + ry * ay) / ew; /* 0 inner corner, 1 outer */
						float hp = rx * upx + ry * upy;       /* height above the inner corner */
						float s = (hp - lid_height(la, lh, nl, along, ew)) / gap;
						float v = sstep(-0.02f, 0.12f, s) * (1.0f - sstep(0.6f, 1.15f, s)) * sstep(-0.15f, 0.08f, along) *
							  (1.0f - sstep(1.0f, 1.3f, along));
						o[1] = (uint8_t)fmaxf(o[1], v * 255.0f);
					}
					/* B: eyeliner, thin at the inner corner, full along the lid, tapering along the wing */
					{
						float d = polyline_dist(p, liner, nliner, &t);
						float half = lt * 0.5f * (0.35f + 0.65f * sstep(0.0f, 0.5f, t));
						if (t > wing_at)
							half *= 1.0f - (t - wing_at) / fmaxf(1.0f - wing_at, 1e-3f) * 0.85f;
						float v = 1.0f - sstep(half - 0.6f, half + 0.6f, d);
						o[2] = (uint8_t)fmaxf(o[2], v * 255.0f);
					}
					/* A: iris, inside the eye outline, without the pupil */
					{
						float inside = sstep(0.0f, 1.0f, polygon_sdist(p, poly, f->neye));
						float d = hypotf(p.x - iris.x, p.y - iris.y);
						float v = inside * (1.0f - sstep(ir * 0.9f - 0.5f, ir + 0.5f, d)) * sstep(ir * 0.28f, ir * 0.48f, d);
						o[3] = (uint8_t)fmaxf(o[3], v * 255.0f);
					}
				}
		}
	}
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
	for (int i = 0; ok && i < r.nfaces; i++)
		find_iris(&r.faces[i], p->frame, w, h);
	uint8_t *mouth = NULL, *eyes = NULL;
	if (ok && (want & PA_WANT_MOUTH) && r.nfaces > 0)
		mouth = mouth_mask(r.faces, r.nfaces, w, h);

	lock(p);
	if (p->have_prev && r.nfaces == p->res.nfaces)
		for (int i = 0; i < r.nfaces; i++)
			smooth_face(&r.faces[i], &p->res.faces[i], 0.55f);
	unlock(p);
	/* the eye mask is drawn from the smoothed landmarks, so it moves as steadily as they do */
	if (ok && (want & PA_WANT_EYES) && r.nfaces > 0)
		eyes = eyes_mask(r.faces, r.nfaces, w, h);
	lock(p);
	r.seq = p->res.seq + 1;
	r.mask_seq = p->res.mask_seq;
	r.mouth_seq = p->res.mouth_seq;
	r.eyes_seq = p->res.eyes_seq;
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
	if (eyes) {
		int need = w * h * 4;
		if (need > p->eyes_cap) {
			p->eyes = realloc(p->eyes, need);
			p->eyes_cap = need;
		}
		memcpy(p->eyes, eyes, need);
		r.eyes_w = w;
		r.eyes_h = h;
		r.eyes_seq++;
	}
	p->res = r;
	p->have_prev = true;
	unlock(p);
	free(bo.mask);
	free(mouth);
	free(eyes);
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
	free(p->eyes);
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

bool pa_get(void *ptr, struct pa_result *out, uint8_t *mask_buf, int cap, uint8_t *mouth_buf, int mouth_cap,
	    uint8_t *eyes_buf, int eyes_cap)
{
	struct pa *p = ptr;
	lock(p);
	*out = p->res;
	if (mask_buf && p->mask && out->mask_w * out->mask_h <= cap)
		memcpy(mask_buf, p->mask, (size_t)out->mask_w * out->mask_h);
	if (mouth_buf && p->mouth && out->mouth_w * out->mouth_h * 2 <= mouth_cap)
		memcpy(mouth_buf, p->mouth, (size_t)out->mouth_w * out->mouth_h * 2);
	if (eyes_buf && p->eyes && out->eyes_w * out->eyes_h * 4 <= eyes_cap)
		memcpy(eyes_buf, p->eyes, (size_t)out->eyes_w * out->eyes_h * 4);
	unlock(p);
	return out->seq > 0;
}
