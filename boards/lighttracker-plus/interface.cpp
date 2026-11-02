#include "core/display.h"
#include "core/powerSave.h"
#include <interface.h>

// QRP Labs LightTracker Plus v1.0 — board glue for Bruce
// No display: Bruce boots straight into the WebUI; control is via WiFi/WebUI and USB Serial CLI.

#ifndef LT_BUTTON
#define LT_BUTTON 0
#endif
#ifndef LT_VBAT_ADC
#define LT_VBAT_ADC 1
#endif

/***************************************************************************************
** Function name: _setup_gpio()
** Location: main.cpp
** Description:   initial setup for the device
***************************************************************************************/
void _setup_gpio() {
    // Power up the E22-400M30S LoRa module
#ifdef LORA_POWER_CTRL
    pinMode(LORA_POWER_CTRL, OUTPUT);
    digitalWrite(LORA_POWER_CTRL, HIGH);
#endif
    // RF switch: default to RX path (RadioLib switches it during TX)
#if defined(LORA_RXEN) && defined(LORA_TXEN)
    pinMode(LORA_RXEN, OUTPUT);
    pinMode(LORA_TXEN, OUTPUT);
    digitalWrite(LORA_RXEN, HIGH);
    digitalWrite(LORA_TXEN, LOW);
#endif
    // Keep LoRa CS high so it does not disturb the bus
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);

    // GPS power
#ifdef GPS_VCC_PIN
    pinMode(GPS_VCC_PIN, OUTPUT);
    digitalWrite(GPS_VCC_PIN, HIGH);
#endif

    pinMode(LT_BUTTON, INPUT_PULLUP);
    analogReadResolution(12);
    analogSetPinAttenuation(LT_VBAT_ADC, ADC_11db);

    bruceConfig.startupApp = "WebUI";
}

/***************************************************************************************
** Function name: getBattery()
** location: display.cpp
** Description:   Delivers the battery value from 1-100
***************************************************************************************/
int getBattery() {
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(LT_VBAT_ADC);
    mv = (mv / 8) * 2; // 1:2 divider
    // Li-ion: 3.3V = 0%, 4.2V = 100%
    int percent = ((int)mv - 3300) * 100 / (4200 - 3300);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}

/***************************************************************************************
** Function name: isCharging()
***************************************************************************************/
bool isCharging() { return false; }

/*********************************************************************
** Function: setBrightness
**********************************************************************/
void _setBrightness(uint8_t brightval) {}

/*********************************************************************
** Function: InputHandler
** Single BOOT button (GPIO0) acts as "OK/Select"
**********************************************************************/
void InputHandler(void) {
    static unsigned long lastPress = 0;
    if (millis() - lastPress < 200) return;
    if (digitalRead(LT_BUTTON) == LOW) {
        lastPress = millis();
        if (!wakeUpScreen()) AnyKeyPress = true;
        else return;
        SelPress = true;
    }
}

/*********************************************************************
** Function: powerOff
**********************************************************************/
void powerOff() {
#ifdef LORA_POWER_CTRL
    digitalWrite(LORA_POWER_CTRL, LOW);
#endif
#ifdef GPS_VCC_PIN
    digitalWrite(GPS_VCC_PIN, LOW);
#endif
    esp_sleep_enable_ext0_wakeup((gpio_num_t)LT_BUTTON, LOW);
    esp_deep_sleep_start();
}

/*********************************************************************
** Function: checkReboot
**********************************************************************/
void checkReboot() {}
