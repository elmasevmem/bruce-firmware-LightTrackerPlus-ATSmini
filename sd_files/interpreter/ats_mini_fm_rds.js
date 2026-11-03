/**
 * Bruce Firmware - ATS Mini (ESP32-S3 + dahili Si4732) FM Radyo & RDS Alıcı (BruceJS)
 * Hedef Repo: https://github.com/elmasevmem/bruce-firmware-LightTrackerPlus-ATSmini
 *
 * GEREKSİNİM: src/modules/bjs_interpreter/i2c_js.cpp yamalı olmalı
 * (orijinal repoda i2c.read() "not implemented" hatası fırlatıyor).
 *
 * Kontroller: Çevir = ayar | Kısa bas = mod | Uzun bas (600 ms) = çıkış
 */

var i2c = require("i2c");
var display = require("display");
var keyboard = require("keyboard");
var gpio = require("gpio");
var device = require("device");

// --- ATS MINI PINLERI (boards/ats-mini/pins_arduino.h) ---
var SDA_PIN = 18;
var SCL_PIN = 17;
var RST_PIN = 16;        // SI4732_RESET
var PIN_POWER_ON = 15;   // Radyo LDO (HIGH = açık)
var PIN_AMP_EN = 10;     // Amfi (HIGH = açık, pulse VERMEYİN)
var AUDIO_MUTE_PIN = 3;  // HIGH = sessiz

var ADDR = 0x11;         // SEN = GND

// --- Si4732 komutları (AN332) ---
var CMD_POWER_UP = 0x01;
var CMD_POWER_DOWN = 0x11;
var CMD_SET_PROPERTY = 0x12;
var CMD_GET_INT_STATUS = 0x14;
var CMD_FM_TUNE_FREQ = 0x20;
var CMD_FM_SEEK_START = 0x21;
var CMD_FM_TUNE_STATUS = 0x22;
var CMD_FM_RSQ_STATUS = 0x23;
var CMD_FM_RDS_STATUS = 0x24;

var BAND_MIN = 8750;
var BAND_MAX = 10800;

// --- Durum ---
var freq = 10000;
var volume = 45;
var rssi = 0;
var snr = 0;
var stereo = false;
var uiMode = 0; // 0 frekans, 1 seek, 2 ses

var psBuf = [];
var rtBuf = [];
var stationName = "";
var radioText = "";
var piCode = "----";
var lastAb = -1;

function clearRds() {
    psBuf = [];
    rtBuf = [];
    for (var i = 0; i < 8; i++) psBuf.push(" ");
    for (var j = 0; j < 64; j++) rtBuf.push(" ");
    stationName = "";
    radioText = "";
    piCode = "----";
    lastAb = -1;
}

function readStatus() {
    var r = i2c.read(ADDR, 1);
    return (r && r.length > 0) ? r[0] : 0;
}

// CTS (bit 7) bekle
function waitCts() {
    for (var i = 0; i < 100; i++) {
        if (readStatus() & 0x80) return true;
        delay(2);
    }
    return false;
}

function cmd(bytes) {
    i2c.write(ADDR, new Uint8Array(bytes));
    waitCts();
}

function cmdRead(bytes, len) {
    cmd(bytes);
    return i2c.read(ADDR, len);
}

function setProperty(id, val) {
    cmd([CMD_SET_PROPERTY, 0x00, (id >> 8) & 0xFF, id & 0xFF, (val >> 8) & 0xFF, val & 0xFF]);
}

// Tune/Seek bitene kadar STC (bit 0) bekle
function waitStc(timeoutMs) {
    var start = Date.now();
    while (Date.now() - start < timeoutMs) {
        cmd([CMD_GET_INT_STATUS]);
        if (readStatus() & 0x01) return true;
        delay(20);
    }
    return false;
}

function readTunedFreq() {
    // INTACK=1 -> STC bayrağını temizler
    var r = cmdRead([CMD_FM_TUNE_STATUS, 0x01], 8);
    if (r && r.length >= 4) {
        var f = (r[2] << 8) | r[3];
        if (f >= BAND_MIN && f <= BAND_MAX) freq = f;
    }
}

function tune(f) {
    if (f < BAND_MIN) f = BAND_MAX;
    if (f > BAND_MAX) f = BAND_MIN;
    cmd([CMD_FM_TUNE_FREQ, 0x00, (f >> 8) & 0xFF, f & 0xFF, 0x00]);
    waitStc(500);
    freq = f;
    readTunedFreq();
    clearRds();
}

