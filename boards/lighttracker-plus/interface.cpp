// QRP Labs LightTracker Plus v1.0 - board glue for Bruce
//
// Display: SSD1306 128x64 I2C OLED (LovyanGFX, configured in lighttracker-plus.ini)
// Input:   single BOOT button (GPIO0) through the shared HAL (HAS_1_BUTTON)
// LoRa:    SX1268 (EBYTE E22-400M30S) on the SX1262 driver, RF switch + TCXO from the .ini
// GPS:     Quectel L80, UART RX 18 / TX 17, power through a PNP switch on GPIO33 (LOW = on)

#include "core/powerSave.h"
#include "hal/device.h"
#include "hal/inputs/buttons.h"
#include <globals.h>
#include <interface.h>

#define BOOT_BTN 0
#define USER_LED 16

static DeviceButtons buttonsCfg() {
    DeviceButtons cfg;
    cfg.btn1 = BOOT_BTN;
    cfg.pullup = true;
    cfg.activeHigh = false;
    return cfg;
}

static void loraPower(bool on) {
    pinMode(LORA_POWER_CTRL, OUTPUT);
    digitalWrite(LORA_POWER_CTRL, on ? HIGH : LOW);
}

// Called by the LoRa module right before the radio is started
bool prepareBoardLoRaRadio() {
    loraPower(true);
    delay(10);
    return true;
}

/***************************************************************************************
** Function name: _setup_gpio()
***************************************************************************************/
void _setup_gpio() {
    // -- Runtime-configurable defaults (Config > Set Device pins) --
#if !defined(LITE_VERSION)
    bruceConfigPins.LoRa_bus = {
        (gpio_num_t)12, (gpio_num_t)13, (gpio_num_t)11, (gpio_num_t)10, (gpio_num_t)9, (gpio_num_t)5
    }; // sck, miso, mosi, cs, rst, dio1
#endif
    bruceConfigPins.gps_bus = {(gpio_num_t)18, (gpio_num_t)17}; // rx, tx (ESP32 side)
    bruceConfigPins.gpsBaudrate = 9600;
    bruceConfigPins.uart_bus = {(gpio_num_t)44, (gpio_num_t)43}; // rx, tx (UART header)
    bruceConfigPins.irTx = 7;
    bruceConfigPins.irRx = 8;
    bruceConfigPins.rfTx = 7;
    bruceConfigPins.rfRx = 8;
    // GPIO3/4 belong to the OLED driver, so the Bruce I2C buses stay unassigned.
    bruceConfigPins.rotation = ROTATION;

    // -- Power rails --
    loraPower(true);
    pinMode(GPS_VCC_PIN, OUTPUT);
    digitalWrite(GPS_VCC_PIN, LOW); // PNP: LOW = GPS on

    // Keep the LoRa chip-select parked HIGH and the RF switch on the RX path
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    pinMode(LORA_RXEN, OUTPUT);
    digitalWrite(LORA_RXEN, HIGH);
    pinMode(LORA_TXEN, OUTPUT);
    digitalWrite(LORA_TXEN, LOW);

    pinMode(USER_LED, OUTPUT);
    digitalWrite(USER_LED, LOW);

    hal_buttons_init(buttonsCfg(), 1);
}

// OLED: no backlight to dim
void _setBrightness(uint8_t brightval) {}

/*********************************************************************
** Function: InputHandler
**********************************************************************/
void InputHandler(void) {
    // LED mirrors the button so presses are visible
    digitalWrite(USER_LED, digitalRead(BOOT_BTN) == LOW ? HIGH : LOW);
    hal_buttons_poll_1(buttonsCfg());
}

/*********************************************************************
** Function: powerOff
**********************************************************************/
void powerOff() {
    loraPower(false);
    digitalWrite(GPS_VCC_PIN, HIGH); // PNP: HIGH = GPS off
    digitalWrite(USER_LED, LOW);
    tft.fillScreen(TFT_BLACK);
    tft.sleep(true);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BTN, LOW);
    esp_deep_sleep_start();
}

void checkReboot() {}
