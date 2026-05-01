/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matthew McMillan
 */

#include "visualizer_common.h"

#ifdef HAVE_VLC_PLAYLIST_LEGACY_H
#include <vlc_playlist_legacy.h>
#include <vlc_input.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static block_t *Filter(filter_t *, block_t *);

static void utf8_to_wide(const char *src, wchar_t *dst, size_t dst_len)
{
    if (dst_len == 0)
        return;

    if (src == NULL || src[0] == '\0')
    {
        dst[0] = L'\0';
        return;
    }

    int written = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)dst_len);
    if (written == 0)
    {
        dst[0] = L'\0';
        return;
    }

    dst[dst_len - 1] = L'\0';
}

typedef struct
{
    DWORD process_id;
    wchar_t title[512];
} vlc_window_title_t;

static BOOL CALLBACK find_vlc_window_proc(HWND hwnd, LPARAM lparam)
{
    vlc_window_title_t *ctx = (vlc_window_title_t *)lparam;
    DWORD window_pid = 0;
    wchar_t title[512];

    if (!IsWindowVisible(hwnd))
        return TRUE;

    GetWindowThreadProcessId(hwnd, &window_pid);
    if (window_pid != ctx->process_id)
        return TRUE;

    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) <= 0)
        return TRUE;

    if (wcsstr(title, L"VLC media player") == NULL)
        return TRUE;

    wcsncpy(ctx->title, title, ARRAYSIZE(ctx->title) - 1);
    ctx->title[ARRAYSIZE(ctx->title) - 1] = L'\0';
    return FALSE;
}

static bool fetch_window_title_metadata(char *out, size_t out_len)
{
    vlc_window_title_t ctx;
    wchar_t cleaned[512];
    const wchar_t *suffix = L" - VLC media player";

    memset(&ctx, 0, sizeof(ctx));
    ctx.process_id = GetCurrentProcessId();

    EnumWindows(find_vlc_window_proc, (LPARAM)&ctx);
    if (ctx.title[0] == L'\0')
        return false;

    wcsncpy(cleaned, ctx.title, ARRAYSIZE(cleaned) - 1);
    cleaned[ARRAYSIZE(cleaned) - 1] = L'\0';

    size_t len = wcslen(cleaned);
    size_t suffix_len = wcslen(suffix);
    if (len > suffix_len && wcscmp(cleaned + len - suffix_len, suffix) == 0)
        cleaned[len - suffix_len] = L'\0';

    while (cleaned[0] == L' ' || cleaned[0] == L'-')
        memmove(cleaned, cleaned + 1, (wcslen(cleaned) + 1) * sizeof(cleaned[0]));

    while (cleaned[0] == L' ')
        memmove(cleaned, cleaned + 1, (wcslen(cleaned) + 1) * sizeof(cleaned[0]));

    if (cleaned[0] == L'\0')
        return false;

    int written = WideCharToMultiByte(CP_UTF8, 0, cleaned, -1, out, (int)out_len, NULL, NULL);
    if (written == 0)
        return false;

    out[out_len - 1] = '\0';
    return true;
}

static void fetch_metadata(filter_t *filter, char *out, size_t out_len)
{
    if (out_len == 0)
        return;

    snprintf(out, out_len, "Track info unavailable");

    if (fetch_window_title_metadata(out, out_len))
        return;

#ifdef HAVE_VLC_PLAYLIST_LEGACY_H
    input_thread_t *input = playlist_CurrentInput(pl_Get(filter));
    if (input == NULL)
        return;

    input_item_t *item = input_GetItem(input);
    if (item == NULL)
    {
        vlc_object_release(input);
        return;
    }

    char *title = input_item_GetTitleFbName(item);
    char *artist = input_item_GetArtist(item);
    char *album = input_item_GetAlbum(item);

    if (artist != NULL && artist[0] != '\0' && album != NULL && album[0] != '\0')
        snprintf(out, out_len, "%s - %s (%s)", artist, title != NULL ? title : "Unknown title", album);
    else if (artist != NULL && artist[0] != '\0')
        snprintf(out, out_len, "%s - %s", artist, title != NULL ? title : "Unknown title");
    else if (title != NULL && title[0] != '\0')
        snprintf(out, out_len, "%s", title);

    free(title);
    free(artist);
    free(album);
    vlc_object_release(input);
#else
    (void)filter;
#endif
}

