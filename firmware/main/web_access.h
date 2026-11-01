#pragma once
struct cJSON;
bool web_access_init();
bool web_access_ready();
bool web_access_check(const char *session,bool touch=false);
cJSON *web_access_status(const char *session);
int web_access_login(const char *pin,cJSON **result);
int web_access_show();
void web_access_logout(const char *session);
bool web_display_pin(char (&out)[5]);

bool web_mqtt_pin(char (&out)[5]); // Explicitly requested MQTT-only disclosure, never HTTP.
