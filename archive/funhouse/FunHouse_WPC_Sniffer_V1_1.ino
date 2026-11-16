/*
  =====================================================================
  FUNHOUSE WPC SNIFFER V1.1
  =====================================================================

  V1.1 keeps the proven V1 hardware behavior and adds support for the
  actual /sounds.csv format already on the SD card:

      command,name,status,file
      "7A 2B","I thought we were pals!","CONFIRMED",""

  IMPORTANT:
  - WiFi starts FIRST.
  - All WPC GPIOs are reclaimed AFTER WiFi starts.
  - CA2 stays on GPIO25.
  - No DAC / no I2S in sniffer mode.
  - SD is used only for:
      /sounds.csv   lookup information
      /buslog.csv   raw capture log

  V1.1 supports:
  - 1-byte commands, e.g. "90"
  - 2-byte commands, e.g. "7A 2B"
  - up to 6-byte command sequences from CSV

  Sequence grouping rule:
  Consecutive SOUND writes to address 0x1C that arrive within
  COMMAND_GAP_US are treated as one possible command sequence.

  Raw logging remains authoritative and records EVERY captured WDEN event.
  =====================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include "driver/gpio.h"

// =====================================================================
// WIFI / WEB
// =====================================================================

static const char* WIFI_SSID = "FUNHOUSE-SNIFFER";
static const char* WIFI_PASS = "funhouse89";

WebServer server(80);

// =====================================================================
// WPC INPUTS
// =====================================================================

static const uint8_t PIN_COUNT = 16;

static const uint8_t wpcPins[PIN_COUNT] = {
  34, // CD7
  35, // CD6
  32, // CD5
  33, // CD4
  27, // CD3
  14, // CD2
  13, // CD1
  12, // CD0

  15, // CA4
  4,  // CA3
  25, // CA2  <-- VERIFIED
  21, // CA1
  16, // CA0
  17, // WDEN
  36, // R/W
  39  // RESET
};

static const char* wpcNames[PIN_COUNT] = {
  "CD7","CD6","CD5","CD4","CD3","CD2","CD1","CD0",
  "CA4","CA3","CA2","CA1","CA0","WDEN","R/W","RESET"
};

static const uint8_t PIN_WDEN  = 17;
static const uint8_t PIN_RW    = 36;
static const uint8_t PIN_RESET = 39;

static const uint8_t SOUND_ADDRESS = 0x1C;

uint8_t liveState[PIN_COUNT] = {0};

// =====================================================================
// SD CARD
// =====================================================================

static const uint8_t SD_CS   = 5;
static const uint8_t SD_SCK  = 18;
static const uint8_t SD_MISO = 19;
static const uint8_t SD_MOSI = 23;

SPIClass sdSPI(VSPI);

bool sdOK = false;
bool loggingEnabled = true;
File logFile;

// =====================================================================
// SOUND COMMAND TABLE
// =====================================================================

static const uint8_t MAX_CMD_BYTES = 6;
static const uint16_t MAX_SOUND_COMMANDS = 256;

struct SoundCommand {
  uint8_t bytes[MAX_CMD_BYTES];
  uint8_t length;
  char name[64];
  char status[24];
  char file[64];
};

SoundCommand soundCommands[MAX_SOUND_COMMANDS];
uint16_t soundCommandCount = 0;

// =====================================================================
// ACTIVE SOUND SEQUENCE BUILDER
//
// We collect consecutive SOUND writes (A=0x1C, R/W=0).
// If another byte does not arrive within this gap, we finalize the
// current sequence.
//
// 5000 us is deliberately generous for the first machine test.
// We can tighten this after seeing real captures.
// =====================================================================

static const uint32_t COMMAND_GAP_US = 5000;

uint8_t activeSeq[MAX_CMD_BYTES] = {0};
uint8_t activeSeqLen = 0;
uint32_t activeSeqLastUs = 0;

// Last matched command for web display.
char lastMatchedCommand[32] = "";
char lastMatchedName[64] = "";
char lastMatchedStatus[24] = "";

// =====================================================================
// ISR EVENT RING
// =====================================================================

struct BusEvent {
  uint32_t timeUs;
  uint8_t address;
  uint8_t data;
  uint8_t rw;
  uint8_t reset;
};

static const uint16_t EVENT_RING_SIZE = 256;

volatile BusEvent eventRing[EVENT_RING_SIZE];
volatile uint16_t eventHead = 0;
volatile uint16_t eventTail = 0;
volatile uint32_t totalWdenEdges = 0;
volatile uint32_t droppedEvents = 0;

portMUX_TYPE eventMux = portMUX_INITIALIZER_UNLOCKED;

// =====================================================================
// RECENT EVENTS
// =====================================================================

struct RecentEvent {
  uint32_t timeUs;
  uint8_t address;
  uint8_t data;
  uint8_t rw;
  uint8_t reset;
  bool soundWrite;
};

static const uint8_t RECENT_COUNT = 32;
RecentEvent recent[RECENT_COUNT];
uint8_t recentUsed = 0;

// =====================================================================
// ISR BUS READS
// =====================================================================

static inline uint8_t IRAM_ATTR readDataBusISR() {
  uint8_t value = 0;

  value |= gpio_get_level(GPIO_NUM_34) ? 0x80 : 0;
  value |= gpio_get_level(GPIO_NUM_35) ? 0x40 : 0;
  value |= gpio_get_level(GPIO_NUM_32) ? 0x20 : 0;
  value |= gpio_get_level(GPIO_NUM_33) ? 0x10 : 0;
  value |= gpio_get_level(GPIO_NUM_27) ? 0x08 : 0;
  value |= gpio_get_level(GPIO_NUM_14) ? 0x04 : 0;
  value |= gpio_get_level(GPIO_NUM_13) ? 0x02 : 0;
  value |= gpio_get_level(GPIO_NUM_12) ? 0x01 : 0;

  return value;
}

static inline uint8_t IRAM_ATTR readAddressBusISR() {
  uint8_t value = 0;

  value |= gpio_get_level(GPIO_NUM_15) ? 0x10 : 0;
  value |= gpio_get_level(GPIO_NUM_4)  ? 0x08 : 0;
  value |= gpio_get_level(GPIO_NUM_25) ? 0x04 : 0; // CA2 VERIFIED
  value |= gpio_get_level(GPIO_NUM_21) ? 0x02 : 0;
  value |= gpio_get_level(GPIO_NUM_16) ? 0x01 : 0;

  return value;
}

// =====================================================================
// WDEN ISR
// =====================================================================

void IRAM_ATTR onWdenRise() {
  BusEvent ev;

  ev.timeUs  = micros();
  ev.address = readAddressBusISR();
  ev.data    = readDataBusISR();
  ev.rw      = gpio_get_level((gpio_num_t)PIN_RW) ? 1 : 0;
  ev.reset   = gpio_get_level((gpio_num_t)PIN_RESET) ? 1 : 0;

  portENTER_CRITICAL_ISR(&eventMux);

  totalWdenEdges++;

  uint16_t next = (uint16_t)((eventHead + 1) % EVENT_RING_SIZE);

  if (next == eventTail) {
    droppedEvents++;
  } else {
    eventRing[eventHead].timeUs  = ev.timeUs;
    eventRing[eventHead].address = ev.address;
    eventRing[eventHead].data    = ev.data;
    eventRing[eventHead].rw      = ev.rw;
    eventRing[eventHead].reset   = ev.reset;

    eventHead = next;
  }

  portEXIT_CRITICAL_ISR(&eventMux);
}

bool popEvent(BusEvent &out) {
  bool haveEvent = false;

  portENTER_CRITICAL(&eventMux);

  if (eventTail != eventHead) {
    out.timeUs  = eventRing[eventTail].timeUs;
    out.address = eventRing[eventTail].address;
    out.data    = eventRing[eventTail].data;
    out.rw      = eventRing[eventTail].rw;
    out.reset   = eventRing[eventTail].reset;

    eventTail = (uint16_t)((eventTail + 1) % EVENT_RING_SIZE);

    haveEvent = true;
  }

  portEXIT_CRITICAL(&eventMux);

  return haveEvent;
}

// =====================================================================
// PROVEN WIFI -> GPIO RECLAIM FIX
// =====================================================================

void reclaimWpcPins() {
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    gpio_reset_pin((gpio_num_t)wpcPins[i]);
    gpio_set_direction((gpio_num_t)wpcPins[i], GPIO_MODE_INPUT);
    pinMode(wpcPins[i], INPUT);
  }
}

void sampleLivePins() {
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    liveState[i] = digitalRead(wpcPins[i]) ? 1 : 0;
  }
}

// =====================================================================
// SMALL CSV PARSER
//
// Handles quoted CSV fields so the actual file format works:
//
// "7A 2B","I thought we were pals!","CONFIRMED",""
//
// Returns up to maxFields fields in-place.
// =====================================================================

int parseCsvLine(char* line, char* fields[], int maxFields) {
  int fieldCount = 0;
  bool inQuotes = false;
  char* src = line;
  char* dst = line;
  char* fieldStart = dst;

  while (*src) {
    char c = *src++;

    if (c == '"') {
      // Escaped quote inside quoted field: ""
      if (inQuotes && *src == '"') {
        *dst++ = '"';
        src++;
      } else {
        inQuotes = !inQuotes;
      }
    }
    else if (c == ',' && !inQuotes) {
      *dst++ = '\0';

      if (fieldCount < maxFields) {
        fields[fieldCount++] = fieldStart;
      }

      fieldStart = dst;
    }
    else if (c != '\r' && c != '\n') {
      *dst++ = c;
    }
  }

  *dst = '\0';

  if (fieldCount < maxFields) {
    fields[fieldCount++] = fieldStart;
  }

  return fieldCount;
}

void trimInPlace(char* s) {
  if (!s) return;

  while (*s == ' ' || *s == '\t') s++;

  // Shift left if we skipped leading spaces.
  char* base = s;
  if (base != s) {}

  // Since callers receive pointers into the line buffer, easiest is
  // just trim trailing whitespace here. Leading spaces are harmless
  // for command parsing because strtok below skips them.
  size_t len = strlen(s);

  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
    s[--len] = '\0';
  }
}

// Parse "7A 2B" into bytes[].
// Returns number of parsed bytes, or 0 if invalid.
uint8_t parseCommandBytes(const char* text, uint8_t out[MAX_CMD_BYTES]) {
  if (!text) return 0;

  char temp[64];
  strncpy(temp, text, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';

  uint8_t count = 0;

  char* save = nullptr;
  char* tok = strtok_r(temp, " \t", &save);

  while (tok && count < MAX_CMD_BYTES) {
    if (strlen(tok) > 2) return 0;

    char* endPtr = nullptr;
    unsigned long v = strtoul(tok, &endPtr, 16);

    if (endPtr == tok || *endPtr != '\0' || v > 0xFF) {
      return 0;
    }

    out[count++] = (uint8_t)v;

    tok = strtok_r(nullptr, " \t", &save);
  }

  return count;
}

// =====================================================================
// LOAD /sounds.csv
// =====================================================================

void loadSoundCommands() {
  soundCommandCount = 0;

  if (!sdOK) return;

  File f = SD.open("/sounds.csv", FILE_READ);

  if (!f) {
    Serial.println("sounds.csv: not found");
    return;
  }

  Serial.println("Loading /sounds.csv...");

  char line[256];

  while (f.available() && soundCommandCount < MAX_SOUND_COMMANDS) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';

    if (n == 0) continue;
    if (line[0] == '#') continue;

    char* fields[4] = {nullptr, nullptr, nullptr, nullptr};
    int fieldCount = parseCsvLine(line, fields, 4);

    if (fieldCount < 2) continue;

    // Skip header.
    if (
      strcasecmp(fields[0], "command") == 0 ||
      strcasecmp(fields[0], "command_hex") == 0
    ) {
      continue;
    }

    SoundCommand &cmd = soundCommands[soundCommandCount];

    cmd.length = parseCommandBytes(fields[0], cmd.bytes);

    if (cmd.length == 0) {
      continue;
    }

    strncpy(cmd.name, fields[1], sizeof(cmd.name) - 1);
    cmd.name[sizeof(cmd.name) - 1] = '\0';

    cmd.status[0] = '\0';
    cmd.file[0] = '\0';

    if (fieldCount >= 3 && fields[2]) {
      strncpy(cmd.status, fields[2], sizeof(cmd.status) - 1);
      cmd.status[sizeof(cmd.status) - 1] = '\0';
    }

    if (fieldCount >= 4 && fields[3]) {
      strncpy(cmd.file, fields[3], sizeof(cmd.file) - 1);
      cmd.file[sizeof(cmd.file) - 1] = '\0';
    }

    soundCommandCount++;
  }

  f.close();

  Serial.print("Sound commands loaded: ");
  Serial.println(soundCommandCount);
}

// =====================================================================
// COMMAND MATCHING
// =====================================================================

int findExactCommand(const uint8_t* seq, uint8_t len) {
  for (uint16_t i = 0; i < soundCommandCount; i++) {
    if (soundCommands[i].length != len) continue;

    bool same = true;

    for (uint8_t j = 0; j < len; j++) {
      if (soundCommands[i].bytes[j] != seq[j]) {
        same = false;
        break;
      }
    }

    if (same) return (int)i;
  }

  return -1;
}

bool anyCommandStartsWith(const uint8_t* seq, uint8_t len) {
  for (uint16_t i = 0; i < soundCommandCount; i++) {
    if (soundCommands[i].length < len) continue;

    bool same = true;

    for (uint8_t j = 0; j < len; j++) {
      if (soundCommands[i].bytes[j] != seq[j]) {
        same = false;
        break;
      }
    }

    if (same) return true;
  }

  return false;
}

void seqToText(const uint8_t* seq, uint8_t len, char* out, size_t outSize) {
  out[0] = '\0';

  size_t pos = 0;

  for (uint8_t i = 0; i < len; i++) {
    int wrote = snprintf(
      out + pos,
      outSize - pos,
      i ? " %02X" : "%02X",
      seq[i]
    );

    if (wrote <= 0) break;

    pos += (size_t)wrote;

    if (pos >= outSize) break;
  }
}

void clearActiveSequence() {
  activeSeqLen = 0;
  activeSeqLastUs = 0;
}

void finalizeActiveSequence() {
  if (activeSeqLen == 0) return;

  int match = findExactCommand(activeSeq, activeSeqLen);

  char seqText[32];
  seqToText(activeSeq, activeSeqLen, seqText, sizeof(seqText));

  if (match >= 0) {
    strncpy(lastMatchedCommand, seqText, sizeof(lastMatchedCommand) - 1);
    lastMatchedCommand[sizeof(lastMatchedCommand) - 1] = '\0';

    strncpy(
      lastMatchedName,
      soundCommands[match].name,
      sizeof(lastMatchedName) - 1
    );
    lastMatchedName[sizeof(lastMatchedName) - 1] = '\0';

    strncpy(
      lastMatchedStatus,
      soundCommands[match].status,
      sizeof(lastMatchedStatus) - 1
    );
    lastMatchedStatus[sizeof(lastMatchedStatus) - 1] = '\0';

    Serial.print("MATCH ");
    Serial.print(seqText);
    Serial.print(" -> ");
    Serial.print(lastMatchedName);

    if (lastMatchedStatus[0]) {
      Serial.print(" [");
      Serial.print(lastMatchedStatus);
      Serial.print("]");
    }

    Serial.println();
  }

  clearActiveSequence();
}

void addSoundByteToSequence(uint8_t b, uint32_t timeUs) {
  // Finalize old sequence if the gap is too large.
  if (
    activeSeqLen > 0 &&
    (uint32_t)(timeUs - activeSeqLastUs) > COMMAND_GAP_US
  ) {
    finalizeActiveSequence();
  }

  // If full, finalize and start again.
  if (activeSeqLen >= MAX_CMD_BYTES) {
    finalizeActiveSequence();
  }

  activeSeq[activeSeqLen++] = b;
  activeSeqLastUs = timeUs;

  // If no known command begins with the current sequence, try salvaging
  // the newest byte as a possible new command start.
  if (!anyCommandStartsWith(activeSeq, activeSeqLen)) {
    uint8_t last = activeSeq[activeSeqLen - 1];

    clearActiveSequence();

    activeSeq[0] = last;
    activeSeqLen = 1;
    activeSeqLastUs = timeUs;

    // If even the newest byte is not a prefix, discard it.
    if (!anyCommandStartsWith(activeSeq, activeSeqLen)) {
      clearActiveSequence();
      return;
    }
  }

  // Exact match:
  // If this exact sequence is also the prefix of a longer known command,
  // wait for the gap before finalizing.
  int exact = findExactCommand(activeSeq, activeSeqLen);

  if (exact >= 0) {
    bool longerPossible = false;

    for (uint16_t i = 0; i < soundCommandCount; i++) {
      if (soundCommands[i].length <= activeSeqLen) continue;

      bool prefix = true;

      for (uint8_t j = 0; j < activeSeqLen; j++) {
        if (soundCommands[i].bytes[j] != activeSeq[j]) {
          prefix = false;
          break;
        }
      }

      if (prefix) {
        longerPossible = true;
        break;
      }
    }

    if (!longerPossible) {
      finalizeActiveSequence();
    }
  }
}

void serviceSequenceTimeout() {
  if (
    activeSeqLen > 0 &&
    (uint32_t)(micros() - activeSeqLastUs) > COMMAND_GAP_US
  ) {
    finalizeActiveSequence();
  }
}

// =====================================================================
// RAW SD LOGGING
// =====================================================================

bool openLog() {
  if (!sdOK) return false;

  logFile = SD.open("/buslog.csv", FILE_APPEND);

  if (!logFile) return false;

  if (logFile.size() == 0) {
    logFile.println(
      "micros,address_hex,data_hex,rw,reset,is_sound_write"
    );
    logFile.flush();
  }

  return true;
}

void logBusEvent(const BusEvent &ev, bool soundWrite) {
  if (!loggingEnabled || !logFile) return;

  char line[96];

  snprintf(
    line,
    sizeof(line),
    "%lu,%02X,%02X,%u,%u,%u",
    (unsigned long)ev.timeUs,
    ev.address,
    ev.data,
    ev.rw,
    ev.reset,
    soundWrite ? 1 : 0
  );

  logFile.println(line);

  static uint8_t pendingFlush = 0;

  pendingFlush++;

  if (pendingFlush >= 16) {
    logFile.flush();
    pendingFlush = 0;
  }
}

// =====================================================================
// RECENT EVENT LIST
// =====================================================================

void addRecent(const BusEvent &ev, bool soundWrite) {
  uint8_t last =
    recentUsed < RECENT_COUNT
    ? recentUsed
    : RECENT_COUNT - 1;

  for (int i = last; i > 0; i--) {
    recent[i] = recent[i - 1];
  }

  recent[0].timeUs = ev.timeUs;
  recent[0].address = ev.address;
  recent[0].data = ev.data;
  recent[0].rw = ev.rw;
  recent[0].reset = ev.reset;
  recent[0].soundWrite = soundWrite;

  if (recentUsed < RECENT_COUNT) {
    recentUsed++;
  }
}

// =====================================================================
// PROCESS CAPTURED EVENTS
// =====================================================================

void processBusEvents() {
  BusEvent ev;

  while (popEvent(ev)) {
    bool soundWrite =
      (ev.address == SOUND_ADDRESS && ev.rw == 0);

    addRecent(ev, soundWrite);
    logBusEvent(ev, soundWrite);

    Serial.printf(
      "%10lu us  A=%02X  D=%02X  %s  RESET=%u%s\n",
      (unsigned long)ev.timeUs,
      ev.address,
      ev.data,
      ev.rw ? "R" : "W",
      ev.reset,
      soundWrite ? "  SOUND" : ""
    );

    if (soundWrite) {
      addSoundByteToSequence(ev.data, ev.timeUs);
    }
  }
}

// =====================================================================
// WEB ENDPOINTS
// =====================================================================

void handlePins() {
  char out[32];
  uint8_t pos = 0;

  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    out[pos++] = liveState[i] ? '1' : '0';

    if (i < PIN_COUNT - 1) {
      out[pos++] = ',';
    }
  }

  out[pos] = '\0';

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", out);
}

void handleStatus() {
  uint32_t edges;
  uint32_t drops;

  portENTER_CRITICAL(&eventMux);
  edges = totalWdenEdges;
  drops = droppedEvents;
  portEXIT_CRITICAL(&eventMux);

  char out[320];

  snprintf(
    out,
    sizeof(out),
    "SD:%s | LOG:%s | commands:%u | WDEN:%lu | dropped:%lu | last:%s%s%s",
    sdOK ? "OK" : "FAIL",
    loggingEnabled ? "ON" : "OFF",
    soundCommandCount,
    (unsigned long)edges,
    (unsigned long)drops,
    lastMatchedCommand[0] ? lastMatchedCommand : "-",
    lastMatchedName[0] ? " -> " : "",
    lastMatchedName[0] ? lastMatchedName : ""
  );

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", out);
}

void handleEvents() {
  String out;
  out.reserve(2200);

  for (uint8_t i = 0; i < recentUsed; i++) {
    char line[96];

    snprintf(
      line,
      sizeof(line),
      "%lu,%02X,%02X,%u,%u,%u\n",
      (unsigned long)recent[i].timeUs,
      recent[i].address,
      recent[i].data,
      recent[i].rw,
      recent[i].reset,
      recent[i].soundWrite ? 1 : 0
    );

    out += line;
  }

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", out);
}

void handleLogOn() {
  if (!sdOK) {
    server.send(500, "text/plain", "SD not ready");
    return;
  }

  if (!logFile && !openLog()) {
    server.send(500, "text/plain", "Could not open buslog.csv");
    return;
  }

  loggingEnabled = true;
  server.send(200, "text/plain", "Logging ON");
}

void handleLogOff() {
  loggingEnabled = false;

  if (logFile) {
    logFile.flush();
  }

  server.send(200, "text/plain", "Logging OFF");
}

void handleClearLog() {
  loggingEnabled = false;

  if (logFile) {
    logFile.flush();
    logFile.close();
  }

  SD.remove("/buslog.csv");

  bool ok = openLog();

  loggingEnabled = ok;

  server.send(
    ok ? 200 : 500,
    "text/plain",
    ok ? "Log cleared and logging ON" : "Log clear/open failed"
  );
}

void handleDownloadLog() {
  if (!sdOK) {
    server.send(500, "text/plain", "SD not ready");
    return;
  }

  if (logFile) {
    logFile.flush();
  }

  File f = SD.open("/buslog.csv", FILE_READ);

  if (!f) {
    server.send(404, "text/plain", "No buslog.csv");
    return;
  }

  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=\"buslog.csv\""
  );

  server.streamFile(f, "text/csv");
  f.close();
}

// =====================================================================
// MINIMAL WEB PAGE
// =====================================================================

const char PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FunHouse WPC Sniffer V1.1</title>
<style>
body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:18px}
h2{margin-bottom:5px}
.note{color:#aaa;margin-bottom:14px}
.grid{display:grid;grid-template-columns:repeat(4,minmax(100px,1fr));gap:7px;max-width:780px}
.pin{background:#222;border:1px solid #444;border-radius:6px;padding:9px}
.led{width:14px;height:14px;display:inline-block;border-radius:50%;background:#555;margin-right:7px}
.on{background:#5cff72;box-shadow:0 0 7px #5cff72}
.value{float:right;font-family:monospace}
.panel{background:#1b1b1b;border:1px solid #444;border-radius:6px;padding:11px;margin-top:13px;max-width:758px}
button,a.button{display:inline-block;background:#333;color:#fff;border:1px solid #666;border-radius:5px;padding:8px 10px;margin:2px;text-decoration:none;cursor:pointer}
table{width:100%;border-collapse:collapse;font-family:Consolas,monospace;font-size:12px}
td,th{border-bottom:1px solid #333;padding:5px;text-align:left}
.sound{color:#72ff8d;font-weight:bold}
.match{color:#72ff8d;font-weight:bold;margin-top:8px}
@media(max-width:650px){.grid{grid-template-columns:repeat(2,minmax(100px,1fr))}}
</style>
</head>
<body>

<h2>FunHouse WPC Sniffer V1.1</h2>
<div class="note">
CA2=GPIO25 • sound address=0x1C • WDEN rising edge • multi-byte /sounds.csv
</div>

<div id="pins" class="grid"></div>

<div class="panel">
  <div id="status">loading...</div>
  <div style="margin-top:8px">
    <button onclick="cmd('/log/on')">LOG ON</button>
    <button onclick="cmd('/log/off')">LOG OFF</button>
    <button onclick="cmd('/log/clear')">CLEAR LOG</button>
    <a class="button" href="/log/download">DOWNLOAD CSV</a>
  </div>
</div>

<div class="panel">
  <b>Recent WDEN captures</b>
  <table>
    <thead>
      <tr>
        <th>us</th>
        <th>A</th>
        <th>D</th>
        <th>R/W</th>
        <th>RST</th>
        <th>TYPE</th>
      </tr>
    </thead>
    <tbody id="events"></tbody>
  </table>
</div>

<script>
const names=[
  "CD7","CD6","CD5","CD4",
  "CD3","CD2","CD1","CD0",
  "CA4","CA3","CA2","CA1",
  "CA0","WDEN","R/W","RESET"
];

const gpios=[
  34,35,32,33,27,14,13,12,
  15,4,25,21,16,17,36,39
];

const pinBox=document.getElementById("pins");

for(let i=0;i<16;i++){
  const d=document.createElement("div");
  d.className="pin";
  d.innerHTML=
    '<span id="l'+i+'" class="led"></span>'+
    '<b>'+names[i]+'</b>'+
    '<span id="v'+i+'" class="value">?</span>'+
    '<div style="font-size:11px;color:#888;margin-top:4px">GPIO'+gpios[i]+'</div>';
  pinBox.appendChild(d);
}

async function updatePins(){
  try{
    const r=await fetch("/pins",{cache:"no-store"});
    const a=(await r.text()).trim().split(",");
    if(a.length!==16)return;

    for(let i=0;i<16;i++){
      const on=a[i]==="1";
      document.getElementById("l"+i).className=on?"led on":"led";
      document.getElementById("v"+i).textContent=on?"1":"0";
    }
  }catch(e){}
}

async function updateStatus(){
  try{
    const r=await fetch("/status",{cache:"no-store"});
    document.getElementById("status").textContent=await r.text();
  }catch(e){}
}

async function updateEvents(){
  try{
    const r=await fetch("/events",{cache:"no-store"});
    const t=await r.text();
    let html="";

    if(t.trim().length){
      for(const row of t.trim().split("\n")){
        const c=row.split(",");
        if(c.length<6)continue;

        html +=
          '<tr>'+
          '<td>'+c[0]+'</td>'+
          '<td>0x'+c[1]+'</td>'+
          '<td>0x'+c[2]+'</td>'+
          '<td>'+(c[3]==="0"?"W":"R")+'</td>'+
          '<td>'+c[4]+'</td>'+
          '<td>'+(c[5]==="1"?'<span class="sound">SOUND</span>':'')+'</td>'+
          '</tr>';
      }
    }

    document.getElementById("events").innerHTML=html;
  }catch(e){}
}

async function cmd(url){
  try{
    const r=await fetch(url,{method:"POST"});
    document.getElementById("status").textContent=await r.text();
    setTimeout(updateStatus,300);
  }catch(e){}
}

setInterval(updatePins,500);
setInterval(updateStatus,1000);
setInterval(updateEvents,750);

updatePins();
updateStatus();
updateEvents();
</script>
</body>
</html>
)HTML";

// =====================================================================
// WEB SETUP
// =====================================================================

void setupWeb() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", PAGE);
  });

  server.on("/pins", HTTP_GET, handlePins);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/events", HTTP_GET, handleEvents);

  server.on("/log/on", HTTP_POST, handleLogOn);
  server.on("/log/off", HTTP_POST, handleLogOff);
  server.on("/log/clear", HTTP_POST, handleClearLog);
  server.on("/log/download", HTTP_GET, handleDownloadLog);

  server.begin();
}

// =====================================================================
// SETUP
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" FUNHOUSE WPC SNIFFER V1.1");
  Serial.println("======================================");

  // 1. WiFi first.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);

  delay(300);

  // 2. Proven GPIO25/CA2 fix.
  reclaimWpcPins();
  sampleLivePins();

  Serial.println("WPC GPIOs reclaimed after Wi-Fi.");

  // 3. SD.
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  sdOK = SD.begin(SD_CS, sdSPI, 4000000);

  Serial.print("SD: ");
  Serial.println(sdOK ? "OK" : "FAILED");

  if (sdOK) {
    loadSoundCommands();

    if (openLog()) {
      loggingEnabled = true;
      Serial.println("buslog.csv: logging ON");
    } else {
      loggingEnabled = false;
      Serial.println("buslog.csv: could not open");
    }
  }

  // 4. Web.
  setupWeb();

  Serial.print("Web: http://");
  Serial.println(WiFi.softAPIP());

  // 5. Capture last.
  attachInterrupt(
    digitalPinToInterrupt(PIN_WDEN),
    onWdenRise,
    RISING
  );

  Serial.println("WDEN capture armed.");
  Serial.println("Expected sound address: 0x1C");
  Serial.println("Sequence gap: 5000 us");
  Serial.println();
}

// =====================================================================
// LOOP
// =====================================================================

void loop() {
  sampleLivePins();

  processBusEvents();

  // Finalize a multi-byte sequence after its inter-byte gap expires.
  serviceSequenceTimeout();

  server.handleClient();

  delay(1);
}