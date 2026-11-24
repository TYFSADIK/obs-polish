/* Face landmarks and person segmentation with Apple's Vision framework.
 * Frames arrive small (a few hundred pixels wide) from the render thread; analysis runs on a
 * serial background queue and never blocks rendering: if it is still busy, the frame is dropped. */
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <os/lock.h>
#include <string.h>
#include "analyzer.h"

@interface PAState : NSObject
@property(nonatomic) dispatch_queue_t queue;
@property(nonatomic) BOOL busy;
@property(nonatomic) VNSequenceRequestHandler *sequence;
@end
@implementation PAState
@end

struct pa {
	void *state;                    /* PAState, retained */
	os_unfair_lock lock;
	struct pa_result res;
	uint8_t *mask;
	int mask_cap;
	uint8_t *mouth;                 /* interleaved R (lips), G (mouth interior) */
	int mouth_cap;
	bool have_prev;
};

void *pa_create(void)
{
	struct pa *p = calloc(1, sizeof(*p));
	PAState *s = [PAState new];
	s.queue = dispatch_queue_create("org.tyfsadik.obs-polish.vision", DISPATCH_QUEUE_SERIAL);
	s.sequence = [VNSequenceRequestHandler new];
	p->state = (__bridge_retained void *)s;
	p->lock = OS_UNFAIR_LOCK_INIT;
	return p;
}

void pa_destroy(void *ptr)
{
	struct pa *p = ptr;
	if (!p)
		return;
	PAState *s = (__bridge_transfer PAState *)p->state;
	dispatch_sync(s.queue, ^{});      /* let any running analysis finish */
	s = nil;
	free(p->mask);
	free(p->mouth);
	free(p);
}

static pa_pt to_pt(CGPoint q, int w, int h) { return (pa_pt){(float)(q.x / w), (float)(1.0 - q.y / h)}; }

static void region(VNFaceLandmarkRegion2D *r, int w, int h, pa_pt *out, int max, int *n)
{
	*n = 0;
	if (!r)
		return;
	const CGPoint *pts = [r pointsInImageOfSize:CGSizeMake(w, h)];
	for (NSUInteger i = 0; i < r.pointCount && *n < max; i++)
		out[(*n)++] = to_pt(pts[i], w, h);
}

static void centre_radius(VNFaceLandmarkRegion2D *r, int w, int h, pa_pt *c, float *rad)
{
	pa_pt pts[32];
	int n;
	region(r, w, h, pts, 32, &n);
	if (n == 0)
		return;
	float sx = 0, sy = 0;
	for (int i = 0; i < n; i++) {
		sx += pts[i].x;
		sy += pts[i].y;
	}
	c->x = sx / n;
	c->y = sy / n;
	float m = 0;
	for (int i = 0; i < n; i++)
		m = fmaxf(m, fabsf(pts[i].x - c->x));
	*rad = m;
}

/* Resample a contour to a fixed number of points along its length. */
static void resample(const pa_pt *in, int n, pa_pt *out, int m)
{
	if (n < 2) {
		for (int i = 0; i < m; i++)
			out[i] = n ? in[0] : (pa_pt){0, 0};
		return;
	}
	float len[64] = {0};
	for (int i = 1; i < n; i++)
		len[i] = len[i - 1] + hypotf(in[i].x - in[i - 1].x, in[i].y - in[i - 1].y);
	for (int k = 0; k < m; k++) {
		float t = len[n - 1] * k / (m - 1);
		int i = 1;
		while (i < n - 1 && len[i] < t)
			i++;
		float seg = len[i] - len[i - 1], u = seg > 0 ? (t - len[i - 1]) / seg : 0;
		out[k] = (pa_pt){in[i - 1].x + (in[i].x - in[i - 1].x) * u, in[i - 1].y + (in[i].y - in[i - 1].y) * u};
	}
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
#undef S
}

static void add_region(CGContextRef ctx, VNFaceLandmarkRegion2D *r, int w, int h)
{
	if (!r || r.pointCount < 3)
		return;
	const CGPoint *pts = [r pointsInImageOfSize:CGSizeMake(w, h)];
	CGContextMoveToPoint(ctx, pts[0].x, pts[0].y);
	for (NSUInteger i = 1; i < r.pointCount; i++)
		CGContextAddLineToPoint(ctx, pts[i].x, pts[i].y);
	CGContextClosePath(ctx);
}

/* Lips (outer minus inner) and mouth interior for each face, rasterised at analysis resolution.
 * Vision's points have their origin at the bottom left, as does a CGBitmapContext, whose memory
 * starts at the top row: so the bytes come out top-down, like the frame. */
