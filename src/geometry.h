#ifndef MORI_GEOMETRY_H
#define MORI_GEOMETRY_H
#include <X11/Xutil.h>

typedef struct {
    int x, y, w, h;
} Geometry;

/* Bounds take precedence when application constraints cannot fit. */
void size_hints(const XSizeHints *hints, int maxw, int maxh, int *w, int *h);
void reserve_strut(Geometry monitor, int rootw, int rooth, const unsigned long strut[12],
                   Geometry *area);
#endif
