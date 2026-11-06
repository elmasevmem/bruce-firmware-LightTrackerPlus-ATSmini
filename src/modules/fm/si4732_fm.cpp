/**
 * ATS Mini (ESP32-S3 + SI4732) built-in FM receiver with RDS for Bruce.
 *
 *   - FM 87.5-108 MHz, 100 kHz steps, seek, volume
 *   - RDS: station name (PS, group 0A), radio text (RT, group 2A)
 *   - RDS clock time (CT, group 4A) sets Bruce's clock, using Bruce's own timezone/DST setting
 *
 * Opened from FM > FM Radio (RDS). Needs the pu2clr/PU2CLR SI4735 library (ats-mini env).
 */
#if defined(ATS_MINI) && !defined(LITE_VERSION)

#include "si4732_fm.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/utils.h"
#include <SI4735.h>
#include <Wire.h>
#include <globals.h>
#include <sys/time.h>

// Pins come from boards/ats-mini/ats-mini.ini; fallbacks:
#ifndef PIN_POWER_ON
#define PIN_POWER_ON 15
#endif
#ifndef PIN_AMP_EN
#define PIN_AMP_EN 10
#endif
#ifndef AUDIO_MUTE
#define AUDIO_MUTE 3
#endif
#ifndef SI4732_SDA
#define SI4732_SDA 18
#endif
#ifndef SI4732_SCL
#define SI4732_SCL 17
#endif
#ifndef SI4732_RESET
#define SI4732_RESET 16
#endif

#define FM_BAND_MIN 8750  // 87.50 MHz
#define FM_BAND_MAX 10800 // 108.00 MHz
#define FM_STEP 10        // 100 kHz
#define VOL_STEP 4        // 0..63, ~16 encoder clicks from silent to full

// Exposes the raw RDS blocks of the last group (protected in the library) for the CT decoder
class RdsRadio : public SI4735 {
public:
    const si47x_rds_status &rds() const { return currentRdsStatus; }
};

static RdsRadio rx;
static uint16_t currentFrequency = 10000; // 100.0 MHz
static uint8_t currentVolume = 45;        // 0..63
static uint8_t uiMode = 0;                // 0: frequency, 1: seek, 2: volume
static String stationName = "";
static String radioText = "";
static uint8_t lastRssi = 0, lastSnr = 0;
static bool lastStereo = false;

// RDS clock time
static time_t ctLastUtc = 0;    // last decoded CT (UTC epoch), used to confirm the next one
static bool ctSynced = false;   // Bruce's clock was set from RDS in this session
static char ctLabel[24] = "";   // shown on screen

static void resetRdsView() {
    stationName = "";
    radioText = "";
}

