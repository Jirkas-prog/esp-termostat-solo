#include "app.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"

static uint8_t mode=None;
static bool error=false,known=false,applied=true,target=true,requested=true,everPulsed=false;
static uint32_t lastPulse=0;
static esp_timer_handle_t cutoff;
static portMUX_TYPE relayMux=portMUX_INITIALIZER_UNLOCKED;
static volatile bool pulsing=false,completed=false;
static void outputs_off() { gpio_set_level(gpio_num_t(Relay1),0); gpio_set_level(gpio_num_t(Relay2),0); }
static void stop_pulse(void *) {
    portENTER_CRITICAL(&relayMux); outputs_off(); pulsing=false; completed=true; portEXIT_CRITICAL(&relayMux);
}
void relay_early_init() {
    outputs_off(); gpio_set_level(gpio_num_t(WatchdogPin),0);
    gpio_config_t cfg{}; cfg.mode=GPIO_MODE_OUTPUT;
    cfg.pin_bit_mask=(1ULL<<Relay1)|(1ULL<<Relay2)|(1ULL<<WatchdogPin);
    ESP_ERROR_CHECK(gpio_config(&cfg)); outputs_off();
}
void relay_start(uint8_t selected) {
    mode=selected;
    esp_timer_create_args_t timer{}; timer.callback=stop_pulse; timer.name="coil_cutoff";
    ESP_ERROR_CHECK(esp_timer_create(&timer,&cutoff));
}
bool relay_fault() { return error; }
bool relay_command_sent() { return mode!=None && known && applied==requested && !pulsing && !error; }
void relay_tick(bool on,uint16_t pulse_ms,uint32_t now) {
    requested=on;
    if(mode!=Latching) {
        uint8_t drive=steady_drive(mode,on);
        gpio_set_level(gpio_num_t(Relay1),drive&1);
        gpio_set_level(gpio_num_t(Relay2),(drive>>1)&1);
        applied=on; known=mode!=None; return;
    }
    portENTER_CRITICAL(&relayMux); bool done=completed,active=pulsing; completed=false; portEXIT_CRITICAL(&relayMux);
    if(done) { applied=target; known=true; }
    if(active || error || (known && applied==on)) return;
    if(everPulsed && age(now,lastPulse)<2100) return;
    // Both bridge inputs LOW before reversing; only one can become HIGH.
    // nSLEEP is held HIGH in hardware, not controlled through the six-pin cable.
    target=on;
    portENTER_CRITICAL(&relayMux);
    outputs_off(); pulsing=true;
    gpio_set_level(gpio_num_t(target?Relay1:Relay2),1);
    portEXIT_CRITICAL(&relayMux);
    if(esp_timer_start_once(cutoff,uint64_t(pulse_ms)*1000)!=ESP_OK) { stop_pulse(nullptr); error=true; known=false; }
    lastPulse=now; everPulsed=true;
}
