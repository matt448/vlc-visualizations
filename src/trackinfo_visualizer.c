/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matthew McMillan
 *
 * MSYS2's VLC 3 Windows headers call poll() from vlc_threads.h, while MinGW
 * exposes the compatible type/function as WSAPoll() in winsock2.h.
 */
#include <winsock2.h>
static inline int poll(struct pollfd *fds, unsigned nfds, int timeout)
{
    return WSAPoll(fds, nfds, timeout);
}

/*****************************************************************************
 * trackinfo_visualizer.c: VLC visualization plugin with track metadata overlay
 *****************************************************************************/

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_block.h>
#include <vlc_codec.h>
#include <vlc_aout.h>
#include <vlc_vout.h>
#include <vlc_picture.h>

#ifdef HAVE_VLC_PLAYLIST_LEGACY_H
#include <vlc_playlist_legacy.h>
#include <vlc_input.h>
#endif

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>

#ifdef LED_SEGMENT_VISUALIZER
#define BAR_COUNT 31
#else
#define BAR_COUNT 80
#endif
#define FFT_SIZE 512
#define VIDEO_WIDTH 800
#define VIDEO_HEIGHT 500
#define SPECTRUM_MAX_HEIGHT 380.0f
#define SPECTRUM_LOG_OFFSET 0.1f
#define SPECTRUM_BAR_DECREASE 5.0f

static const int spectrum_xscale[BAR_COUNT + 1] = {
#ifdef LED_SEGMENT_VISUALIZER
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 11, 14, 17, 21, 26, 32,
    40, 50, 63, 79, 99, 124, 155, 194,
    216, 231, 240, 247, 251, 253, 254, 255
#else
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    61, 63, 67, 72, 77, 82, 87, 93, 99, 105,
    110, 115, 121, 130, 141, 152, 163, 174, 185, 200,
    255
#endif
};

#ifdef LED_SEGMENT_VISUALIZER
static const wchar_t *frequency_labels[BAR_COUNT] = {
    L"20", L"25", L"31.5", L"40", L"50", L"63", L"80", L"100",
    L"125", L"160", L"200", L"250", L"315", L"400", L"500", L"630",
    L"800", L"1K", L"1.25K", L"1.6K", L"2K", L"2.5K", L"3.15K", L"4K",
    L"5K", L"6.3K", L"8K", L"10K", L"12.5K", L"16K", L"20K"
};
#endif

struct filter_sys_t
{
    CRITICAL_SECTION lock;
    vout_thread_t *vout;
    HDC render_dc;
    HBITMAP render_bitmap;
    void *render_pixels;
    int render_width;
    int render_height;
    float bars[BAR_COUNT];
    float analysis_bars[BAR_COUNT];
    wchar_t track_text[512];
    DWORD last_meta_tick;
};

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);
static block_t *Filter(filter_t *, block_t *);

vlc_module_begin()
#ifdef LED_SEGMENT_VISUALIZER
    set_shortname("LED Segments")
    set_description("31-band LED segment visualization with frequency labels")
#else
    set_shortname("Spectrum Info")
    set_description("Spectrum visualization with persistent track information")
#endif
    set_capability("visualization", 0)
    set_subcategory(SUBCAT_AUDIO_VISUAL)
#ifdef LED_SEGMENT_VISUALIZER
    add_shortcut("led_segments", "led_segment_visualizer")
#else
    add_shortcut("spectrum_info", "trackinfo_visualizer")
#endif
    set_callbacks(Open, Close)
vlc_module_end()

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
    size_t len;

    memset(&ctx, 0, sizeof(ctx));
    ctx.process_id = GetCurrentProcessId();

    EnumWindows(find_vlc_window_proc, (LPARAM)&ctx);
    if (ctx.title[0] == L'\0')
        return false;

    wcsncpy(cleaned, ctx.title, ARRAYSIZE(cleaned) - 1);
    cleaned[ARRAYSIZE(cleaned) - 1] = L'\0';

    len = wcslen(cleaned);
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

static COLORREF blend_color(COLORREF from, COLORREF to, float amount)
{
    if (amount < 0.0f)
        amount = 0.0f;
    if (amount > 1.0f)
        amount = 1.0f;

    int r = (int)((float)GetRValue(from) + ((float)GetRValue(to) - (float)GetRValue(from)) * amount);
    int g = (int)((float)GetGValue(from) + ((float)GetGValue(to) - (float)GetGValue(from)) * amount);
    int b = (int)((float)GetBValue(from) + ((float)GetBValue(to) - (float)GetBValue(from)) * amount);
    return RGB(r, g, b);
}

