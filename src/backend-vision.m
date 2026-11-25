/* macOS backend: face landmarks and person segmentation with Apple's Vision framework, which
 * ships with macOS (no model files). Runs synchronously on the analyzer's worker thread. */
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <CoreVideo/CoreVideo.h>
#include <math.h>
#include <string.h>
#include "analyzer.h"

@interface PAVision : NSObject
@property(nonatomic) VNSequenceRequestHandler *sequence;
@end
@implementation PAVision
@end

const char *pa_backend_name(void) { return "vision"; }

void *pa_backend_create(const char **error)
{
	PAVision *v = [PAVision new];
	v.sequence = [VNSequenceRequestHandler new];
	return (__bridge_retained void *)v;
}

void pa_backend_destroy(void *b)
{
	CFRelease(b); /* balances the __bridge_retained in pa_backend_create */
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

static void lip_polygon(VNFaceLandmarkRegion2D *r, int w, int h, pa_pt *out, int *n)
{
	pa_pt raw[65], ring[PA_LIP_PTS + 1];
	int k;
	region(r, w, h, raw, 64, &k);
	if (k < 3) {
		*n = 0;
		return;
	}
	raw[k] = raw[0]; /* close the loop so resampling goes all the way round */
	pa_resample(raw, k + 1, ring, PA_LIP_PTS + 1);
	memcpy(out, ring, sizeof(pa_pt) * PA_LIP_PTS); /* the last point repeats the first */
	*n = PA_LIP_PTS;
}

/* A closed outline resampled to n points evenly spaced around it. */
static int loop(VNFaceLandmarkRegion2D *r, int w, int h, pa_pt *out, int n)
{
	pa_pt raw[65], ring[PA_EYE_PTS + 1];
	int k;
	region(r, w, h, raw, 64, &k);
	if (k < 3 || n > PA_EYE_PTS)
		return 0;
	raw[k] = raw[0];
	pa_resample(raw, k + 1, ring, n + 1);
	memcpy(out, ring, sizeof(pa_pt) * n);
	return n;
}

bool pa_backend_run(void *b, const uint8_t *bgra, int w, int h, int stride, int want, struct pa_backend_out *out)
{
	PAVision *v = (__bridge PAVision *)b;
	CVPixelBufferRef pb = NULL;
	if (CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA,
				(__bridge CFDictionaryRef) @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &pb) != kCVReturnSuccess)
		return false;
	CVPixelBufferLockBaseAddress(pb, 0);
	uint8_t *dst = CVPixelBufferGetBaseAddress(pb);
	size_t dstride = CVPixelBufferGetBytesPerRow(pb);
	for (int y = 0; y < h; y++)
		memcpy(dst + y * dstride, bgra + (size_t)y * stride, (size_t)w * 4);
	CVPixelBufferUnlockBaseAddress(pb, 0);

	float aspect = (float)w / (float)h;
	@autoreleasepool {
		NSDate *t0 = [NSDate date];
		VNDetectFaceLandmarksRequest *faces = [VNDetectFaceLandmarksRequest new];
		NSError *err = nil;
		[v.sequence performRequests:@[faces] onCVPixelBuffer:pb error:&err];
		out->face_ms = -[t0 timeIntervalSinceNow] * 1000.0;
		for (VNFaceObservation *o in faces.results) {
			if (out->nfaces >= PA_MAX_FACES || !o.landmarks)
				break;
			struct pa_face *f = &out->faces[out->nfaces];
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
			pa_resample(raw, n, f->contour, PA_CONTOUR);
			f->ncontour = n ? PA_CONTOUR : 0;
			region(lm.noseCrest, w, h, raw, 64, &n);
			f->nose = n ? raw[n - 1] : (pa_pt){f->x + f->w / 2, f->y + f->h * 0.6f};
			lip_polygon(lm.outerLips, w, h, f->lip_outer, &f->nlip_outer);
			lip_polygon(lm.innerLips, w, h, f->lip_inner, &f->nlip_inner);
			/* nose: wings are the outline's extremes across the face, the bridge the crest's top */
			region(lm.nose, w, h, raw, 64, &n);
			if (n >= 2) {
				int lo = 0, hi = 0;
				for (int i = 1; i < n; i++) {
					if (raw[i].x < raw[lo].x)
						lo = i;
					if (raw[i].x > raw[hi].x)
						hi = i;
				}
				f->nose_l = raw[lo];
				f->nose_r = raw[hi];
			}
			region(lm.noseCrest, w, h, raw, 64, &n);
			if (n >= 2)
				f->nose_top = raw[0].y < raw[n - 1].y ? raw[0] : raw[n - 1];
			/* eye outlines, stored by position in the image like the eye centres */
			pa_pt ea[PA_EYE_PTS], eb[PA_EYE_PTS];
			int na = loop(lm.leftEye, w, h, ea, PA_EYE_PTS), nb = loop(lm.rightEye, w, h, eb, PA_EYE_PTS);
			if (na && nb) {
				memcpy(f->eye_poly[0], swap ? eb : ea, sizeof ea);
				memcpy(f->eye_poly[1], swap ? ea : eb, sizeof ea);
				f->neye = PA_EYE_PTS;
			}
			pa_finish_face(f, aspect);
			/* eyebrows need the head's roll, so after finish_face */
			pa_pt ba[64], bb2[64];
			int nba, nbb;
			region(lm.leftEyebrow, w, h, ba, 64, &nba);
			region(lm.rightEyebrow, w, h, bb2, 64, &nbb);
			if (nba >= 2 && nbb >= 2) {
				pa_brow_from_points(swap ? bb2 : ba, swap ? nbb : nba, f->roll, aspect, f->brow_line[0], &f->brow_thick[0]);
				pa_brow_from_points(swap ? ba : bb2, swap ? nba : nbb, f->roll, aspect, f->brow_line[1], &f->brow_thick[1]);
				/* centre lines run image-left to image-right; the inner end of the left brow is its right end */
				for (int i = 0; i < PA_BROW_PTS / 2; i++) {
					pa_pt t = f->brow_line[0][i];
					f->brow_line[0][i] = f->brow_line[0][PA_BROW_PTS - 1 - i];
					f->brow_line[0][PA_BROW_PTS - 1 - i] = t;
				}
				f->nbrow = PA_BROW_PTS;
			}
			out->nfaces++;
		}
		if (want & PA_WANT_PERSON) {
			NSDate *t1 = [NSDate date];
			VNGeneratePersonSegmentationRequest *seg = [VNGeneratePersonSegmentationRequest new];
			seg.qualityLevel = VNGeneratePersonSegmentationRequestQualityLevelBalanced;
			seg.outputPixelFormat = kCVPixelFormatType_OneComponent8;
			if ([v.sequence performRequests:@[seg] onCVPixelBuffer:pb error:&err] && seg.results.count) {
				CVPixelBufferRef m = seg.results.firstObject.pixelBuffer;
				CVPixelBufferLockBaseAddress(m, kCVPixelBufferLock_ReadOnly);
				out->mask_w = (int)CVPixelBufferGetWidth(m);
				out->mask_h = (int)CVPixelBufferGetHeight(m);
				size_t ms = CVPixelBufferGetBytesPerRow(m);
				const uint8_t *src = CVPixelBufferGetBaseAddress(m);
				out->mask = malloc((size_t)out->mask_w * out->mask_h);
				for (int y = 0; y < out->mask_h; y++)
					memcpy(out->mask + y * out->mask_w, src + y * ms, out->mask_w);
				CVPixelBufferUnlockBaseAddress(m, kCVPixelBufferLock_ReadOnly);
			}
			out->mask_ms = -[t1 timeIntervalSinceNow] * 1000.0;
		}
	}
	CVPixelBufferRelease(pb);
	return true;
}
