#include "app.h"
#include "web_access.h"
#include "access.h"
#include "nvs.h"
#include "esp_random.h"
#include "cJSON.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <mutex>
static solo::AccessRate rate;
static solo::AccessSessions sessions;
static std::atomic<bool> ready{false};
static std::atomic<uint32_t> pinUntil{0};
static uint64_t nextDisplay=0;
static solo::AccessPin currentPin;
static std::mutex pinMutex;
struct Record { uint32_t magic=0x50494e31; uint16_t pin=0; uint8_t failures=0,format=1; uint32_t crc=0; };
static Record record;
static uint64_t clock_ms() { return uint64_t(esp_timer_get_time()/1000); }
static bool persist(Record next) {
    next.crc=crc32(&next,offsetof(Record,crc)); nvs_handle_t h;
    if(nvs_open("solo_access",NVS_READWRITE,&h)!=ESP_OK) return false;
    bool ok=nvs_set_blob(h,"access",&next,sizeof(next))==ESP_OK && nvs_commit(h)==ESP_OK;
    Record check; size_t size=sizeof(check);
    ok=ok && nvs_get_blob(h,"access",&check,&size)==ESP_OK && size==sizeof(check) && !memcmp(&check,&next,sizeof(next));
    nvs_close(h); if(ok) record=next; return ok;
}
bool web_access_init() {
    nvs_handle_t h; if(nvs_open("solo_access",NVS_READWRITE,&h)!=ESP_OK) return false;
    size_t size=sizeof(record); esp_err_t result=nvs_get_blob(h,"access",&record,&size); nvs_close(h);
    if(result==ESP_ERR_NVS_NOT_FOUND) { record=Record{}; record.pin=uint16_t(esp_random()%10000); if(!persist(record)) return false; }
    else if(result!=ESP_OK || size!=sizeof(record) || record.magic!=0x50494e31 || record.format!=1 || record.pin>9999 || record.failures>10 || record.crc!=crc32(&record,offsetof(Record,crc))) return false;
    // Fresh on boot: rebooting cannot extend the lifetime of the previous PIN.
    currentPin.value=record.pin; currentPin.rotate(esp_random(),clock_ms());
    rate.boot(record.failures,clock_ms()); ready=true; return true;
}
static void pin_text(char (&out)[5]) {
    const auto now=clock_ms();
    if(currentPin.expired(now)) currentPin.rotate(esp_random(),now);
    snprintf(out,sizeof(out),"%04u",unsigned(currentPin.value)%10000u);
}
bool web_mqtt_pin(char (&out)[5]) {
    std::lock_guard<std::mutex> lock(pinMutex);
    if(!ready) return false;
    pin_text(out); return true;
}
bool web_access_ready() { return ready; }
bool web_access_check(const char *session,bool touch) { return ready && sessions.check(session,clock_ms(),touch); }
cJSON *web_access_status(const char *session) {
    auto doc=cJSON_CreateObject(); const auto now=clock_ms();
    cJSON_AddBoolToObject(doc,"ready",ready); cJSON_AddBoolToObject(doc,"authenticated",ready && sessions.check(session,now));
    cJSON_AddNumberToObject(doc,"expires_in_s",ready?sessions.remaining(session,now):0);
    cJSON_AddNumberToObject(doc,"retry_after_s",rate.wait(now));
    auto until=pinUntil.load(); cJSON_AddBoolToObject(doc,"pin_display_active",until && int32_t(until-now_ms())>0);
    cJSON_AddNumberToObject(doc,"display_retry_s",now<nextDisplay?(nextDisplay-now+999)/1000:0);
    return doc;
}
int web_access_login(const char *pin,cJSON **result) {
    std::lock_guard<std::mutex> lock(pinMutex);
    if(!ready) return 503;
    const auto now=clock_ms(); if(rate.wait(now)) return 429;
    char pinText[5]; pin_text(pinText);
    unsigned difference=0; bool correct=pin && strlen(pin)==4;
    if(correct) for(unsigned i=0;i<4;++i) difference|=unsigned(pin[i]^pinText[i]);
    correct=correct && !difference;
    auto nextRate=rate; nextRate.result(correct,now); auto next=record; next.pin=currentPin.value; next.failures=uint8_t(nextRate.failures);
    if(!persist(next)) { ready=false; return 503; }
    rate=nextRate;
    if(!correct) return 401;
    char session[33]; snprintf(session,sizeof(session),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
    sessions.add(session,now); pinUntil=0;
    *result=web_access_status(session); cJSON_AddStringToObject(*result,"session",session); return 200;
}
int web_access_show() {
    std::lock_guard<std::mutex> lock(pinMutex);
    if(!ready || !display_ready()) return 503;
    const auto now=clock_ms(); if(now<nextDisplay) return 429;
    currentPin.rotate(esp_random(),now);
    nextDisplay=now+60000; auto until=now_ms()+60000; pinUntil=until?until:1; return 200;
}
void web_access_logout(const char *session) { sessions.remove(session); }
bool web_display_pin(char (&out)[5]) {
    uint32_t until=pinUntil.load(); if(!until) return false;
    if(int32_t(until-now_ms())<=0) { pinUntil.compare_exchange_strong(until,0); return false; }
    return web_mqtt_pin(out);
}