function seek(up) {
    // bit3 SEEKUP, bit2 WRAP
    cmd([CMD_FM_SEEK_START, (up ? 0x08 : 0x00) | 0x04]);
    waitStc(8000);
    readTunedFreq();
    clearRds();
}

function readSignal() {
    var r = cmdRead([CMD_FM_RSQ_STATUS, 0x01], 8);
    if (!r || r.length < 6) return false;
    var st = (r[3] & 0x80) !== 0;
    var changed = (st !== stereo) || (r[4] !== rssi) || (r[5] !== snr);
    stereo = st;
    rssi = r[4];
    snr = r[5];
    return changed;
}

function ch(b) {
    return (b >= 32 && b <= 126) ? String.fromCharCode(b) : " ";
}

function readRds() {
    var changed = false;
    for (var n = 0; n < 8; n++) {
        var r = cmdRead([CMD_FM_RDS_STATUS, 0x01], 13);
        if (!r || r.length < 13) break;
        var fifo = r[3];
        if (fifo === 0 && (r[1] & 0x01) === 0) break;

        var bleA = (r[12] >> 6) & 3, bleB = (r[12] >> 4) & 3;
        var bleC = (r[12] >> 2) & 3, bleD = r[12] & 3;
        if (bleB === 3 || bleD === 3) { if (fifo <= 1) break; continue; }

        var A = (r[4] << 8) | r[5];
        var B = (r[6] << 8) | r[7];
        var C = (r[8] << 8) | r[9];
        var D = (r[10] << 8) | r[11];

        if (bleA === 0 && A !== 0) piCode = A.toString(16).toUpperCase();

        var group = (B >> 12) & 0x0F;
        var verB = (B >> 11) & 1;

        if (group === 0) {
            var pi = (B & 3) * 2;
            psBuf[pi] = ch((D >> 8) & 0xFF);
            psBuf[pi + 1] = ch(D & 0xFF);
            var ps = psBuf.join("").trim();
            if (ps !== stationName) { stationName = ps; changed = true; }
        } else if (group === 2) {
            var ab = (B >> 4) & 1;
            if (ab !== lastAb) {
                for (var k = 0; k < 64; k++) rtBuf[k] = " ";
                lastAb = ab;
            }
            var seg = B & 0x0F;
            if (verB === 0) {
                if (bleC < 3) {
                    rtBuf[seg * 4] = ch((C >> 8) & 0xFF);
                    rtBuf[seg * 4 + 1] = ch(C & 0xFF);
                }
                rtBuf[seg * 4 + 2] = ch((D >> 8) & 0xFF);
                rtBuf[seg * 4 + 3] = ch(D & 0xFF);
            } else {
                rtBuf[seg * 2] = ch((D >> 8) & 0xFF);
                rtBuf[seg * 2 + 1] = ch(D & 0xFF);
            }
            var rt = rtBuf.join("").trim();
            if (rt !== radioText) { radioText = rt; changed = true; }
        }
        if (fifo <= 1) break;
    }
    return changed;
}

function draw() {
    display.fill(0x0000);

    display.setTextSize(1);
    display.setTextColor(0x07E0);
    var modes = ["[FREKANS]", "[SEEK ARA]", "[SES]"];
    display.drawString("ATS-MINI FM RDS  " + modes[uiMode], 6, 6);

    display.setTextSize(4);
    display.setTextColor(0xFFFF);
    display.drawString(Math.floor(freq / 100) + "." + Math.floor((freq % 100) / 10) + " MHz", 12, 26);

    display.setTextSize(1);
    display.setTextColor(0xFFE0);
    display.drawString((stereo ? "STEREO" : "MONO") + "  RSSI:" + rssi + "dBuV  SNR:" + snr +
        "dB  Vol:" + volume + "/63  PI:" + piCode, 6, 68);

    display.setTextSize(2);
    display.setTextColor(0x07FF);
    display.drawString("RDS: " + (stationName || "---"), 6, 86);

    display.setTextSize(1);
    display.setTextColor(0xF81F);
    display.drawString("RT: " + (radioText.substring(0, 48) || "..."), 6, 114);
    if (radioText.length > 48) display.drawString(radioText.substring(48, 64), 30, 128);

    display.setTextColor(0x7BEF);
    display.drawString("Cevir: Ayar | Kisa Bas: Mod | Uzun Bas: Cikis", 6, 152);
}

