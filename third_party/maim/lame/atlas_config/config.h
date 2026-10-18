/* Atlas's build configuration for Maim's LAME (libmp3lame 3.99.5 with Maim's
   bends, plus mpglib for decoding). One header for every platform, in place
   of the one LAME's configure script or Makefile.MSVC would generate, so the
   encoder compiles the same code on macOS and Windows. */
#ifndef ATLAS_LAME_CONFIG_H
#define ATLAS_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1

/* The hip decoder (mpglib) turns the encoded frames back into audio. */
#define HAVE_MPGLIB 1

#define LAME_LIBRARY_BUILD 1

/* The bends feed the encoder values its asserts never expected. */
#ifndef NDEBUG
#define NDEBUG 1
#endif

#define PACKAGE "lame"
#define VERSION "3.99.5"

typedef float ieee754_float32_t;
typedef double ieee754_float64_t;
typedef long double ieee854_float80_t;

#endif /* ATLAS_LAME_CONFIG_H */