static COLORREF bar_color_at_y(int y, int graph_top, int graph_height)
{
    const COLORREF green = RGB(40, 255, 0);
    const COLORREF yellow = RGB(255, 232, 0);
    const COLORREF red = RGB(255, 48, 32);
    int graph_bottom = graph_top + graph_height;
    float height_ratio = (float)(graph_bottom - y) / (float)graph_height;

    if (height_ratio <= 0.25f)
        return green;
    if (height_ratio <= 0.375f)
        return blend_color(green, yellow, (height_ratio - 0.25f) / 0.125f);
    return blend_color(yellow, red, (height_ratio - 0.375f) / 0.625f);
}

static void fill_gradient_rect(HDC dc, const RECT *rect, COLORREF top_color, COLORREF bottom_color)
{
    TRIVERTEX vertex[2];
    GRADIENT_RECT mesh = { 0, 1 };

    if (rect->top >= rect->bottom || rect->left >= rect->right)
        return;

    vertex[0].x = rect->left;
    vertex[0].y = rect->top;
    vertex[0].Red = (COLOR16)(GetRValue(top_color) << 8);
    vertex[0].Green = (COLOR16)(GetGValue(top_color) << 8);
    vertex[0].Blue = (COLOR16)(GetBValue(top_color) << 8);
    vertex[0].Alpha = 0;

    vertex[1].x = rect->right;
    vertex[1].y = rect->bottom;
    vertex[1].Red = (COLOR16)(GetRValue(bottom_color) << 8);
    vertex[1].Green = (COLOR16)(GetGValue(bottom_color) << 8);
    vertex[1].Blue = (COLOR16)(GetBValue(bottom_color) << 8);
    vertex[1].Alpha = 0;

    GradientFill(dc, vertex, 2, &mesh, 1, GRADIENT_FILL_RECT_V);
}

static void fill_spectrum_bar(HDC dc, int x, int bar_width, int graph_top, int graph_height,
                              int bar_height)
{
    if (bar_height <= 0)
        return;

    int graph_bottom = graph_top + graph_height;
    int bar_top = graph_bottom - bar_height;
    int green_end = graph_bottom - graph_height / 4;
    int yellow_end = graph_bottom - (graph_height * 3) / 8;
    RECT segment;

    segment.left = x;
    segment.right = x + bar_width;
    segment.top = bar_top > green_end ? bar_top : green_end;
    segment.bottom = graph_bottom;
    if (segment.top < segment.bottom)
    {
        HBRUSH green = CreateSolidBrush(RGB(40, 255, 0));
        FillRect(dc, &segment, green);
        DeleteObject(green);
    }

    segment.top = bar_top > yellow_end ? bar_top : yellow_end;
    segment.bottom = green_end;
    if (segment.top < segment.bottom)
        fill_gradient_rect(dc, &segment,
                           bar_color_at_y(segment.top, graph_top, graph_height),
                           bar_color_at_y(segment.bottom, graph_top, graph_height));

    segment.top = bar_top;
    segment.bottom = yellow_end;
    if (segment.top < segment.bottom)
        fill_gradient_rect(dc, &segment,
                           bar_color_at_y(segment.top, graph_top, graph_height),
                           bar_color_at_y(segment.bottom, graph_top, graph_height));
}

#ifdef LED_SEGMENT_VISUALIZER
static COLORREF led_segment_color(int segment, int segment_count)
{
    float ratio = (float)(segment + 1) / (float)segment_count;

    if (ratio > 0.86f)
        return RGB(255, 42, 32);
    if (ratio > 0.72f)
        return RGB(255, 216, 0);
    return RGB(35, 235, 72);
}

static COLORREF dim_led_color(COLORREF color)
{
    return RGB(GetRValue(color) / 12, GetGValue(color) / 12, GetBValue(color) / 12);
}

static void fill_led_bar(HDC dc, int x, int bar_width, int graph_top, int graph_height,
                         float value)
{
    const int segment_count = 32;
    const int segment_gap = 3;
    int segment_height = (graph_height - (segment_count - 1) * segment_gap) / segment_count;
    int lit_segments;

    if (segment_height < 3)
        segment_height = 3;

    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;

    lit_segments = (int)(value * (float)segment_count + 0.5f);

    for (int segment = 0; segment < segment_count; ++segment)
    {
        int from_bottom = segment;
        int y = graph_top + graph_height - (from_bottom + 1) * segment_height - from_bottom * segment_gap;
        COLORREF base_color = led_segment_color(segment, segment_count);
        COLORREF color = segment < lit_segments ? base_color : dim_led_color(base_color);
        HBRUSH brush = CreateSolidBrush(color);
        RECT rect = {
            x,
            y,
            x + bar_width,
            y + segment_height
        };

        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }
}
#endif

static void fft_in_place(float *real, float *imag, size_t count)
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

