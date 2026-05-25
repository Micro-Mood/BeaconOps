#include "decode.h"
#include "_fileIO.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// ---------- decoder context shared between PNG / JPG draw callbacks ----------

namespace {
struct DecodeCtx {
    uint16_t *canvas;
    int       width;
    int       height;
};
} // namespace

// ===========================================================================
//  ctor / dtor
// ===========================================================================

decode::decode()
{
    lv_canvas_obj = nullptr;
    canvas        = nullptr;
    width         = 0;
    height        = 0;
}

decode::~decode()
{
    destroy();
}

// ===========================================================================
//  helpers
// ===========================================================================

static decode_type detect_type_from_path(const char *path)
{
    if (!path) return DECODE_AUTO;
    const char *dot = strrchr(path, '.');
    if (!dot) return DECODE_AUTO;
    const char *ext = dot + 1;
    if (!strcasecmp(ext, "png"))                    return DECODE_PNG;
    if (!strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg")) return DECODE_JPG;
    return DECODE_AUTO;
}

// ===========================================================================
//  PNG draw callback - copy one decoded scanline into the RGB565 canvas
// ===========================================================================

void decode::PNGDrawStatic(PNGDRAW *pDraw)
{
    DecodeCtx *ctx = static_cast<DecodeCtx *>(pDraw->pUser);
    if (!ctx || !ctx->canvas) return;
    if (pDraw->y < 0 || pDraw->y >= ctx->height) return;

    // Use the library helper; it handles every PNG colour type.
    // We need a tiny static PNG instance only for the helper, but
    // PNGdec exposes getLineAsRGB565 as a non-static method. Since
    // we don't have a PNG* here, do the conversion inline for the
    // most common pixel types.
    uint16_t *dst = ctx->canvas + pDraw->y * ctx->width;
    int w = pDraw->iWidth;
    if (w > ctx->width) w = ctx->width;

    switch (pDraw->iPixelType) {
    case PNG_PIXEL_TRUECOLOR: {           // 24-bit RGB
        uint8_t *s = pDraw->pPixels;
        for (int i = 0; i < w; ++i) {
            uint8_t r = s[0], g = s[1], b = s[2];
            dst[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            s += 3;
        }
        break;
    }
    case PNG_PIXEL_TRUECOLOR_ALPHA: {     // 32-bit RGBA
        uint8_t *s = pDraw->pPixels;
        for (int i = 0; i < w; ++i) {
            uint8_t r = s[0], g = s[1], b = s[2];
            dst[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            s += 4;
        }
        break;
    }
    case PNG_PIXEL_GRAYSCALE: {           // 8-bit gray (assume 8bpp)
        uint8_t *s = pDraw->pPixels;
        for (int i = 0; i < w; ++i) {
            uint8_t v = s[i];
            dst[i] = ((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3);
        }
        break;
    }
    case PNG_PIXEL_GRAY_ALPHA: {          // 8-bit gray + 8-bit alpha
        uint8_t *s = pDraw->pPixels;
        for (int i = 0; i < w; ++i) {
            uint8_t v = s[0];
            dst[i] = ((v & 0xF8) << 8) | ((v & 0xFC) << 3) | (v >> 3);
            s += 2;
        }
        break;
    }
    case PNG_PIXEL_INDEXED: {             // 8-bit palette
        uint8_t *s   = pDraw->pPixels;
        uint8_t *pal = pDraw->pPalette;   // RGB triplets
        if (!pal) break;
        for (int i = 0; i < w; ++i) {
            uint8_t *c = pal + s[i] * 3;
            uint8_t r = c[0], g = c[1], b = c[2];
            dst[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
        break;
    }
    default:
        break;
    }
}

// ===========================================================================
//  JPG draw callback - copy one MCU block into the RGB565 canvas
// ===========================================================================

int decode::JPEGDrawStatic(JPEGDRAW *pDraw)
{
    DecodeCtx *ctx = static_cast<DecodeCtx *>(pDraw->pUser);
    if (!ctx || !ctx->canvas) return 0;

    int srcW = pDraw->iWidthUsed > 0 ? pDraw->iWidthUsed : pDraw->iWidth;
    int srcH = pDraw->iHeight;

    int x0 = pDraw->x;
    int y0 = pDraw->y;

    // Clip
    int xStart = 0, yStart = 0;
    if (x0 < 0) { xStart = -x0; x0 = 0; }
    if (y0 < 0) { yStart = -y0; y0 = 0; }

    int xEnd = srcW;
    int yEnd = srcH;
    if (x0 + (xEnd - xStart) > ctx->width)  xEnd = xStart + (ctx->width  - x0);
    if (y0 + (yEnd - yStart) > ctx->height) yEnd = yStart + (ctx->height - y0);

    if (xEnd <= xStart || yEnd <= yStart) return 1;

    for (int y = yStart; y < yEnd; ++y) {
        const uint16_t *src = pDraw->pPixels + y * pDraw->iWidth + xStart;
        uint16_t *dst = ctx->canvas + (y0 + y - yStart) * ctx->width + x0;
        memcpy(dst, src, (xEnd - xStart) * sizeof(uint16_t));
    }
    return 1;
}

// ===========================================================================
//  PNG / JPG decode
// ===========================================================================

bool decode::decodePNG(const char *path)
{
    PNG png;
    int rc = png.open(path,
                      FileOpen, FileClose,
                      PNGRead, PNGSeek,
                      PNGDrawStatic);
    if (rc != PNG_SUCCESS) return false;

    width  = png.getWidth();
    height = png.getHeight();
    if (width <= 0 || height <= 0) {
        png.close();
        return false;
    }

    canvas = (uint16_t *)heap_caps_malloc(width * height * sizeof(uint16_t),
                                          MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!canvas) {
        canvas = (uint16_t *)malloc(width * height * sizeof(uint16_t));
    }
    if (!canvas) {
        png.close();
        return false;
    }
    memset(canvas, 0, width * height * sizeof(uint16_t));

    DecodeCtx ctx{ canvas, width, height };
    rc = png.decode(&ctx, 0);
    png.close();
    return rc == PNG_SUCCESS;
}

bool decode::decodeJPG(const char *path)
{
    JPEGDEC jpg;
    int rc = jpg.open(path,
                      FileOpen, FileClose,
                      JPGRead, JPGSeek,
                      JPEGDrawStatic);
    if (rc != 1) return false;

    width  = jpg.getWidth();
    height = jpg.getHeight();
    if (width <= 0 || height <= 0) {
        jpg.close();
        return false;
    }

    canvas = (uint16_t *)heap_caps_malloc(width * height * sizeof(uint16_t),
                                          MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!canvas) {
        canvas = (uint16_t *)malloc(width * height * sizeof(uint16_t));
    }
    if (!canvas) {
        jpg.close();
        return false;
    }
    memset(canvas, 0, width * height * sizeof(uint16_t));

    jpg.setPixelType(RGB565_LITTLE_ENDIAN);

    DecodeCtx ctx{ canvas, width, height };
    jpg.setUserPointer(&ctx);
    rc = jpg.decode(0, 0, 0);
    jpg.close();
    return rc == 1;
}

// ===========================================================================
//  public api
// ===========================================================================

lv_obj_t *decode::create(lv_obj_t *parent, const char *path, decode_type type)
{
    if (!parent || !path) return nullptr;

    if (type == DECODE_AUTO) type = detect_type_from_path(path);

    bool ok = false;
    switch (type) {
    case DECODE_PNG: ok = decodePNG(path); break;
    case DECODE_JPG: ok = decodeJPG(path); break;
    default:         ok = false;           break;
    }

    if (!ok) {
        if (canvas) { free(canvas); canvas = nullptr; }
        width = height = 0;
        return nullptr;
    }

    lvgl_port_lock(0);
    lv_canvas_obj = lv_canvas_create(parent);
    if (lv_canvas_obj) {
        lv_canvas_set_buffer(lv_canvas_obj, canvas, width, height,
                             LV_COLOR_FORMAT_RGB565);
    }
    lvgl_port_unlock();

    return lv_canvas_obj;
}

void decode::destroy()
{
    if (lv_canvas_obj) {
        lvgl_port_lock(0);
        lv_obj_del(lv_canvas_obj);
        lvgl_port_unlock();
        lv_canvas_obj = nullptr;
    }
    if (canvas) {
        free(canvas);
        canvas = nullptr;
    }
    width = 0;
    height = 0;
}
