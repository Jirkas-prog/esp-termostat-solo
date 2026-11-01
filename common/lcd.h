#pragma once
#include "core.h"

namespace solo {
// Render directly into the existing 240x135 DMA frame, including quarter turns.
// No second framebuffer or panel-specific rotation offsets are needed.
struct LcdCanvas {
    uint16_t *pixels;
    uint16_t rotation;
    int width() const { return rotation%180?135:240; }
    int height() const { return rotation%180?240:135; }
    void pixel(int x,int y,uint16_t rgb);
    void text(int x,int y,const char *value,uint16_t rgb,int sx=1,int sy=1);
    void temperatures(const Values &values);
};
int lcd_text_width(const char *value,int scale=1);
}
