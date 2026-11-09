// SPDX-License-Identifier: MIT
#pragma once
#include "rk3128_jpeg.h"

typedef struct {
    int source_w, source_h;  // header dimensions the sizing decision was based on
    int w, h;               // requested dimensions BEFORE EXIF orientation
} Rk3128JpegRgaTarget;

// Only measured beneficial reductions (>=1 MP, strictly smaller, exact layout)
// are admitted. Refusal/failure returns NULL; caller uses rk3128_jpeg_to_rgb on the
// same still-owned capture, followed by its original transforms. No source
// export survives this call, including cancellation/error paths.
uint8_t *rk3128_jpeg_rga_prepare(const Rk3128JpegPlanes *pl, int capture_fd, size_t capture_length,
    const Rk3128JpegRgaTarget *target, char *why, size_t whylen);

#ifdef RK3128_JPEG_RGA_DIAGNOSTIC
// Measurement builds only. Native CMake never defines this macro.
int rk3128_jpeg_rga_test_fail(const char *stage);
#endif
