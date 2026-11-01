#pragma once
#include <cstddef>
#include <cstdint>

namespace solo {
constexpr int16_t InvalidTemp=INT16_MIN;
constexpr uint32_t SampleTimeoutMs=3500;
inline uint32_t age(uint32_t now,uint32_t then) { return now-then; }
enum Source : uint8_t { Inlet, Outlet, Average, Minimum, Maximum };
enum RelayMode : uint8_t { None, NC, NO, Latching };
enum Fault : uint32_t {
    Sensor1=1, Sensor2=2, Storage=4, NotCommissioned=8, HardwareChange=16,
    RelayError=32, RouteError=64
};
#pragma pack(push,1)
struct DisplaySettings {
    int16_t off_below=3000;
    uint16_t night_start=22*60, night_end=7*60;
    uint8_t source=Maximum, day_brightness=100, night_brightness=10;
    uint8_t night_enabled=1, temp_off_enabled=1;
    uint16_t rotation=0; // clockwise relative to the original landscape orientation
};
struct Settings {
    uint32_t revision=1;
    int16_t on=6000, off=5500, force=8500;
    int16_t calibration[2]={0,0};
    uint16_t min_on_s=60, manual_s=600, pulse_ms=50;
    uint8_t on_source=Maximum, off_source=Maximum, relay=None, commissioned=0;
    DisplaySettings display;
    uint8_t outlet_sensor=2; // physical probe; calibration and ROM binding never move
};
struct Stored { uint32_t magic=0x534f4c4f; uint16_t format=3; Settings settings; uint32_t crc=0; };
#pragma pack(pop)
static_assert(offsetof(Settings,display)==24 && offsetof(Settings,outlet_sensor)==37 && sizeof(Settings)==38,"Persistent layout changed: bump Stored.format");
struct Reading { int16_t raw=InvalidTemp; bool valid=false; uint32_t at=0; };
struct Values {
    int16_t temperature[2]={InvalidTemp,InvalidTemp};
    int16_t inlet=InvalidTemp, outlet=InvalidTemp;
    int16_t average=InvalidTemp, minimum=InvalidTemp, maximum=InvalidTemp, difference=InvalidTemp;
    bool valid[2]={false,false};
};
struct State {
    Settings settings;
    Values values;
    int16_t raw[2]={InvalidTemp,InvalidTemp};
    uint32_t sample_age_ms[2]={UINT32_MAX,UINT32_MAX};
    uint32_t sequence=0, boot=0, uptime_ms=0, faults=0, fault_events=0, last_fault=0;
    uint32_t manual_remaining_s=0;
    uint8_t active_relay=None;
    bool demand=true, saved=false, link=false, delivered=false, command_sent=false;
};
uint32_t crc32(const void *data,size_t length);
bool hostname_valid(const char *name);
bool valid(const Settings &s);
bool valid(const Stored &s);
bool decode_stored(const void *data,size_t size,Stored &out);
void seal(Stored &s);
Values calculate(const Reading (&r)[2],const Settings &s,uint32_t now);
int16_t select(const Values &v,uint8_t source);
const char *source_name(uint8_t source);
const char *relay_name(uint8_t mode);
struct DisplayView {
    int16_t temperature=InvalidTemp;
    int16_t threshold_temperature=InvalidTemp;
    bool night=false, diagnostics=true;
    uint8_t brightness=100;
};
// minute = local minutes since midnight, -1 until the clock is synchronized.
bool display_network_problem(const State &s,bool configured,bool ap,int minute);
class DisplayDiagnosticsTimer {
public:
    bool show(uint32_t now,bool network_problem);
private:
    uint32_t since=0;
    bool active=false,expired=false;
};
DisplayView display_view(const State &s,bool configured,bool ap,int minute,bool interacting=false,bool network_grace=true);
// Outputs for the two IN pins of the bridge (00 = coast). None is always 00.
uint8_t steady_drive(uint8_t mode,bool demand);
class Controller {
public:
    Controller(const Stored &initial,bool loaded,uint32_t boot);
    Stored store;
    bool dirty=false;
    bool change(Settings next);
    void persisted(uint32_t revision,bool success);
    void sample(unsigned index,const Reading &r);
    void manual(uint32_t now);
    State tick(uint32_t now,bool link,bool delivered,bool relay_error,bool command_sent);
private:
    Reading readings[2];
    uint8_t activeRelay;
    uint32_t bootId, sequence, onAt=0, manualAt=0, manualDuration=0;
    uint32_t oldFaults=0, faultEvents=0, lastFault=0;
    bool saved=false, demand=true, started=false;
};
}
