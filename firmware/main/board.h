#pragma once
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_ESP32
#error SOLO requires the original LILYGO TTGO T-Display ESP32 (not S3/C3/C6)
#endif
constexpr int SensorPins[2]={27,26}; // inlet, outlet: two independent 1-Wire buses
constexpr int Relay1=25, Relay2=33, WatchdogPin=13;
constexpr int KeyPins[4]={32,17,36,39}; // TTP224: lock, minus, plus, save
constexpr char BoardName[]="LILYGO TTGO T-Display (ESP32, ST7789 135x240)";
