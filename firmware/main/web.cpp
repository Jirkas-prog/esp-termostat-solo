#include "app.h"
#include "network_settings.h"
#include "web_access.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <strings.h>
#include <initializer_list>

extern const char html_start[] asm("_binary_index_html_start");
extern const char html_end[] asm("_binary_index_html_end");
extern const char logo_start[] asm("_binary_logo_svg_start");
extern const char logo_end[] asm("_binary_logo_svg_end");
static char token[33];
static esp_timer_handle_t restartTimer;
static esp_timer_handle_t resumeTimer;
static std::atomic<bool> prepared{false};
static uint32_t preparedAt=0;
static std::atomic<bool> webReady{false};
static std::atomic<bool> otaReady{false};
static bool restarting=false;

static bool pending_verification() {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(),&state)==ESP_OK && state==ESP_OTA_IMG_PENDING_VERIFY;
}
static void verify_boot(void *) {
    esp_ota_img_states_t state;
    const auto *running=esp_ota_get_running_partition();
    bool initial=esp_ota_get_state_partition(running,&state)==ESP_ERR_NOT_FOUND;
    if(initial || pending_verification()) {
        for(unsigned seconds=0;seconds<60;++seconds) {
            auto s=snapshot();
            if(s.uptime_ms>=10000 && s.saved && web_access_ready() && network_settings_saved() && (s.link || network_info().ap) && webReady && display_ready() &&
               sensor_diagnostics(0).attempts>=3 && sensor_diagnostics(1).attempts>=3) {
                // Seed the initial USB-installed slot as a valid rollback target.
                if(initial) ESP_ERROR_CHECK(esp_ota_set_boot_partition(running));
                ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
                otaReady=true;
                ESP_LOGI("ota","New application confirmed after startup checks"); vTaskDelete(nullptr); return;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGE("ota","Startup checks failed; rolling back");
        if(!initial) esp_ota_mark_app_invalid_rollback_and_reboot();
    } else otaReady=true;
    vTaskDelete(nullptr);
}
void ota_verify_start() { configASSERT(xTaskCreate(verify_boot,"ota_verify",4096,nullptr,2,nullptr)==pdPASS); }

static esp_err_t reply(httpd_req_t *r,const char *status,const char *json) {
    httpd_resp_set_status(r,status); httpd_resp_set_type(r,"application/json; charset=utf-8");
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    // Do not drain an untrusted/rejected upload or keep its socket occupied.
    bool close=r->method==HTTP_POST && status[0]!='2';
    if(close) httpd_resp_set_hdr(r,"Connection","close");
    auto result=httpd_resp_send(r,json,HTTPD_RESP_USE_STRLEN);
    return close?ESP_FAIL:result;
}
static esp_err_t json_reply(httpd_req_t *r,cJSON *doc) {
    char *text=doc?cJSON_PrintUnformatted(doc):nullptr; cJSON_Delete(doc);
    if(!text) return reply(r,"500 Internal Server Error","{\"error\":\"Nedostatek pameti.\"}");
    auto result=reply(r,"200 OK",text); cJSON_free(text); return result;
}
static bool allowed_host(httpd_req_t *r) {
    // Reject DNS rebinding. No external site may turn its origin into this device.
    char host[80]{};
    if(httpd_req_get_hdr_value_str(r,"Host",host,sizeof(host))!=ESP_OK) return false;
    if(char *port=strchr(host,':')) { if(strcmp(port,":80")) return false; *port=0; }
    char local[40]; snprintf(local,sizeof(local),"%s.local",network_hostname());
    auto net=network_info();
    return !strcasecmp(host,network_hostname()) || !strcasecmp(host,local) ||
        (*net.station_ip && !strcmp(host,net.station_ip)) || (*net.ap_ip && !strcmp(host,net.ap_ip));
}
static bool authorize(httpd_req_t *r) {
    char supplied[33]{};
    return allowed_host(r) && httpd_req_get_hdr_value_str(r,"X-Thermostat-Token",supplied,sizeof(supplied))==ESP_OK && !strcmp(supplied,token);
}
static cJSON *body(httpd_req_t *r) {
    char data[6144];
    if(r->content_len<2 || r->content_len>=sizeof(data)) return nullptr;
    size_t done=0;
    while(done<r->content_len) {
        int n=httpd_req_recv(r,data+done,r->content_len-done);
        if(n<=0) return nullptr;
        done+=n;
    }
    if(memchr(data,0,done)) return nullptr;
    data[done]=0; return cJSON_ParseWithOpts(data,nullptr,true);
}
static void session_token(httpd_req_t *r,char (&session)[33]) {
    if(httpd_req_get_hdr_value_str(r,"X-Thermostat-Session",session,sizeof(session))!=ESP_OK) session[0]=0;
}
static bool authenticated(httpd_req_t *r,bool touch=false) { char session[33]{}; session_token(r,session); return web_access_check(session,touch); }
static cJSON *integrity() {
    auto result=cJSON_CreateObject();
    for(const char *name:{"network","analysis"}) {
        auto doc=!strcmp(name,"network")?network_config_json():analysis_config_json();
        if(!strcmp(name,"analysis")) cJSON_DeleteItemFromObject(doc,"fields");
        char *serialized=doc?cJSON_PrintUnformatted(doc):nullptr;
        if(serialized) {
            unsigned char digest[32]; char hex[65];
            mbedtls_sha256(reinterpret_cast<const unsigned char *>(serialized),strlen(serialized),digest,0);
            for(unsigned i=0;i<32;++i) snprintf(hex+2*i,3,"%02x",digest[i]);
            cJSON_AddStringToObject(result,name,hex); cJSON_free(serialized);
        }
        cJSON_Delete(doc);
    }
    return result;
}
static esp_err_t get(httpd_req_t *r) {
    if(!allowed_host(r)) return reply(r,"403 Forbidden","{\"error\":\"Neplatna adresa zarizeni.\"}");
    if(!strcmp(r->uri,"/api/auth")) { char session[33]{}; session_token(r,session); return json_reply(r,web_access_status(session)); }
    if((!strcmp(r->uri,"/api/network") || !strcmp(r->uri,"/api/analysis")) && !authenticated(r)) return reply(r,"401 Unauthorized","{\"error\":\"Odemknete nastaveni PINem.\"}");
    if(!strcmp(r->uri,"/api/state")) {
        char *text=snapshot_json(snapshot());
        if(!text) return reply(r,"503 Service Unavailable","{\"error\":\"Stav neni dostupny.\"}");
        auto result=reply(r,"200 OK",text); cJSON_free(text); return result;
    }
    if(!strcmp(r->uri,"/api/analysis")) return json_reply(r,analysis_config_json());
    if(!strcmp(r->uri,"/api/network")) return json_reply(r,network_config_json());
    if(!strcmp(r->uri,"/api/info")) {
        cJSON *doc=cJSON_CreateObject();
        cJSON_AddStringToObject(doc,"hostname",network_hostname());
        cJSON_AddStringToObject(doc,"topic",network_topic());
        cJSON_AddStringToObject(doc,"token",token);
        cJSON_AddBoolToObject(doc,"auth_required",true);
        cJSON_AddItemToObject(doc,"integrity",integrity());
        cJSON_AddNumberToObject(doc,"free_heap",esp_get_free_heap_size());
        auto ota=cJSON_AddObjectToObject(doc,"ota"); auto next=esp_ota_get_next_update_partition(nullptr);
        cJSON_AddBoolToObject(ota,"supported",next!=nullptr && otaReady.load());
        cJSON_AddNumberToObject(ota,"max_size",next?next->size:0);
        cJSON_AddStringToObject(ota,"slot",esp_ota_get_running_partition()->label);
        cJSON_AddBoolToObject(ota,"pending_verify",pending_verification());
        cJSON_AddBoolToObject(ota,"prepare_required",true);
        cJSON_AddBoolToObject(ota,"rollback_available",esp_ota_check_rollback_is_possible());
        auto app=esp_app_get_description(); char sha[65];
        for(unsigned i=0;i<32;++i) snprintf(sha+2*i,3,"%02x",app->app_elf_sha256[i]);
        cJSON_AddStringToObject(ota,"elf_sha256",sha);
        cJSON_AddStringToObject(ota,"version",app->version);
        return json_reply(r,doc);
    }
    bool logo=!strcmp(r->uri,"/logo.svg");
    httpd_resp_set_type(r,logo?"image/svg+xml":"text/html; charset=utf-8");
    httpd_resp_set_hdr(r,"Cache-Control",logo?"no-cache":"no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
    return httpd_resp_send(r,logo?logo_start:html_start,logo?logo_end-logo_start-1:html_end-html_start-1);
}
static esp_err_t update(httpd_req_t *r) {
    if(!prepared.exchange(false) || age(now_ms(),preparedAt)>10000)
        return reply(r,"409 Conflict","{\"error\":\"Nejprve pripravte OTA pomoci /api/update/prepare.\"}");
    struct ResumeSensors { ~ResumeSensors() { esp_timer_stop(resumeTimer); sensors_resume(); } } resume;
    const auto *partition=esp_ota_get_next_update_partition(nullptr);
    if(!partition || !otaReady.load() || pending_verification() || !snapshot().saved)
        return reply(r,"409 Conflict","{\"error\":\"OTA neni pripravene; vyckejte na potvrzeni startu a ulozeni nastaveni.\"}");
    char type[40]{};
    if(httpd_req_get_hdr_value_str(r,"Content-Type",type,sizeof(type))!=ESP_OK || strcmp(type,"application/octet-stream"))
        return reply(r,"415 Unsupported Media Type","{\"error\":\"Ocekavan samotny aplikacni BIN.\"}");
    constexpr size_t prefix=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t);
    if(r->content_len<prefix || r->content_len>partition->size)
        return reply(r,"400 Bad Request","{\"error\":\"Neplatna velikost firmwaru.\"}");
    uint8_t buffer[4096]; size_t received=0;
    while(received<prefix) {
        int n=httpd_req_recv(r,reinterpret_cast<char *>(buffer)+received,prefix-received);
        if(n<=0) return reply(r,"400 Bad Request","{\"error\":\"Preruseny prenos hlavicky.\"}");
        received+=n;
    }
    esp_image_header_t header; esp_app_desc_t app;
    memcpy(&header,buffer,sizeof(header)); memcpy(&app,buffer+sizeof(header)+sizeof(esp_image_segment_header_t),sizeof(app));
    unsigned major=0,minor=0,patch=0; char trailing=0;
    bool version=memchr(app.version,0,sizeof(app.version)) && sscanf(app.version,"%u.%u.%u%c",&major,&minor,&patch,&trailing)==3 && (major>0 || minor>5 || (minor==5 && patch>=3));
    if(header.magic!=ESP_IMAGE_HEADER_MAGIC || header.chip_id!=ESP_CHIP_ID_ESP32 || !header.hash_appended ||
       app.magic_word!=ESP_APP_DESC_MAGIC_WORD || memcmp(app.project_name,esp_app_get_description()->project_name,sizeof(app.project_name)) || !version)
        return reply(r,"400 Bad Request","{\"error\":\"Vyberte aplikaci ESP TERMOSTAT SOLO 0.5.3 nebo novejsi, nikoli factory obraz.\"}");
    ESP_LOGI("ota","Writing %u bytes to %s",unsigned(r->content_len),partition->label);
    esp_ota_handle_t handle;
    esp_err_t err=esp_ota_begin(partition,OTA_WITH_SEQUENTIAL_WRITES,&handle);
    if(err!=ESP_OK) return reply(r,"500 Internal Server Error","{\"error\":\"Nelze zahajit zapis OTA.\"}");
    err=esp_ota_write(handle,buffer,received); uint32_t start=now_ms();
    while(err==ESP_OK && received<r->content_len) {
        if(age(now_ms(),start)>120000) { err=ESP_ERR_TIMEOUT; break; }
        size_t count=r->content_len-received; if(count>sizeof(buffer)) count=sizeof(buffer);
        int n=httpd_req_recv(r,reinterpret_cast<char *>(buffer),count);
        if(n<=0) { err=ESP_FAIL; break; }
        err=esp_ota_write(handle,buffer,n); received+=n;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if(err!=ESP_OK) {
        esp_ota_abort(handle);
        return reply(r,"400 Bad Request","{\"error\":\"Prenos nebo zapis selhal; puvodni firmware zustava aktivni.\"}");
    }
    if(esp_ota_end(handle)!=ESP_OK)
        return reply(r,"400 Bad Request","{\"error\":\"Kontrola obrazu selhala; puvodni firmware zustava aktivni.\"}");
    if(esp_ota_set_boot_partition(partition)!=ESP_OK)
        return reply(r,"500 Internal Server Error","{\"error\":\"Nelze vybrat novy firmware pro start.\"}");
    ESP_LOGI("ota","Image validated; restarting into %s",partition->label);
    restarting=true;
    auto result=reply(r,"200 OK","{\"ok\":true,\"restarting\":true}");
    esp_timer_start_once(restartTimer,1500000); return result;
}
static esp_err_t post(httpd_req_t *r) {
    if(!authorize(r)) return reply(r,"403 Forbidden","{\"error\":\"Obnovte stranku a zkuste to znovu.\"}");
    if(!strncmp(r->uri,"/api/auth/",10)) {
        auto doc=body(r); char session[33]{}; session_token(r,session);
        if(!strcmp(r->uri,"/api/auth/login")) {
            auto p=cJSON_GetObjectItemCaseSensitive(doc,"pin");
            const char *pin=cJSON_IsObject(doc) && cJSON_GetArraySize(doc)==1 && cJSON_IsString(p)?p->valuestring:nullptr;
            cJSON *result=nullptr; int status=web_access_login(pin,&result); cJSON_Delete(doc);
            if(status==200) return json_reply(r,result);
            result=web_access_status("");
            cJSON_AddStringToObject(result,"error",status==429?"Dalsi pokus je zatim omezen.":status==401?"Nespravny PIN.":"Pristup nelze overit; zkontrolujte pamet zarizeni.");
            char *json=cJSON_PrintUnformatted(result); cJSON_Delete(result);
            auto response=reply(r,status==429?"429 Too Many Requests":status==401?"401 Unauthorized":"503 Service Unavailable",json?json:"{}"); cJSON_free(json); return response;
        }
        bool empty=cJSON_IsObject(doc) && cJSON_GetArraySize(doc)==0; cJSON_Delete(doc);
        if(!empty) return reply(r,"400 Bad Request","{\"error\":\"Ocekavan prazdny objekt.\"}");
        if(!strcmp(r->uri,"/api/auth/display")) {
            int status=web_access_show();
            if(status==200) return json_reply(r,web_access_status(session));
            return reply(r,status==429?"429 Too Many Requests":"503 Service Unavailable",status==429?"{\"error\":\"PIN lze vyzadat jednou za minutu.\"}":"{\"error\":\"Displej nebo pamet neni pripraven.\"}");
        }
        if(!strcmp(r->uri,"/api/auth/logout")) { web_access_logout(session); return json_reply(r,web_access_status("")); }
        if(!web_access_check(session,true)) return reply(r,"401 Unauthorized","{\"error\":\"Prihlaseni vyprselo.\"}");
        return json_reply(r,web_access_status(session));
    }
    if(!authenticated(r,true)) return reply(r,"401 Unauthorized","{\"error\":\"Odemknete nastaveni PINem.\"}");
    if(restarting) return reply(r,"409 Conflict","{\"error\":\"Zarizeni se restartuje.\"}");
    if(!strcmp(r->uri,"/api/update/prepare")) {
        if(r->content_len || !otaReady.load() || pending_verification() || !snapshot().saved)
            return reply(r,"409 Conflict","{\"error\":\"OTA neni pripravene nebo pozadavek neni prazdny.\"}");
        if(!sensors_pause_for_update())
            return reply(r,"503 Service Unavailable","{\"error\":\"Komunikaci se sondami se nepodarilo dokoncit.\"}");
        preparedAt=now_ms(); prepared=true;
        esp_timer_stop(resumeTimer); esp_timer_start_once(resumeTimer,150000000);
        ESP_LOGI("ota","Sensors quiescent; ready to receive firmware");
        return reply(r,"200 OK","{\"ready\":true}");
    }
    if(!strcmp(r->uri,"/api/update")) return update(r);
    if(prepared.load()) return reply(r,"409 Conflict","{\"error\":\"Prave je pripravena OTA aktualizace. Vyckejte na jeji dokonceni.\"}");
    cJSON *doc=body(r);
    if(!cJSON_IsObject(doc)) { cJSON_Delete(doc); return reply(r,"400 Bad Request","{\"error\":\"Neplatny nebo prilis dlouhy pozadavek.\"}"); }
    if(!strcmp(r->uri,"/api/hotspot-key")) {
        bool empty=cJSON_GetArraySize(doc)==0; cJSON_Delete(doc);
        if(!empty) return reply(r,"400 Bad Request","{\"error\":\"Ocekavan prazdny objekt.\"}");
        auto result=cJSON_CreateObject(); cJSON_AddStringToObject(result,"password",network_ap_password()); return json_reply(r,result);
    }
    if(!strcmp(r->uri,"/api/network")) {
        int status=network_save_json(doc); cJSON_Delete(doc);
        if(status==409) return reply(r,"409 Conflict","{\"error\":\"Sit se mezitim zmenila. Nactete aktualni nastaveni.\"}");
        if(status==500) return reply(r,"500 Internal Server Error","{\"error\":\"Sit se nepodarilo ulozit a overit.\"}");
        if(status!=200) return reply(r,"400 Bad Request","{\"error\":\"Neplatne nastaveni site. Zkontrolujte hesla, jedinecna SSID, rozsahy a MQTT temata. Nove SSID potrebuje heslo nebo vyber otevrene site.\"}");
        restarting=true; auto result=reply(r,"200 OK","{\"ok\":true,\"restarting\":true}");
        esp_timer_start_once(restartTimer,2500000); return result;
    }
    if(!strcmp(r->uri,"/api/analysis")) {
        int status=analysis_save_json(doc); cJSON_Delete(doc);
        if(status==200) return json_reply(r,analysis_config_json());
        if(status==409) return reply(r,"409 Conflict","{\"error\":\"Model se zmenil. Nactete aktualni parametry.\"}");
        if(status==500) return reply(r,"500 Internal Server Error","{\"error\":\"Parametry se nepodarilo ulozit a overit.\"}");
        return reply(r,"400 Bad Request","{\"error\":\"Neplatne parametry: zkontrolujte rozsahy, mezeru < okno trendu a kalibraci tepelneho modelu.\"}");
    }
    if(!strcmp(r->uri,"/api/settings")) {
        auto state=snapshot(); auto rev=cJSON_GetObjectItemCaseSensitive(doc,"revision");
        auto patch=cJSON_GetObjectItemCaseSensitive(doc,"settings");
        if(cJSON_GetArraySize(doc)!=2 || !cJSON_IsNumber(rev) || rev->valuedouble!=state.settings.revision) {
            cJSON_Delete(doc); return reply(r,"409 Conflict","{\"error\":\"Nastaveni se zmenilo. Nactete aktualni hodnoty.\"}");
        }
        Command c{}; c.type=CommandType::Settings; c.settings=state.settings;
        bool validPatch=settings_patch(patch,c.settings); cJSON_Delete(doc);
        if(!validPatch) return reply(r,"400 Bad Request","{\"error\":\"Neplatna kombinace hodnot. Vypnuti musi byt alespon 1 C pod zapnutim a havarijni mez alespon 5 C nad nim.\"}");
        if(!command(c)) return reply(r,"503 Service Unavailable","{\"error\":\"Zarizeni je zaneprazdnene.\"}");
        for(int attempt=0;attempt<50;++attempt) {
            vTaskDelay(pdMS_TO_TICKS(20)); auto actual=snapshot(); auto expected=c.settings;
            expected.revision=actual.settings.revision;
            if(expected.relay!=state.settings.relay) expected.commissioned=0;
            if(actual.saved && !memcmp(&expected,&actual.settings,sizeof(expected))) {
                char *text=snapshot_json(actual);
                if(!text) break;
                auto result=reply(r,"200 OK",text); cJSON_free(text); return result;
            }
            if(actual.settings.revision!=state.settings.revision && memcmp(&expected,&actual.settings,sizeof(expected))) break;
        }
        return reply(r,"409 Conflict","{\"error\":\"Ulozeni nebylo potvrzeno. Nactete aktualni stav.\"}");
    }
    if(!strcmp(r->uri,"/api/hostname")) {
        auto name=cJSON_GetObjectItemCaseSensitive(doc,"hostname");
        if(cJSON_GetArraySize(doc)!=1 || !cJSON_IsString(name) || !hostname_valid(name->valuestring)) {
            cJSON_Delete(doc); return reply(r,"400 Bad Request","{\"error\":\"Nazev: 1-32 malych pismen, cislic nebo pomlcek; pomlcka nesmi byt na kraji.\"}");
        }
        if(!network_hostname_save(name->valuestring)) { cJSON_Delete(doc); return reply(r,"500 Internal Server Error","{\"error\":\"Nazev se nepodarilo ulozit.\"}"); }
        cJSON *response=cJSON_CreateObject(); cJSON_AddBoolToObject(response,"ok",true);
        cJSON_AddStringToObject(response,"hostname",name->valuestring); cJSON_Delete(doc);
        cJSON_AddBoolToObject(response,"restarting",true);
        restarting=true; auto result=json_reply(r,response); esp_timer_start_once(restartTimer,1500000); return result;
    }
    auto action=cJSON_GetObjectItemCaseSensitive(doc,"action");
    bool manual=cJSON_GetArraySize(doc)==1 && cJSON_IsString(action) && !strcmp(action->valuestring,"manual"); cJSON_Delete(doc);
    if(!manual) return reply(r,"400 Bad Request","{\"error\":\"Neznamy prikaz.\"}");
    Command c{}; c.type=CommandType::Manual;
    if(!command(c)) return reply(r,"503 Service Unavailable","{\"error\":\"Zarizeni je zaneprazdnene.\"}");
    return reply(r,"200 OK","{\"ok\":true}");
}
bool web_start() {
    if(!web_access_init()) ESP_LOGE("access","PIN storage unavailable; settings locked");
    snprintf(token,sizeof(token),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
    esp_timer_create_args_t timer{}; timer.callback=[](void *) { esp_restart(); }; timer.name="web_restart";
    if(esp_timer_create(&timer,&restartTimer)!=ESP_OK) return false;
    timer.callback=[](void *) { prepared=false; sensors_resume(); }; timer.name="ota_timeout";
    if(esp_timer_create(&timer,&resumeTimer)!=ESP_OK) return false;
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG(); cfg.stack_size=16384; cfg.task_priority=3; cfg.max_uri_handlers=22;
    cfg.max_open_sockets=6; cfg.lru_purge_enable=true; cfg.recv_wait_timeout=3; cfg.send_wait_timeout=3;
    httpd_handle_t server=nullptr; if(httpd_start(&server,&cfg)!=ESP_OK) return false;
    for(const char *path:{"/","/logo.svg","/api/state","/api/info","/api/analysis","/api/network","/api/auth"}) {
        httpd_uri_t uri{}; uri.uri=path; uri.method=HTTP_GET; uri.handler=get;
        if(httpd_register_uri_handler(server,&uri)!=ESP_OK) { httpd_stop(server); return false; }
    }
    for(const char *path:{"/api/analysis","/api/settings","/api/hostname","/api/action","/api/update","/api/update/prepare","/api/network","/api/hotspot-key","/api/auth/login","/api/auth/display","/api/auth/activity","/api/auth/logout"}) {
        httpd_uri_t uri{}; uri.uri=path; uri.method=HTTP_POST; uri.handler=post;
        if(httpd_register_uri_handler(server,&uri)!=ESP_OK) { httpd_stop(server); return false; }
    }
    webReady=true; return true;
}
