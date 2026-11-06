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
static String stationName = ""; // most frequently seen PS (the station's real name)
static String currentPs = "";   // PS being sent right now (stations often rotate texts)
static String radioText = "";

// ---- RDS decoder state (PS = group 0A/0B, RT = group 2A/2B) ----
// A PS is only shown once all 4 segments arrived without errors AND the same 8 characters
// were assembled twice in a row. That filters half-received names ("KRA") and the mixed
// names produced while a station switches its PS text.
static char psBuf[9];
static uint8_t psMask = 0;
static char psCandidate[9];
// Seen PS texts and how often, to tell the real name from rotating texts.
#define PS_SLOTS 6
static String psSeen[PS_SLOTS];
static uint16_t psCount[PS_SLOTS];
// RT is shown only when complete (up to the 0x0D terminator, or all 16 segments).
static char rtBuf[65];
static uint16_t rtMask = 0;
static int8_t rtAB = -1;
static int8_t rtLen = -1;
static uint8_t lastRssi = 0, lastSnr = 0;
static bool lastStereo = false;

// RDS clock time
static time_t ctLastUtc = 0;    // last decoded CT (UTC epoch), used to confirm the next one
static bool ctSynced = false;   // Bruce's clock was set from RDS in this session
static char ctLabel[24] = "";   // shown on screen

static void resetRdsView() {
    stationName = "";
    currentPs = "";
    radioText = "";
    memset(psBuf, ' ', 8);
    psBuf[8] = 0;
    psCandidate[0] = 0;
    psMask = 0;
    for (int i = 0; i < PS_SLOTS; i++) {
        psSeen[i] = "";
        psCount[i] = 0;
    }
    memset(rtBuf, ' ', 64);
    rtBuf[64] = 0;
    rtMask = 0;
    rtAB = -1;
    rtLen = -1;
}

// RDS uses its own character table; keep printable ASCII, map the rest to a space.
static char rdsChar(uint8_t c) { return (c >= 0x20 && c < 0x7F) ? (char)c : ' '; }

static String trimmed(const char *buf) {
    String t = String(buf);
    t.trim();
    return t;
}

// Called with a fully confirmed PS. Returns true when what is shown changed.
static bool acceptPs(const String &ps) {
    if (!ps.length()) return false;
    int slot = -1, weakest = 0;
    for (int i = 0; i < PS_SLOTS; i++) {
        if (psSeen[i] == ps) {
            slot = i;
            break;
        }
        if (psCount[i] < psCount[weakest]) weakest = i;
    }
    if (slot < 0) {
        slot = weakest;
        psSeen[slot] = ps;
        psCount[slot] = 0;
    }
    if (psCount[slot] < 60000) psCount[slot]++;

    int best = 0;
    for (int i = 1; i < PS_SLOTS; i++)
        if (psCount[i] > psCount[best]) best = i;

    bool changed = false;
    if (psSeen[best] != stationName) {
        stationName = psSeen[best];
        changed = true;
    }
    if (ps != currentPs) {
        currentPs = ps;
        changed = true;
    }
    return changed;
}

