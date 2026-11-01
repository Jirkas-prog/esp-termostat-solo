#pragma once
#include "network_config.h"
namespace solo {
unsigned discovery_count();
bool discovery_enabled(const NetworkConfig &c,unsigned index);
bool discovery_topic(char *out,size_t size,const NetworkConfig &cfg,const char *device,unsigned index);
cJSON *discovery_payload(const NetworkConfig &cfg,const char *device,const char *hostname,const char *version,unsigned index);
}
