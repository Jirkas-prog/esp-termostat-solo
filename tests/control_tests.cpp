#include "core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
using namespace solo;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static Stored config() { Stored s; s.settings.min_on_s=10; seal(s); return s; }
static State tick(Controller &c,uint32_t t,int a=5000,int b=5000,bool va=true,bool vb=true,bool ha=true) {
    c.sample(0,Reading{int16_t(a),va,t}); c.sample(1,Reading{int16_t(b),vb,t});
    return c.tick(t,ha,ha,false,false);
}
static void save(Controller &c) { c.persisted(c.store.settings.revision,true); }
int main() {
    CHECK(hostname_valid("ter01") && hostname_valid("termostat-kamna"));
    CHECK(hostname_valid("12345678901234567890123456789012"));
    for(const char *name:{"","-ter01","ter01-","Ter01","ter01.local","ter 01","a/b","123456789012345678901234567890123"}) CHECK(!hostname_valid(name));
    CHECK(crc32("123456789",9)==0xcbf43926u);
    // Real v1 binary layout with non-default thresholds, calibration, sources and durations.
    uint8_t legacy[]={0x4f,0x4c,0x4f,0x53,0x01,0x00,0x07,0x00,0x00,0x00,0x64,0x19,0x44,0x16,0x28,0x23,0x7d,0x00,0xb5,0xff,0x50,0x00,0x2c,0x01,0x32,0x00,0x01,0x00,0x00,0x00,0xca,0xc7,0x66,0x5c};
    Stored migrated;
    CHECK(decode_stored(legacy,sizeof(legacy),migrated) && valid(migrated));
    CHECK(migrated.format==3 && migrated.settings.revision==7);
    CHECK(migrated.settings.display.rotation==0 && migrated.settings.outlet_sensor==2);
    CHECK(migrated.settings.on==6500 && migrated.settings.off==5700 && migrated.settings.force==9000);
    CHECK(migrated.settings.calibration[0]==125 && migrated.settings.calibration[1]==-75);
    CHECK(migrated.settings.min_on_s==80 && migrated.settings.manual_s==300 && migrated.settings.pulse_ms==50);
    CHECK(migrated.settings.on_source==Outlet && migrated.settings.off_source==Inlet && migrated.settings.relay==None);
    CHECK(migrated.settings.display.source==Maximum && migrated.settings.display.off_below==3000);
    Stored decoded; CHECK(decode_stored(&migrated,sizeof(migrated),decoded));
    CHECK(!memcmp(&decoded,&migrated,sizeof(migrated)));
    legacy[10]^=1; CHECK(!decode_stored(legacy,sizeof(legacy),decoded));
    CHECK(!decode_stored(legacy,sizeof(legacy)-1,decoded));
    // v2 used exactly the first 35 settings bytes. Keep all custom values on upgrade.
    uint8_t legacy2[45]{}; uint32_t magic=0x534f4c4f; uint16_t format=2;
    auto old=migrated.settings; old.display.source=Inlet; old.display.day_brightness=42;
    old.display.night_brightness=17; old.display.night_start=1200; old.display.night_end=360;
    memcpy(legacy2,&magic,4); memcpy(legacy2+4,&format,2); memcpy(legacy2+6,&old,35);
    uint32_t crc=crc32(legacy2,41); memcpy(legacy2+41,&crc,4);
    CHECK(decode_stored(legacy2,sizeof(legacy2),decoded) && valid(decoded));
    CHECK(!memcmp(&decoded.settings,&old,35));
    CHECK(decoded.settings.outlet_sensor==2 && decoded.settings.display.rotation==0);
    for(size_t i=0;i<sizeof(legacy2);++i) { legacy2[i]^=1; CHECK(!decode_stored(legacy2,sizeof(legacy2),decoded)); legacy2[i]^=1; }
    Stored initial=config(); CHECK(valid(initial));
    for(size_t i=0;i<sizeof(Stored);++i) { Stored bad=initial; reinterpret_cast<uint8_t *>(&bad)[i]^=1; CHECK(!valid(bad)); }
    Settings bad=initial.settings; bad.on_source=5; CHECK(!valid(bad));
    bad=initial.settings; bad.off=bad.on; CHECK(!valid(bad));
    bad=initial.settings; bad.calibration[1]=501; CHECK(!valid(bad));
    Reading r[2]={{4000,true,0},{6000,true,0}}; auto cfg=initial.settings;
    cfg.calibration[0]=100; cfg.calibration[1]=-200;
    auto v=calculate(r,cfg,0);
    CHECK(v.temperature[0]==4100 && v.temperature[1]==5800);
    CHECK(v.average==4950 && v.minimum==4100 && v.maximum==5800 && v.difference==1700);
    CHECK(v.inlet==4100 && v.outlet==5800);
    cfg.outlet_sensor=1; v=calculate(r,cfg,0);
    CHECK(v.inlet==5800 && v.outlet==4100 && v.difference==-1700);
    CHECK(select(v,Outlet)==4100 && select(v,Inlet)==5800);
    CHECK(v.temperature[0]==4100 && v.temperature[1]==5800 && v.average==4950); // calibration stays physical
    cfg.outlet_sensor=2;
    r[0].raw=6000; r[1].raw=4000; v=calculate(r,cfg,0); CHECK(v.difference==-2300);
    r[1].valid=false; v=calculate(r,cfg,0);
    CHECK(v.temperature[0]==6100 && v.temperature[1]==InvalidTemp && v.average==InvalidTemp && v.difference==InvalidTemp);

    // Exhaust every independent ON/OFF source combination. The oracle uses
    // explicit five-element temperature arrays, not select()/calculate().
    const int pairs[][2]={{5000,5000},{6200,5100},{5100,6200},{5700,5700},{5500,5500},{6000,6000},{6100,4900}};
    for(uint8_t on=0;on<5;++on) for(uint8_t off=0;off<5;++off) for(const auto &p:pairs) {
        auto s=config(); s.settings.on_source=on; s.settings.off_source=off; seal(s);
        Controller c(s,true,1); tick(c,0); CHECK(!tick(c,10000).demand);
        int vals[5]={p[0],p[1],(p[0]+p[1])/2,p[0]<p[1]?p[0]:p[1],p[0]>p[1]?p[0]:p[1]};
        bool fromOff=vals[on]>=6000;
        CHECK(tick(c,11000,p[0],p[1]).demand==fromOff);
        tick(c,12000,7000,7000); CHECK(tick(c,12001,7000,7000).demand);
        bool fromOn=vals[on]>=6000 || vals[off]>5500;
        CHECK(tick(c,23000,p[0],p[1]).demand==fromOn);
    }
    Controller c(initial,true,1); CHECK(tick(c,0).demand);
    CHECK(tick(c,9999).demand); CHECK(!tick(c,10000).demand);
    CHECK(!tick(c,11000,5999,5999).demand); CHECK(tick(c,12000,6000,6000).demand);
    CHECK(tick(c,21999).demand); CHECK(!tick(c,22000).demand);
    CHECK(tick(c,23000,5000,5000,false,true).demand);
    CHECK(tick(c,24000,5000,5000,true,false).demand);
    auto unavailable=c.tick(28000,true,true,false,false);
    CHECK(unavailable.demand && (unavailable.faults&(Sensor1|Sensor2))==(Sensor1|Sensor2));
    CHECK(c.tick(24000,true,true,false,false).values.temperature[0]==InvalidTemp); // emulate whole uint32 cycle, no revival
    CHECK(tick(c,29000).values.temperature[0]==5000);
    auto s=config(); s.settings.on_source=Inlet; s.settings.off_source=Inlet; seal(s);
    Controller force(s,true,2); tick(force,0); tick(force,10000);
    CHECK(tick(force,11000,4000,8500).demand); // hot unselected probe overrides
    s.settings.outlet_sensor=1; s.settings.on_source=Outlet; s.settings.off_source=Outlet; seal(s);
    Controller mapped(s,true,7); tick(mapped,0); CHECK(!tick(mapped,10000,4000,7000).demand);
    CHECK(tick(mapped,11000,7000,4000).demand); // outlet mapping also drives local selection
    Controller net(initial,true,3); tick(net,0);
    auto offline=tick(net,10000,5000,5000,true,true,false);
    CHECK(!offline.demand && offline.faults==RouteError);

    Controller manual(initial,true,4); tick(manual,0); CHECK(!tick(manual,10000).demand);
    manual.manual(11000); CHECK(tick(manual,11000).manual_remaining_s==600);
    CHECK(tick(manual,610999).demand); CHECK(!tick(manual,611000).demand);
    CHECK(tick(manual,611000).manual_remaining_s==0);
    auto changed=manual.store.settings; changed.calibration[0]=125; CHECK(manual.change(changed));
    CHECK(!manual.change(changed)); // stale revision cannot overwrite a newer edit
    manual.persisted(changed.revision,true); CHECK(!tick(manual,612000).saved);
    save(manual); auto calibrated=tick(manual,613000);
    CHECK(calibrated.saved && calibrated.values.temperature[0]==5125 && calibrated.values.temperature[1]==5000);

    Controller first(Stored{},false,0); CHECK(first.store.settings.relay==None && first.dirty);
    save(first); CHECK(tick(first,0).active_relay==None);
    changed=first.store.settings; changed.relay=NC; changed.commissioned=1;
    CHECK(first.change(changed)); CHECK(first.store.settings.commissioned==0); save(first);
    CHECK(tick(first,10000).active_relay==None && (tick(first,10000).faults&HardwareChange));
    Controller reboot(first.store,true,5);
    auto boot=tick(reboot,0); CHECK(boot.active_relay==NC && (boot.faults&NotCommissioned));
    changed=reboot.store.settings; changed.commissioned=1; CHECK(reboot.change(changed)); save(reboot);
    CHECK(!tick(reboot,60000).demand); // Fresh-install default minimum run is 60 s.
    CHECK(steady_drive(None,false)==0 && steady_drive(None,true)==0);
    CHECK(steady_drive(NC,false)==1 && steady_drive(NC,true)==0);
    CHECK(steady_drive(NO,false)==0 && steady_drive(NO,true)==1);
    CHECK(reboot.tick(10000,true,true,true,true).demand);

    Controller wrap(initial,true,6);
    tick(wrap,UINT32_MAX-5000); CHECK(!tick(wrap,4999).demand);
    wrap.manual(UINT32_MAX-1000);
    CHECK(tick(wrap,2000).manual_remaining_s==597);
    for(int x=0;x<10000;++x) {
        uint32_t t=uint32_t(x)*1000u; auto z=tick(wrap,t,x%2?-5501:12501,5000);
        CHECK(z.demand && (z.faults&Sensor1));
    }
    State lcd; lcd.saved=true; lcd.link=true; lcd.faults=RouteError;
    Reading probes[2]={{4000,true,0},{6000,true,0}};
    lcd.values=calculate(probes,lcd.settings,0);
    const int temperatures[]={4000,6000,5000,4000,6000};
    for(uint8_t source=0;source<=Maximum;++source) {
        lcd.settings.display.source=source;
        auto view=display_view(lcd,true,false,12*60);
        CHECK(view.temperature==6000 && view.threshold_temperature==temperatures[source] && !view.diagnostics && view.brightness==100);
    }
    lcd.settings.display.source=Maximum;
    for(int minute=0;minute<1440;++minute) {
        bool night=minute<420 || minute>=1320;
        auto hot=display_view(lcd,true,false,minute);
        CHECK(hot.night==night && hot.brightness==(night?10:100) && !hot.diagnostics);
    }
    lcd.values.maximum=2999;
    CHECK(display_view(lcd,true,false,720).brightness==0);
    CHECK(display_view(lcd,true,false,23*60).brightness==0);
    CHECK(display_view(lcd,true,false,23*60,true).brightness==100);
    lcd.values.maximum=3000;
    CHECK(display_view(lcd,true,false,23*60).brightness==10); // rises back to the threshold
    CHECK(display_view(lcd,true,false,7*60).brightness==100);
    lcd.values.maximum=1000;
    lcd.link=false;
    CHECK(display_view(lcd,true,false,23*60).diagnostics && display_view(lcd,true,false,23*60).brightness==10);
    lcd.link=true;
    CHECK(display_view(lcd,true,true,720).diagnostics && display_view(lcd,true,true,720).brightness==100);
    CHECK(display_view(lcd,false,false,720).diagnostics);
    CHECK(display_view(lcd,true,false,-1).diagnostics && display_view(lcd,true,false,-1).brightness==100);
    lcd.settings.display.night_enabled=0;
    CHECK(!display_view(lcd,true,false,-1).diagnostics && display_view(lcd,true,false,-1).brightness==0);
    lcd.settings.display.temp_off_enabled=0;
    CHECK(display_view(lcd,true,false,23*60).brightness==100);
    lcd.settings.display.night_enabled=1;
    lcd.settings.display.night_start=8*60; lcd.settings.display.night_end=10*60;
    CHECK(!display_view(lcd,true,false,479).night && display_view(lcd,true,false,480).night);
    CHECK(display_view(lcd,true,false,599).night && !display_view(lcd,true,false,600).night);
    // The same offline/AP/clock grace controls both the physical LCD and API preview.
    State offlineLcd=lcd;
    offlineLcd.values=calculate(probes,offlineLcd.settings,0);
    offlineLcd.settings.display.night_start=1320; offlineLcd.settings.display.night_end=420;
    struct NetworkCase { bool configured,ap,link; int minute; };
    for(const auto &scenario : {NetworkCase{true,true,true,720}, NetworkCase{true,false,false,1380},
                        NetworkCase{false,false,false,-1}, NetworkCase{true,false,true,-1}}) {
        DisplayDiagnosticsTimer timer; offlineLcd.link=scenario.link;
        bool issue=display_network_problem(offlineLcd,scenario.configured,scenario.ap,scenario.minute);
        CHECK(issue && timer.show(100000,issue) && timer.show(159999,issue));
        CHECK(display_view(offlineLcd,scenario.configured,scenario.ap,scenario.minute,false,true).diagnostics);
        bool grace=timer.show(160000,issue); CHECK(!grace);
        auto view=display_view(offlineLcd,scenario.configured,scenario.ap,scenario.minute,false,grace);
        CHECK(!view.diagnostics && view.temperature==6000 && view.brightness==(scenario.minute==1380?10:100));
        CHECK(!timer.show(200000,issue)); // reconnect attempts do not restart the minute
        CHECK(!timer.show(200001,false)); // recovered: temperature view immediately
        CHECK(timer.show(250000,true) && !timer.show(310000,true)); // a fresh outage gets a minute
    }
    offlineLcd.link=false; offlineLcd.settings.display.temp_off_enabled=1;
    offlineLcd.values.maximum=2999;
    CHECK(display_view(offlineLcd,true,false,-1,false,false).brightness==0);
    offlineLcd.values.maximum=3000;
    CHECK(display_view(offlineLcd,true,false,-1,false,false).brightness==100);
    CHECK(display_view(offlineLcd,true,false,1380,false,false).brightness==10);
    offlineLcd.faults|=Sensor1;
    CHECK(display_view(offlineLcd,true,false,-1,false,false).diagnostics);
    offlineLcd.faults=RouteError; offlineLcd.values.inlet=InvalidTemp;
    CHECK(display_view(offlineLcd,true,false,-1,false,false).diagnostics);
    offlineLcd.values.inlet=4000; offlineLcd.saved=false;
    CHECK(display_view(offlineLcd,true,false,-1,false,false).diagnostics);
    DisplayDiagnosticsTimer timerWrap; uint32_t outage=UINT32_MAX-1000;
    CHECK(timerWrap.show(outage,true) && timerWrap.show(outage+59999u,true));
    CHECK(!timerWrap.show(outage+60000u,true) && !timerWrap.show(outage,true));
    std::puts("PASS: offline/AP/no-clock diagnostics 59999/60000 ms, recovery/new outage, timer wrap, dimming and real faults");
    lcd.values.maximum=InvalidTemp;
    CHECK(display_view(lcd,true,false,720).diagnostics);
    lcd.settings.display.night_end=lcd.settings.display.night_start; CHECK(!valid(lcd.settings));
    lcd.settings.display.night_enabled=0; CHECK(valid(lcd.settings));
    lcd.settings.display.day_brightness=101; CHECK(!valid(lcd.settings));
    lcd.settings.display.day_brightness=0; CHECK(!valid(lcd.settings));
    lcd.settings.display.day_brightness=100;
    for(int rotation:{0,90,180,270}) { lcd.settings.display.rotation=uint16_t(rotation); CHECK(valid(lcd.settings)); }
    for(int rotation:{1,45,89,91,360}) { lcd.settings.display.rotation=uint16_t(rotation); CHECK(!valid(lcd.settings)); }
    lcd.settings.display.rotation=0; lcd.settings.outlet_sensor=0; CHECK(!valid(lcd.settings));
    lcd.settings.outlet_sensor=3; CHECK(!valid(lcd.settings));
    std::puts("PASS: v1/v2 migration; physical calibrations, inlet/outlet mapping and delta sign; four rotations; five dimming sources; all 1440 night minutes; OFF/wake and diagnostics");
    std::puts("PASS: 25 source combinations, calibration/metrics, ON priority, thresholds/min-on, sensor/stale/overheat failures, manual expiry, persistence/CRC, output NONE, mode restart, uint32 wrap, 10000 invalid samples");
}