static void drawRadioUI(bool full) {
    if (full) tft.fillScreen(TFT_BLACK);

    // Active mode in large text, so it is obvious what the encoder is changing
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    const char *modes[] = {"FREQ  ", "SEEK  ", "VOLUME"};
    tft.setCursor(6, 4);
    tft.printf("FM RDS  [%s]", modes[uiMode]);

    // Frequency
    tft.setTextSize(4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(12, 26);
    tft.printf("%3u.%u MHz ", currentFrequency / 100, (currentFrequency % 100) / 10);

    // Signal
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

    // RDS station name (PS); clear the area first so shorter text leaves no leftovers
    tft.fillRect(0, 84, tftWidth, 62, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(6, 86);
    tft.printf("RDS: %s", stationName.length() ? stationName.c_str() : "---");

    // RDS radio text (RT), wrapped over two lines
    tft.setTextSize(1);
    tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft.setCursor(6, 114);
    tft.printf("RT: %s", radioText.length() ? radioText.substring(0, 48).c_str() : "...");
    if (radioText.length() > 48) {
        tft.setCursor(30, 128);
        tft.print(radioText.substring(48, 96));
    }

    // RDS clock
    tft.setTextColor(ctSynced ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(6, 137);
    tft.print(ctLabel[0] ? ctLabel : "Clock: waiting for RDS CT");

    // Volume bar (highlighted while the encoder adjusts the volume)
    {
        const int bx = 6, by = 146, bw = tftWidth - 12, bh = 4;
        int filled = (bw * currentVolume) / 63;
        tft.fillRect(bx, by, bw, bh, TFT_DARKGREY);
        if (filled > 0) tft.fillRect(bx, by, filled, bh, uiMode == 2 ? TFT_GREEN : TFT_DARKGREEN);
    }

    if (full) {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setCursor(6, 152);
        tft.print("Rotate: adjust | Push: mode | Hold: exit");
    }
}

// RDS group 4A (clock time): MJD + UTC hour/minute in blocks B-D.
// Returns the UTC epoch, or 0 when the group is not a clean 4A.
static time_t decodeRdsClock() {
    const si47x_rds_status &st = rx.rds();
    if (rx.getRdsGroupType() != 4 || rx.getRdsVersionCode() != 0) return 0;
    // Only accept blocks without uncorrected bit errors
    if (st.resp.BLEB > 1 || st.resp.BLEC > 1 || st.resp.BLED > 1) return 0;

    uint16_t b = ((uint16_t)st.resp.BLOCKBH << 8) | st.resp.BLOCKBL;
    uint16_t c = ((uint16_t)st.resp.BLOCKCH << 8) | st.resp.BLOCKCL;
    uint16_t d = ((uint16_t)st.resp.BLOCKDH << 8) | st.resp.BLOCKDL;

    uint32_t mjd = ((uint32_t)(b & 0x3) << 15) | (c >> 1);
    uint32_t hour = ((c & 0x1) << 4) | (d >> 12);
    uint32_t minute = (d >> 6) & 0x3F;
    if (hour > 23 || minute > 59 || mjd < 40587) return 0;

    time_t utc = (time_t)(mjd - 40587) * 86400 + hour * 3600 + minute * 60; // MJD 40587 = 1970-01-01
    if (utc < 1735689600) return 0;                                          // before 2025: not plausible
    return utc;
}

// Applies an RDS CT to Bruce's clock. A CT is only trusted when the previous one agrees
// (same minute or the minute after), so a single corrupted group can never set a wrong time.
static bool handleRdsClock(time_t utc) {
    bool confirmed = ctLastUtc != 0 && utc >= ctLastUtc && utc - ctLastUtc <= 180;
    ctLastUtc = utc;
    if (!confirmed) return false;

    // Same convention as the NTP sync: Bruce keeps local time = UTC + timezone (+ DST)
    time_t local = utc + (time_t)(bruceConfig.tmz * 3600) + (bruceConfig.dst ? 3600 : 0);
#if defined(HAS_RTC)
    struct tm *t = localtime(&local);
    RTC_TimeTypeDef ts;
    ts.Hours = t->tm_hour;
    ts.Minutes = t->tm_min;
    ts.Seconds = t->tm_sec;
    _rtc.SetTime(&ts);
    updateTimeStr(_rtc.getTimeStruct());
#else
    rtc.setTime(local);
    updateTimeStr(rtc.getTimeStruct());
#endif
    clock_set = true;
    localTime = local;
    struct timeval tv = {.tv_sec = local};
    settimeofday(&tv, nullptr);

    struct tm lt;
    gmtime_r(&local, &lt);
    snprintf(
        ctLabel,
        sizeof(ctLabel),
        "Clock set by RDS %02d:%02d",
        lt.tm_hour,
        lt.tm_min
    );
    ctSynced = true;
    return true;
}

// Reads signal quality and drains the RDS FIFO. Returns true when something on screen changed.
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

    // Stations send ~11 groups/s; read every queued group so the once-a-minute CT group
    // (and PS/RT segments) are not lost between polls.
    for (int i = 0; i < 24; i++) {
        rx.getRdsStatus();
        if (!rx.getRdsReceived()) break;
        if (rx.getRdsSync()) {
            char *ps = rx.getRdsText0A(); // Program Service name, 8 chars
            if (ps != nullptr && strlen(ps) > 0) {
                String s = String(ps);
                s.trim();
                if (s.length() && s != stationName) {
                    stationName = s;
                    changed = true;
                }
            }
            char *rt = rx.getRdsText2A(); // Radio Text, up to 64 chars
            if (rt != nullptr && strlen(rt) > 0) {
                String s = String(rt);
                s.trim();
                if (s.length() && s != radioText) {
                    radioText = s;
                    changed = true;
                }
            }
            time_t ct = decodeRdsClock();
            if (ct && handleRdsClock(ct)) changed = true;
        }
        if (rx.getNumRdsFifoUsed() == 0) break;
    }
    return changed;
}

static void audioOn() {
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, HIGH); // amp on: drive it straight HIGH, never pulse it
    pinMode(AUDIO_MUTE, OUTPUT);
    digitalWrite(AUDIO_MUTE, currentVolume == 0 ? HIGH : LOW); // unmute
}

static void audioOff() {
    pinMode(AUDIO_MUTE, OUTPUT);
    digitalWrite(AUDIO_MUTE, HIGH); // mute
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, LOW); // amp off (Bruce's boot state)
}

