#include "app.h"
#include "analysis.h"
#include "cJSON.h"
#include "nvs.h"
#include <cstring>
#include <cmath>
#include <limits>

static portMUX_TYPE analysisMux=portMUX_INITIALIZER_UNLOCKED;
static AnalysisConfig config;
static AnalysisResult result;
static bool configSaved=false;
static ThermalAnalysis model;

static bool persist(const AnalysisConfig &next) {
    AnalysisStored stored; stored.config=next; seal(stored);
    nvs_handle_t h;
    if(nvs_open("solo_analysis",NVS_READWRITE,&h)!=ESP_OK) return false;
    bool ok=nvs_set_blob(h,"model",&stored,sizeof(stored))==ESP_OK && nvs_commit(h)==ESP_OK;
    AnalysisStored read; size_t size=sizeof(read);
    ok=ok && nvs_get_blob(h,"model",&read,&size)==ESP_OK && size==sizeof(read) &&
        valid(read) && !memcmp(&read,&stored,sizeof(read));
    nvs_close(h); return ok;
}
static AnalysisConfig get_config(bool &saved) {
    portENTER_CRITICAL(&analysisMux); auto copy=config; saved=configSaved; portEXIT_CRITICAL(&analysisMux); return copy;
}
static void analysis_task(void *) {
    TickType_t wake=xTaskGetTickCount();
    for(;;) {
        bool saved; auto cfg=get_config(saved); auto state=snapshot();
        AnalysisResult next;
        if(saved) next=model.update(state,cfg,now_ms());
        else { next.quality="config_error"; next.revision=cfg.revision; next.at=now_ms(); next.boot=state.boot; }
        portENTER_CRITICAL(&analysisMux); result=next; portEXIT_CRITICAL(&analysisMux);
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(1000));
    }
}
void analysis_start() {
    nvs_handle_t h;
    if(nvs_open("solo_analysis",NVS_READWRITE,&h)==ESP_OK) {
        AnalysisStored stored; size_t size=sizeof(stored);
        auto error=nvs_get_blob(h,"model",&stored,&size); nvs_close(h);
        if(error==ESP_OK && size==sizeof(stored) && valid(stored)) { config=stored.config; configSaved=true; }
        else if(error==ESP_ERR_NVS_NOT_FOUND) configSaved=persist(config);
        // Corrupt/unknown formats are not silently overwritten. Saving valid settings repairs them.
    }
    configASSERT(xTaskCreate(analysis_task,"thermal_analysis",4096,nullptr,1,nullptr)==pdPASS);
}
cJSON *analysis_config_json() {
    bool saved; auto cfg=get_config(saved); auto doc=cJSON_CreateObject();
    cJSON_AddNumberToObject(doc,"schema",1); cJSON_AddStringToObject(doc,"model","thermal-v1");
    cJSON_AddNumberToObject(doc,"revision",cfg.revision); cJSON_AddBoolToObject(doc,"saved",saved);
    auto parameters=cJSON_AddObjectToObject(doc,"parameters"); auto fields=cJSON_AddArrayToObject(doc,"fields");
    auto decimal=[](float v) { return std::round(double(v)*1000000)/1000000; };
    for(size_t i=0;i<AnalysisFieldCount;++i) {
        const auto &f=AnalysisFields[i]; cJSON_AddNumberToObject(parameters,f.key,decimal(cfg.*(f.member)));
        auto field=cJSON_CreateObject(); cJSON_AddItemToArray(fields,field);
        cJSON_AddStringToObject(field,"key",f.key); cJSON_AddStringToObject(field,"label",f.label);
        cJSON_AddStringToObject(field,"group",f.group); cJSON_AddStringToObject(field,"unit",f.unit);
        cJSON_AddNumberToObject(field,"min",decimal(f.minimum)); cJSON_AddNumberToObject(field,"max",decimal(f.maximum));
        cJSON_AddNumberToObject(field,"step",decimal(f.step)); cJSON_AddNumberToObject(field,"default",decimal(f.initial));
    }
    return doc;
}
int analysis_save_json(const cJSON *doc) {
    bool saved; auto next=get_config(saved);
    if(!cJSON_IsObject(doc) || cJSON_GetArraySize(doc)!=3) return 400;
    const auto schema=cJSON_GetObjectItemCaseSensitive(doc,"schema");
    const auto revision=cJSON_GetObjectItemCaseSensitive(doc,"revision");
    const auto parameters=cJSON_GetObjectItemCaseSensitive(doc,"parameters");
    if(!cJSON_IsNumber(schema) || schema->valuedouble!=1 || !cJSON_IsNumber(revision)) return 400;
    if(revision->valuedouble!=next.revision) return 409;
    if(!cJSON_IsObject(parameters) || cJSON_GetArraySize(parameters)!=int(AnalysisFieldCount)) return 400;
    // Full, strict parameter set: no duplicate, unknown, missing, string, NaN or infinite values.
    for(size_t i=0;i<AnalysisFieldCount;++i) {
        const auto &f=AnalysisFields[i]; auto item=cJSON_GetObjectItemCaseSensitive(parameters,f.key);
        if(!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || std::fabs(item->valuedouble)>std::numeric_limits<float>::max()) return 400;
        next.*(f.member)=float(item->valuedouble);
    }
    if(!valid(next)) return 400;
    ++next.revision; if(!next.revision) next.revision=1;
    if(!persist(next)) return 500;
    portENTER_CRITICAL(&analysisMux); config=next; configSaved=true;
    result=AnalysisResult{}; result.revision=next.revision; portEXIT_CRITICAL(&analysisMux);
    return 200;
}
cJSON *analysis_json(const State &state) {
    portENTER_CRITICAL(&analysisMux); auto r=result; portEXIT_CRITICAL(&analysisMux);
    if(r.boot!=state.boot || age(now_ms(),r.at)>3000 || !state.values.valid[0] || !state.values.valid[1]) {
        uint32_t rev=r.revision; r=AnalysisResult{}; r.revision=rev; r.quality="stale";
    }
    auto doc=cJSON_CreateObject();
    cJSON_AddNumberToObject(doc,"schema",1); cJSON_AddStringToObject(doc,"model","thermal-v1");
    cJSON_AddNumberToObject(doc,"config_revision",r.revision);
    cJSON_AddNumberToObject(doc,"source_boot",r.boot); cJSON_AddNumberToObject(doc,"source_sequence",r.sequence);
    cJSON_AddNumberToObject(doc,"valid_for_s",15);
    cJSON_AddStringToObject(doc,"quality",r.quality); cJSON_AddStringToObject(doc,"thermal_state",r.thermal);
    cJSON_AddStringToObject(doc,"stove_state",stove_phase_name(r.stove));
    cJSON_AddNumberToObject(doc,"stove_history_s",r.stove_history_s);
    cJSON_AddStringToObject(doc,"circulation",circulation_name(r.circulation));
    cJSON_AddNullToObject(doc,"pump_running"); // motor operation has no measured input yet
    cJSON_AddStringToObject(doc,"score_kind","logistic_not_verified_probability");
    cJSON_AddStringToObject(doc,"model_status",r.model); cJSON_AddStringToObject(doc,"power_trend",r.power_trend);
    cJSON_AddStringToObject(doc,"advice",r.advice); cJSON_AddBoolToObject(doc,"reference_exceeded",r.reference_exceeded);
    auto number=[&](const char *key,float value) {
        if(std::isfinite(value)) cJSON_AddNumberToObject(doc,key,std::round(value*10000.0)/10000.0);
        else cJSON_AddNullToObject(doc,key);
    };
    number("inlet_filtered_c",r.inlet); number("outlet_filtered_c",r.outlet); number("delta_filtered_c",r.delta);
    number("inlet_rate_c_min",r.inlet_rate); number("outlet_rate_c_min",r.outlet_rate);
    number("circulation_score",r.flow_score); number("heat_rate_c_min",r.heat_rate);
    number("relative_output_pct",r.relative_pct); number("source_to_water_kw_est",r.source_kw);
    number("water_export_kw_est",r.export_kw); number("output_rate_pct_min",r.power_rate);
    number("useful_heat_eta_min",r.refill_eta_min); number("high_temp_eta_min",r.limit_eta_min);
    number("long_inlet_rate_c_min",r.long_inlet_rate); number("long_outlet_rate_c_min",r.long_outlet_rate);
    number("mixing_minimum_c",r.mixing_minimum); number("mixing_eta_min",r.mixing_eta_min);
    return doc;
}
