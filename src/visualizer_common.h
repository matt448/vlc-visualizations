/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matthew McMillan
 */

#ifndef VLC_VISUALIZATIONS_COMMON_H
#define VLC_VISUALIZATIONS_COMMON_H

/*
 * MSYS2's VLC 3 Windows headers call poll() from vlc_threads.h, while MinGW
 * exposes the compatible type/function as WSAPoll() in winsock2.h.
 */
#include <winsock2.h>
static inline int poll(struct pollfd *fds, unsigned nfds, int timeout)
{
    return WSAPoll(fds, nfds, timeout);
}

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_block.h>
#include <vlc_codec.h>
#include <vlc_aout.h>
#include <vlc_vout.h>
#include <vlc_picture.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include <windows.h>

#define VISUALIZER_MAX_BARS 80
#define VISUALIZER_MAX_FFT_SIZE 4096

typedef struct filter_sys_t visualizer_sys_t;

typedef struct visualizer_config_t
{
    unsigned bar_count;
    unsigned fft_size;
    int video_width;
    int video_height;
    void (*analyze)(visualizer_sys_t *sys, const float *samples, size_t frame_count,
                    size_t channels, unsigned sample_rate);
    void (*draw)(visualizer_sys_t *sys);
} visualizer_config_t;

struct filter_sys_t
{
    CRITICAL_SECTION lock;
    const visualizer_config_t *config;
    vout_thread_t *vout;
    HDC render_dc;
    HBITMAP render_bitmap;
    void *render_pixels;
    int render_width;
    int render_height;
    float bars[VISUALIZER_MAX_BARS];
    float analysis_bars[VISUALIZER_MAX_BARS];
    float sample_history[VISUALIZER_MAX_FFT_SIZE];
    size_t sample_history_pos;
    size_t sample_history_count;
    float adaptive_peak;
    wchar_t track_text[512];
    DWORD last_meta_tick;
};

int visualizer_open(vlc_object_t *object, const visualizer_config_t *config);
void visualizer_close(vlc_object_t *object);

void visualizer_fft_in_place(float *real, float *imag, size_t count);
uint8_t visualizer_clamp_byte(int value);

#endif