void visualizer_fft_in_place(float *real, float *imag, size_t count)
{
    for (size_t i = 1, j = 0; i < count; ++i)
    {
        size_t bit = count >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j)
        {
            float tmp = real[i];
            real[i] = real[j];
            real[j] = tmp;

            tmp = imag[i];
            imag[i] = imag[j];
            imag[j] = tmp;
        }
    }

    for (size_t len = 2; len <= count; len <<= 1)
    {
        float angle = (float)(-2.0 * 3.14159265358979323846 / (double)len);
        float wlen_real = cosf(angle);
        float wlen_imag = sinf(angle);

        for (size_t i = 0; i < count; i += len)
        {
            float w_real = 1.0f;
            float w_imag = 0.0f;

            for (size_t j = 0; j < len / 2; ++j)
            {
                size_t even = i + j;
                size_t odd = even + len / 2;

                float odd_real = real[odd] * w_real - imag[odd] * w_imag;
                float odd_imag = real[odd] * w_imag + imag[odd] * w_real;

                real[odd] = real[even] - odd_real;
                imag[odd] = imag[even] - odd_imag;
                real[even] += odd_real;
                imag[even] += odd_imag;

                float next_w_real = w_real * wlen_real - w_imag * wlen_imag;
                w_imag = w_real * wlen_imag + w_imag * wlen_real;
                w_real = next_w_real;
            }
        }
    }
}

static bool create_render_surface(visualizer_sys_t *sys, int width, int height)
{
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    sys->render_dc = CreateCompatibleDC(NULL);
    if (sys->render_dc == NULL)
        return false;

    sys->render_bitmap = CreateDIBSection(sys->render_dc, &bmi, DIB_RGB_COLORS, &sys->render_pixels, NULL, 0);
    if (sys->render_bitmap == NULL || sys->render_pixels == NULL)
    {
        DeleteDC(sys->render_dc);
        sys->render_dc = NULL;
        return false;
    }

    SelectObject(sys->render_dc, sys->render_bitmap);
    sys->render_width = width;
    sys->render_height = height;
    return true;
}

