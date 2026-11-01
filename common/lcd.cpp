#include "lcd.h"
#include "../firmware/main/font8x8_basic.h"
#include <algorithm>
#include <cstdio>

namespace solo {
static void glyph(unsigned char c,int &left,int &right) {
    unsigned bits=0; for(int row=0;row<8;++row) bits|=unsigned(font8x8_basic[c][row]);
    left=0; right=7;
    if(!bits) { right=2; return; }
    while(!(bits&(1u<<left))) ++left;
    while(!(bits&(1u<<right))) --right;
}
int lcd_text_width(const char *value,int scale) {
    int width=0;
    for(;*value;++value) {
        unsigned char c=static_cast<unsigned char>(*value); if(c>127) c='?';
        int left,right; glyph(c,left,right); width+=(right-left+2)*scale;
    }
    return width?width-scale:0;
}
void LcdCanvas::pixel(int x,int y,uint16_t rgb) {
    if(x<0 || y<0 || x>=width() || y>=height()) return;
    int px=x,py=y;
    if(rotation==90) { px=239-y; py=x; }
    else if(rotation==180) { px=239-x; py=134-y; }
    else if(rotation==270) { px=y; py=134-x; }
    pixels[py*240+px]=uint16_t(rgb<<8 | rgb>>8);
}
void LcdCanvas::text(int x,int y,const char *value,uint16_t rgb,int sx,int sy) {
    for(;*value;++value) {
        unsigned char c=static_cast<unsigned char>(*value); if(c>127) c='?';
        int left,right; glyph(c,left,right);
        for(int row=0;row<8;++row) for(int col=left;col<=right;++col)
            if(unsigned(font8x8_basic[c][row])&(1u<<col))
                for(int a=0;a<sy;++a) for(int b=0;b<sx;++b) pixel(x+(col-left)*sx+b,y+row*sy+a,rgb);
        x+=(right-left+2)*sx;
    }
}
static void reading(LcdCanvas &c,int x,int y,int w,int h,const char *label,int16_t value,bool large,uint16_t rgb) {
    char number[16];
    if(value==InvalidTemp) snprintf(number,sizeof(number),"--.-");
    else snprintf(number,sizeof(number),"%.1f",value/100.0);
    c.text(x+4,y+6,label,0x9cf3);
    int sx=std::max(1,std::min(large?7:3,(w-8)/lcd_text_width(number)));
    int sy=std::max(1,std::min(large?10:4,(h-26)/8));
    c.text(x+(w-lcd_text_width(number,sx))/2,y+20+(h-20-8*sy)/2,number,rgb,sx,sy);
}
void LcdCanvas::temperatures(const Values &v) {
    int w=width(),h=height();
    if(w>h) {
        int split=w*2/3;
        for(int y=0;y<h;++y) pixel(split,y,0x4208);
        for(int x=split;x<w;++x) pixel(x,h/2,0x4208);
        reading(*this,0,0,split,h,"VYSTUP C",v.outlet,true,0xffff);
        reading(*this,split+1,0,w-split-1,h/2,"VSTUP C",v.inlet,false,0x07ff);
        reading(*this,split+1,h/2+1,w-split-1,h-h/2-1,"DELTA C",v.difference,false,0xffe0);
    } else {
        for(int x=0;x<w;++x) { pixel(x,h/2,0x4208); pixel(x,h*3/4,0x4208); }
        reading(*this,0,0,w,h/2,"VYSTUP C",v.outlet,true,0xffff);
        reading(*this,0,h/2+1,w,h/4-1,"VSTUP C",v.inlet,false,0x07ff);
        reading(*this,0,h*3/4+1,w,h-h*3/4-1,"DELTA C",v.difference,false,0xffe0);
    }
}
}
