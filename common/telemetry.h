#pragma once
#include "core.h"
#include "network_config.h"
#include <cstdlib>
namespace solo {
struct TelemetryGate {
    bool sent=false; uint32_t at=0; State previous;
    bool due(const State &s,const NetworkConfig &c,uint32_t now) const {
        if(!sent) return true;
        if(s.faults!=previous.faults || s.demand!=previous.demand || s.active_relay!=previous.active_relay || s.command_sent!=previous.command_sent || s.settings.revision!=previous.settings.revision || bool(s.manual_remaining_s)!=bool(previous.manual_remaining_s)) return true;
        for(unsigned i=0;i<2;++i) if(s.values.valid[i]!=previous.values.valid[i]) return true;
        if(age(now,at)>=uint32_t(c.publish_s)*1000) return true;
        if(age(now,at)<uint32_t(c.publish_min_s)*1000) return false;
        for(unsigned i=0;i<2;++i) if(s.values.valid[i] && std::abs(int(s.values.temperature[i])-previous.values.temperature[i])>=c.publish_delta_centi) return true;
        return false;
    }
    void published(const State &s,uint32_t now) { previous=s; at=now; sent=true; }
};
}
