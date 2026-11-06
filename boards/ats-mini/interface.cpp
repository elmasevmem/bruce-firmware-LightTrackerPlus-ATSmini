// ATS Mini (ESP32-S3 + SI4732) board glue for Bruce
//
// Input:
//   encoder rotate      -> Prev / Next   (shared HAL, full-step decoding: 1 detent = 1 step)
//   encoder short push  -> Select        (fires on release)
//   encoder long push   -> Back / Esc    (600 ms)
//   BOOT button (GPIO0) -> Back / Esc    (only on units where it is fitted)
// Pushing and turning at the same time never counts as a Select.

#include "core/powerSave.h"
#include "hal/bright/bright.h"
#include "hal/device.h"
#include "hal/inputs/encoder.h"
#include <driver/rtc_io.h>
#include <globals.h>
#include <interface.h>

#define ENC_A 2
#define ENC_B 1
#define ENC_KEY 21
#define BOOT_BTN 0

#define LONG_PRESS_MS 600
#define DEBOUNCE_MS 30

// The encoder push button is handled below (short = Sel, long = Esc), so the HAL only gets
// the rotation and the BOOT button.
static DeviceEncoder encoderCfg() {
    DeviceEncoder cfg;
    cfg.pin_a = ENC_A;
    cfg.pin_b = ENC_B;
    cfg.pin_sel = -1;
    cfg.pin_esc = BOOT_BTN;
    cfg.pullup = true;
    return cfg;
}

/***************************************************************************************
** Function name: _setup_gpio()
***************************************************************************************/
void _setup_gpio() {
    // powerOff() holds these pads through deep sleep, release them after wake-up
    gpio_hold_dis((gpio_num_t)PIN_POWER_ON);
    gpio_hold_dis((gpio_num_t)PIN_AMP_EN);
    gpio_hold_dis((gpio_num_t)TFT_BL);
    gpio_deep_sleep_hold_dis();

    // Receiver LDO on, same as the stock ATS Mini firmware does at boot
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);

    // Receiver audio path stays off until the FM radio app opens it
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW);
    pinMode(AUDIO_MUTE, OUTPUT);
    digitalWrite(AUDIO_MUTE, HIGH);

    // Spare GPIOs (IO11-IO14) as defaults for external IR / one-pin RF modules
    bruceConfigPins.irTx = 11;
    bruceConfigPins.irRx = 12;
    bruceConfigPins.rfTx = 13;
    bruceConfigPins.rfRx = 14;
    bruceConfigPins.rotation = ROTATION;

    pinMode(ENC_KEY, INPUT_PULLUP);
    // ATS Mini encoder: one full quadrature cycle per detent, resting with A and B high.
    // The stock firmware decodes it full-step too. TWO03 here made one click move two
    // menu items and a light touch move one.
    hal_encoder_init(encoderCfg(), EncoderLatchMode::FOUR3);
}

/***************************************************************************************
** Function name: _post_setup_gpio()
***************************************************************************************/
void _post_setup_gpio() {
    hal_bright_attach(TFT_BL);
    hal_bright_set(TFT_BL, 100);
}

void _setBrightness(uint8_t brightval) { hal_bright_set(TFT_BL, brightval); }

/*********************************************************************
** Function: InputHandler
**********************************************************************/
void InputHandler(void) {
    static bool keyDown = false;       // encoder push button currently held
    static bool longFired = false;     // Esc already sent for the current hold
    static bool ignoreRelease = false; // button was used for push-and-turn
    static unsigned long keyDownAt = 0;

    // Rotation (Prev/Next) and BOOT (Esc). The input task clears the globals before each
    // call, so NextPress/PrevPress here mean "the encoder moved during this pass".
    hal_encoder_poll(encoderCfg());
    bool turned = NextPress || PrevPress;

    unsigned long now = millis();
    bool pushed = digitalRead(ENC_KEY) == LOW;
    bool selEvent = false;
    bool escEvent = false;

    if (pushed && !keyDown) { // pressed
        keyDown = true;
        longFired = false;
        ignoreRelease = false;
        keyDownAt = now;
    } else if (pushed && keyDown) { // held
        if (turned) ignoreRelease = true;
        if (!longFired && !ignoreRelease && now - keyDownAt >= LONG_PRESS_MS) {
            longFired = true;
            escEvent = true;
        }
    } else if (!pushed && keyDown) { // released
        keyDown = false;
        if (!longFired && !ignoreRelease && now - keyDownAt >= DEBOUNCE_MS) selEvent = true;
    }

    if (selEvent || escEvent) {
        if (wakeUpScreen()) return;
        AnyKeyPress = true;
        if (selEvent) SelPress = true;
        if (escEvent) EscPress = true;
    }
}

/*********************************************************************
** Function: powerOff
** Deep sleep with the receiver LDO, amplifier and backlight kept off.
** Wake-up: push the encoder button.
**********************************************************************/
void powerOff() {
    tft.fillScreen(bruceConfig.bgColor);
    hal_bright_set(TFT_BL, 0);
    ledcDetach(TFT_BL);
    tft.writecommand(0x10); // ST7789 SLPIN
    delay(120);

    const int offPins[] = {TFT_BL, PIN_AMP_EN, PIN_POWER_ON};
    for (int pin : offPins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        gpio_hold_en((gpio_num_t)pin);
    }
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_ext0_wakeup((gpio_num_t)ENC_KEY, LOW);
    rtc_gpio_pullup_en((gpio_num_t)ENC_KEY);
    rtc_gpio_pulldown_dis((gpio_num_t)ENC_KEY);
    esp_deep_sleep_start();
}

void checkReboot() {}
