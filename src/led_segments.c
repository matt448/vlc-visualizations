/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matthew McMillan
 */

#include "visualizer_common.h"

#include <math.h>
#include <string.h>

#define BAR_COUNT 31
#define FFT_SIZE 4096
#define VIDEO_WIDTH 1050
#define VIDEO_HEIGHT 460

static const float frequency_centers[BAR_COUNT] = {
    20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f,
    125.0f, 160.0f, 200.0f, 250.0f, 315.0f, 400.0f, 500.0f, 630.0f,
    800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f, 2500.0f, 3150.0f, 4000.0f,
    5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f, 16000.0f, 20000.0f
};

static const wchar_t *frequency_labels[BAR_COUNT] = {
    L"20", L"25", L"31.5", L"40", L"50", L"63", L"80", L"100",
    L"125", L"160", L"200", L"250", L"315", L"400", L"500", L"630",
    L"800", L"1K", L"1.25K", L"1.6K", L"2K", L"2.5K", L"3.15K", L"4K",
    L"5K", L"6.3K", L"8K", L"10K", L"12.5K", L"16K", L"20K"
};

static void led_band_range(int bar, unsigned sample_rate, int *start, int *end)
{
    float nyquist = (float)sample_rate / 2.0f;
    float low;
    float high;

    if (bar == 0)
        low = frequency_centers[bar] / sqrtf(frequency_centers[bar + 1] / frequency_centers[bar]);
    else
        low = sqrtf(frequency_centers[bar - 1] * frequency_centers[bar]);

    if (bar == BAR_COUNT - 1)
        high = frequency_centers[bar] * sqrtf(frequency_centers[bar] / frequency_centers[bar - 1]);
    else
        high = sqrtf(frequency_centers[bar] * frequency_centers[bar + 1]);

    if (low < 0.0f)
        low = 0.0f;
    if (high > nyquist)
        high = nyquist;

    *start = (int)floorf((low * (float)FFT_SIZE) / (float)sample_rate);
    *end = (int)ceilf((high * (float)FFT_SIZE) / (float)sample_rate);

    if (*start < 1)
        *start = 1;
    if (*start >= FFT_SIZE / 2)
        *start = (FFT_SIZE / 2) - 1;
    if (*end <= *start)
        *end = *start + 1;
    if (*end > FFT_SIZE / 2)
        *end = FFT_SIZE / 2;
}

static void push_history(visualizer_sys_t *sys, const float *samples, size_t frame_count,
                         size_t channels)
{
    for (size_t frame = 0; frame < frame_count; ++frame)
    {
        const float *sample = samples + frame * channels;
        float mono = 0.0f;

        for (size_t ch = 0; ch < channels; ++ch)
            mono += sample[ch];
        mono /= (float)channels;

        sys->sample_history[sys->sample_history_pos] = mono;
        sys->sample_history_pos = (sys->sample_history_pos + 1) % FFT_SIZE;
        if (sys->sample_history_count < FFT_SIZE)
            sys->sample_history_count++;
    }
}

static void load_history_window(visualizer_sys_t *sys, float *real)
{
    size_t filled = sys->sample_history_count;
    size_t zero_count = FFT_SIZE - filled;
    size_t start = (sys->sample_history_pos + FFT_SIZE - filled) % FFT_SIZE;

    for (size_t i = 0; i < zero_count; ++i)
        real[i] = 0.0f;

    for (size_t i = 0; i < filled; ++i)
        real[zero_count + i] = sys->sample_history[(start + i) % FFT_SIZE];
}

static float normalize_band(visualizer_sys_t *sys, const float magnitudes[BAR_COUNT], int bar)
{
    float frame_peak = 0.0f;

    for (int i = 0; i < BAR_COUNT; ++i)
    {
        if (magnitudes[i] > frame_peak)
            frame_peak = magnitudes[i];
    }

    float peak = sys->adaptive_peak * 0.985f;
    if (frame_peak > peak)
        peak = frame_peak;
    if (peak < 1.0f)
        peak = 1.0f;
    sys->adaptive_peak = peak;

    return powf(magnitudes[bar] / peak, 0.58f);
}