static uint8_t *mouth_mask(NSArray<VNFaceObservation *> *faces, int w, int h)
{
	CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
	uint8_t *lips = calloc((size_t)w * h, 1), *inner = calloc((size_t)w * h, 1);
	CGContextRef a = CGBitmapContextCreate(lips, w, h, 8, w, gray, (CGBitmapInfo)kCGImageAlphaNone);
	CGContextRef b = CGBitmapContextCreate(inner, w, h, 8, w, gray, (CGBitmapInfo)kCGImageAlphaNone);
	CGContextSetGrayFillColor(a, 1, 1);
	CGContextSetGrayFillColor(b, 1, 1);
	int n = 0;
	for (VNFaceObservation *o in faces) {
		if (n++ >= PA_MAX_FACES || !o.landmarks)
			break;
		add_region(a, o.landmarks.outerLips, w, h);
		add_region(a, o.landmarks.innerLips, w, h);
		CGContextEOFillPath(a);
		add_region(b, o.landmarks.innerLips, w, h);
		CGContextFillPath(b);
	}
	CGContextRelease(a);
	CGContextRelease(b);
	CGColorSpaceRelease(gray);
	uint8_t *out = malloc((size_t)w * h * 2);
	/* 3x3 box blur for soft edges */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int sa = 0, sb = 0, c = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int xx = x + dx, yy = y + dy;
					if (xx < 0 || yy < 0 || xx >= w || yy >= h)
						continue;
					sa += lips[yy * w + xx];
					sb += inner[yy * w + xx];
					c++;
				}
			out[(y * w + x) * 2] = (uint8_t)(sa / c);
			out[(y * w + x) * 2 + 1] = (uint8_t)(sb / c);
		}
	free(lips);
	free(inner);
	return out;
}

