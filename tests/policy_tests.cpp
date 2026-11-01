#include "access.h"
#include "telemetry.h"
#include <cassert>
#include <cstdio>
using namespace solo;
int main() {
    AccessPin pin; assert(pin.expired(0)); pin.rotate(0,0);
    assert(pin.value==1 && !pin.expired(3599999) && pin.expired(3600000));
    auto old=pin.value; pin.rotate(UINT32_MAX,3600000);
    assert(pin.value!=old && pin.value<10000 && !pin.expired(7199999) && pin.expired(7200000));
    // A display request resets the deadline, even when issued before expiry.
    old=pin.value; pin.rotate(9999,3601000);
    assert(pin.value!=old && pin.expires==7201000);
    for(unsigned value=0;value<10000;++value) {
        pin.value=uint16_t(value); pin.rotate(UINT32_MAX,UINT64_C(0x100000000));
        assert(pin.value!=value && pin.value<10000 && !pin.expired(UINT64_C(0x100000000)+3599999));
    }
    AccessRate rate; rate.boot(0,0); assert(!rate.wait(0));
    for(unsigned i=0;i<10;++i) {
        uint64_t now=i*1000; assert(!rate.wait(now)); rate.result(false,now);
        assert(rate.wait(now+1)>0 && rate.failures==i+1);
    }
    assert(rate.wait(10000)==3599); assert(rate.wait(3608999)==1); assert(!rate.wait(3609000));
    AccessRate reboot; reboot.boot(rate.failures,0); assert(reboot.wait(0)==3600 && reboot.wait(3599999)==1);
    rate.result(true,3609000); assert(rate.failures==0 && rate.wait(3609000)==1 && !rate.wait(3610000));
    reboot.boot(rate.failures,0); assert(!reboot.wait(0));
    AccessSessions sessions; const char *key="0123456789abcdef0123456789abcdef";
    sessions.add(key,0); assert(sessions.check(key,299999)); assert(!sessions.check("wrong",1)); assert(!sessions.check(key,300000));
    sessions.add(key,0); assert(sessions.check(key,290000,true)); assert(sessions.check(key,589999)); assert(!sessions.check(key,590000));
    sessions.add(key,0); for(unsigned i=0;i<300;++i) assert(sessions.check(key,i*1000)); assert(!sessions.check(key,300000));
    sessions.add(key,UINT64_C(0x100000000)); assert(sessions.check(key,UINT64_C(0x100000001))); sessions.remove(key); assert(!sessions.check(key,UINT64_C(0x100000002)));
    TelemetryGate gate; NetworkConfig c; State state; state.values.valid[0]=state.values.valid[1]=true; state.values.temperature[0]=2500; state.values.temperature[1]=6000;
    assert(gate.due(state,c,0)); gate.published(state,0);
    for(unsigned i=1;i<60;++i) { state.sequence++; state.uptime_ms=i*1000; assert(!gate.due(state,c,i*1000)); }
    assert(gate.due(state,c,60000)); gate.published(state,60000);
    state.values.temperature[1]+=19; assert(!gate.due(state,c,65000)); state.values.temperature[1]++; assert(gate.due(state,c,65000)); gate.published(state,65000);
    state.values.temperature[0]-=20; assert(!gate.due(state,c,65500)); assert(gate.due(state,c,66000)); gate.published(state,66000);
    state.faults=Sensor1; assert(gate.due(state,c,66001)); gate.published(state,66001);
    state.values.valid[0]=false; assert(gate.due(state,c,66002)); gate.published(state,66002);
    assert(!gate.due(state,c,67002)); state.demand=!state.demand; assert(gate.due(state,c,66003));
    gate.published(state,UINT32_MAX-500); state.values.temperature[1]+=20; assert(gate.due(state,c,499));
    puts("PASS access: global 1s/10 failures/1h, reboot lockout, successful reset, inactivity and passive polling; telemetry: heartbeat, cumulative calibrated delta, rate floor, faults, validity, pump, timer wrap");
}
