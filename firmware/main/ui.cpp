#include "app.h"
#include "network_settings.h"
#include "web_access.h"
#include "board.h"
#if CONFIG_IDF_TARGET_ESP32
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lcd.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <atomic>

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t drawn;
static uint16_t *pixels;
static LcdCanvas canvas{nullptr,0};
static std::atomic<bool> screen{false};
static std::atomic<int> currentBrightness{0};
static DisplayDiagnosticsTimer diagnosticsTimer;
static std::atomic<bool> networkGrace{true};
static bool editing=false,touched=false;
static int field=0;
static Settings draft;
static uint32_t lastTouch=0, errorUntil=0;
static constexpr const char *names[]={"Zdroj ZAP","Zapnout C","Zdroj VYP","Vypnout C","Havarijni C","Min. beh s","Rucni beh s","Kalibrace sondy 1 C","Kalibrace sondy 2 C","Vystup rele","Impulz ms","LCD zdroj zhasnuti","LCD vypinat teplotou","LCD vypnout pod C","LCD denni jas %","LCD nocni casovac","LCD noc od","LCD noc do","LCD nocni jas %","Vystupni sonda","LCD otoceni"};
static constexpr int FieldCount=sizeof(names)/sizeof(names[0]);
int display_brightness() { return currentBrightness.load(); }
bool display_network_grace() { return networkGrace.load(); }
bool display_ready() { return screen.load(); }
static bool transfer(esp_lcd_panel_io_handle_t,esp_lcd_panel_io_event_data_t *,void *) {
    BaseType_t wake=pdFALSE; xSemaphoreGiveFromISR(drawn,&wake); return wake==pdTRUE;
}
static bool display_init() {
    gpio_set_direction(GPIO_NUM_4,GPIO_MODE_OUTPUT); gpio_set_level(GPIO_NUM_4,0);
    pixels=static_cast<uint16_t *>(heap_caps_malloc(240*135*2,MALLOC_CAP_DMA)); drawn=xSemaphoreCreateBinary();
    if (!pixels || !drawn) return false;
    spi_bus_config_t bus{}; bus.mosi_io_num=19; bus.sclk_io_num=18; bus.miso_io_num=-1;
    bus.quadwp_io_num=-1; bus.quadhd_io_num=-1; bus.max_transfer_sz=240*135*2;
    if (spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO)!=ESP_OK) return false;
    esp_lcd_panel_io_spi_config_t ioCfg{};
    ioCfg.cs_gpio_num=5; ioCfg.dc_gpio_num=16; ioCfg.pclk_hz=20000000; ioCfg.trans_queue_depth=1;
    ioCfg.lcd_cmd_bits=8; ioCfg.lcd_param_bits=8; ioCfg.on_color_trans_done=transfer;
    esp_lcd_panel_io_handle_t io;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&ioCfg,&io)!=ESP_OK) return false;
    esp_lcd_panel_dev_config_t dev{}; dev.reset_gpio_num=23; dev.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB; dev.bits_per_pixel=16;
    if (esp_lcd_new_panel_st7789(io,&dev,&panel)!=ESP_OK) return false;
    if (esp_lcd_panel_reset(panel)!=ESP_OK || esp_lcd_panel_init(panel)!=ESP_OK ||
        esp_lcd_panel_swap_xy(panel,true)!=ESP_OK || esp_lcd_panel_mirror(panel,true,false)!=ESP_OK ||
        esp_lcd_panel_set_gap(panel,40,53)!=ESP_OK || esp_lcd_panel_invert_color(panel,true)!=ESP_OK ||
        esp_lcd_panel_disp_on_off(panel,true)!=ESP_OK) return false;
    ledc_timer_config_t timer{};
    timer.speed_mode=LEDC_LOW_SPEED_MODE; timer.duty_resolution=LEDC_TIMER_10_BIT;
    timer.timer_num=LEDC_TIMER_0; timer.freq_hz=5000; timer.clk_cfg=LEDC_AUTO_CLK;
    if(ledc_timer_config(&timer)!=ESP_OK) return false;
    ledc_channel_config_t channel{};
    channel.gpio_num=4; channel.speed_mode=LEDC_LOW_SPEED_MODE; channel.channel=LEDC_CHANNEL_0;
    channel.timer_sel=LEDC_TIMER_0; channel.duty=0;
    return ledc_channel_config(&channel)==ESP_OK;
}
static bool backlight(uint8_t percent) {
    if(currentBrightness.load()==percent) return true;
    if(esp_lcd_panel_disp_on_off(panel,percent!=0)!=ESP_OK ||
       ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,uint32_t(percent)*1023/100)!=ESP_OK ||
       ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0)!=ESP_OK) return false;
    currentBrightness=percent; return true;
}
static void line(int y,const char *text,uint16_t rgb=0xffff,int scale=1) {
    // Compact proportional glyphs also keep the diagnostic/menu text legible in portrait.
    int sx=std::max(1,std::min(scale,(canvas.width()-8)/std::max(1,lcd_text_width(text))));
    canvas.text(4,y,text,rgb,sx,scale);
}
static int value() {
    switch(field) {
    case 0:return draft.on_source; case 1:return draft.on; case 2:return draft.off_source;
    case 3:return draft.off; case 4:return draft.force; case 5:return draft.min_on_s;
    case 6:return draft.manual_s; case 7:return draft.calibration[0]; case 8:return draft.calibration[1];
    case 9:return draft.relay; case 10:return draft.pulse_ms;
    case 11:return draft.display.source; case 12:return draft.display.temp_off_enabled;
    case 13:return draft.display.off_below; case 14:return draft.display.day_brightness;
    case 15:return draft.display.night_enabled; case 16:return draft.display.night_start;
    case 17:return draft.display.night_end; case 18:return draft.display.night_brightness;
    case 19:return draft.outlet_sensor; default:return draft.display.rotation;
    }
}
static void adjust(int delta) {
    int step=field==20?90:(field==1 || field==3 || field==4 || field==13)?50:
        (field==16 || field==17)?5:(field==0 || field==2 || field==9 || field>=11)?1:10;
    int n=value()+delta*step;
    switch(field) {
    case 0:draft.on_source=std::clamp(n,0,4);break;
    case 1:draft.on=std::clamp(n,3000,8000);break;
    case 2:draft.off_source=std::clamp(n,0,4);break;
    case 3:draft.off=std::clamp(n,2000,7900);break;
    case 4:draft.force=std::clamp(n,3500,9500);break;
    case 5:draft.min_on_s=std::clamp(n,10,600);break;
    case 6:draft.manual_s=std::clamp(n,30,3600);break;
    case 7:draft.calibration[0]=std::clamp(n,-500,500);break;
    case 8:draft.calibration[1]=std::clamp(n,-500,500);break;
    case 9:draft.relay=std::clamp(n,0,3);break;
    case 10:draft.pulse_ms=std::clamp(n,20,200);break;
    case 11:draft.display.source=std::clamp(n,0,4);break;
    case 12:draft.display.temp_off_enabled=std::clamp(n,0,1);break;
    case 13:draft.display.off_below=std::clamp(n,-500,12500);break;
    case 14:draft.display.day_brightness=std::clamp(n,1,100);break;
    case 15:draft.display.night_enabled=std::clamp(n,0,1);break;
    case 16:draft.display.night_start=(n+1440)%1440;break;
    case 17:draft.display.night_end=(n+1440)%1440;break;
    case 18:draft.display.night_brightness=std::clamp(n,1,100);break;
    case 19:draft.outlet_sensor=std::clamp(n,1,2);break;
    case 20:draft.display.rotation=(n+360)%360;break;
    }
}
static const char *source_label(int n) {
    static const char *labels[]={"VSTUP","VYSTUP","PRUMER","MINIMUM","MAXIMUM"};
    return n>=0 && n<5?labels[n]:"?";
}
static const char *relay_label(int n) {
    static const char *labels[]={"BEZ VYSTUPU","NC","NO","BISTABILNI"};
    return n>=0 && n<4?labels[n]:"?";
}
static void unlock(uint32_t now) { draft=snapshot().settings; editing=true; field=0; lastTouch=now; }
static void save(uint32_t now) {
    Command c{}; c.type=CommandType::Settings; c.settings=draft;
    // Avoid overwriting a newer USB edit with an old on-screen draft.
    if(draft.revision==snapshot().settings.revision && valid(draft) && command(c)) editing=false;
    else errorUntil=now+3000;
}
static void temp_text(char *buf,size_t size,int16_t v) {
    if(v==InvalidTemp) snprintf(buf,size,"--.-"); else snprintf(buf,size,"%.1f",v/100.0);
}
static void render(uint32_t now) {
    auto s=snapshot(); memset(pixels,0,240*135*2);
    canvas={pixels,s.settings.display.rotation};
    bool portrait=canvas.width()<canvas.height();
    auto net=network_info(); int minute=local_minute();
    static bool wasAP=false;
    if(net.ap && !wasAP) diagnosticsTimer=DisplayDiagnosticsTimer{};
    wasAP=net.ap;
    bool grace=diagnosticsTimer.show(now,display_network_problem(s,net.configured,net.ap,minute));
    networkGrace=grace;
    auto view=display_view(s,net.configured,net.ap,minute,editing || (touched && age(now,lastTouch)<30000),grace);
    char text[48],a[12],b[12];
    temp_text(a,sizeof(a),s.values.inlet); temp_text(b,sizeof(b),s.values.outlet);
    char pin[5]; bool showingPin=web_display_pin(pin);
    if(showingPin) {
        line(8,"PIN / NASTAVENI",0x07ff);
        int scale=portrait?4:6;
        int width=lcd_text_width(pin)*scale;
        canvas.text((canvas.width()-width)/2,portrait?80:43,pin,0xffff,scale,scale);
        line(portrait?180:112,"Platnost zobrazeni 1 min");
        view.brightness=s.settings.display.day_brightness;
    } else if(!editing && net.ap && grace) {
        line(3,"HOTSPOT / AP",0x07ff);
        int y=17,chunk=portrait?14:25;
        auto wrapped=[&](const char *value) {
            for(size_t i=0;i<strlen(value);i+=chunk) { snprintf(text,sizeof(text),"%.*s",chunk,value+i); line(y,text); y+=12; }
        };
        const auto &cfg=network_settings();
        line(y,"Wi-Fi sit:"); y+=12; wrapped(cfg.ap_ssid);
        line(y,"Heslo:"); y+=12; wrapped(cfg.ap_password);
        snprintf(text,sizeof(text),"IP %s",net.ap_ip); line(y,text,0x07e0);
    } else if(!editing && !view.diagnostics) {
        canvas.temperatures(s.values);
    } else {
    line(3,editing?"SOLO / NASTAVENI":"SOLO / DIAGNOSTIKA",0x07ff);
    snprintf(text,sizeof(text),"Vst %s C",a);line(17,text,0xffff,2);
    snprintf(text,sizeof(text),"Vys %s C",b);line(36,text,0xffff,2);
    snprintf(text,sizeof(text),"%s / %s",s.demand?"OBEH ON":"OBEH OFF",s.active_relay==None?"SIMULACE":"VYSTUP");line(56,text,0xffe0);
    if(editing) {
        if(portrait) {
            snprintf(text,sizeof(text),"Nastaveni %d/%d",field+1,FieldCount);line(87,text,0x07ff);
            line(101,names[field],0x07ff);
        } else { snprintf(text,sizeof(text),"%d/%d %s",field+1,FieldCount,names[field]);line(71,text,0x07ff); }
        if(field==0 || field==2 || field==11) snprintf(text,sizeof(text),"%s",source_label(value()));
        else if(field==9) snprintf(text,sizeof(text),"%s",relay_label(value()));
        else if(field==12 || field==15) snprintf(text,sizeof(text),"%s",value()?"ZAP":"VYP");
        else if(field==16 || field==17) snprintf(text,sizeof(text),"%02d:%02d",value()/60,value()%60);
        else if(field==19) snprintf(text,sizeof(text),"Sonda %d / GPIO%d",value(),value()==1?27:26);
        else if(field==20) snprintf(text,sizeof(text),"%d stupnu",value());
        else if(field==1 || field==3 || field==4 || field==7 || field==8 || field==13) snprintf(text,sizeof(text),"%.2f",value()/100.0);
        else snprintf(text,sizeof(text),"%d",value());
        line(portrait?126:85,text,0xffff,2);
        if(portrait) { line(186,"Zamek = dalsi"); line(201,"Ulozit = potvrdit"); }
        else line(110,"Zamek=dalsi Ulozit=potvrdit");
    } else {
        snprintf(text,sizeof(text),"WiFi:%s MQTT:%s",net.ap?"AP":s.link?"OK":"--",net.mqtt?"OK":"--");line(70,text);
        snprintf(text,sizeof(text),"IP %s",*net.ip?net.ip:"--");line(82,text);
        if(minute<0) snprintf(text,sizeof(text),"Cas: cekam na NTP");
        else snprintf(text,sizeof(text),"Cas %02d:%02d %s",minute/60,minute%60,view.night?"NOC":"DEN");
        line(94,text);
        line(106,net.configured?"Sit nastavena":"Nastav sit pres AP",net.configured?0xffff:0xf800);
    }
    int footer=portrait?224:123;
    if(int32_t(errorUntil-now)>0) line(footer,portrait?"CHYBA / NOVEJSI STAV":"NEPLATNE / NOVEJSI NASTAVENI",0xf800);
    else if(s.faults & HardwareChange) line(footer,"VYSTUP: NUTNY RESTART",0xf800);
    else if(s.faults & NotCommissioned) line(footer,"VYSTUP: NUTNE UVEDENI",0xf800);
    else { snprintf(text,sizeof(text),"HA:%s NVS:%s F:%02lX",s.delivered?"OK":"--",s.saved?"OK":"--",(unsigned long)s.faults);line(footer,text,s.faults?0xf800:0x07e0); }
    }
    if(esp_lcd_panel_draw_bitmap(panel,0,0,240,135,pixels)!=ESP_OK || xSemaphoreTake(drawn,pdMS_TO_TICKS(500))!=pdTRUE || !backlight(view.brightness)) {
        screen=false; editing=false; ESP_LOGE("display","LCD failed; local control continues");
    }
}
struct Key { bool raw=false,stable=false,longDone=false; uint32_t changed=0,down=0; };
static void ui_task(void *) {
    gpio_config_t cfg{}; cfg.mode=GPIO_MODE_INPUT;
    for(int pin:KeyPins) cfg.pin_bit_mask |= 1ULL<<pin;
    if(gpio_config(&cfg)!=ESP_OK) { vTaskDelete(nullptr); return; }
    screen=display_init(); Key keys[4]; uint32_t drawnAt=now_ms()-500;
    for (;;) {
        uint32_t now=now_ms();
        for(int i=0;i<4;++i) {
            auto &k=keys[i]; bool pressed=gpio_get_level(gpio_num_t(KeyPins[i]));
            if(pressed!=k.raw) { k.raw=pressed; k.changed=now; }
            if(age(now,k.changed)>=60 && k.stable!=k.raw) {
                k.stable=k.raw;
                if(k.stable) { k.down=now; k.longDone=false; lastTouch=now; touched=true; }
                else if(!k.longDone && screen && editing) {
                    if(i==0) field=(field+1)%FieldCount;
                    else if(i==1 || i==2) adjust(i==1?-1:1);
                    else if(i==3) save(now);
                }
            }
            if(k.stable && !k.longDone && age(now,k.down)>=(i==0?2000u:3000u)) {
                k.longDone=true;
                if(i==0 && screen) { if(editing) editing=false; else unlock(now); }
                if(i==3) { Command c{}; c.type=CommandType::Manual; command(c); editing=false; }
            }
        }
        if(editing && age(now,lastTouch)>30000) editing=false;
        if(screen && age(now,drawnAt)>=300) { render(now); drawnAt=now; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void ui_start() { configASSERT(xTaskCreate(ui_task,"display_keys",6144,nullptr,3,nullptr)==pdPASS); }
#else
void ui_start() {}
int display_brightness() { return 0; }
bool display_network_grace() { return false; }
bool display_ready() { return false; }
#endif
