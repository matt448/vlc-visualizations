/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matthew McMillan
 */

#include "visualizer_common.h"

#include <math.h>
#include <string.h>

#define BAR_COUNT 40
#define FFT_SIZE 1024
#define VIDEO_WIDTH 900
#define VIDEO_HEIGHT 520
#define BRICK_ROWS 10
#define BALL_RADIUS 8
#define FIELD_SIDE_PAD 30
#define BRICK_TOP 48
#define BRICK_GAP 2
#define BRICK_HEIGHT 13
#define FIELD_BOTTOM_PAD 100

static void breakout_band_range(int bar, unsigned sample_rate, int *start, int *end)
{
    const float min_hz = 35.0f;
    float nyquist = (float)sample_rate / 2.0f;
    float low = min_hz * powf(nyquist / min_hz, (float)bar / (float)BAR_COUNT);
    float high = min_hz * powf(nyquist / min_hz, (float)(bar + 1) / (float)BAR_COUNT);

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

static float normalize_band(visualizer_sys_t *sys, const float magnitudes[BAR_COUNT], int bar)
{
    float frame_peak = 0.0f;

    for (int i = 0; i < BAR_COUNT; ++i)
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

    return powf(magnitudes[bar] / peak, 0.62f);
}

static void initialize_game(visualizer_sys_t *sys)
{
    if (sys->game_initialized)
        return;

    sys->ball_x = 0.5f;
    sys->ball_y = 0.76f;
    sys->ball_vx = 0.006f;
    sys->ball_vy = -0.008f;
    sys->paddle_x = 0.5f;
    sys->game_initialized = true;
}

static void reset_bricks_for_track(visualizer_sys_t *sys)
{
    if (wcscmp(sys->game_track_text, sys->track_text) == 0)
        return;

    memset(sys->brick_broken, 0, sizeof(sys->brick_broken));
    memset(sys->brick_flash_frames, 0, sizeof(sys->brick_flash_frames));
    memset(sys->brick_flash_rows, 0, sizeof(sys->brick_flash_rows));
    wcsncpy(sys->game_track_text, sys->track_text, ARRAYSIZE(sys->game_track_text) - 1);
    sys->game_track_text[ARRAYSIZE(sys->game_track_text) - 1] = L'\0';
    sys->game_initialized = false;
    sys->silence_reset_done = false;
    sys->silence_start_tick = 0;
}

static void reset_bricks_after_silence(visualizer_sys_t *sys)
{
    memset(sys->brick_broken, 0, sizeof(sys->brick_broken));
    memset(sys->brick_flash_frames, 0, sizeof(sys->brick_flash_frames));
    memset(sys->brick_flash_rows, 0, sizeof(sys->brick_flash_rows));
    sys->game_initialized = false;
}

static void update_silence_reset(visualizer_sys_t *sys, float rms)
{
    const DWORD silence_reset_delay_ms = 2000;
    const float bar_threshold = 0.015f;
    const float rms_threshold = 0.0025f;
    bool silent = rms < rms_threshold;

    for (int i = 0; i < BAR_COUNT && silent; ++i)
    {
        if (sys->bars[i] > bar_threshold)
            silent = false;
    }

    DWORD now = GetTickCount();
    if (!silent)
    {
        sys->silence_start_tick = 0;
        sys->silence_reset_done = false;
        return;
    }

    if (sys->silence_start_tick == 0)
    {
        sys->silence_start_tick = now;
        return;
    }

    if (!sys->silence_reset_done && now - sys->silence_start_tick >= silence_reset_delay_ms)
    {
        reset_bricks_after_silence(sys);
        sys->silence_reset_done = true;
    }
}

static bool ball_overlaps_rect(float ball_x, float ball_y, RECT rect)
{
    float closest_x = ball_x;
    float closest_y = ball_y;

    if (closest_x < (float)rect.left)
        closest_x = (float)rect.left;
    if (closest_x > (float)rect.right)
        closest_x = (float)rect.right;
    if (closest_y < (float)rect.top)
        closest_y = (float)rect.top;
    if (closest_y > (float)rect.bottom)
        closest_y = (float)rect.bottom;

    float dx = ball_x - closest_x;
    float dy = ball_y - closest_y;
    return dx * dx + dy * dy <= (float)(BALL_RADIUS * BALL_RADIUS);
}

static int brick_collision_axis(float previous_x, float previous_y, RECT rect)
{
    if (previous_x + (float)BALL_RADIUS <= (float)rect.left ||
        previous_x - (float)BALL_RADIUS >= (float)rect.right)
        return 0;

    if (previous_y + (float)BALL_RADIUS <= (float)rect.top ||
        previous_y - (float)BALL_RADIUS >= (float)rect.bottom)
        return 1;

    float left_penetration = fabsf(previous_x - (float)rect.left);
    float right_penetration = fabsf(previous_x - (float)rect.right);
    float top_penetration = fabsf(previous_y - (float)rect.top);
    float bottom_penetration = fabsf(previous_y - (float)rect.bottom);
    float x_penetration = left_penetration < right_penetration ? left_penetration : right_penetration;
    float y_penetration = top_penetration < bottom_penetration ? top_penetration : bottom_penetration;

    return x_penetration < y_penetration ? 0 : 1;
}

static bool break_touched_lit_brick(visualizer_sys_t *sys, float previous_x, float previous_y,
                                    float ball_x, float ball_y, int *collision_axis)
{
    int field_top = BRICK_TOP - 20;
    int field_bottom = sys->render_height - FIELD_BOTTOM_PAD;
    int field_left = FIELD_SIDE_PAD;
    int field_right = sys->render_width - FIELD_SIDE_PAD;
    int brick_width = (sys->render_width - FIELD_SIDE_PAD * 2 - (BAR_COUNT - 1) * BRICK_GAP) / BAR_COUNT;
    float previous_px = (float)field_left + previous_x * (float)(field_right - field_left);
    float previous_py = (float)field_top + previous_y * (float)(field_bottom - field_top);
    float ball_px = (float)field_left + ball_x * (float)(field_right - field_left);
    float ball_py = (float)field_top + ball_y * (float)(field_bottom - field_top);

    for (int col = 0; col < BAR_COUNT; ++col)
    {
        int lit_rows = (int)(sys->bars[col] * (float)BRICK_ROWS + 0.5f);
        int x = FIELD_SIDE_PAD + col * (brick_width + BRICK_GAP);

        for (int row = 0; row < BRICK_ROWS; ++row)
        {
            int from_bottom = BRICK_ROWS - row - 1;
            if (from_bottom >= lit_rows || sys->brick_broken[col][row])
                continue;

            RECT brick = {
                x,
                BRICK_TOP + row * (BRICK_HEIGHT + BRICK_GAP),
                x + brick_width,
                BRICK_TOP + row * (BRICK_HEIGHT + BRICK_GAP) + BRICK_HEIGHT
            };

            if (!ball_overlaps_rect(ball_px, ball_py, brick))
                continue;

            sys->brick_broken[col][row] = 1;
            sys->brick_energy[col] = 1.0f;
            sys->brick_flash_frames[col] = 10;
            sys->brick_flash_rows[col] = row;
            *collision_axis = brick_collision_axis(previous_px, previous_py, brick);
            return true;
        }
    }

    return false;
}

static void update_game_motion(visualizer_sys_t *sys)
{
    const float paddle_y = 0.965f;
    const float paddle_half_width = 0.090f;
    const float wall_margin = (float)BALL_RADIUS / (float)(VIDEO_WIDTH - FIELD_SIDE_PAD * 2);
    const float speed_boost = 0.88f;
    float target_x;
    float follow_rate;
    float next_x;
    float next_y;
    int collision_axis = 1;

    initialize_game(sys);

    target_x = sys->ball_vy > 0.0f ? sys->ball_x : 0.5f + (sys->ball_x - 0.5f) * 0.35f;
    follow_rate = sys->ball_vy > 0.0f ? 0.22f : 0.08f;
    sys->paddle_x += (target_x - sys->paddle_x) * follow_rate;
    if (sys->paddle_x < paddle_half_width)
        sys->paddle_x = paddle_half_width;
    if (sys->paddle_x > 1.0f - paddle_half_width)
        sys->paddle_x = 1.0f - paddle_half_width;

    next_x = sys->ball_x + sys->ball_vx * speed_boost;
    next_y = sys->ball_y + sys->ball_vy * speed_boost;

    if (next_x < wall_margin || next_x > 1.0f - wall_margin)
    {
        sys->ball_vx = -sys->ball_vx;
        if (next_x < wall_margin)
            next_x = wall_margin;
        else
            next_x = 1.0f - wall_margin;
        sys->ball_x = next_x;
        next_x = sys->ball_x + sys->ball_vx * speed_boost;
    }

    if (next_y < 0.08f)
    {
        sys->ball_vy = fabsf(sys->ball_vy);
        next_y = sys->ball_y + sys->ball_vy * speed_boost;
    }

    if (sys->ball_vy > 0.0f && sys->ball_y < paddle_y && next_y >= paddle_y)
    {
        float distance = fabsf(next_x - sys->paddle_x);
        if (distance < paddle_half_width)
        {
            float english = (next_x - sys->paddle_x) / paddle_half_width;
            sys->ball_vy = -fabsf(sys->ball_vy);
            sys->ball_vx += english * 0.006f;
            if (sys->ball_vx < -0.012f)
                sys->ball_vx = -0.012f;
            if (sys->ball_vx > 0.012f)
                sys->ball_vx = 0.012f;
            next_y = paddle_y - 0.018f;
        }
    }

    if (next_y > 0.995f)
    {
        sys->ball_x = sys->paddle_x;
        sys->ball_y = paddle_y - 0.05f;
        sys->ball_vx = sys->ball_vx < 0.0f ? -0.007f : 0.007f;
        sys->ball_vy = -0.008f;
        return;
    }

    if (break_touched_lit_brick(sys, sys->ball_x, sys->ball_y, next_x, next_y, &collision_axis))
    {
        if (collision_axis == 0)
        {
            sys->ball_vx = -sys->ball_vx;
            next_x = sys->ball_x + sys->ball_vx * speed_boost;
        }
        else
        {
            sys->ball_vy = -sys->ball_vy;
            next_y = sys->ball_y + sys->ball_vy * speed_boost;
        }
    }

    sys->ball_x = next_x;
    sys->ball_y = next_y;
}

static void breakout_analyze(visualizer_sys_t *sys, const float *samples, size_t frame_count,
                             size_t channels, unsigned sample_rate)
{
    float real[FFT_SIZE];
    float imag[FFT_SIZE];
    float magnitudes[BAR_COUNT];
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

    for (int bar = 0; bar < BAR_COUNT; ++bar)
    {
        float band_peak = 0.0f;
        int start;
        int end;
        breakout_band_range(bar, sample_rate, &start, &end);

        for (int bin = start; bin < end; ++bin)
        {
            float magnitude = sqrtf(real[bin] * real[bin] + imag[bin] * imag[bin]);
            if (magnitude > band_peak)
                band_peak = magnitude;
        }

        magnitudes[bar] = band_peak;
    }

    EnterCriticalSection(&sys->lock);
    reset_bricks_for_track(sys);
    for (int i = 0; i < BAR_COUNT; ++i)
    {
        float value = normalize_band(sys, magnitudes, i);
        if (value > 1.0f)
            value = 1.0f;

        if (value > sys->bars[i])
            sys->bars[i] += (value - sys->bars[i]) * 0.65f;
        else
            sys->bars[i] *= 0.90f;

        sys->brick_energy[i] *= 0.94f;
        if (value > sys->brick_energy[i])
            sys->brick_energy[i] = value;
        if (sys->brick_flash_frames[i] > 0)
            sys->brick_flash_frames[i]--;
    }

    sys->overall_level = sys->overall_level * 0.86f + fminf(rms * 7.0f, 1.0f) * 0.14f;
    update_silence_reset(sys, rms);
    update_game_motion(sys);
    LeaveCriticalSection(&sys->lock);
}

static COLORREF brick_color(int row, float energy)
{
    static const COLORREF palette[5] = {
        RGB(48, 236, 92),
        RGB(48, 210, 235),
        RGB(255, 218, 55),
        RGB(255, 128, 46),
        RGB(255, 58, 65)
    };
    int color_row = row * 5 / BRICK_ROWS;
    COLORREF base = palette[color_row];
    float mix = 0.22f + energy * 0.78f;

    return RGB((int)(GetRValue(base) * mix), (int)(GetGValue(base) * mix),
               (int)(GetBValue(base) * mix));
}

static COLORREF flash_color(int row)
{
    static const COLORREF palette[5] = {
        RGB(170, 255, 190),
        RGB(170, 250, 255),
        RGB(255, 248, 170),
        RGB(255, 205, 145),
        RGB(255, 175, 175)
    };
    int color_row = row * 5 / BRICK_ROWS;

    return palette[color_row];
}

static void draw_ball(HDC dc, int cx, int cy, int radius, float level)
{
    HBRUSH ball_brush = CreateSolidBrush(RGB(245, 252, 255));
    HGDIOBJ old_brush;

    (void)level;
    old_brush = SelectObject(dc, ball_brush);
    Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc, old_brush);

    DeleteObject(ball_brush);
}

