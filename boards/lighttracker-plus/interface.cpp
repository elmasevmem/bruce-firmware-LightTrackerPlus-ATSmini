// QRP Labs LightTracker Plus v1.0 - board glue for Bruce
//
// Display: SSD1306 128x64 I2C OLED (LovyanGFX, configured in lighttracker-plus.ini)
// Input:   single BOOT button (GPIO0) through the shared HAL (HAS_1_BUTTON)
// LoRa:    SX1268 (EBYTE E22-400M30S) on the SX1262 driver, RF switch + TCXO from the .ini
// GPS:     Quectel L80, UART RX 18 / TX 17, power through a PNP switch on GPIO33 (LOW = on)

#include "core/display.h"
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
    // The .ini declares the screen as 64x128 so Bruce lays out a 128x64 landscape UI from the
    // start (its convention for panels that are portrait natively). The SSD1306 is natively
    // 128x64, so give LovyanGFX the real geometry before tft.init(); TFT_ROTATION in the .ini
    // turns Bruce's landscape rotation into the panel's own orientation.
    displayConfig.width = 128;
    displayConfig.height = 64;
    displayConfig.memWidth = 128;
    displayConfig.memHeight = 64;

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

/***************************************************************************************
** Function name: _post_setup_gpio()
** Bruce only uses landscape rotations (1 and 3) here. Firmware before v0.7 used rotation 0,
** which a saved config may still hold: move it to the matching landscape rotation.
***************************************************************************************/
void _post_setup_gpio() {
    if ((bruceConfigPins.rotation & 1) == 0) {
        bruceConfigPins.setRotation(bruceConfigPins.rotation == 2 ? 1 : 3);
        tft.setRotation(bruceConfigPins.rotation);
        tftWidth = tft.width();
        tftHeight = tft.height();
        tft.fillScreen(bruceConfig.bgColor);
    }
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
