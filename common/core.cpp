#include "core.h"
#include <algorithm>
#include <cstring>

namespace solo {
bool hostname_valid(const char *name) {
    if(!name) return false;
    size_t n=strlen(name); if(n<1 || n>32 || name[0]=='-' || name[n-1]=='-') return false;
    for(const char *p=name;*p;++p) if(!(*p>='a' && *p<='z') && !(*p>='0' && *p<='9') && *p!='-') return false;
    return true;
}
uint32_t crc32(const void *data,size_t length) {
    uint32_t crc=0xffffffff; auto p=static_cast<const uint8_t *>(data);
    while(length--) { crc^=*p++; for(int i=0;i<8;++i) crc=(crc>>1)^((crc&1)?0xedb88320u:0u); }
    return ~crc;
}
bool valid(const Settings &s) {
    const auto &d=s.display;
    return s.revision && s.on>=3000 && s.on<=8000 && s.off>=2000 && s.off<=7900 &&
        s.off<=s.on-100 && s.force>=s.on+500 && s.force<=9500 &&
        s.calibration[0]>=-500 && s.calibration[0]<=500 && s.calibration[1]>=-500 && s.calibration[1]<=500 &&
        s.min_on_s>=10 && s.min_on_s<=600 && s.manual_s>=30 && s.manual_s<=3600 &&
        s.pulse_ms>=20 && s.pulse_ms<=200 && s.on_source<=Maximum && s.off_source<=Maximum &&
        s.relay<=Latching && s.commissioned<=1 && d.source<=Maximum && s.outlet_sensor>=1 && s.outlet_sensor<=2 &&
        (d.rotation==0 || d.rotation==90 || d.rotation==180 || d.rotation==270) &&
        d.off_below>=-500 && d.off_below<=12500 && d.night_start<1440 && d.night_end<1440 &&
        d.day_brightness>=1 && d.day_brightness<=100 && d.night_brightness>=1 && d.night_brightness<=100 &&
        d.night_enabled<=1 && d.temp_off_enabled<=1 && (!d.night_enabled || d.night_start!=d.night_end);
}
void seal(Stored &s) { s.crc=crc32(&s,offsetof(Stored,crc)); }
bool valid(const Stored &s) { return s.magic==0x534f4c4f && s.format==3 && valid(s.settings) && s.crc==crc32(&s,offsetof(Stored,crc)); }
bool decode_stored(const void *data,size_t size,Stored &out) {
    Stored next;
    if(size==sizeof(Stored)) {
        memcpy(&next,data,size); if(!valid(next)) return false;
    } else if(size==34 || size==45) {
        // v1/v2: 6-byte header, 24/35-byte settings, CRC. Append new defaults only.
        const auto *bytes=static_cast<const uint8_t *>(data);
        uint32_t magic,crc; uint16_t format;
        memcpy(&magic,bytes,4); memcpy(&format,bytes+4,2); memcpy(&crc,bytes+size-4,4);
        if(magic!=0x534f4c4f || format!=(size==34?1:2) || crc32(bytes,size-4)!=crc) return false;
        memcpy(&next.settings,bytes+6,size-10);
        if(!valid(next.settings)) return false;
        seal(next);
    } else return false;
    out=next; return true;
}
bool display_network_problem(const State &s,bool configured,bool ap,int minute) {
    return !configured || !s.link || ap || (s.settings.display.night_enabled && minute<0);
}
bool DisplayDiagnosticsTimer::show(uint32_t now,bool network_problem) {
    if(!network_problem) { active=false; expired=false; return false; }
    if(!active) { since=now; active=true; }
    // Latch expiration so a prolonged outage cannot reopen the window after uint32 wrap.
    if(age(now,since)>=60000) expired=true;
    return !expired;
}
DisplayView display_view(const State &s,bool configured,bool ap,int minute,bool interacting,bool network_grace) {
    const auto &d=s.settings.display; DisplayView v;
    v.temperature=s.values.outlet;
    v.threshold_temperature=select(s.values,d.source);
    if(d.night_enabled && minute>=0 && minute<1440)
        v.night=d.night_start<d.night_end ? minute>=d.night_start && minute<d.night_end :
            minute>=d.night_start || minute<d.night_end;
    v.diagnostics=(network_grace && display_network_problem(s,configured,ap,minute)) ||
        !s.saved || (s.faults & ~uint32_t(RouteError)) || v.temperature==InvalidTemp ||
        s.values.inlet==InvalidTemp || v.threshold_temperature==InvalidTemp;
    v.brightness=v.night?d.night_brightness:d.day_brightness;
    if(interacting) v.brightness=d.day_brightness;
    else if(!v.diagnostics && d.temp_off_enabled && v.threshold_temperature<d.off_below) v.brightness=0;
    return v;
}
Values calculate(const Reading (&r)[2],const Settings &s,uint32_t now) {
    Values v;
    for(unsigned i=0;i<2;++i) {
        v.valid[i]=r[i].valid && age(now,r[i].at)<SampleTimeoutMs && r[i].raw>=-5500 && r[i].raw<=12500;
        if(v.valid[i]) v.temperature[i]=int16_t(r[i].raw+s.calibration[i]);
    }
    unsigned out=s.outlet_sensor==1?0:1;
    v.outlet=v.temperature[out]; v.inlet=v.temperature[1-out];
    if(v.valid[0] && v.valid[1]) {
        v.average=int16_t((int(v.temperature[0])+v.temperature[1])/2);
        v.minimum=std::min(v.temperature[0],v.temperature[1]);
        v.maximum=std::max(v.temperature[0],v.temperature[1]);
        v.difference=int16_t(v.outlet-v.inlet);
    }
    return v;
}
int16_t select(const Values &v,uint8_t source) {
    switch(source) {
    case Inlet:return v.inlet; case Outlet:return v.outlet;
    case Average:return v.average; case Minimum:return v.minimum; case Maximum:return v.maximum;
    default:return InvalidTemp;
    }
}
const char *source_name(uint8_t source) {
    static const char *names[]={"inlet","outlet","average","minimum","maximum"};
    return source<=Maximum?names[source]:"invalid";
}
const char *relay_name(uint8_t mode) {
    static const char *names[]={"none","nc","no","latching"};
    return mode<=Latching?names[mode]:"invalid";
}
uint8_t steady_drive(uint8_t mode,bool demand) {
    if(mode==NC) return demand?0:1;
    if(mode==NO) return demand?1:0;
    return 0;
}
Controller::Controller(const Stored &initial,bool loaded,uint32_t boot) :
    activeRelay(None),bootId(boot?boot:1),sequence(boot&0x3fffffffu) {
    if(loaded && valid(initial)) { store=initial; saved=true; }
    else { store=Stored{}; seal(store); dirty=true; }
    activeRelay=store.settings.relay;
}
bool Controller::change(Settings next) {
    if(next.revision!=store.settings.revision) return false;
    if(!valid(next)) return false;
    if(!memcmp(&next,&store.settings,sizeof(next))) return true;
    if(next.relay!=store.settings.relay) next.commissioned=0;
    ++next.revision; if(!next.revision) next.revision=1;
    store.settings=next; seal(store); saved=false; dirty=true; return true;
}
void Controller::persisted(uint32_t revision,bool success) { if(revision==store.settings.revision) saved=success; }
void Controller::sample(unsigned index,const Reading &r) { if(index<2) readings[index]=r; }
void Controller::manual(uint32_t now) { manualAt=now; manualDuration=uint32_t(store.settings.manual_s)*1000; }
State Controller::tick(uint32_t now,bool link,bool delivered,bool relay_error,bool command_sent) {
    if(!started) { onAt=now; started=true; }
    // Latch a stale sample invalid until a new sample arrives. A stalled sensor
    // task cannot revive an old value after the millisecond counter wraps.
    for(auto &r:readings) if(r.valid && age(now,r.at)>=SampleTimeoutMs) r.valid=false;
    State s; s.settings=store.settings; s.values=calculate(readings,store.settings,now);
    s.active_relay=activeRelay; s.saved=saved; s.link=link; s.delivered=delivered;
    s.command_sent=activeRelay!=None && command_sent; s.boot=bootId; s.sequence=++sequence; s.uptime_ms=now;
    for(unsigned i=0;i<2;++i) {
        s.raw[i]=s.values.valid[i]?readings[i].raw:InvalidTemp;
        s.sample_age_ms[i]=readings[i].valid?age(now,readings[i].at):UINT32_MAX;
        if(!s.values.valid[i]) s.faults|=i?Sensor2:Sensor1;
    }
    if(!saved) s.faults|=Storage;
    if(activeRelay!=None && !store.settings.commissioned) s.faults|=NotCommissioned;
    if(activeRelay!=store.settings.relay) s.faults|=HardwareChange;
    if(relay_error) s.faults|=RelayError;
    if(!delivered) s.faults|=RouteError; // Connectivity never affects local control.
    uint32_t elapsed=age(now,manualAt);
    if(manualDuration && elapsed<manualDuration) s.manual_remaining_s=(manualDuration-elapsed+999)/1000;
    else manualDuration=0;
    const int16_t onValue=select(s.values,store.settings.on_source), offValue=select(s.values,store.settings.off_source);
    bool forced=(s.faults & ~uint32_t(RouteError)) || s.manual_remaining_s;
    for(unsigned i=0;i<2;++i) if(s.values.valid[i] && s.values.temperature[i]>=store.settings.force) forced=true;
    // ON wins if independent selectors satisfy ON and OFF simultaneously.
    bool next=demand;
    if(forced || (onValue!=InvalidTemp && onValue>=store.settings.on)) next=true;
    else if(offValue!=InvalidTemp && offValue<=store.settings.off && age(now,onAt)>=uint32_t(store.settings.min_on_s)*1000) next=false;
    if(next && !demand) onAt=now;
    demand=next; s.demand=demand;
    if(s.faults!=oldFaults) { ++faultEvents; oldFaults=s.faults; if(s.faults) lastFault=s.faults; }
    s.fault_events=faultEvents; s.last_fault=lastFault; return s;
}
}