static void led_analyze(visualizer_sys_t *sys, const float *samples, size_t frame_count,
                        size_t channels, unsigned sample_rate)
{
    float next[BAR_COUNT];
    float real[FFT_SIZE];
    float imag[FFT_SIZE];
    float magnitudes[BAR_COUNT];
    memset(next, 0, sizeof(next));
    memset(imag, 0, sizeof(imag));
    memset(magnitudes, 0, sizeof(magnitudes));

    if (frame_count == 0 || channels == 0)
        return;

    EnterCriticalSection(&sys->lock);
    push_history(sys, samples, frame_count, channels);
    load_history_window(sys, real);
    LeaveCriticalSection(&sys->lock);

    for (size_t i = 0; i < FFT_SIZE; ++i)
        real[i] *= 0.5f - 0.5f * cosf((float)(2.0 * 3.14159265358979323846 * (double)i / (double)(FFT_SIZE - 1)));

    visualizer_fft_in_place(real, imag, FFT_SIZE);

    for (int bar = 0; bar < BAR_COUNT; ++bar)
    {
        float band_peak = 0.0f;
        int start;
        int end;
        led_band_range(bar, sample_rate, &start, &end);

        for (int bin = start; bin < end && bin < FFT_SIZE / 2; ++bin)
        {
            float magnitude = sqrtf(real[bin] * real[bin] + imag[bin] * imag[bin]);
            if (magnitude > band_peak)
                band_peak = magnitude;
        }

        magnitudes[bar] = band_peak;
    }

    EnterCriticalSection(&sys->lock);
    for (int i = 0; i < BAR_COUNT; ++i)
    {
        next[i] = normalize_band(sys, magnitudes, i);
        if (next[i] > 1.0f)
            next[i] = 1.0f;

        float current = sys->analysis_bars[i];
        float decrease = 0.035f;
        if (next[i] <= current - decrease)
            sys->analysis_bars[i] = current - decrease;
        else
            sys->analysis_bars[i] = next[i];
    }
    memcpy(sys->bars, sys->analysis_bars, sizeof(sys->bars));
    LeaveCriticalSection(&sys->lock);
}

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

    if (segment_height < 3)
        segment_height = 3;

    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;

    int lit_segments = (int)(value * (float)segment_count + 0.5f);

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

static void led_draw(visualizer_sys_t *sys)
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

    int label_band = 28;
    int track_band = 40;
    int graph_top = 24;
    int graph_height = sys->render_height - label_band - track_band - graph_top - 12;
    int gap = 4;
    int side_pad = 14;
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
        RECT label_rc = { x - gap, graph_top + graph_height + 8,
                          x + bar_width + gap, graph_top + graph_height + label_band };
        DrawTextW(sys->render_dc, frequency_labels[i], -1, &label_rc,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SelectObject(sys->render_dc, old_font);
    DeleteObject(label_font);

    HFONT track_font = CreateFontW(
        -17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    old_font = SelectObject(sys->render_dc, track_font);
    SetTextColor(sys->render_dc, RGB(245, 248, 246));

    RECT track_rc = { side_pad, sys->render_height - track_band + 4,
                      sys->render_width - side_pad, sys->render_height - 6 };
    DrawTextW(sys->render_dc, track_text, -1, &track_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(sys->render_dc, old_font);
    DeleteObject(track_font);
}

static const visualizer_config_t led_config = {
    .bar_count = BAR_COUNT,
    .fft_size = FFT_SIZE,
    .video_width = VIDEO_WIDTH,
    .video_height = VIDEO_HEIGHT,
    .analyze = led_analyze,
    .draw = led_draw,
};

static int Open(vlc_object_t *object)
{
    return visualizer_open(object, &led_config);
}

static void Close(vlc_object_t *object)
{
    visualizer_close(object);
}

vlc_module_begin()
    set_shortname("LED Segments")
    set_description("31-band LED segment visualization with frequency labels")
    set_capability("visualization", 0)
    set_subcategory(SUBCAT_AUDIO_VISUAL)
    add_shortcut("led_segments", "led_segment_visualizer")
    set_callbacks(Open, Close)
vlc_module_end()
