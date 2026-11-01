#include "analysis.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
using namespace solo;
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
static AnalysisResult sample(ThermalAnalysis &model,const AnalysisConfig &cfg,uint32_t now,float out=60,float in=40,bool good=true) {
    State s; s.boot=1; s.sequence=now/20; s.saved=true; s.sample_age_ms[0]=s.sample_age_ms[1]=0;
    Reading readings[2]={{int16_t(std::round(in*100)),good,now},{int16_t(std::round(out*100)),true,now}};
    s.values=calculate(readings,s.settings,now); return model.update(s,cfg,now);
}
static AnalysisResult run(ThermalAnalysis &model,const AnalysisConfig &cfg) {
    AnalysisResult r; for(uint32_t t=0;t<=400000;t+=1000) r=sample(model,cfg,t); return r;
}
static void stove_phase_tests() {
    AnalysisConfig cfg; cfg.smooth_s=10; cfg.trend_s=60;
    ThermalAnalysis normal; AnalysisResult r; bool waterCooled=false,waterWarmed=false;
    // The reported 52.1/46.4 and 52.3/46.6 belong to one heating phase.
    for(uint32_t t=0;t<=1800000;t+=1000) {
        float wave=.3f*std::sin(t/45000.f);
        r=sample(normal,cfg,t,52.2f+wave,46.5f+.1f*std::sin(t/60000.f));
        if(t>=60000) CHECK(r.stove==StovePhase::Heating);
        waterCooled|=!strcmp(r.thermal,"cooling"); waterWarmed|=!strcmp(r.thermal,"warming");
    }
    CHECK(waterCooled && waterWarmed);
    // A gradual drop alone is still normal heating if the temperature lift remains.
    ThermalAnalysis decliningWater;
    for(uint32_t t=0;t<=1800000;t+=1000) {
        r=sample(decliningWater,cfg,t,55-t/300000.f,49.3f-t/300000.f);
        if(t>=60000) CHECK(r.stove==StovePhase::Heating);
    }
    // Weakening requires long history, both declining, and a smaller/closing lift.
    ThermalAnalysis weakening; bool sawDecline=false;
    for(uint32_t t=0;t<=2400000;t+=1000) {
        r=sample(weakening,cfg,t,50-t/200000.f,45-t/266666.667f);
        if(t<900000) CHECK(r.stove!=StovePhase::Declining);
        sawDecline|=r.stove==StovePhase::Declining;
    }
    CHECK(sawDecline);
    // Crossing a temperature-lift threshold by a tenth of a degree is not recovery.
    for(uint32_t t=2401000;t<=2700000;t+=1000) {
        float out=38-(t-2400000)/600000.f;
        r=sample(weakening,cfg,t,out,out-3-.1f*std::sin(t/30000.f));
        CHECK(r.stove==StovePhase::Declining);
    }
    for(uint32_t t=2701000;t<=3000000;t+=1000)
        r=sample(weakening,cfg,t,37.5f+(t-2700000)/30000.f,34.5f);
    CHECK(r.stove==StovePhase::Heating);
    ThermalAnalysis cold; r=sample(cold,cfg,0,24,24);
    for(uint32_t t=1000;t<=240000;t+=1000) r=sample(cold,cfg,t,24,24);
    CHECK(r.stove==StovePhase::Cold);
    // Initial warm-up is possible before the water exceeds the cold-temperature band.
    for(uint32_t t=241000;t<=360000;t+=1000) r=sample(cold,cfg,t,24+(t-240000)/60000.f,24);
    CHECK(r.stove==StovePhase::Warming && r.outlet<30);
    ThermalAnalysis residual;
    for(uint32_t t=0;t<=300000;t+=1000) r=sample(residual,cfg,t,32,31.5);
    CHECK(r.stove==StovePhase::Warm); // no claim that the unmeasured pump has stopped
    r=sample(residual,cfg,301000,32,31.5,false);
    CHECK(r.stove==StovePhase::Unknown && std::isnan(r.long_outlet_rate));
    r=sample(residual,cfg,302000,32,31.5);
    CHECK(r.stove==StovePhase::Unknown && r.stove_history_s==0);
}
static void transition_tests() {
    AnalysisConfig cfg; cfg.smooth_s=10; cfg.trend_s=60;
    cfg.flow_model_enabled=1; cfg.flow_bias=10; cfg.hold_s=0;
    cfg.heat_model_enabled=1; cfg.reference_rate=1; cfg.loss_per_min=.01f;
    ThermalAnalysis model; AnalysisResult r; bool seen=false,forecast=false;
    for(uint32_t t=0;t<=430000;t+=1000) {
        float seconds=t/1000.f;
        float out=seconds<=120?50+seconds/12:seconds<=320?60-(seconds-120)*.105f:39+(seconds-320)*.01f;
        float in=seconds<=120?25:25+(seconds-120)*.035f;
        r=sample(model,cfg,t,out,in);
        if(!strcmp(r.thermal,"redistributing")) {
            seen=true; CHECK(std::isnan(r.relative_pct) && std::isnan(r.refill_eta_min));
            CHECK(!strcmp(r.model,"settling"));
        }
        if(std::isfinite(r.mixing_minimum)) {
            forecast=true; CHECK(std::fabs(r.mixing_minimum-38.65f)<.01f);
            CHECK(r.mixing_eta_min>0 && r.mixing_eta_min<200.f/60);
        }
        if(t>=320000) CHECK(std::isnan(r.mixing_minimum));
    }
    CHECK(seen && forecast && !strcmp(r.thermal,"warming"));
    ThermalAnalysis contradicted;
    for(uint32_t t=0;t<=180000;t+=1000) {
        float seconds=t/1000.f;
        r=sample(contradicted,cfg,t,seconds<=120?50+seconds/12:60-(seconds-120)*.105f,
                 seconds<=120?25:25+(seconds-120)*.035f);
    }
    CHECK(std::isfinite(r.mixing_minimum));
    r=sample(contradicted,cfg,181000,35,30); CHECK(std::isnan(r.mixing_minimum));
    r=sample(contradicted,cfg,182000,50,30);
    CHECK(!strcmp(r.thermal,"redistributing") && std::isnan(r.mixing_minimum));
    r=sample(contradicted,cfg,183000,50,30,false); CHECK(!strcmp(r.quality,"invalid_data"));
    r=sample(contradicted,cfg,184000,50,30);
    CHECK(!strcmp(r.quality,"warming_up") && std::isnan(r.mixing_minimum));
    // Loss of a probe and a configuration change must discard the peak/forecast.
    r=sample(model,cfg,431000,45,35,false); CHECK(std::isnan(r.mixing_minimum));
    r=sample(model,cfg,432000,45,35); CHECK(!strcmp(r.quality,"warming_up"));
    ++cfg.revision; r=sample(model,cfg,433000,45,35); CHECK(!strcmp(r.quality,"warming_up"));
    // Other pump stages still identify the signature but have no fitted forecast.
    cfg.pump_stage=3; ++cfg.revision; ThermalAnalysis other;
    bool otherSeen=false;
    for(uint32_t t=0;t<=240000;t+=1000) {
        float seconds=t/1000.f;
        r=sample(other,cfg,t,seconds<=120?50+seconds/12:60-(seconds-120)*.105f,
                 seconds<=120?25:25+(seconds-120)*.035f);
        if(!strcmp(r.thermal,"redistributing")) otherSeen=true;
        CHECK(std::isnan(r.mixing_minimum));
    }
    CHECK(otherSeen);
    // Ordinary cooling of both pipes is not the opposing-temperature signature.
    ThermalAnalysis cooling; cfg.heat_model_enabled=0; cfg.flow_model_enabled=0; ++cfg.revision;
    for(uint32_t t=0;t<=300000;t+=1000) {
        r=sample(cooling,cfg,t,60-t/60000.f,40-t/60000.f);
        CHECK(strcmp(r.thermal,"redistributing") && std::isnan(r.mixing_minimum));
    }
    // Slow but continuing changes retain direction; genuinely flat water exits it.
    ThermalAnalysis hysteresis; cfg=AnalysisConfig{}; cfg.smooth_s=1; cfg.trend_s=30;
    uint32_t now=0; float out=60;
    const float rates[]={.2f,.1f,.02f,-.2f,-.1f,-.02f};
    const char *expected[]={"warming","warming","steady","cooling","cooling","steady"};
    for(unsigned phase=0;phase<6;++phase) {
        for(unsigned sec=0;sec<180;++sec) { out+=rates[phase]/60; r=sample(hysteresis,cfg,now,out,40); now+=1000; }
        CHECK(!strcmp(r.thermal,expected[phase]));
    }
    // A high inlet keeps warning priority even during a sharp redistribution.
    r=sample(hysteresis,cfg,now,55,90); CHECK(!strcmp(r.advice,"check_heat_removal"));
}
int main() {
    stove_phase_tests();
    transition_tests();
    AnalysisConfig cfg; CHECK(valid(cfg)); AnalysisStored stored; seal(stored); CHECK(valid(stored));
    for(size_t i=0;i<sizeof(stored);++i) { auto bad=stored; reinterpret_cast<unsigned char *>(&bad)[i]^=1; CHECK(!valid(bad)); }
    auto bad=cfg; bad.ambient_c=std::numeric_limits<float>::infinity(); CHECK(!valid(bad));
    bad=cfg; bad.enabled=0.5f; CHECK(!valid(bad)); bad=cfg; bad.max_gap_s=bad.trend_s; CHECK(!valid(bad));
    bad=cfg; bad.heat_model_enabled=1; CHECK(!valid(bad));
    ThermalAnalysis model; auto r=run(model,cfg);
    CHECK(!strcmp(r.quality,"ready") && !strcmp(r.thermal,"steady"));
    CHECK(std::fabs(r.outlet_rate)<0.0001 && std::isnan(r.relative_pct) && std::isnan(r.flow_score));
    CHECK(std::isnan(r.refill_eta_min) && r.circulation==Circulation::Unknown);
    r=sample(model,cfg,401000,60,40,false); CHECK(!strcmp(r.quality,"invalid_data") && std::isnan(r.outlet_rate));
    r=sample(model,cfg,402000); CHECK(!strcmp(r.quality,"warming_up"));
    cfg.flow_model_enabled=1; cfg.flow_bias=-10; cfg.hold_s=0; cfg.settle_s=0;
    cfg.heat_model_enabled=1; cfg.loss_per_min=0.01f; cfg.reference_rate=1; cfg.capacity_kj_k=120; ++cfg.revision;
    CHECK(valid(cfg)); r=run(model,cfg);
    CHECK(r.circulation==Circulation::Stagnant && std::fabs(r.relative_pct-35)<0.001 && std::fabs(r.source_kw-0.7)<0.001);
    cfg.flow_bias=10; cfg.flow_l_min=5; cfg.flow_per_min=cfg.water_kj_l_k*cfg.flow_l_min/cfg.capacity_kj_k; ++cfg.revision;
    r=run(model,cfg); CHECK(r.circulation==Circulation::Flow && r.reference_exceeded);
    CHECK(std::fabs(r.export_kw-6.966667)<0.001 && std::fabs(r.source_kw-7.666667)<0.001);
    CHECK(std::isnan(sample(model,cfg,500000).relative_pct)); // gap invalidates learned history
    cfg.flow_bias=0; ++cfg.revision; r=run(model,cfg);
    CHECK(r.circulation==Circulation::Unknown && std::isnan(r.relative_pct));
    cfg.flow_bias=-10; cfg.loss_per_min=0.05f; cfg.reference_rate=3; ++cfg.revision;
    for(uint32_t t=0;t<=500000;t+=1000) { float seconds=t/1000.f; r=sample(model,cfg,t,60+0.01f*seconds-0.00002f*seconds*seconds); }
    CHECK(!strcmp(r.power_trend,"falling") && std::isfinite(r.refill_eta_min) && r.refill_eta_min>=0);
    r=sample(model,cfg,501000,90); CHECK(!strcmp(r.advice,"check_heat_removal"));
    cfg.ambient_c=26; ++cfg.revision; r=sample(model,cfg,502000); CHECK(!strcmp(r.quality,"warming_up"));
    ThermalAnalysis wrapping; cfg=AnalysisConfig{};
    for(uint32_t t=0;t<=120000;t+=1000) r=sample(wrapping,cfg,UINT32_MAX-2000+t);
    CHECK(!strcmp(r.quality,"ready") && std::fabs(r.outlet_rate)<0.0001);
    std::puts("PASS: analysis CRC/ranges, unknown defaults, slopes, stale/gaps, inferred modes, heat balance/units, unclipped reference, forecast, warning priority, reset and timer wrap");
}
