#include "area_resize.h"

#include <stdlib.h>

typedef struct {
    int index;
    float weight;
} AreaEntry;

static int floor_nonnegative(double value)
{
    return (int)value;
}

static int ceil_nonnegative(double value)
{
    int whole = (int)value;
    return (double)whole < value ? whole + 1 : whole;
}

static double min_double(double a, double b) { return a < b ? a : b; }
static double max_double(double a, double b) { return a > b ? a : b; }

static int make_table(int src_size, int dst_size, AreaEntry **out,
                      int **starts_out, int **counts_out)
{
    const double scale = (double)src_size / dst_size;
    size_t capacity = (size_t)src_size + (size_t)dst_size;
    AreaEntry *entries = (AreaEntry *)malloc(capacity * sizeof(*entries));
    int *starts = (int *)malloc((size_t)dst_size * sizeof(*starts));
    int *counts = (int *)malloc((size_t)dst_size * sizeof(*counts));
    size_t used = 0;
    int d;
    if (!entries || !starts || !counts) {
        free(entries);
        free(starts);
        free(counts);
        return -1;
    }
    for (d = 0; d < dst_size; ++d) {
        const double left = d * scale;
        const double right = left + scale;
        int first = floor_nonnegative(left);
        int last = ceil_nonnegative(right);
        int s;
        starts[d] = (int)used;
        for (s = first; s < last && s < src_size; ++s) {
            const double overlap = min_double((double)s + 1.0, right) - max_double((double)s, left);
            float weight;
            if (overlap <= 0.0)
                continue;
            weight = (float)overlap;
            {
                double delta = (double)weight - scale;
                if (delta < 0.0) delta = -delta;
                if (delta < 1e-3)
                weight = 1.0f;
                else
                weight = (float)(weight / (float)scale);
            }
            entries[used].index = s;
            entries[used].weight = weight;
            ++used;
        }
        counts[d] = (int)used - starts[d];
    }
    *out = entries;
    *starts_out = starts;
    *counts_out = counts;
    return 0;
}

static uint8_t round_saturate(float value)
{
    double rounded;
    if (!(value > 0.0f))
        return 0;
    if (value >= 255.0f)
        return 255;
    {
        double lower = (double)(int)value;
        double fraction = (double)value - lower;
        if (fraction > 0.5 || (fraction == 0.5 && ((int)lower & 1) != 0))
            lower += 1.0;
        rounded = lower;
    }
    if (rounded < 0.0)
        return 0;
    if (rounded > 255.0)
        return 255;
    return (uint8_t)rounded;
}

int area_resize_u8(const uint8_t *src, int src_w, int src_h, int src_stride,
                   uint8_t *dst, int dst_w, int dst_h)
{
    AreaEntry *xtab = NULL;
    AreaEntry *ytab = NULL;
    int *xstarts = NULL, *xcounts = NULL, *ystarts = NULL, *ycounts = NULL;
    int dx, dy;
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 ||
        src_stride < src_w || dst_w > src_w || dst_h > src_h)
        return -1;
    if (make_table(src_w, dst_w, &xtab, &xstarts, &xcounts) != 0)
        return -1;
    if (make_table(src_h, dst_h, &ytab, &ystarts, &ycounts) != 0) {
        free(xtab); free(xstarts); free(xcounts);
        return -1;
    }
    for (dy = 0; dy < dst_h; ++dy) {
        for (dx = 0; dx < dst_w; ++dx) {
            float sum = 0.0f;
            int yi, xi;
            for (yi = 0; yi < ycounts[dy]; ++yi) {
                const AreaEntry ye = ytab[ystarts[dy] + yi];
                const uint8_t *row = src + (size_t)ye.index * (size_t)src_stride;
                for (xi = 0; xi < xcounts[dx]; ++xi) {
                    const AreaEntry xe = xtab[xstarts[dx] + xi];
                    const float alpha = xe.weight * ye.weight;
                    sum += (float)row[xe.index] * alpha;
                }
            }
            dst[(size_t)dy * (size_t)dst_w + (size_t)dx] = round_saturate(sum);
        }
    }
    free(xtab); free(xstarts); free(xcounts);
    free(ytab); free(ystarts); free(ycounts);
    return 0;
}