bool pa_submit(void *ptr, const uint8_t *bgra, int w, int h, int stride, int want)
{
	struct pa *p = ptr;
	PAState *s = (__bridge PAState *)p->state;
	@synchronized(s) {
		if (s.busy)
			return false;
		s.busy = YES;
	}
	CVPixelBufferRef pb = NULL;
	if (CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA,
				(__bridge CFDictionaryRef) @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &pb) != kCVReturnSuccess) {
		@synchronized(s) { s.busy = NO; }
		return false;
	}
	CVPixelBufferLockBaseAddress(pb, 0);
	uint8_t *dst = CVPixelBufferGetBaseAddress(pb);
	size_t dstride = CVPixelBufferGetBytesPerRow(pb);
	for (int y = 0; y < h; y++)
		memcpy(dst + y * dstride, bgra + y * stride, (size_t)w * 4);
	CVPixelBufferUnlockBaseAddress(pb, 0);

	dispatch_async(s.queue, ^{
		@autoreleasepool {
			struct pa_result r = {0};
			r.aspect = (float)w / (float)h;
			NSDate *t0 = [NSDate date];
			VNDetectFaceLandmarksRequest *faces = [VNDetectFaceLandmarksRequest new];
			NSError *err = nil;
			[s.sequence performRequests:@[faces] onCVPixelBuffer:pb error:&err];
			r.face_ms = -[t0 timeIntervalSinceNow] * 1000.0;
			NSArray<VNFaceObservation *> *obs = [faces.results sortedArrayUsingComparator:^NSComparisonResult(VNFaceObservation *a, VNFaceObservation *b) {
				double aa = a.boundingBox.size.width * a.boundingBox.size.height, bb = b.boundingBox.size.width * b.boundingBox.size.height;
				return aa > bb ? NSOrderedAscending : aa < bb ? NSOrderedDescending : NSOrderedSame;
			}];
			for (VNFaceObservation *o in obs) {
				if (r.nfaces >= PA_MAX_FACES || !o.landmarks)
					break;
				struct pa_face *f = &r.faces[r.nfaces];
				CGRect bb = o.boundingBox;
				f->x = (float)bb.origin.x;
				f->y = (float)(1.0 - bb.origin.y - bb.size.height);
				f->w = (float)bb.size.width;
				f->h = (float)bb.size.height;
				VNFaceLandmarks2D *lm = o.landmarks;
				pa_pt c1 = {0}, c2 = {0};
				float r1 = 0, r2 = 0;
				centre_radius(lm.leftEye, w, h, &c1, &r1);
				centre_radius(lm.rightEye, w, h, &c2, &r2);
				/* Vision's left/right are the subject's; store by position in the image */
				bool swap = c1.x > c2.x;
				f->eye_l = swap ? c2 : c1;
				f->eye_r = swap ? c1 : c2;
				f->eye_rl = swap ? r2 : r1;
				f->eye_rr = swap ? r1 : r2;
				pa_pt raw[64];
				int n;
				region(lm.faceContour, w, h, raw, 64, &n);
				if (n >= 2 && raw[0].x > raw[n - 1].x)
					for (int i = 0; i < n / 2; i++) {
						pa_pt t = raw[i];
						raw[i] = raw[n - 1 - i];
						raw[n - 1 - i] = t;
					}
				resample(raw, n, f->contour, PA_CONTOUR);
				f->ncontour = n ? PA_CONTOUR : 0;
				f->chin = f->contour[PA_CONTOUR / 2];
				region(lm.noseCrest, w, h, raw, 64, &n);
				f->nose = n ? raw[n - 1] : (pa_pt){f->x + f->w / 2, f->y + f->h * 0.6f};
				pa_pt mid = {(f->eye_l.x + f->eye_r.x) / 2, (f->eye_l.y + f->eye_r.y) / 2};
				f->brow = (pa_pt){mid.x - (f->chin.x - mid.x) * 0.25f, mid.y - (f->chin.y - mid.y) * 0.25f};
				/* roll from the eye line, in pixel space */
				f->roll = atan2f((f->eye_r.y - f->eye_l.y) / r.aspect, f->eye_r.x - f->eye_l.x);
				r.nfaces++;
			}
			uint8_t *mouth = NULL;
			if ((want & PA_WANT_MOUTH) && r.nfaces > 0)
				mouth = mouth_mask(obs, w, h);
			uint8_t *mask = NULL;
			bool want_mask = want & PA_WANT_PERSON;
			if (want_mask) {
				NSDate *t1 = [NSDate date];
				VNGeneratePersonSegmentationRequest *seg = [VNGeneratePersonSegmentationRequest new];
				seg.qualityLevel = VNGeneratePersonSegmentationRequestQualityLevelBalanced;
				seg.outputPixelFormat = kCVPixelFormatType_OneComponent8;
				if ([s.sequence performRequests:@[seg] onCVPixelBuffer:pb error:&err] && seg.results.count) {
					CVPixelBufferRef m = seg.results.firstObject.pixelBuffer;
					CVPixelBufferLockBaseAddress(m, kCVPixelBufferLock_ReadOnly);
					r.mask_w = (int)CVPixelBufferGetWidth(m);
					r.mask_h = (int)CVPixelBufferGetHeight(m);
					size_t ms = CVPixelBufferGetBytesPerRow(m);
					const uint8_t *src = CVPixelBufferGetBaseAddress(m);
					mask = malloc((size_t)r.mask_w * r.mask_h);
					for (int y = 0; y < r.mask_h; y++)
						memcpy(mask + y * r.mask_w, src + y * ms, r.mask_w);
					CVPixelBufferUnlockBaseAddress(m, kCVPixelBufferLock_ReadOnly);
				}
				r.mask_ms = -[t1 timeIntervalSinceNow] * 1000.0;
			}
			CVPixelBufferRelease(pb);

			os_unfair_lock_lock(&p->lock);
			/* temporal smoothing against landmark jitter, when the same number of faces is tracked */
			if (p->have_prev && r.nfaces == p->res.nfaces)
				for (int i = 0; i < r.nfaces; i++)
					smooth_face(&r.faces[i], &p->res.faces[i], 0.55f);
			r.seq = p->res.seq + 1;
			r.mask_seq = p->res.mask_seq;
			r.mouth_seq = p->res.mouth_seq;
			if (mouth) {
				int need = w * h * 2;
				if (need > p->mouth_cap) {
					p->mouth = realloc(p->mouth, need);
					p->mouth_cap = need;
				}
				memcpy(p->mouth, mouth, need);
				free(mouth);
				r.mouth_w = w;
				r.mouth_h = h;
				r.mouth_seq++;
			}
			if (mask) {
				int need = r.mask_w * r.mask_h;
				if (need > p->mask_cap) {
					p->mask = realloc(p->mask, need);
					p->mask_cap = need;
				}
				memcpy(p->mask, mask, need);
				free(mask);
				r.mask_seq++;
			} else if (!want_mask) {
				r.mask_w = r.mask_h = 0;
			} else {
				r.mask_w = p->res.mask_w;
				r.mask_h = p->res.mask_h;
			}
			p->res = r;
			p->have_prev = true;
			os_unfair_lock_unlock(&p->lock);
			@synchronized(s) { s.busy = NO; }
		}
	});
	return true;
}

bool pa_get(void *ptr, struct pa_result *out, uint8_t *mask_buf, int cap, uint8_t *mouth_buf, int mouth_cap)
{
	struct pa *p = ptr;
	os_unfair_lock_lock(&p->lock);
	*out = p->res;
	if (mask_buf && p->mask && out->mask_w * out->mask_h <= cap)
		memcpy(mask_buf, p->mask, (size_t)out->mask_w * out->mask_h);
	if (mouth_buf && p->mouth && out->mouth_w * out->mouth_h * 2 <= mouth_cap)
		memcpy(mouth_buf, p->mouth, (size_t)out->mouth_w * out->mouth_h * 2);
	os_unfair_lock_unlock(&p->lock);
	return out->seq > 0;
}