static void uppercase_text(wchar_t *text)
{
    for (size_t i = 0; text[i] != L'\0'; ++i)
    {
        if (text[i] >= L'a' && text[i] <= L'z')
            text[i] = (wchar_t)(text[i] - L'a' + L'A');
    }
}

static void breakout_draw(visualizer_sys_t *sys)
{
    float bars[BAR_COUNT];
    float bricks[BAR_COUNT];
    unsigned flashes[BAR_COUNT];
    int flash_rows[BAR_COUNT];
    uint8_t broken[BAR_COUNT][16];
    float ball_x;
    float ball_y;
    float paddle_x;
    float level;
    wchar_t track_text[512];
    RECT rc = { 0, 0, sys->render_width, sys->render_height };

    EnterCriticalSection(&sys->lock);
    memcpy(bars, sys->bars, sizeof(bars));
    memcpy(bricks, sys->brick_energy, sizeof(bricks));
    memcpy(flashes, sys->brick_flash_frames, sizeof(flashes));
    memcpy(flash_rows, sys->brick_flash_rows, sizeof(flash_rows));
    memcpy(broken, sys->brick_broken, sizeof(broken));
    ball_x = sys->ball_x;
    ball_y = sys->ball_y;
    paddle_x = sys->paddle_x;
    level = sys->overall_level;
    wcsncpy(track_text, sys->track_text, ARRAYSIZE(track_text) - 1);
    track_text[ARRAYSIZE(track_text) - 1] = L'\0';
    LeaveCriticalSection(&sys->lock);
    uppercase_text(track_text);

    HBRUSH background = CreateSolidBrush(RGB(3, 4, 7));
    FillRect(sys->render_dc, &rc, background);
    DeleteObject(background);

    int side_pad = FIELD_SIDE_PAD;
    int top = BRICK_TOP;
    int brick_gap = BRICK_GAP;
    int rows = BRICK_ROWS;
    int brick_h = BRICK_HEIGHT;
    int brick_w = (sys->render_width - side_pad * 2 - (BAR_COUNT - 1) * brick_gap) / BAR_COUNT;

    HPEN rail_pen = CreatePen(PS_SOLID, 2, RGB(42, 55, 66));
    HGDIOBJ old_pen = SelectObject(sys->render_dc, rail_pen);
    MoveToEx(sys->render_dc, side_pad - 12, top - 18, NULL);
    LineTo(sys->render_dc, side_pad - 12, sys->render_height - 92);
    MoveToEx(sys->render_dc, sys->render_width - side_pad + 12, top - 18, NULL);
    LineTo(sys->render_dc, sys->render_width - side_pad + 12, sys->render_height - 92);
    SelectObject(sys->render_dc, old_pen);
    DeleteObject(rail_pen);

    for (int col = 0; col < BAR_COUNT; ++col)
    {
        int lit_rows = (int)(bars[col] * (float)rows + 0.5f);
        int x = side_pad + col * (brick_w + brick_gap);

        for (int row = 0; row < rows; ++row)
        {
            int from_bottom = rows - row - 1;
            float energy = bricks[col];
            bool is_flashing = flashes[col] > 0 && flash_rows[col] == row;
            if (broken[col][row] && !is_flashing)
                continue;

            if (from_bottom >= lit_rows)
                energy *= 0.22f;

            COLORREF color = is_flashing ? flash_color(row) : brick_color(row, energy);
            HBRUSH brush = CreateSolidBrush(color);
            RECT brick = {
                x,
                top + row * (brick_h + brick_gap),
                x + brick_w,
                top + row * (brick_h + brick_gap) + brick_h
            };
            FillRect(sys->render_dc, &brick, brush);
            DeleteObject(brush);
        }
    }

    int field_top = top - 20;
    int field_bottom = sys->render_height - FIELD_BOTTOM_PAD;
    int field_left = side_pad;
    int field_right = sys->render_width - side_pad;
    int ball_px = field_left + (int)(ball_x * (float)(field_right - field_left));
    int ball_py = field_top + (int)(ball_y * (float)(field_bottom - field_top));
    draw_ball(sys->render_dc, ball_px, ball_py, BALL_RADIUS, level);

    int paddle_w = 86 + (int)(level * 34.0f);
    int paddle_h = 13;
    int paddle_center = field_left + (int)(paddle_x * (float)(field_right - field_left));
    HBRUSH paddle_brush = CreateSolidBrush(RGB(235, 245, 255));
    RECT paddle = {
        paddle_center - paddle_w / 2,
        field_bottom - 14,
        paddle_center + paddle_w / 2,
        field_bottom - 14 + paddle_h
    };
    FillRect(sys->render_dc, &paddle, paddle_brush);
    DeleteObject(paddle_brush);

    SetBkMode(sys->render_dc, TRANSPARENT);
    SetTextColor(sys->render_dc, RGB(116, 232, 255));

    HFONT track_font = CreateFontW(
        -18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ old_font = SelectObject(sys->render_dc, track_font);

    RECT track_rc = { side_pad, sys->render_height - 56, sys->render_width - side_pad,
                      sys->render_height - 18 };
    DrawTextW(sys->render_dc, track_text, -1, &track_rc,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    SelectObject(sys->render_dc, old_font);
    DeleteObject(track_font);
}

static const visualizer_config_t breakout_config = {
    .bar_count = BAR_COUNT,
    .fft_size = FFT_SIZE,
    .video_width = VIDEO_WIDTH,
    .video_height = VIDEO_HEIGHT,
    .analyze = breakout_analyze,
    .draw = breakout_draw,
};

static int Open(vlc_object_t *object)
{
    return visualizer_open(object, &breakout_config);
}

static void Close(vlc_object_t *object)
{
    visualizer_close(object);
}

vlc_module_begin()
    set_shortname("Breakout")
    set_description("Breakout-inspired audio visualization with frequency bricks")
    set_capability("visualization", 0)
    set_subcategory(SUBCAT_AUDIO_VISUAL)
    add_shortcut("breakout", "breakout_visualizer")
    set_callbacks(Open, Close)
vlc_module_end()
