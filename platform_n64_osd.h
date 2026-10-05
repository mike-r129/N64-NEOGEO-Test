#ifndef PLATFORM_N64_OSD_H
#define PLATFORM_N64_OSD_H
// Diagnostic overlays (platform_n64_osd.c); everything here exists only in
// the matching diagnostic builds.
#include <libdragon.h>

#if defined(MVS64_SNDOSD) || defined(MVS64_PERFOSD)
// Draw s at (x, y) in a 4x6 font scaled 2x, white, into a 16-bit buffer.
void osd_text(uint16_t *fb, int stride_px, int x, int y, const char *s);
#endif

#if defined(MVS64_FBCRC) || defined(MVS64_FBCRC_PIPE)
uint32_t osd_fb_crc(const surface_t *disp);
#endif

#ifdef MVS64_PERFOSD
extern uint32_t posd_wait;   // plat_beginframe adds its display_get wait here
void perfosd_endframe(void);
#endif

#endif
