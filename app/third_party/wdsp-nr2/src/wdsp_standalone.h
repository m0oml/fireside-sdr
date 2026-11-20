/*  wdsp_standalone.h -- fireside

    What WDSP's emnr.c (NR2, Warren Pratt NR0V) needs from the rest of WDSP, so that it can be built on its own
    (-DWDSP_STANDALONE) without the channel machinery, the Win32 layer or the other modules. Written for fireside;
    GPL v2 or later like the files it serves. See ../PATCHES.md.
*/
#ifndef _wdsp_standalone_h
#define _wdsp_standalone_h

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fftw3.h>                      /* the header is bundled (../include); the library is the system libfftw3 */

#define PI      3.1415926535897932
#define TWOPI   6.2831853071795864
#ifndef M_PI
#define M_PI    3.14159265358979323846
#endif
#define PORT
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

typedef double complex[2];             /* as in WDSP's comm.h: only sizeof() is used */

/* zeroed, 64-byte aligned memory (WDSP's malloc0 / _aligned_free) */
static inline void* malloc0(int size)
{
    void* p = NULL;
    size_t n = ((size_t)(size > 0 ? size : 1) + 63u) & ~(size_t)63u;
    if (posix_memalign(&p, 64, n) != 0) return NULL;
    memset(p, 0, n);
    return p;
}
#define _aligned_malloc(size, align) malloc0((int)(size))
#define _aligned_free(p) free(p)

/* WDSP's table-driven log10 (meterlog10.c) is only a speed-up: the exact function gives the same results to its table accuracy */
static inline double mlog10(double v) { return log10(v); }

#include "emnr.h"                      /* in WDSP this comes in through comm.h */

#endif
