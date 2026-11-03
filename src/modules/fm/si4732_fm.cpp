/**
 * Bruce Firmware Native C++ Modülü - ATS Mini (ESP32-S3 + SI4732) FM & RDS Alıcı
 * Hedef Repo: https://github.com/elmasevmem/bruce-firmware-LightTrackerPlus-ATSmini
 *
 * Gereken diğer değişiklikler (repo_upload klasöründe hazır):
 *   - boards/ats-mini/ats-mini.ini     -> lib_deps: pu2clr/PU2CLR SI4735
 *   - src/core/main_menu.cpp           -> FM menüsü ATS_MINI için de görünür
 *   - src/core/menu_items/FMMenu.cpp   -> "FM Radio (RDS)" seçeneği
 */
#if defined(ATS_MINI) && !defined(LITE_VERSION)

#include "si4732_fm.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include <SI4735.h>
#include <Wire.h>

// ATS Mini pinleri boards/ats-mini/pins_arduino.h içinde tanımlı; yedek değerler:
#ifndef PIN_POWER_ON
#define PIN_POWER_ON 15
#endif
#ifndef PIN_AMP_EN
#define PIN_AMP_EN 10
#endif
#ifndef AUDIO_MUTE
#define AUDIO_MUTE 3
#endif
#ifndef SI4732_RESET
#define SI4732_RESET 16
#endif

#define FM_BAND_MIN 8750  // 87.50 MHz
#define FM_BAND_MAX 10800 // 108.00 MHz
#define FM_STEP 10        // 100 kHz

static SI4735 rx;
static uint16_t currentFrequency = 10000; // 100.0 MHz
static uint8_t currentVolume = 45;        // 0..63
static uint8_t uiMode = 0;                // 0: Frekans, 1: Seek, 2: Ses
static String stationName = "";
static String radioText = "";
static uint8_t lastRssi = 0, lastSnr = 0;
static bool lastStereo = false;

static void resetRdsView() {
    stationName = "";
    radioText = "";
}

static void drawRadioUI(bool full) {
    if (full) tft.fillScreen(TFT_BLACK);

    // Başlık
    tft.setTextSize(1);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    const char *modes[] = {"[FREKANS]  ", "[SEEK ARA] ", "[SES]      "};
    tft.setCursor(6, 6);
    tft.printf("ATS-MINI FM RDS  %s", modes[uiMode]);

    // Frekans
    tft.setTextSize(4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(12, 26);
    tft.printf("%3u.%u MHz ", currentFrequency / 100, (currentFrequency % 100) / 10);

    // Sinyal bilgisi
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(6, 68);
    tft.printf(
        "%-6s RSSI:%3udBuV SNR:%2udB Vol:%2u/63   ",
        lastStereo ? "STEREO" : "MONO",
        lastRssi,
        lastSnr,
        currentVolume
    );

    // RDS istasyon adı (PS) - eski yazıyı silmek için satırı temizle
    tft.fillRect(0, 84, tftWidth, 62, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(6, 86);
    tft.printf("RDS: %s", stationName.length() ? stationName.c_str() : "---");

    // RDS radyo metni (RT) - 2 satıra böl
    tft.setTextSize(1);
    tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft.setCursor(6, 114);
    tft.printf("RT: %s", radioText.length() ? radioText.substring(0, 48).c_str() : "...");
    if (radioText.length() > 48) {
        tft.setCursor(30, 128);
        tft.print(radioText.substring(48, 96));
    }

    if (full) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setCursor(6, 152);
        tft.print("Cevir: Ayar | Kisa Bas: Mod | Uzun Bas: Cikis");
    }
}

// Sinyal ve RDS'i oku; ekranda gösterilen bir şey değiştiyse true döner
static bool pollRadio() {
    bool changed = false;

    rx.getCurrentReceivedSignalQuality();
    uint8_t rssi = rx.getCurrentRSSI();
    uint8_t snr = rx.getCurrentSNR();
    bool stereo = rx.getCurrentPilot();
    if (rssi != lastRssi || snr != lastSnr || stereo != lastStereo) {
        lastRssi = rssi;
        lastSnr = snr;
        lastStereo = stereo;
        changed = true;
    }

    rx.getRdsStatus();
    if (rx.getRdsReceived() && rx.getRdsSync() && rx.getRdsSyncFound()) {
        char *ps = rx.getRdsText0A(); // Program Service (istasyon adı, 8 karakter)
        if (ps != nullptr && strlen(ps) > 0) {
            String s = String(ps);
            s.trim();
            if (s.length() && s != stationName) {
                stationName = s;
                changed = true;
            }
        }
        char *rt = rx.getRdsText2A(); // Radio Text (şarkı/program bilgisi, 64 karakter)
        if (rt != nullptr && strlen(rt) > 0) {
            String s = String(rt);
            s.trim();
            if (s.length() && s != radioText) {
                radioText = s;
                changed = true;
            }
        }
    }
    return changed;
}

static void audioOn() {
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, HIGH); // Amfi açık (pulse vermeden direkt HIGH)
    pinMode(AUDIO_MUTE, OUTPUT);
    digitalWrite(AUDIO_MUTE, LOW); // Sesi aç
}

