/*  fireside_emnr.c -- fireside

    One setter for WDSP's EMNR post-processing stage (the "post2" smoothing of the gain mask), which upstream only offers through its channel-bound
    SetRXAEMNRpost2* functions (compiled out in the standalone build, see ../PATCHES.md). Same effect as those: sets the three fields.
    factor / nlevel are the fractions WDSP keeps internally (its setters take percent and divide by 100); WDSP's defaults are 0.15 and 0.15.
    GPL v2 or later, like the files it serves.
*/
#include "wdsp_standalone.h"

void fireside_emnr_post2(EMNR a, int run, double factor, double nlevel)
{
    a->post2.run = run;
    a->post2.factor = factor;
    a->post2.nlevel = nlevel;
}
