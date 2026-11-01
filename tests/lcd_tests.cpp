#include "lcd.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <initializer_list>
using namespace solo;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"LCD FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static uint16_t swap(uint16_t c) { return uint16_t(c<<8 | c>>8); }
int main(int argc,char **argv) {
    CHECK(argc==2);
    const int readings[][2]={{5430,6540},{-6000,13000},{13000,-6000},{980,1020},{InvalidTemp,InvalidTemp}};
    for(unsigned rotation:{0u,90u,180u,270u}) for(const auto &t:readings) {
        std::vector<uint16_t> data(240*135+2,0);data.front()=data.back()=0xa55a;
        LcdCanvas c{data.data()+1,uint16_t(rotation)};
        Values v;v.inlet=int16_t(t[0]);v.outlet=int16_t(t[1]);
        if(t[0]!=InvalidTemp && t[1]!=InvalidTemp) v.difference=int16_t(t[1]-t[0]);
        c.temperatures(v);CHECK(data.front()==0xa55a && data.back()==0xa55a);
        int main=0,inlet=0,delta=0;
        for(int py=0;py<135;++py) for(int px=0;px<240;++px) {
            int x=px,y=py;
            if(rotation==90) { x=py;y=239-px; }
            if(rotation==180) { x=239-px;y=134-py; }
            if(rotation==270) { x=134-py;y=px; }
            uint16_t rgb=swap(c.pixels[py*240+px]);bool portrait=rotation%180!=0;
            if(rgb==0xffff) { CHECK(portrait?y<120:x<160);++main; }
            if(rgb==0x07ff) { CHECK(portrait?y>120&&y<180:x>160&&y<67);++inlet; }
            if(rgb==0xffe0) { CHECK(portrait?y>180:x>160&&y>67);++delta; }
        }
        CHECK(main>0 && inlet>0 && delta>0);
        if(t[0]==5430) {
            auto name=std::string(argv[1])+"/lcd-"+std::to_string(rotation)+".ppm";
            FILE *f=std::fopen(name.c_str(),"wb");CHECK(f);
            std::fprintf(f,"P6\n240 135\n255\n");
            for(int i=0;i<240*135;++i) { uint16_t rgb=swap(c.pixels[i]); unsigned char p[]={static_cast<unsigned char>(((rgb>>11)&31)*255/31),static_cast<unsigned char>(((rgb>>5)&63)*255/63),static_cast<unsigned char>((rgb&31)*255/31)};CHECK(std::fwrite(p,1,3,f)==3); }
            std::fclose(f);
        }
    }
    std::puts("PASS: actual LCD raster in all four rotations; outlet/inlet/delta regions; positive, negative, extreme and missing values; framebuffer bounds");
}
