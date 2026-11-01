#pragma once
#include "network_config.h"
void network_settings_init();
const solo::NetworkConfig &network_settings();
const char *network_device_id();
const char *network_ap_password();
void network_before_restart(const solo::NetworkConfig &next);

bool network_settings_saved();
