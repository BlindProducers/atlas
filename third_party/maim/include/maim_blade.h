/* Maim's BladeEnc, for the Maim effect. blade.h pulls in BladeEnc's common.h,
   whose names (FLOAT, MIN, MAX, PI, SBLIMIT) are macros: include this last,
   in one translation unit, never from a header. */
#pragma once
#include "../blade/bladeenc/blade.h"