// Feeds one RDS group into the PS / RT decoders. Returns true when the screen needs an update.
static bool decodeRdsText(const si47x_rds_status &st) {
    uint16_t b = ((uint16_t)st.resp.BLOCKBH << 8) | st.resp.BLOCKBL;
    uint16_t c = ((uint16_t)st.resp.BLOCKCH << 8) | st.resp.BLOCKCL;
    uint16_t d = ((uint16_t)st.resp.BLOCKDH << 8) | st.resp.BLOCKDL;
    uint8_t type = b >> 12;
    bool versionB = (b >> 11) & 1;
    if (st.resp.BLEB > 1) return false; // group header not trustworthy

    if (type == 0) { // PS: 2 chars per group in block D
        if (st.resp.BLED > 1) return false;
        uint8_t addr = b & 0x3;
        psBuf[addr * 2] = rdsChar(d >> 8);
        psBuf[addr * 2 + 1] = rdsChar(d & 0xFF);
        psMask |= 1 << addr;
        if (addr == 3) {
            bool complete = psMask == 0x0F;
            psMask = 0;
            if (!complete) return false;
            bool same = strcmp(psBuf, psCandidate) == 0;
            memcpy(psCandidate, psBuf, sizeof(psCandidate));
            if (same) return acceptPs(trimmed(psBuf));
        }
        return false;
    }

    if (type == 2) { // RT: 4 chars per group (2A, blocks C+D) or 2 chars (2B, block D)
        uint8_t ab = (b >> 4) & 1;
        uint8_t addr = b & 0xF;
        if (rtAB != ab) { // station started a new text: collect it from scratch
            rtAB = ab;
            memset(rtBuf, ' ', 64);
            rtMask = 0;
            rtLen = -1;
        }
        char chars[4];
        uint8_t n;
        uint8_t pos;
        if (!versionB) {
            if (st.resp.BLEC > 1 || st.resp.BLED > 1) return false;
            chars[0] = c >> 8;
            chars[1] = c & 0xFF;
            chars[2] = d >> 8;
            chars[3] = d & 0xFF;
            n = 4;
            pos = addr * 4;
        } else {
            if (st.resp.BLED > 1) return false;
            chars[0] = d >> 8;
            chars[1] = d & 0xFF;
            n = 2;
            pos = addr * 2;
        }
        for (uint8_t i = 0; i < n && pos + i < 64; i++) {
            if (chars[i] == 0x0D) {
                if (rtLen < 0 || pos + i < rtLen) rtLen = pos + i;
                break;
            }
            rtBuf[pos + i] = rdsChar(chars[i]);
        }
        rtMask |= 1 << addr;

        int len = rtLen >= 0 ? rtLen : (versionB ? 32 : 64);
        int segs = (len + n - 1) / n;
        if (segs < 1) segs = 1;
        uint16_t need = segs >= 16 ? 0xFFFF : (uint16_t)((1u << segs) - 1);
        if ((rtMask & need) != need) return false;

        char out[65];
        memcpy(out, rtBuf, len);
        out[len] = 0;
        String t = trimmed(out);
        if (t.length() && t != radioText) {
            radioText = t;
            return true;
        }
    }
    return false;
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

    // Text is drawn with a background colour and padded to a fixed width, so it overwrites the
    // previous value in place. Clearing the area first made the RDS lines flash on every update.

    // RDS station name (PS): the station's main name, plus what it is sending right now
    // when that differs (many stations rotate city names, slogans, song titles in PS).
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(6, 86);
    tft.printf("%-8s", stationName.length() ? stationName.c_str() : "--------");
    tft.setTextSize(1);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setCursor(112, 91);
    if (currentPs.length() && currentPs != stationName) tft.printf("> %-8s", currentPs.c_str());
    else tft.print("          ");

    // RDS radio text (RT), wrapped over two lines of 48 characters
    char line[49];
    tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
    tft.setCursor(6, 112);
    snprintf(line, sizeof(line), "%-48s", radioText.length() ? radioText.substring(0, 48).c_str() : "...");
    tft.print("RT: ");
    tft.print(line);
    tft.setCursor(30, 124);
    snprintf(line, sizeof(line), "%-48s", radioText.length() > 48 ? radioText.substring(48).c_str() : "");
    tft.print(line);

    // RDS clock
    tft.setTextColor(ctSynced ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(6, 136);
    snprintf(line, sizeof(line), "%-40s", ctLabel[0] ? ctLabel : "Clock: waiting for RDS CT");
    tft.print(line);

    // Volume bar (highlighted while the encoder adjusts the volume)
    {
        const int bx = 6, by = 146, bw = tftWidth - 12, bh = 4;
        int filled = (bw * currentVolume) / 63;
        if (filled > 0) tft.fillRect(bx, by, filled, bh, uiMode == 2 ? TFT_GREEN : TFT_DARKGREEN);
        if (filled < bw) tft.fillRect(bx + filled, by, bw - filled, bh, TFT_DARKGREY);
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
            if (decodeRdsText(rx.rds())) changed = true;
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
    resetRdsView();
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
