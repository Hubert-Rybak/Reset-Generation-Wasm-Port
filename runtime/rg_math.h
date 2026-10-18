#ifndef RG_MATH_H
#define RG_MATH_H
#include "rg_defs.h"

/* Freestanding double-precision math (no libm in wasm32). */
double rg_m_fmod(double x, double y);        /* exact, like C fmod */
double rg_m_remainder(double x, double y);   /* exact, like C remainder */
double rg_m_sin(double x);
double rg_m_cos(double x);

#endif