static void analyze_float_samples(filter_sys_t *sys, const float *samples, size_t frame_count, size_t channels)
{
    float next[BAR_COUNT];
    float real[FFT_SIZE];
    float imag[FFT_SIZE];
    memset(next, 0, sizeof(next));
    memset(imag, 0, sizeof(imag));

    if (frame_count == 0 || channels == 0)
        return;

    for (size_t i = 0; i < FFT_SIZE; ++i)
    {
        size_t frame = i % frame_count;
        const float *sample = samples + frame * channels;
        float mono = 0.0f;

        for (size_t ch = 0; ch < channels; ++ch)
            mono += sample[ch];
        mono /= (float)channels;

        real[i] = mono * (0.5f - 0.5f * cosf((float)(2.0 * 3.14159265358979323846 * (double)i / (double)(FFT_SIZE - 1))));
    }

    fft_in_place(real, imag, FFT_SIZE);

    for (int bar = 0; bar < BAR_COUNT; ++bar)
    {
        float band_peak = 0.0f;
        int start = spectrum_xscale[bar];
        int end = spectrum_xscale[bar + 1];

        for (int bin = start; bin < end && bin < FFT_SIZE / 2; ++bin)
        {
            float magnitude = sqrtf(real[bin] * real[bin] + imag[bin] * imag[bin]);
            if (magnitude > band_peak)
                band_peak = magnitude;
        }

        /*
         * VLC's Spectrum uses fixed FFT buckets plus log(y + 0.1) * 30.
         * Our FFT operates on normalized floats, so this scale puts a full
         * amplitude sine wave in roughly the same integer range as VLC's s16
         * FFT output without letting bass-heavy songs suppress the high bands.
         */
        float y = band_peak * (65536.0f / (float)FFT_SIZE);
        float height = 0.0f;
        if (y > 0.0f)
        {
            height = logf(y + SPECTRUM_LOG_OFFSET) * 30.0f;
            if (height < 0.0f)
                height = 0.0f;
            if (height > SPECTRUM_MAX_HEIGHT)
                height = SPECTRUM_MAX_HEIGHT;
        }

        next[bar] = height / SPECTRUM_MAX_HEIGHT;
    }

    EnterCriticalSection(&sys->lock);
    for (int i = 0; i < BAR_COUNT; ++i)
    {
        float current = sys->analysis_bars[i];
        float decrease = SPECTRUM_BAR_DECREASE / SPECTRUM_MAX_HEIGHT;
        if (next[i] <= current - decrease)
            sys->analysis_bars[i] = current - decrease;
        else
            sys->analysis_bars[i] = next[i];
    }
    memcpy(sys->bars, sys->analysis_bars, sizeof(sys->bars));
    LeaveCriticalSection(&sys->lock);
}

static void analyze_s16_samples(filter_sys_t *sys, const int16_t *samples, size_t frame_count, size_t channels)
{
    float stack_samples[FFT_SIZE * 8];
    size_t sample_count = frame_count * channels;
    size_t count = sample_count < ARRAYSIZE(stack_samples) ? sample_count : ARRAYSIZE(stack_samples);

    if (count == 0)
        return;

    size_t stride = sample_count / count;
    if (stride == 0)
        stride = 1;

    for (size_t i = 0; i < count; ++i)
        stack_samples[i] = (float)samples[i * stride] / 32768.0f;

    analyze_float_samples(sys, stack_samples, count / channels, channels);
}

static void decay_bars(filter_sys_t *sys)
{
    EnterCriticalSection(&sys->lock);
    for (int i = 0; i < BAR_COUNT; ++i)
    {
        sys->analysis_bars[i] *= 0.96f;
        sys->bars[i] *= 0.96f;
    }
    LeaveCriticalSection(&sys->lock);
}

static bool create_render_surface(filter_sys_t *sys, int width, int height)
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

