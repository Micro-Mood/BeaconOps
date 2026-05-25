#pragma once

#include "lvgl.h"
#include "PNGdec.h"
#include "JPEGDEC.h"
#include "stdio.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st7789.h"

typedef enum {
    DECODE_AUTO = 0,
    DECODE_PNG,
    DECODE_JPG
} decode_type;

class decode {
public:
    decode();
    ~decode();

    // Decode a still image (PNG or JPG) and bind it to a fresh lv_canvas
    // child of `parent`. Type is auto-detected from the file extension
    // when `type == DECODE_AUTO`. Returns the canvas object on success,
    // nullptr on failure.
    lv_obj_t *create(lv_obj_t *parent, const char *path,
                     decode_type type = DECODE_AUTO);

    // Release the canvas, the pixel buffer and any decoder state.
    void destroy();

    int getWidth()  const { return width; }
    int getHeight() const { return height; }

    lv_obj_t *lv_canvas_obj;

private:
    int width  = 0;
    int height = 0;
    uint16_t *canvas = nullptr;

    bool decodePNG(const char *path);
    bool decodeJPG(const char *path);

    static void PNGDrawStatic(PNGDRAW *pDraw);
    static int  JPEGDrawStatic(JPEGDRAW *pDraw);
};
