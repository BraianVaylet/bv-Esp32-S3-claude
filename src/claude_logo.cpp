#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>

#include "claude_logo.h"

// Ray profile, all normalised to the outer radius:
//   the ray is a kite that grows from the centre to its widest point at RM
//   and then tapers to a point at the rim.
static constexpr float RM     = 0.40f;   // radius of the widest point
static constexpr float HALF_W = 0.19f;   // half width there
static constexpr float R_CORE = 0.055f;  // small disc so the rays join cleanly
static constexpr int   SS     = 3;       // supersampling factor per axis

static inline bool inside_ray(float lx, float ly)
{
    if (lx < 0.0f || lx > 1.0f) return false;
    const float ay = fabsf(ly);
    if (lx <= RM) return ay <= HALF_W * (lx / RM);
    return ay <= HALF_W * ((1.0f - lx) / (1.0f - RM));
}

// Alpha for the pixel whose top-left corner is (x, y), in pixel coordinates.
static uint8_t coverage(int x, int y, float half, float step)
{
    int hits = 0;
    for (int sy = 0; sy < SS; sy++) {
        for (int sx = 0; sx < SS; sx++) {
            const float nx = (x + (sx + 0.5f) / SS - half) / half;
            const float ny = (y + (sy + 0.5f) / SS - half) / half;
            const float r  = sqrtf(nx * nx + ny * ny);
            if (r > 1.0f) continue;
            if (r <= R_CORE) { hits++; continue; }

            // Fold the point into a single canonical ray sector.
            const float a  = atan2f(ny, nx);
            const float a2 = a - roundf(a / step) * step;
            if (inside_ray(r * cosf(a2), r * sinf(a2))) hits++;
        }
    }
    return static_cast<uint8_t>((hits * 255) / (SS * SS));
}

// Claude Code's mascot on a 16x9 grid: body with two eye notches, side tabs,
// four legs. '#' is a filled cell, '.' is transparent. Kept as text so the
// shape stays editable at a glance.
static const char *CLAWD[CLAWD_ROWS] = {
    "....##########..",
    "....##########..",
    "....##.####.##..",
    "....##.####.##..",
    "..##############",
    "..##############",
    "....##########..",
    ".....#.#..#.#...",
    ".....#.#..#.#...",
};

lv_obj_t *claude_code_logo_create(lv_obj_t *parent, int block, uint32_t color)
{
    if (block < 1) block = 1;
    const int w = CLAWD_COLS * block;
    const int h = CLAWD_ROWS * block;

    const size_t bytes = static_cast<size_t>(w) * h * 4 + 64;
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) buf = heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
    if (!buf) return nullptr;
    memset(buf, 0, bytes);

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buf, w, h, LV_COLOR_FORMAT_ARGB8888);

    const lv_color_t col = lv_color_hex(color);
    for (int row = 0; row < CLAWD_ROWS; row++) {
        for (int cellX = 0; cellX < CLAWD_COLS; cellX++) {
            if (CLAWD[row][cellX] != '#') continue;
            for (int dy = 0; dy < block; dy++)
                for (int dx = 0; dx < block; dx++)
                    lv_canvas_set_px(canvas, cellX * block + dx, row * block + dy, col, LV_OPA_COVER);
        }
    }

    lv_obj_set_size(canvas, w, h);
    return canvas;
}

lv_obj_t *claude_logo_create(lv_obj_t *parent, int size, uint32_t color, int rays)
{
    if (rays <= 0) rays = (size < 40) ? 8 : 12;

    const size_t bytes = static_cast<size_t>(size) * size * 4 + 64;
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) buf = heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
    if (!buf) return nullptr;
    memset(buf, 0, bytes);

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buf, size, size, LV_COLOR_FORMAT_ARGB8888);

    const lv_color_t col  = lv_color_hex(color);
    const float      half = size / 2.0f;
    const float      step = 2.0f * (float)M_PI / rays;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            const uint8_t a = coverage(x, y, half, step);
            if (a) lv_canvas_set_px(canvas, x, y, col, a);
        }
    }

    lv_obj_set_size(canvas, size, size);
    return canvas;
}