static uint8_t clamp_byte(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

static void draw_native_frame(filter_sys_t *sys)
{
    float bars[BAR_COUNT];
#ifndef LED_SEGMENT_VISUALIZER
    wchar_t track_text[512];
#endif
    RECT rc = { 0, 0, sys->render_width, sys->render_height };

    EnterCriticalSection(&sys->lock);
    memcpy(bars, sys->bars, sizeof(bars));
#ifndef LED_SEGMENT_VISUALIZER
    wcsncpy(track_text, sys->track_text, ARRAYSIZE(track_text) - 1);
    track_text[ARRAYSIZE(track_text) - 1] = L'\0';
#endif
    LeaveCriticalSection(&sys->lock);

    HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(sys->render_dc, &rc, background);
    DeleteObject(background);

#ifdef LED_SEGMENT_VISUALIZER
    int label_band = 40;
    int graph_top = 24;
    int graph_height = sys->render_height - label_band - graph_top - 12;
    int gap = 6;
    int side_pad = 22;
    int bar_width = (sys->render_width - side_pad * 2 - (BAR_COUNT - 1) * gap) / BAR_COUNT;
    if (bar_width < 8)
        bar_width = 8;

    for (int i = 0; i < BAR_COUNT; ++i)
    {
        int x = side_pad + i * (bar_width + gap);
        fill_led_bar(sys->render_dc, x, bar_width, graph_top, graph_height, bars[i]);
    }

    SetBkMode(sys->render_dc, TRANSPARENT);
    SetTextColor(sys->render_dc, RGB(230, 235, 232));

    HFONT label_font = CreateFontW(
        -9, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(sys->render_dc, label_font);

    for (int i = 0; i < BAR_COUNT; ++i)
    {
        int x = side_pad + i * (bar_width + gap);
        RECT label_rc = { x - gap / 2, sys->render_height - label_band + 8,
                          x + bar_width + gap / 2, sys->render_height - 8 };
        DrawTextW(sys->render_dc, frequency_labels[i], -1, &label_rc,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SelectObject(sys->render_dc, old_font);
    DeleteObject(label_font);
#else
    int text_band = 58;
    int graph_height = sys->render_height - text_band - 18;
    int gap = 2;
    int side_pad = 18;
    int bar_width = (sys->render_width - side_pad * 2 - (BAR_COUNT - 1) * gap) / BAR_COUNT;
    if (bar_width < 2)
        bar_width = 2;

    for (int i = 0; i < BAR_COUNT; ++i)
    {
        float value = bars[i];
        if (value < 0.0f)
            value = 0.0f;
        if (value > 1.0f)
            value = 1.0f;

        int h = (int)(value * (float)graph_height);
        int x = side_pad + i * (bar_width + gap);
        fill_spectrum_bar(sys->render_dc, x, bar_width, 0, graph_height, h);
    }

    SetBkMode(sys->render_dc, TRANSPARENT);
    SetTextColor(sys->render_dc, RGB(245, 248, 255));

    HFONT font = CreateFontW(
        -24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(sys->render_dc, font);

    RECT text_rc = { 20, sys->render_height - text_band + 8, sys->render_width - 20, sys->render_height - 8 };
    DrawTextW(sys->render_dc, track_text, -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(sys->render_dc, old_font);
    DeleteObject(font);
#endif
}

static void copy_bgra_to_i420(filter_sys_t *sys, picture_t *picture)
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
            dst_y[x] = clamp_byte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
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
            dst_u[x / 2] = clamp_byte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            dst_v[x / 2] = clamp_byte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

static void render_vlc_picture(filter_t *filter, block_t *block)
{
    filter_sys_t *sys = filter->p_sys;
    if (sys->vout == NULL || sys->render_pixels == NULL)
        return;

    picture_t *picture = vout_GetPicture(sys->vout);
    if (picture == NULL)
        return;

    picture->b_progressive = true;
    draw_native_frame(sys);
    copy_bgra_to_i420(sys, picture);
    picture->date = block->i_pts + (block->i_length / 2);
    vout_PutPicture(sys->vout, picture);
}

static block_t *Filter(filter_t *filter, block_t *block)
{
    if (block == NULL)
        return NULL;

    filter_sys_t *sys = filter->p_sys;

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

    if (filter->fmt_in.audio.i_format == VLC_CODEC_FL32)
        analyze_float_samples(sys, (const float *)block->p_buffer, (size_t)block->i_nb_samples, channels);
    else if (filter->fmt_in.audio.i_format == VLC_CODEC_S16N)
        analyze_s16_samples(sys, (const int16_t *)block->p_buffer, (size_t)block->i_nb_samples, channels);
    else
        decay_bars(sys);

    render_vlc_picture(filter, block);

    return block;
}

static int Open(vlc_object_t *object)
{
    filter_t *filter = (filter_t *)object;
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (sys == NULL)
        return VLC_ENOMEM;

    InitializeCriticalSection(&sys->lock);
    wcscpy(sys->track_text, L"Waiting for track metadata...");
    if (!create_render_surface(sys, VIDEO_WIDTH, VIDEO_HEIGHT))
    {
        DeleteCriticalSection(&sys->lock);
        free(sys);
        return VLC_EGENERIC;
    }

    filter->p_sys = sys;
    video_format_t fmt = {
        .i_chroma = VLC_CODEC_I420,
        .i_width = VIDEO_WIDTH,
        .i_height = VIDEO_HEIGHT,
        .i_visible_width = VIDEO_WIDTH,
        .i_visible_height = VIDEO_HEIGHT,
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

static void Close(vlc_object_t *object)
{
    filter_t *filter = (filter_t *)object;
    filter_sys_t *sys = filter->p_sys;
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
