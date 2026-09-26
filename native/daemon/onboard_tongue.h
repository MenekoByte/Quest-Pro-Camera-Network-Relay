#ifndef QPRO_ONBOARD_TONGUE_H
#define QPRO_ONBOARD_TONGUE_H

#include <stddef.h>
#include <stdint.h>

/* Runs the Enhanced-FT stereo tongue model(s) on the HTA through
   /vendor/lib64/libhta_hexagon_runtime.so. A bundle directory holds either one
   merged model in "model/" (bundle v2, preferred when present) or the v1 pair
   "gate/" + "direction/". Bundles are produced by
   hta-probe/port/export_onboard_bundle.py; the format is BUNDLE.md next to it. */

#define TONGUE_MAX_TARGETS 16 /* largest supported model output count */
#define TONGUE_MAX_IMAGE 256  /* largest supported model input size */

typedef struct OnboardTongue OnboardTongue;

typedef struct {
    float gate[TONGUE_MAX_TARGETS];      /* gate model outputs, in target order */
    float direction[TONGUE_MAX_TARGETS]; /* direction model outputs, in target order */
                                     /* A single merged model fills both arrays
                                        with its own outputs. */
    double hta_ms;                   /* summed execute time of all graphs run */
    double total_ms;                 /* whole tongue_run call */
} TongueResult;

/* Loads and prepares the model(s). Returns NULL and writes a message into
   error on failure. */
OnboardTongue *tongue_open(const char *bundle_dir, char *error, size_t error_len);

/* Model input size S (square). Callers resize each 400x400 camera panel to
   S x S with area interpolation before tongue_run. */
int tongue_image_size(const OnboardTongue *tongue);

/* 1 for a merged single-model bundle, 0 for the gate + direction pair. */
int tongue_is_single_model(const OnboardTongue *tongue);

/* left and right are S x S uint8 images (camera2 and camera3 panels already
   resized, S = tongue_image_size), row-major, no padding. Returns 0 on
   success, otherwise a nonzero code and a message in error. */
int tongue_run(OnboardTongue *tongue, const uint8_t *left, const uint8_t *right,
               TongueResult *result, char *error, size_t error_len);

/* Number of model outputs (1..TONGUE_MAX_TARGETS), from the bundle's target list. */
int tongue_target_count(const OnboardTongue *tongue);

/* Target name i (0..tongue_target_count-1). */
const char *tongue_target_name(const OnboardTongue *tongue, int index);

void tongue_close(OnboardTongue *tongue);

#endif
