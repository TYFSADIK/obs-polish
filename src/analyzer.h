/* Face landmarks and person segmentation, off the render thread.
 *
 * The core (analyzer-core.c) owns the worker, the latest result, temporal smoothing and the
 * lips mask. A backend fills in faces and the person mask for one frame:
 *   backend-vision.m   Apple's Vision framework (macOS default)
 *   backend-onnx.c     MediaPipe models on ONNX Runtime (Windows, Linux; optional on macOS) */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PA_MAX_FACES 2
#define PA_CONTOUR 17
#define PA_LIP_PTS 20
#define PA_EYE_PTS 16  /* closed eye outline */
#define PA_BROW_PTS 8  /* eyebrow centre line, inner end first */

typedef struct {
	float x, y;
} pa_pt; /* normalised image coordinates, origin top-left */

struct pa_face {
	float x, y, w, h;          /* bounding box */
	pa_pt eye_l, eye_r;        /* eye centres (image left / image right) */
	float eye_rl, eye_rr;      /* eye radii (normalised to image width) */
	pa_pt contour[PA_CONTOUR]; /* jaw line, one ear to the other through the chin */
	int ncontour;
	pa_pt nose, chin, brow; /* nose tip, chin, mid-brow */
	float roll;             /* radians, positive = head tilted clockwise on screen */
	pa_pt lip_outer[PA_LIP_PTS], lip_inner[PA_LIP_PTS]; /* closed polygons */
	int nlip_outer, nlip_inner;
	/* index 0 is the image-left eye or brow, 1 the image-right */
	pa_pt eye_poly[2][PA_EYE_PTS]; /* closed eye outlines; 0 points if the backend has none */
	int neye;
	pa_pt brow_line[2][PA_BROW_PTS]; /* eyebrow centre lines, inner end (nearest the nose) first */
	float brow_thick[2];             /* eyebrow thickness (normalised to image height) */
	int nbrow;
	pa_pt iris[2];     /* iris centres, found from the darkest part of each eye */
	float iris_r[2];   /* iris radii (normalised to image width) */
	pa_pt nose_l, nose_r, nose_top; /* nose wings (image left, right) and the bridge between the eyes */
};

struct pa_result {
	uint64_t seq; /* increments with every completed analysis */
	int nfaces;
	struct pa_face faces[PA_MAX_FACES]; /* largest first */
	int mask_w, mask_h;                 /* person mask, 0 if none */
	uint64_t mask_seq;
	int mouth_w, mouth_h; /* lips (R) and mouth interior (G) mask, 0 if none */
	uint64_t mouth_seq;
	int eyes_w, eyes_h; /* eye makeup mask: brows (R), eyeshadow (G), eyeliner (B), iris (A); 0 if none */
	uint64_t eyes_seq;
	double face_ms, mask_ms; /* last analysis times */
	float aspect;            /* width / height of the analysed frame */
	const char *backend;     /* "vision" or "onnx" */
};

#define PA_WANT_PERSON 1 /* person segmentation mask */
#define PA_WANT_MOUTH 2  /* lips and mouth-interior mask, for makeup */
#define PA_WANT_EYES 4   /* eyebrow, eyeshadow, eyeliner and iris mask, for eye makeup */

void *pa_create(void);
void pa_destroy(void *pa);
/* Copies the frame and queues analysis; returns false (frame dropped) if the last one is still running. */
bool pa_submit(void *pa, const uint8_t *bgra, int w, int h, int stride, int want);
/* Copies the latest result. Person mask: mask_w*mask_h bytes; mouth mask: mouth_w*mouth_h*2 bytes (R, G);
   eye mask: eyes_w*eyes_h*4 bytes (RGBA). Any buffer may be NULL; each is copied only if it fits in its cap. */
bool pa_get(void *pa, struct pa_result *out, uint8_t *mask_buf, int cap, uint8_t *mouth_buf, int mouth_cap,
	    uint8_t *eyes_buf, int eyes_cap);
/* Why analysis is unavailable (missing models or runtime), or NULL when it works. */
const char *pa_error(void *pa);

/* ---- backend interface (called on the worker thread only, except create/destroy) ---- */
struct pa_backend_out {
	int nfaces;
	struct pa_face faces[PA_MAX_FACES]; /* any order; the core puts the largest first */
	uint8_t *mask;                       /* person mask, malloc'd by the backend, freed by the core */
	int mask_w, mask_h;
	double face_ms, mask_ms;
};
void *pa_backend_create(const char **error);
void pa_backend_destroy(void *backend);
bool pa_backend_run(void *backend, const uint8_t *bgra, int w, int h, int stride, int want, struct pa_backend_out *out);
const char *pa_backend_name(void);

/* shared helpers */
void pa_resample(const pa_pt *in, int n, pa_pt *out, int m);
void pa_finish_face(struct pa_face *f, float aspect); /* chin, brow and roll from the rest */
/* Eyebrow centre line and thickness from any outline or line of brow points (in any order along it). */
void pa_brow_from_points(const pa_pt *pts, int n, float roll, float aspect, pa_pt *line, float *thick);

#ifdef __cplusplus
}
#endif
