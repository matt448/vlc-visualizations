/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matthew McMillan
 */

#include "visualizer_common.h"

#include <math.h>
#include <string.h>

#define LANE_COUNT 16
#define FFT_SIZE 1024
#define VIDEO_WIDTH 900
#define VIDEO_HEIGHT 560

typedef struct point_t
{
    int x;
    int y;
} point_t;

static void tempest_band_range(int lane, unsigned sample_rate, int *start, int *end)
{
    const float min_hz = 45.0f;
    float nyquist = (float)sample_rate / 2.0f;
    float low = min_hz * powf(nyquist / min_hz, (float)lane / (float)LANE_COUNT);
    float high = min_hz * powf(nyquist / min_hz, (float)(lane + 1) / (float)LANE_COUNT);

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

static float normalize_lane(visualizer_sys_t *sys, const float magnitudes[LANE_COUNT], int lane)
{
    float frame_peak = 0.0f;

    for (int i = 0; i < LANE_COUNT; ++i)
    {
        if (magnitudes[i] > frame_peak)
            frame_peak = magnitudes[i];
    }

    float peak = sys->adaptive_peak * 0.975f;
    if (frame_peak > peak)
        peak = frame_peak;
    if (peak < 1.0f)
        peak = 1.0f;
    sys->adaptive_peak = peak;

    return powf(magnitudes[lane] / peak, 0.60f);
}

static int strongest_lane(const float lanes[LANE_COUNT])
{
    int best = 0;
    float best_value = lanes[0];

    for (int i = 1; i < LANE_COUNT; ++i)
    {
        if (lanes[i] > best_value)
        {
            best = i;
            best_value = lanes[i];
        }
    }

    return best;
}

static void update_tempest_game(visualizer_sys_t *sys, const float lanes[LANE_COUNT])
{
    int target_lane = strongest_lane(lanes);
    int player_lane;

    if (!sys->game_initialized)
    {
        sys->paddle_x = 0.0f;
        for (int i = 0; i < LANE_COUNT; ++i)
        {
            sys->vector_enemy_active[i] = 0;
            sys->vector_shot_active[i] = 0;
        }
        sys->game_initialized = true;
    }

    sys->paddle_x += ((float)target_lane - sys->paddle_x) * 0.18f;
    if (sys->paddle_x < 0.0f)
        sys->paddle_x += (float)LANE_COUNT;
    if (sys->paddle_x >= (float)LANE_COUNT)
        sys->paddle_x -= (float)LANE_COUNT;

    player_lane = (int)(sys->paddle_x + 0.5f) % LANE_COUNT;

    for (int lane = 0; lane < LANE_COUNT; ++lane)
    {
        if (sys->vector_enemy_active[lane])
        {
            sys->vector_enemy_depth[lane] -= 0.010f + sys->overall_level * 0.010f;
            if (sys->vector_enemy_depth[lane] <= 0.05f)
                sys->vector_enemy_active[lane] = 0;
        }
        else if (lanes[lane] > 0.72f && ((sys->animation_frame + (unsigned)lane * 7U) % 18U) == 0)
        {
            sys->vector_enemy_active[lane] = 1;
            sys->vector_enemy_depth[lane] = 0.95f;
        }

        if (sys->vector_shot_active[lane])
        {
            sys->vector_shot_depth[lane] += 0.075f + sys->overall_level * 0.035f;
            if (sys->vector_shot_depth[lane] >= 1.0f)
                sys->vector_shot_active[lane] = 0;
        }
    }

    if (!sys->vector_shot_active[player_lane] && lanes[player_lane] > 0.45f)
    {
        sys->vector_shot_active[player_lane] = 1;
        sys->vector_shot_depth[player_lane] = 0.08f;
    }

    if (sys->vector_enemy_active[player_lane] && sys->vector_shot_active[player_lane] &&
        sys->vector_shot_depth[player_lane] >= sys->vector_enemy_depth[player_lane] - 0.08f)
    {
        sys->vector_enemy_active[player_lane] = 0;
        sys->vector_shot_active[player_lane] = 0;
        sys->peak_bars[player_lane] = 1.0f;
    }

    for (int lane = 0; lane < LANE_COUNT; ++lane)
        sys->peak_bars[lane] *= 0.84f;

    sys->animation_frame++;
}

static void tempest_analyze(visualizer_sys_t *sys, const float *samples, size_t frame_count,
                            size_t channels, unsigned sample_rate)
{
    float real[FFT_SIZE];
    float imag[FFT_SIZE];
    float magnitudes[LANE_COUNT];
    float rms = 0.0f;
    size_t usable = frame_count < FFT_SIZE ? frame_count : FFT_SIZE;
    memset(real, 0, sizeof(real));
    memset(imag, 0, sizeof(imag));
    memset(magnitudes, 0, sizeof(magnitudes));

    if (frame_count == 0 || channels == 0)
        return;

    for (size_t i = 0; i < usable; ++i)
    {
        const float *sample = samples + i * channels;
        float mono = 0.0f;

        for (size_t ch = 0; ch < channels; ++ch)
            mono += sample[ch];
        mono /= (float)channels;

        rms += mono * mono;
        real[i] = mono * (0.5f - 0.5f * cosf((float)(2.0 * 3.14159265358979323846 * (double)i / (double)(FFT_SIZE - 1))));
    }

    rms = sqrtf(rms / (float)usable);
    visualizer_fft_in_place(real, imag, FFT_SIZE);

    for (int lane = 0; lane < LANE_COUNT; ++lane)
    {
        float band_peak = 0.0f;
        int start;
        int end;
        tempest_band_range(lane, sample_rate, &start, &end);

        for (int bin = start; bin < end; ++bin)
        {
            float magnitude = sqrtf(real[bin] * real[bin] + imag[bin] * imag[bin]);
            if (magnitude > band_peak)
                band_peak = magnitude;
        }

        magnitudes[lane] = band_peak;
    }

    EnterCriticalSection(&sys->lock);
    for (int lane = 0; lane < LANE_COUNT; ++lane)
    {
        float value = normalize_lane(sys, magnitudes, lane);
        if (value > 1.0f)
            value = 1.0f;

        if (value > sys->bars[lane])
            sys->bars[lane] += (value - sys->bars[lane]) * 0.62f;
        else
            sys->bars[lane] *= 0.88f;
    }
    sys->overall_level = sys->overall_level * 0.86f + fminf(rms * 7.0f, 1.0f) * 0.14f;
    update_tempest_game(sys, sys->bars);
    LeaveCriticalSection(&sys->lock);
}

static point_t tube_point(int width, int height, int lane, float depth)
{
    float angle = -3.14159265358979323846f / 2.0f +
                  (float)lane * (float)(2.0 * 3.14159265358979323846 / (double)LANE_COUNT);
    float center_x = (float)width * 0.5f;
    float center_y = (float)height * 0.43f;
    float near_rx = (float)width * 0.43f;
    float near_ry = (float)height * 0.30f;
    float far_rx = (float)width * 0.075f;
    float far_ry = (float)height * 0.052f;
    float curved = depth * depth;
    float rx = near_rx + (far_rx - near_rx) * curved;
    float ry = near_ry + (far_ry - near_ry) * curved;
    point_t p;

    p.x = (int)(center_x + cosf(angle) * rx);
    p.y = (int)(center_y + sinf(angle) * ry);
    return p;
}

static COLORREF lane_color(int lane, float level)
{
    static const COLORREF colors[4] = {
        RGB(30, 238, 255),
        RGB(92, 125, 255),
        RGB(255, 60, 220),
        RGB(255, 220, 55)
    };
    COLORREF base = colors[lane % 4];
    float mix = 0.45f + level * 0.55f;

    return RGB((int)(GetRValue(base) * mix), (int)(GetGValue(base) * mix),
               (int)(GetBValue(base) * mix));
}

static void draw_line(HDC dc, point_t a, point_t b, COLORREF color, int width)
{
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);

    MoveToEx(dc, a.x, a.y, NULL);
    LineTo(dc, b.x, b.y);

    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

static void draw_ring(HDC dc, int width, int height, float depth, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    point_t first = tube_point(width, height, 0, depth);
    point_t prev = first;

    MoveToEx(dc, first.x, first.y, NULL);
    for (int lane = 1; lane < LANE_COUNT; ++lane)
    {
        point_t p = tube_point(width, height, lane, depth);
        LineTo(dc, p.x, p.y);
        prev = p;
    }
    (void)prev;
    LineTo(dc, first.x, first.y);

    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

static void uppercase_text(wchar_t *text)
{
    for (size_t i = 0; text[i] != L'\0'; ++i)
    {
        if (text[i] >= L'a' && text[i] <= L'z')
            text[i] = (wchar_t)(text[i] - L'a' + L'A');
    }
}

static void draw_diamond(HDC dc, point_t center, int radius, COLORREF color)
{
    POINT points[4] = {
        { center.x, center.y - radius },
        { center.x + radius, center.y },
        { center.x, center.y + radius },
        { center.x - radius, center.y }
    };
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);

    Polygon(dc, points, 4);

    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void tempest_draw(visualizer_sys_t *sys)
{
    float lanes[LANE_COUNT];
    float bursts[LANE_COUNT];
    float enemy_depth[LANE_COUNT];
    float shot_depth[LANE_COUNT];
    unsigned enemy_active[LANE_COUNT];
    unsigned shot_active[LANE_COUNT];
    float player_pos;
    float level;
    wchar_t track_text[512];
    RECT rc = { 0, 0, sys->render_width, sys->render_height };

    EnterCriticalSection(&sys->lock);
    memcpy(lanes, sys->bars, sizeof(lanes));
    memcpy(bursts, sys->peak_bars, sizeof(bursts));
    memcpy(enemy_depth, sys->vector_enemy_depth, sizeof(enemy_depth));
    memcpy(shot_depth, sys->vector_shot_depth, sizeof(shot_depth));
    memcpy(enemy_active, sys->vector_enemy_active, sizeof(enemy_active));
    memcpy(shot_active, sys->vector_shot_active, sizeof(shot_active));
    player_pos = sys->paddle_x;
    level = sys->overall_level;
    wcsncpy(track_text, sys->track_text, ARRAYSIZE(track_text) - 1);
    track_text[ARRAYSIZE(track_text) - 1] = L'\0';
    LeaveCriticalSection(&sys->lock);
    uppercase_text(track_text);

    HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(sys->render_dc, &rc, background);
    DeleteObject(background);

    for (int i = 1; i <= 7; ++i)
    {
        float depth = (float)i / 8.0f;
        int intensity = 24 + (int)(level * 34.0f);
        draw_ring(sys->render_dc, sys->render_width, sys->render_height, depth,
                  RGB(intensity / 2, intensity, intensity + 30));
    }

    for (int lane = 0; lane < LANE_COUNT; ++lane)
    {
        point_t near_point = tube_point(sys->render_width, sys->render_height, lane, 0.0f);
        point_t far_point = tube_point(sys->render_width, sys->render_height, lane, 1.0f);
        COLORREF color = lane_color(lane, lanes[lane]);
        draw_line(sys->render_dc, near_point, far_point, color, lanes[lane] > 0.55f ? 2 : 1);
    }

    for (int lane = 0; lane < LANE_COUNT; ++lane)
    {
        float depth = 0.26f + (float)(lane % 4) * 0.085f;
        point_t base = tube_point(sys->render_width, sys->render_height, lane, depth);
        point_t tip = tube_point(sys->render_width, sys->render_height, lane,
                                 fminf(depth + 0.16f + lanes[lane] * 0.34f, 0.96f));
        int width = 2 + (int)(lanes[lane] * 3.0f);
        draw_line(sys->render_dc, base, tip, lane_color(lane, lanes[lane]), width);

        if (bursts[lane] > 0.08f)
            draw_diamond(sys->render_dc, tip, 5 + (int)(bursts[lane] * 8.0f), RGB(255, 245, 140));
    }

    for (int lane = 0; lane < LANE_COUNT; ++lane)
    {
        if (enemy_active[lane])
        {
            point_t enemy = tube_point(sys->render_width, sys->render_height, lane, enemy_depth[lane]);
            draw_diamond(sys->render_dc, enemy, 7, RGB(255, 74, 82));
        }

        if (shot_active[lane])
        {
            point_t shot = tube_point(sys->render_width, sys->render_height, lane, shot_depth[lane]);
            point_t shot_tail = tube_point(sys->render_width, sys->render_height, lane,
                                           fmaxf(shot_depth[lane] - 0.07f, 0.0f));
            draw_line(sys->render_dc, shot_tail, shot, RGB(255, 245, 130), 3);
        }
    }

    int player_lane = (int)(player_pos + 0.5f) % LANE_COUNT;
    point_t player = tube_point(sys->render_width, sys->render_height, player_lane, 0.0f);
    point_t player_inner = tube_point(sys->render_width, sys->render_height, player_lane, 0.08f);
    draw_diamond(sys->render_dc, player, 10, RGB(130, 255, 255));
    draw_line(sys->render_dc, player, player_inner, RGB(255, 255, 255), 2);

    SetBkMode(sys->render_dc, TRANSPARENT);
    SetTextColor(sys->render_dc, RGB(92, 238, 255));

    HFONT track_font = CreateFontW(
        -18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ old_font = SelectObject(sys->render_dc, track_font);
    RECT text_rc = { 28, sys->render_height - 50, sys->render_width - 28, sys->render_height - 14 };
    DrawTextW(sys->render_dc, track_text, -1, &text_rc,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(sys->render_dc, old_font);
    DeleteObject(track_font);
}

static const visualizer_config_t tempest_config = {
    .bar_count = LANE_COUNT,
    .fft_size = FFT_SIZE,
    .video_width = VIDEO_WIDTH,
    .video_height = VIDEO_HEIGHT,
    .analyze = tempest_analyze,
    .draw = tempest_draw,
};

static int Open(vlc_object_t *object)
{
    return visualizer_open(object, &tempest_config);
}

static void Close(vlc_object_t *object)
{
    visualizer_close(object);
}

vlc_module_begin()
    set_shortname("Tempest")
    set_description("Tempest-inspired vector tube audio visualization")
    set_capability("visualization", 0)
    set_subcategory(SUBCAT_AUDIO_VISUAL)
    add_shortcut("tempest", "tempest_visualizer")
    set_callbacks(Open, Close)
vlc_module_end()