uint8_t visualizer_clamp_byte(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

static void copy_bgra_to_i420(visualizer_sys_t *sys, picture_t *picture)
{
    const uint8_t *pixels = (const uint8_t *)sys->render_pixels;
    int width = sys->render_width;
    int height = sys->render_height;

    for (int y = 0; y < height; ++y)
    {
        uint8_t *dst_y = picture->p[0].p_pixels + y * picture->p[0].i_pitch;
        const uint8_t *src = pixels + y * width * 4;

        for (int x = 0; x < width; ++x)
        {
            int b = src[x * 4 + 0];
            int g = src[x * 4 + 1];
            int r = src[x * 4 + 2];
            dst_y[x] = visualizer_clamp_byte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    for (int y = 0; y < height; y += 2)
    {
        uint8_t *dst_u = picture->p[1].p_pixels + (y / 2) * picture->p[1].i_pitch;
        uint8_t *dst_v = picture->p[2].p_pixels + (y / 2) * picture->p[2].i_pitch;

        for (int x = 0; x < width; x += 2)
        {
            int sum_r = 0;
            int sum_g = 0;
            int sum_b = 0;

            for (int yy = 0; yy < 2; ++yy)
            {
                const uint8_t *src = pixels + (y + yy) * width * 4;
                for (int xx = 0; xx < 2; ++xx)
                {
                    const uint8_t *px = src + (x + xx) * 4;
                    sum_b += px[0];
                    sum_g += px[1];
                    sum_r += px[2];
                }
            }

            int r = sum_r / 4;
            int g = sum_g / 4;
            int b = sum_b / 4;
            dst_u[x / 2] = visualizer_clamp_byte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            dst_v[x / 2] = visualizer_clamp_byte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

static void render_vlc_picture(filter_t *filter, block_t *block)
{
    visualizer_sys_t *sys = filter->p_sys;
    if (sys->vout == NULL || sys->render_pixels == NULL)
        return;

    picture_t *picture = vout_GetPicture(sys->vout);
    if (picture == NULL)
        return;

    picture->b_progressive = true;
    sys->config->draw(sys);
    copy_bgra_to_i420(sys, picture);
    picture->date = block->i_pts + (block->i_length / 2);
    vout_PutPicture(sys->vout, picture);
}

static void analyze_s16_samples(visualizer_sys_t *sys, const int16_t *samples, size_t frame_count,
                                size_t channels, unsigned sample_rate)
{
    float stack_samples[VISUALIZER_MAX_FFT_SIZE * 8];
    size_t sample_count = frame_count * channels;
    size_t count = sample_count < ARRAYSIZE(stack_samples) ? sample_count : ARRAYSIZE(stack_samples);

    if (count == 0)
        return;

    size_t stride = sample_count / count;
    if (stride == 0)
        stride = 1;

    for (size_t i = 0; i < count; ++i)
        stack_samples[i] = (float)samples[i * stride] / 32768.0f;

    sys->config->analyze(sys, stack_samples, count / channels, channels, sample_rate);
}

static void decay_bars(visualizer_sys_t *sys)
{
    EnterCriticalSection(&sys->lock);
    for (unsigned i = 0; i < sys->config->bar_count; ++i)
    {
        sys->analysis_bars[i] *= 0.96f;
        sys->bars[i] *= 0.96f;
    }
    LeaveCriticalSection(&sys->lock);
}

static block_t *Filter(filter_t *filter, block_t *block)
{
    if (block == NULL)
        return NULL;

    visualizer_sys_t *sys = filter->p_sys;

    DWORD now = GetTickCount();
    if (now - sys->last_meta_tick > 1000)
    {
        char metadata[512];
        fetch_metadata(filter, metadata, sizeof(metadata));

        wchar_t wide[512];
        utf8_to_wide(metadata, wide, ARRAYSIZE(wide));

        EnterCriticalSection(&sys->lock);
        wcsncpy(sys->track_text, wide, ARRAYSIZE(sys->track_text) - 1);
        sys->track_text[ARRAYSIZE(sys->track_text) - 1] = L'\0';
        LeaveCriticalSection(&sys->lock);

        sys->last_meta_tick = now;
    }

    size_t channels = filter->fmt_in.audio.i_channels;
    if (channels == 0)
        channels = 1;

    unsigned sample_rate = filter->fmt_in.audio.i_rate;
    if (sample_rate == 0)
        sample_rate = 44100;

    if (filter->fmt_in.audio.i_format == VLC_CODEC_FL32)
        sys->config->analyze(sys, (const float *)block->p_buffer, (size_t)block->i_nb_samples, channels, sample_rate);
    else if (filter->fmt_in.audio.i_format == VLC_CODEC_S16N)
        analyze_s16_samples(sys, (const int16_t *)block->p_buffer, (size_t)block->i_nb_samples, channels, sample_rate);
    else
        decay_bars(sys);

    render_vlc_picture(filter, block);

    return block;
}

int visualizer_open(vlc_object_t *object, const visualizer_config_t *config)
{
    filter_t *filter = (filter_t *)object;
    visualizer_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    sys->config = config;
    InitializeCriticalSection(&sys->lock);
    wcscpy(sys->track_text, L"Waiting for track metadata...");
    if (!create_render_surface(sys, config->video_width, config->video_height))
    {
        DeleteCriticalSection(&sys->lock);
        free(sys);
        return VLC_EGENERIC;
    }

    filter->p_sys = sys;
    video_format_t fmt = {
        .i_chroma = VLC_CODEC_I420,
        .i_width = (unsigned)config->video_width,
        .i_height = (unsigned)config->video_height,
        .i_visible_width = (unsigned)config->video_width,
        .i_visible_height = (unsigned)config->video_height,
        .i_sar_num = 1,
        .i_sar_den = 1,
    };

    sys->vout = aout_filter_RequestVout(filter, NULL, &fmt);
    if (sys->vout == NULL)
    {
        DeleteObject(sys->render_bitmap);
        DeleteDC(sys->render_dc);
        DeleteCriticalSection(&sys->lock);
        free(sys);
        return VLC_EGENERIC;
    }

    filter->fmt_in.audio.i_format = VLC_CODEC_FL32;
    filter->fmt_out.audio = filter->fmt_in.audio;
    filter->pf_audio_filter = Filter;
    return VLC_SUCCESS;
}

void visualizer_close(vlc_object_t *object)
{
    filter_t *filter = (filter_t *)object;
    visualizer_sys_t *sys = filter->p_sys;
    if (sys == NULL)
        return;

    if (sys->vout != NULL)
        aout_filter_RequestVout(filter, sys->vout, NULL);
    if (sys->render_bitmap != NULL)
        DeleteObject(sys->render_bitmap);
    if (sys->render_dc != NULL)
        DeleteDC(sys->render_dc);

    DeleteCriticalSection(&sys->lock);
    free(sys);
}
