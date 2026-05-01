/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matthew McMillan
 */

#include "visualizer_common.h"

#include <math.h>
#include <string.h>

#define BAR_COUNT 80
#define FFT_SIZE 512
#define VIDEO_WIDTH 800
#define VIDEO_HEIGHT 500
#define SPECTRUM_MAX_HEIGHT 380.0f
#define SPECTRUM_LOG_OFFSET 0.1f
#define SPECTRUM_BAR_DECREASE 5.0f

static const int spectrum_xscale[BAR_COUNT + 1] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    61, 63, 67, 72, 77, 82, 87, 93, 99, 105,
    110, 115, 121, 130, 141, 152, 163, 174, 185, 200,
    255
};

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

static void spectrum_analyze(visualizer_sys_t *sys, const float *samples, size_t frame_count,
                             size_t channels, unsigned sample_rate)
{
    (void)sample_rate;

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

    visualizer_fft_in_place(real, imag, FFT_SIZE);

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

static void spectrum_draw(visualizer_sys_t *sys)
{
    float bars[BAR_COUNT];
    wchar_t track_text[512];
    RECT rc = { 0, 0, sys->render_width, sys->render_height };

    EnterCriticalSection(&sys->lock);
    memcpy(bars, sys->bars, sizeof(bars));
    wcsncpy(track_text, sys->track_text, ARRAYSIZE(track_text) - 1);
    track_text[ARRAYSIZE(track_text) - 1] = L'\0';
    LeaveCriticalSection(&sys->lock);

    HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(sys->render_dc, &rc, background);
    DeleteObject(background);

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
}

static const visualizer_config_t spectrum_config = {
    .bar_count = BAR_COUNT,
    .fft_size = FFT_SIZE,
    .video_width = VIDEO_WIDTH,
    .video_height = VIDEO_HEIGHT,
    .analyze = spectrum_analyze,
    .draw = spectrum_draw,
};

static int Open(vlc_object_t *object)
{
    return visualizer_open(object, &spectrum_config);
}

static void Close(vlc_object_t *object)
{
    visualizer_close(object);
}

vlc_module_begin()
    set_shortname("Spectrum Info")
    set_description("Spectrum visualization with persistent track information")
    set_capability("visualization", 0)
    set_subcategory(SUBCAT_AUDIO_VISUAL)
    add_shortcut("spectrum_info", "trackinfo_visualizer")
    set_callbacks(Open, Close)
vlc_module_end()
