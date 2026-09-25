#include "geometry.h"
#include <stdint.h>

static int lo(int a, int b) {
    return a < b ? a : b;
}

static int hi(int a, int b) {
    return a > b ? a : b;
}

static int clamp(int v, int a, int b) {
    return hi(a, lo(v, b));
}

/* Return the largest grid point no greater than the requested size, unless
 * the minimum requires rounding up. Invalid or impossible grids are ignored. */
static int grid(int value, int min, int max, int base, int inc) {
    value = clamp(value, min, max);
    if (inc <= 0 || base < 0 || base > max)
        return value;
    int64_t first = base + ((int64_t)hi(0, min - base) + inc - 1) / inc * inc;
    if (first > max)
        return value;
    int last = base + (max - base) / inc * inc;
    return clamp(base + hi(0, value - base) / inc * inc, (int)first, last);
}

void size_hints(const XSizeHints *s, int maxw, int maxh, int *w, int *h) {
    maxw = clamp(maxw, 1, 65535);
    maxh = clamp(maxh, 1, 65535);
    int bw = s->flags & PBaseSize  ? hi(0, s->base_width)
             : s->flags & PMinSize ? hi(0, s->min_width)
                                   : 0;
    int bh = s->flags & PBaseSize  ? hi(0, s->base_height)
             : s->flags & PMinSize ? hi(0, s->min_height)
                                   : 0;
    int minw = clamp(s->flags & PMinSize ? s->min_width : bw, 1, maxw);
    int minh = clamp(s->flags & PMinSize ? s->min_height : bh, 1, maxh);
    if ((s->flags & PMaxSize) && s->max_width > 0)
        maxw = hi(minw, lo(maxw, s->max_width));
    if ((s->flags & PMaxSize) && s->max_height > 0)
        maxh = hi(minh, lo(maxh, s->max_height));
    int iw = s->flags & PResizeInc ? s->width_inc : 0;
    int ih = s->flags & PResizeInc ? s->height_inc : 0;
    int wantw = clamp(*w, minw, maxw), wanth = clamp(*h, minh, maxh);
    *w = grid(wantw, minw, maxw, bw, iw);
    *h = grid(wanth, minh, maxh, bh, ih);
    if (!(s->flags & PAspect))
        return;
    /* Only an explicit base size is subtracted for aspect ratios (ICCCM). */
    int ax = s->flags & PBaseSize ? bw : 0, ay = s->flags & PBaseSize ? bh : 0;
    int64_t best = INT64_MAX;
    int resultw = *w, resulth = *h;
    for (int y = minh; y <= maxh; ++y) {
        if (y <= ay || grid(y, minh, maxh, bh, ih) != y)
            continue;
        int64_t left = minw, right = maxw;
        if (left <= ax)
            left = (int64_t)ax + 1;
        if (s->min_aspect.x > 0 && s->min_aspect.y > 0) {
            int64_t limit =
                ax + ((int64_t)(y - ay) * s->min_aspect.x + s->min_aspect.y - 1) / s->min_aspect.y;
            if (limit > left)
                left = limit;
        }
        if (s->max_aspect.x > 0 && s->max_aspect.y > 0) {
            int64_t limit = ax + (int64_t)(y - ay) * s->max_aspect.x / s->max_aspect.y;
            if (limit < right)
                right = limit;
        }
        if (left > right)
            continue;
        int x = grid(wantw, (int)left, (int)right, bw, iw);
        /* Do not silently relax increments just to satisfy aspect. */
        if (grid(x, minw, maxw, bw, iw) != x)
            continue;
        int64_t dx = x - wantw, dy = y - wanth, distance = dx * dx + dy * dy;
        if (distance < best) {
            best = distance;
            resultw = x;
            resulth = y;
        }
    }
    *w = resultw;
    *h = resulth;
}

void reserve_strut(Geometry m, int rootw, int rooth, const unsigned long s[12], Geometry *a) {
    int right = a->x + a->w, bottom = a->y + a->h;
    /* CARDINAL values are unsigned; clamp before converting to signed geometry. */
    int l = s[0] > (unsigned)rootw ? rootw : (int)s[0];
    int r = s[1] > (unsigned)rootw ? rootw : (int)s[1];
    int t = s[2] > (unsigned)rooth ? rooth : (int)s[2];
    int b = s[3] > (unsigned)rooth ? rooth : (int)s[3];
#define OVERLAP(start, end, origin, length)                                                        \
    (s[start] <= s[end] && (int64_t)(origin) <= (int64_t)s[end] &&                                 \
     (int64_t)(origin) + (length) - 1 >= (int64_t)s[start])
    if (l && OVERLAP(4, 5, m.y, m.h))
        a->x = hi(a->x, lo(m.x + m.w - 1, l));
    if (r && OVERLAP(6, 7, m.y, m.h))
        right = lo(right, hi(m.x + 1, rootw - r));
    if (t && OVERLAP(8, 9, m.x, m.w))
        a->y = hi(a->y, lo(m.y + m.h - 1, t));
    if (b && OVERLAP(10, 11, m.x, m.w))
        bottom = lo(bottom, hi(m.y + 1, rooth - b));
#undef OVERLAP
    a->w = hi(1, right - a->x);
    a->h = hi(1, bottom - a->y);
}