void si4732_fm_setup() {
    // 1) Receiver LDO on (already HIGH from boot), audio muted while the chip starts
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    audioOff();
    delay(50);

    // 2) Internal SI4732 I2C bus
    Wire.begin(SI4732_SDA, SI4732_SCL);

    int16_t addr = rx.getDeviceI2CAddress(SI4732_RESET);
    if (addr == 0) {
        displayError("SI4732 not found", true);
        return;
    }

    // 3) Start in FM (32.768 kHz crystal, analog audio out)
    rx.setup(SI4732_RESET, 0); // 0 = FM, 1 = AM
    rx.setFM(FM_BAND_MIN, FM_BAND_MAX, currentFrequency, FM_STEP);
    rx.setFMDeEmphasis(1); // 1 = 50 us (Europe / Turkey), 2 = 75 us (Americas)
    rx.setSeekFmLimits(FM_BAND_MIN, FM_BAND_MAX);
    rx.setSeekFmSpacing(FM_STEP);
    rx.setVolume(currentVolume);

    // 4) RDS
    rx.setRdsConfig(1, 2, 2, 2, 2);
    rx.setFifoCount(1);

    // 5) Open the audio path
    audioOn();

    ctLastUtc = 0;
    ctSynced = false;
    ctLabel[0] = 0;
    si4732_fm_loop();

    // Exit: mute, amp off, SI4732 powered down to save battery
    audioOff();
    rx.powerDown();
}

void si4732_fm_loop() {
    uint32_t lastPoll = 0;
    drawRadioUI(true);

    while (true) {
        bool redraw = false;
        bool full = false;

        if (check(EscPress)) break; // long push (600 ms) or BOOT

        if (check(SelPress)) {
            uiMode = (uiMode + 1) % 3;
            redraw = true;
        }

        bool next = check(NextPress);
        bool prev = !next && check(PrevPress);
        if (next || prev) {
            if (uiMode == 0) {
                ctLastUtc = 0; // new station: start CT confirmation again
                if (next) rx.frequencyUp();
                else rx.frequencyDown();
                currentFrequency = rx.getFrequency();
                resetRdsView();
            } else if (uiMode == 1) {
                tft.setTextSize(1);
                tft.setTextColor(TFT_ORANGE, TFT_BLACK);
                tft.setCursor(6, 68);
                tft.print("Seeking...                               ");
                ctLastUtc = 0;
                rx.seekStationProgress(nullptr, next ? SEEK_UP : SEEK_DOWN);
                currentFrequency = rx.getFrequency();
                resetRdsView();
                full = true;
            } else {
                int v = (int)currentVolume + (next ? VOL_STEP : -VOL_STEP);
                if (v < 0) v = 0;
                if (v > 63) v = 63;
                currentVolume = (uint8_t)v;
                rx.setVolume(currentVolume);
                // Hardware mute at 0 so the amplifier hiss goes away too
                digitalWrite(AUDIO_MUTE, currentVolume == 0 ? HIGH : LOW);
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