static void audioOff() {
    pinMode(AUDIO_MUTE, OUTPUT);
    digitalWrite(AUDIO_MUTE, HIGH); // Sustur
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW); // Amfi kapalı (Bruce'un açılış durumu)
}

void si4732_fm_setup() {
    // 1) Radyo katı LDO'su açık olmalı (Bruce açılışta zaten HIGH yapıyor), sesi önce sustur
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    audioOff();
    delay(50);

    // 2) Dahili Si4732 I2C hattı (SDA=18, SCL=17)
    Wire.begin(SDA, SCL);

    int16_t addr = rx.getDeviceI2CAddress(SI4732_RESET);
    if (addr == 0) {
        displayError("Si4732 bulunamadi!", true);
        return;
    }

    // 3) FM modunda başlat (32.768 kHz kristal, analog ses çıkışı)
    rx.setup(SI4732_RESET, 0); // 0 = FM, 1 = AM
    rx.setFM(FM_BAND_MIN, FM_BAND_MAX, currentFrequency, FM_STEP);
    rx.setFMDeEmphasis(1); // 1 = 50 us (Türkiye / Avrupa)
    rx.setSeekFmLimits(FM_BAND_MIN, FM_BAND_MAX);
    rx.setSeekFmSpacing(FM_STEP);
    rx.setVolume(currentVolume);

    // 4) RDS
    rx.setRdsConfig(1, 2, 2, 2, 2);
    rx.setFifoCount(1);

    // 5) Ses yolunu aç
    audioOn();

    si4732_fm_loop();

    // Çıkış: sesi kapat, Si4732'yi uyut (pil tasarrufu)
    audioOff();
    rx.powerDown();
}

void si4732_fm_loop() {
    uint32_t lastPoll = 0;
    drawRadioUI(true);

    while (true) {
        bool redraw = false;
        bool full = false;

        if (check(EscPress)) break; // Uzun basış (600 ms) veya BOOT tuşu

        if (check(SelPress)) {
            uiMode = (uiMode + 1) % 3;
            redraw = true;
        }

        bool next = check(NextPress);
        bool prev = !next && check(PrevPress);
        if (next || prev) {
            if (uiMode == 0) {
                if (next) rx.frequencyUp();
                else rx.frequencyDown();
                currentFrequency = rx.getFrequency();
                resetRdsView();
            } else if (uiMode == 1) {
                tft.setTextSize(1);
                tft.setTextColor(TFT_ORANGE, TFT_BLACK);
                tft.setCursor(6, 68);
                tft.print("Araniyor...                              ");
                rx.seekStationProgress(nullptr, next ? SEEK_UP : SEEK_DOWN);
                currentFrequency = rx.getFrequency();
                resetRdsView();
                full = true;
            } else {
                if (next) rx.volumeUp();
                else rx.volumeDown();
                currentVolume = rx.getVolume();
            }
            redraw = true;
        }

        if (millis() - lastPoll > 300) {
            lastPoll = millis();
            if (pollRadio()) redraw = true;
        }

        if (redraw) drawRadioUI(full);
        delay(20);
    }
}

#endif
