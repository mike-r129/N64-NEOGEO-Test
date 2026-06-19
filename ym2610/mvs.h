// Compatibility shim for the vendored gngeo YM2610 core.
//
// The original gngeo mvs.h pulled types from SDL. mvs64 has no SDL on the N64
// side, so this provides the same type names + helper macros from <stdint.h>.
#ifndef MVS64_YM2610_MVS_H
#define MVS64_YM2610_MVS_H

#include <stdint.h>

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

// SDL type names still used directly by ym2610.c
typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int16_t  Sint16;

#define ALIGN_DATA
#ifndef INLINE
#define INLINE static inline
#endif
#define SOUND_SAMPLES 512

#define Limit(val, max, min)        \
{                                   \
	if (val > max) val = max;       \
	else if (val < min) val = min;  \
}

// MAME logging — disabled (no-op). LOG_LEVEL is defined by ym2610.c itself.
#define logerror(...) ((void)0)

// gngeo's 2610intf.h defined this no-op; we update the YM2610 stream once per
// frame chunk in the sound module instead of on every register write.
#define YM2610UpdateRequest()

#endif // MVS64_YM2610_MVS_H
