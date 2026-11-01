#include "analysis.h"
#include <algorithm>
#include <cstring>

namespace solo {
// Empirical first-cycle calibration; see docs/kalibrace-2026-11-01/DRUHY_CYKLUS.md.
static constexpr float MixingDropFraction=0.61f;
static constexpr uint32_t MixingDurationMs=200000;
// Provisional phase calibration, independent of the minute-scale water trend.
// See docs/STAVY_KAMEN.md; pump-off/cooldown is deliberately not inferred here.
static constexpr float HeatingDeltaC=2,DecliningDeltaC=3;
static constexpr uint32_t StoveSmoothMs=600000,StoveHistoryMs=900000;
const AnalysisField AnalysisFields[]={
#define X(key,initial,lo,hi,step,label,group,unit) {#key,label,group,unit,&AnalysisConfig::key,float(initial),float(lo),float(hi),float(step)},
#include "analysis_fields.inc"
#undef X
};
const size_t AnalysisFieldCount=sizeof(AnalysisFields)/sizeof(AnalysisFields[0]);
bool valid(const AnalysisConfig &c) {
    if(!c.revision) return false;
    for(size_t i=0;i<AnalysisFieldCount;++i) {
        const auto &f=AnalysisFields[i]; float v=c.*(f.member);
        if(!std::isfinite(v) || v<f.minimum || v>f.maximum || (f.step==1 && v!=std::floor(v))) return false;
    }
    return c.max_gap_s<c.trend_s && c.flow_off_score<c.flow_on_score &&
        (!c.heat_model_enabled || (c.flow_model_enabled && c.reference_rate>=0.01f));
}
void seal(AnalysisStored &s) { s.crc=crc32(&s,offsetof(AnalysisStored,crc)); }
bool valid(const AnalysisStored &s) {
    return s.magic==0x414e4c31 && s.format==1 && valid(s.config) && s.crc==crc32(&s,offsetof(AnalysisStored,crc));
}
const char *circulation_name(Circulation state) {
    return state==Circulation::Flow?"likely_flow":state==Circulation::Stagnant?"likely_stagnant":"unknown";
}
const char *stove_phase_name(StovePhase state) {
    switch(state) {
        case StovePhase::Cold: return "cold";
        case StovePhase::Warming: return "warming";
        case StovePhase::Heating: return "heating";
        case StovePhase::Declining: return "declining";
        case StovePhase::Warm: return "warm";
        default: return "unknown";
    }
}
void ThermalAnalysis::reset() {
    count=0; head=0; initialized=false; mode=Circulation::Unknown; candidate=Circulation::Unknown;
    peakTracking=false; redistributing=false; mixingForecastValid=false; thermalTrend=Trend::Steady;
    stoveInitialized=false; stove=stoveCandidate=StovePhase::Unknown;
}
void ThermalAnalysis::update_stove(AnalysisResult &r,const AnalysisConfig &c,uint32_t now) {
    if(!stoveInitialized || redistributing) {
        slowInlet=r.inlet; slowOutlet=r.outlet; stoveStart=stoveLast=now; stoveInitialized=true;
    }
    float alpha=1-std::exp(-float(age(now,stoveLast))/StoveSmoothMs); stoveLast=now;
    slowInlet+=alpha*(r.inlet-slowInlet); slowOutlet+=alpha*(r.outlet-slowOutlet);
    r.stove_history_s=age(now,stoveStart)/1000;
    if(age(now,stoveStart)>=StoveHistoryMs) {
        // Derivatives of the ten-minute exponential averages, in degrees/minute.
        r.long_inlet_rate=(r.inlet-slowInlet)/(StoveSmoothMs/60000.f);
        r.long_outlet_rate=(r.outlet-slowOutlet)/(StoveSmoothMs/60000.f);
    }
    const float cold=c.ambient_c+c.cold_band_c,operatingFloor=cold+c.cold_band_c;
    const bool normalHeat=r.outlet>=operatingFloor && r.delta>=HeatingDeltaC;
    const bool established=stove==StovePhase::Heating || stove==StovePhase::Declining;
    const bool declining=established && r.outlet>=operatingFloor && r.delta<DecliningDeltaC &&
        r.long_outlet_rate<-0.15f && r.long_inlet_rate<-0.1f &&
        r.long_outlet_rate-r.long_inlet_rate<-0.05f;
    const bool recovered=normalHeat &&
        ((r.long_outlet_rate>=-0.03f && r.long_inlet_rate>=-0.03f) ||
         (r.delta>=DecliningDeltaC+1 && r.outlet_rate>=0.3f));
    const bool keepDeclining=stove==StovePhase::Declining && r.outlet>=operatingFloor && !recovered;
    StovePhase next=StovePhase::Warm;
    if(std::max(r.inlet,r.outlet)<=cold && std::fabs(r.inlet_rate)<c.trend_deadband && std::fabs(r.outlet_rate)<c.trend_deadband)
        next=StovePhase::Cold;
    else if(redistributing) next=StovePhase::Heating;
    else if(!established && (stove!=StovePhase::Unknown || r.inlet<operatingFloor) &&
            r.outlet_rate>=std::max(0.3f,2*c.trend_deadband) && r.outlet_rate-r.inlet_rate>=0.2f)
        next=StovePhase::Warming;
    else if(declining || keepDeclining) next=StovePhase::Declining;
    else if(normalHeat) next=StovePhase::Heating;
    if(next!=stoveCandidate) { stoveCandidate=next; stoveCandidateAt=now; }
    uint32_t hold=next==StovePhase::Warming?30000:
        next==StovePhase::Declining || next==StovePhase::Warm?180000:60000;
    if(stove==StovePhase::Unknown || age(now,stoveCandidateAt)>=hold) stove=next;
    r.stove=stove;
}
float ThermalAnalysis::slope(float Point::*field,uint32_t now,float window_s) const {
    double sx=0,sy=0,sxx=0,sxy=0; unsigned n=0; uint32_t oldest=0;
    for(unsigned i=0;i<count;++i) {
        const auto &p=points[(head+301-count+i)%301]; auto elapsed=age(now,p.at);
        float y=p.*field;
        if(elapsed>window_s*1000 || !std::isfinite(y)) continue;
        double x=-double(elapsed)/60000; // all derivatives use minutes
        sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; ++n; oldest=std::max(oldest,elapsed);
    }
    double denominator=n*sxx-sx*sx;
    if(n<3 || oldest<window_s*900 || denominator<=0) return NAN;
    return float((n*sxy-sx*sy)/denominator);
}
AnalysisResult ThermalAnalysis::update(const State &s,const AnalysisConfig &c,uint32_t now) {
    AnalysisResult r; r.revision=c.revision; r.boot=s.boot; r.sequence=s.sequence; r.at=now;
    if(!valid(c)) { reset(); r.quality="invalid_config"; return r; }
    if(!c.enabled) { reset(); r.quality="disabled"; return r; }
    if(c.heat_model_enabled) r.model="waiting_for_circulation";
    // The independent thermostat remains responsible for protective control.
    if((s.values.valid[0] && s.values.temperature[0]/100.f>=c.high_temp_c) ||
       (s.values.valid[1] && s.values.temperature[1]/100.f>=c.high_temp_c)) r.advice="check_heat_removal";
    if(!s.values.valid[0] || !s.values.valid[1] || s.sample_age_ms[0]>=SampleTimeoutMs || s.sample_age_ms[1]>=SampleTimeoutMs) {
        reset(); r.quality="invalid_data"; return r;
    }
    if(c.revision!=revision || s.settings.revision!=settingsRevision || s.boot!=boot ||
       (initialized && age(now,last)>c.max_gap_s*1000)) reset();
    revision=c.revision; settingsRevision=s.settings.revision; boot=s.boot;
    float in=s.values.inlet/100.f,out=s.values.outlet/100.f;
    if(!redistributing && (!peakTracking || out>peakOutlet || age(now,peakAt)>300000)) {
        peakOutlet=out; peakInlet=in; peakAt=now; peakTracking=true;
    }
    if(!initialized) { start=now; last=now; filteredIn=in; filteredOut=out; modeAt=now; initialized=true; }
    float alpha=1-std::exp(-float(age(now,last))/(1000.f*c.smooth_s));
    filteredIn+=alpha*(in-filteredIn); filteredOut+=alpha*(out-filteredOut); last=now;
    auto &point=points[head]; point={now,filteredIn,filteredOut,NAN}; head=(head+1)%301; if(count<301) ++count;
    r.inlet=filteredIn; r.outlet=filteredOut; r.delta=filteredOut-filteredIn;
    if(age(now,start)<c.trend_s*1000) return r;
    r.inlet_rate=slope(&Point::inlet,now,c.trend_s); r.outlet_rate=slope(&Point::outlet,now,c.trend_s);
    if(!std::isfinite(r.inlet_rate) || !std::isfinite(r.outlet_rate)) return r;
    r.quality="ready";
    // Schmitt band: enter at the configured limit, leave at half the limit.
    if(r.outlet_rate>c.trend_deadband) thermalTrend=Trend::Warming;
    else if(r.outlet_rate<-c.trend_deadband) thermalTrend=Trend::Cooling;
    else if((thermalTrend==Trend::Warming && r.outlet_rate<c.trend_deadband*0.5f) ||
            (thermalTrend==Trend::Cooling && r.outlet_rate>-c.trend_deadband*0.5f)) thermalTrend=Trend::Steady;
    r.thermal=std::max(in,out)<c.ambient_c+c.cold_band_c?"cool":
        thermalTrend==Trend::Warming?"warming":thermalTrend==Trend::Cooling?"cooling":"steady";
    // First-cycle calibration (2026-11-01); second refuelling cycle held out.
    // A temperature signature, not confirmation of motor operation or fire output.
    if(!redistributing && age(now,peakAt)<=120000 && peakOutlet-peakInlet>=5 &&
       peakOutlet-out>=1 && r.inlet_rate>=0.3f && r.outlet_rate<=-0.5f) {
        redistributing=true;
        mixingForecastValid=c.pump_stage==2 && peakOutlet>=50 && peakOutlet<=65 && peakInlet>=20 && peakInlet<=40;
    }
    if(redistributing && (age(now,peakAt)>300000 || r.outlet_rate>-c.trend_deadband*0.5f)) {
        redistributing=false; peakTracking=false;
    }
    if(redistributing) {
        r.thermal="redistributing";
        const float predicted=peakOutlet-MixingDropFraction*(peakOutlet-peakInlet);
        const auto elapsed=age(now,peakAt);
        // Do not retain an expired forecast or one contradicted by the measurements.
        if(elapsed>=MixingDurationMs || out<predicted-0.5f) mixingForecastValid=false;
        if(mixingForecastValid) {
            r.mixing_minimum=predicted; r.mixing_eta_min=(MixingDurationMs-elapsed)/60000.f;
        }
    }
    if(!strcmp(r.advice,"none") && r.outlet_rate>=c.rise_warn_c_min) r.advice="check_rising_temperature";
    if(c.flow_model_enabled) {
        float z=c.flow_bias+c.flow_w_in*r.inlet_rate+c.flow_w_out*r.outlet_rate+c.flow_w_delta*r.delta;
        r.flow_score=1/(1+std::exp(-std::clamp(z,-60.f,60.f)));
        Circulation next=r.flow_score>=c.flow_on_score?Circulation::Flow:
            r.flow_score<=c.flow_off_score?Circulation::Stagnant:Circulation::Unknown;
        if(next!=candidate) { candidate=next; candidateAt=now; }
        if(age(now,candidateAt)>=c.hold_s*1000 && mode!=candidate) {
            mode=candidate; modeAt=now;
            for(auto &p:points) p.power=NAN; // never predict refuelling across a pump transition
        }
    }
    r.circulation=mode;
    if(r.outlet_rate>c.trend_deadband && c.high_temp_c>out) {
        float eta=(c.high_temp_c-out)/r.outlet_rate;
        if(eta<=c.forecast_max_min) r.limit_eta_min=eta;
    }
    update_stove(r,c,now);
    if(redistributing) {
        modeAt=now;
        if(c.heat_model_enabled) r.model="settling";
        for(auto &p:points) p.power=NAN;
        return r;
    }
    if(!c.heat_model_enabled || mode==Circulation::Unknown) return r;
    if(age(now,modeAt)<c.settle_s*1000) { r.model="settling"; return r; }
    r.model="conditional_estimate";
    r.heat_rate=r.outlet_rate+c.loss_per_min*(r.outlet-c.ambient_c)+
        (mode==Circulation::Flow?c.flow_per_min*r.delta:0);
    // A negative residual is evidence against this heating model, not zero power.
    if(r.heat_rate<0) { r.quality="model_mismatch"; r.model="inconsistent"; return r; }
    r.relative_pct=100*r.heat_rate/c.reference_rate; r.reference_exceeded=r.relative_pct>100;
    if(c.capacity_kj_k>0) r.source_kw=c.capacity_kj_k*r.heat_rate/60;
    if(mode==Circulation::Flow && c.flow_l_min>0) r.export_kw=c.water_kj_l_k*c.flow_l_min*r.delta/60;
    point.power=r.relative_pct;
    r.power_rate=slope(&Point::power,now,c.trend_s);
    if(std::isfinite(r.power_rate)) {
        r.power_trend=r.power_rate>c.power_deadband_pct_min?"rising":
            r.power_rate<-c.power_deadband_pct_min?"falling":"steady";
        if(r.power_rate<=-c.refill_min_decline_pct_min) {
            float eta=std::max(0.f,(r.relative_pct-c.refill_level_pct)/-r.power_rate);
            if(eta<=c.forecast_max_min) r.refill_eta_min=eta;
            if(r.relative_pct<=c.refill_level_pct && !strcmp(r.advice,"none")) r.advice="check_fire";
        }
    }
    return r;
}
}