function initRadio() {
    gpio.pinMode(PIN_POWER_ON, "output");
    gpio.digitalWrite(PIN_POWER_ON, true);
    gpio.pinMode(AUDIO_MUTE_PIN, "output");
    gpio.digitalWrite(AUDIO_MUTE_PIN, true); // açılış patlamasına karşı sustur

    gpio.pinMode(RST_PIN, "output");
    gpio.digitalWrite(RST_PIN, false);
    delay(20);
    gpio.digitalWrite(RST_PIN, true);
    delay(20);

    i2c.begin(SDA_PIN, SCL_PIN, 100000);
    var found = i2c.scan();
    var ok = false;
    for (var i = 0; i < found.length; i++) {
        if (found[i] === 0x11 || found[i] === 0x63) { ADDR = found[i]; ok = true; }
    }
    if (!ok) return false;

    // POWER_UP: XOSCEN + FM alıcı, analog ses çıkışı
    cmd([CMD_POWER_UP, 0x10, 0x05]);
    delay(500); // kristalin oturması

    setProperty(0x1100, 0x0001); // De-emphasis 50 us
    setProperty(0x1400, BAND_MIN); // Seek alt sınır
    setProperty(0x1401, BAND_MAX); // Seek üst sınır
    setProperty(0x1402, 10);       // Seek adımı 100 kHz
    setProperty(0x4000, volume);   // Ses
    setProperty(0x1502, 0xAA01);   // RDS aç, blok hata eşikleri = 2

    tune(freq);

    gpio.pinMode(PIN_AMP_EN, "output");
    gpio.digitalWrite(PIN_AMP_EN, true);
    gpio.digitalWrite(AUDIO_MUTE_PIN, false);
    return true;
}

function shutdown() {
    gpio.digitalWrite(AUDIO_MUTE_PIN, true);
    gpio.digitalWrite(PIN_AMP_EN, false);
    cmd([CMD_POWER_DOWN]);
}

function showError(lines) {
    display.fill(0x0000);
    display.setTextColor(0xF800);
    display.setTextSize(2);
    display.drawString(lines[0], 6, 50);
    display.setTextSize(1);
    display.setTextColor(0xFFFF);
    for (var i = 1; i < lines.length; i++) display.drawString(lines[i], 6, 80 + (i - 1) * 14);
    delay(4000);
}

// Bu betik sadece ATS Mini'de çalışır: başka cihazlarda bu pinler farklı donanıma bağlı.
function isAtsMini() {
    var b = "";
    try { b = String(device.getBoard()); } catch (e) { b = ""; }
    return b.toLowerCase().indexOf("ats mini") >= 0;
}

// i2c.read() bazı Bruce sürümlerinde henüz yok ("not implemented")
function i2cReadSupported() {
    try {
        i2c.begin(SDA_PIN, SCL_PIN, 100000);
        i2c.read(0x11, 1);
        return true;
    } catch (e) {
        return String(e).indexOf("not implemented") < 0;
    }
}

// --- ANA PROGRAM ---
clearRds();
if (!isAtsMini()) {
    showError(["Sadece ATS Mini!", "This app only runs on the ATS Mini", "(ESP32-S3 + SI4732).", "Device: " + device.getBoard()]);
} else if (!i2cReadSupported()) {
    showError(["i2c.read yok", "This firmware lacks i2c.read().", "Use the built-in FM menu or a", "newer Bruce build."]);
} else if (!initRadio()) {
    showError(["Si4732 bulunamadi!", "Si4732 not found on I2C 18/17."]);
} else {
    draw();
    var lastPoll = 0;
    while (true) {
        var redraw = false;

        if (keyboard.getEscPress()) break;

        if (keyboard.getSelPress()) {
            uiMode = (uiMode + 1) % 3;
            redraw = true;
        }

        var next = keyboard.getNextPress();
        var prev = !next && keyboard.getPrevPress();
        if (next || prev) {
            if (uiMode === 0) {
                tune(freq + (next ? 10 : -10));
            } else if (uiMode === 1) {
                display.setTextColor(0xFD20);
                display.drawString("Araniyor...", 220, 6);
                seek(next);
            } else {
                volume += next ? 2 : -2;
                if (volume > 63) volume = 63;
                if (volume < 0) volume = 0;
                setProperty(0x4000, volume);
            }
            redraw = true;
        }

        if (Date.now() - lastPoll > 300) {
            lastPoll = Date.now();
            if (readSignal()) redraw = true;
            if (readRds()) redraw = true;
        }

        if (redraw) draw();
        delay(20);
    }
    shutdown();
}

