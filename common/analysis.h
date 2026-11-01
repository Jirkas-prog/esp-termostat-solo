#pragma once
#include "core.h"
#include <cmath>

namespace solo {
struct AnalysisConfig {
    uint32_t revision=1;
#define X(key,initial,lo,hi,step,label,group,unit) float key=float(initial);
#include "analysis_fields.inc"
#undef X
};
struct AnalysisField {
    const char *key,*label,*group,*unit;
    float AnalysisConfig::*member;
    float initial,minimum,maximum,step;
};
extern const AnalysisField AnalysisFields[];
extern const size_t AnalysisFieldCount;
bool valid(const AnalysisConfig &c);
struct AnalysisStored {
    uint32_t magic=0x414e4c31,format=1;
    AnalysisConfig config;
    uint32_t crc=0;
};
void seal(AnalysisStored &s);
bool valid(const AnalysisStored &s);
enum class Circulation { Unknown, Flow, Stagnant };
enum class StovePhase { Unknown, Cold, Warming, Heating, Declining, Warm };
struct AnalysisResult {
    const char *quality="warming_up",*thermal="unknown",*power_trend="unknown",*advice="none",*model="not_calibrated";
    StovePhase stove=StovePhase::Unknown;
    float long_inlet_rate=NAN,long_outlet_rate=NAN;
    uint32_t stove_history_s=0;
    uint32_t revision=0,boot=0,sequence=0,at=0;
    Circulation circulation=Circulation::Unknown;
    float inlet=NAN,outlet=NAN,delta=NAN,inlet_rate=NAN,outlet_rate=NAN;
    float flow_score=NAN,heat_rate=NAN,relative_pct=NAN,source_kw=NAN,export_kw=NAN;
    float power_rate=NAN,refill_eta_min=NAN,limit_eta_min=NAN;
    bool reference_exceeded=false;
    float mixing_minimum=NAN,mixing_eta_min=NAN;
};
class ThermalAnalysis {
public:
    AnalysisResult update(const State &s,const AnalysisConfig &c,uint32_t now);
private:
    struct Point { uint32_t at; float inlet,outlet,power; };
    Point points[301]{};
    unsigned head=0,count=0;
    uint32_t revision=0,settingsRevision=0,boot=0,start=0,last=0,candidateAt=0,modeAt=0;
    float filteredIn=0,filteredOut=0;
    Circulation mode=Circulation::Unknown,candidate=Circulation::Unknown;
    bool initialized=false,peakTracking=false,redistributing=false,mixingForecastValid=false;
    float peakOutlet=0,peakInlet=0;
    uint32_t peakAt=0;
    enum class Trend { Steady, Warming, Cooling };
    Trend thermalTrend=Trend::Steady;
    StovePhase stove=StovePhase::Unknown,stoveCandidate=StovePhase::Unknown;
    bool stoveInitialized=false;
    float slowInlet=0,slowOutlet=0;
    uint32_t stoveStart=0,stoveLast=0,stoveCandidateAt=0;
    void reset();
    float slope(float Point::*field,uint32_t now,float window_s) const;
    void update_stove(AnalysisResult &r,const AnalysisConfig &c,uint32_t now);
};
const char *circulation_name(Circulation state);
const char *stove_phase_name(StovePhase state);
}
