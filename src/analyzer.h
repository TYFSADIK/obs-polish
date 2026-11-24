/* Face landmarks and person segmentation with Apple's Vision framework, off the render thread. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PA_MAX_FACES 2
#define PA_CONTOUR 17

typedef struct { float x, y; } pa_pt;   /* normalised image coordinates, origin top-left */

struct pa_face {
	float x, y, w, h;                     /* bounding box */
	pa_pt eye_l, eye_r;                   /* eye centres (image left / image right) */
	float eye_rl, eye_rr;                 /* eye radii (normalised to image width) */
	pa_pt contour[PA_CONTOUR];            /* jaw line, one ear to the other through the chin */
	int ncontour;
	pa_pt nose, chin, brow;               /* nose tip, chin, mid-brow */
	float roll;                           /* radians, positive = head tilted clockwise on screen */
};

struct pa_result {
	uint64_t seq;                         /* increments with every completed analysis */
	int nfaces;
	struct pa_face faces[PA_MAX_FACES];   /* largest first */
	int mask_w, mask_h;                   /* person mask, 0 if none */
	uint64_t mask_seq;
	double face_ms, mask_ms;              /* last analysis times */
	float aspect;                         /* width / height of the analysed frame */
};

void *pa_create(void);
void pa_destroy(void *pa);
/* Copies the frame and starts analysis; returns false (frame dropped) if the last one is still running. */
bool pa_submit(void *pa, const uint8_t *bgra, int w, int h, int stride, bool want_mask);
/* Copies the latest result; if mask_buf is not NULL and a mask exists, copies it (mask_w*mask_h bytes, up to cap). */
bool pa_get(void *pa, struct pa_result *out, uint8_t *mask_buf, int cap);
