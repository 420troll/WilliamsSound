/*
  ======================================================================
  FUNHOUSE WPC WIFI SNIFFER / LOGGER / DAC TEST
  Rebuilt from the ground up for the proven wiring on this project.

  HARDWARE ASSUMPTIONS
  --------------------
  ESP32 DevKit / WROOM-32, 30-pin board
  Two TXS0108E level translators:
      WPC 5V bus -> TXS B side
      ESP32 3.3V -> TXS A side
      OE = 3.3V
      Common GND

  IMPORTANT VERIFIED WPC PIN MAP
  ------------------------------
  J506 odd pins are signals, even pins are GND.

  Data:
      CD7   GPIO34
      CD6   GPIO35
      CD5   GPIO32
      CD4   GPIO33
      CD3   GPIO27
      CD2   GPIO14
      CD1   GPIO13
      CD0   GPIO12

  Address/control:
      CA4   GPIO15
      CA3   GPIO4
      CA2   GPIO25   <-- VERIFIED WORKING HERE
      CA1   GPIO21
      CA0   GPIO16
      WDEN  GPIO17
      R/W   GPIO36
      RESET GPIO39

  microSD:
      CS    GPIO5
      SCK   GPIO18
      MISO  GPIO19
      MOSI  GPIO23

  PCM5102A:
      BCK   GPIO26
      LCK   GPIO1 / TX0
      DIN   GPIO22
      SCK   open

  GPIO1 is deliberately shared with UART0 TX.
  Serial works normally until DAC playback begins.
  During DAC playback, Serial is shut down, GPIO1 becomes I2S LRCK/LCK,
  and the web interface + SD logging continue. Serial is restored after
  the WAV finishes.

  EXPECTED FUNHOUSE SOUND ACCESS
  ------------------------------
  Sound-board address:
      CA4..CA0 = 11100b = 0x1C
  WPC write:
      R/W = LOW
  Sound latch:
      WDEN rising edge

  Wi-Fi AP:
      SSID:     FUNHOUSE-SNIFFER
      Password: funhouse89
      URL:      http://192.168.4.1

  Serial:
      115200 baud

  SD files:
      /buslog.csv
      /test.wav      (used by DAC test button)

  DESIGN GOALS
  ------------
  1. Every one of the 16 proven input pins is read directly.
  2. Live pin display is independent from sound-command decoding.
  3. Transition counters show whether every line is actually changing.
  4. WDEN rising edges are captured in an ISR into a ring buffer.
  5. ISR does NO Serial, SD, String, Wi-Fi, allocation, or file work.
  6. SD logging happens later in loop(), outside the ISR.
  7. CA2 is GPIO25 everywhere in this program.
  8. The DAC is completely inactive until explicitly requested.
  ======================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <ESP_I2S.h>
#include "driver/gpio.h"

// ---------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------

static const char* WIFI_SSID = "FUNHOUSE-SNIFFER";
static const char* WIFI_PASS = "funhouse89";

WebServer server(80);

// ---------------------------------------------------------------------
// VERIFIED WPC INPUT MAP
// ---------------------------------------------------------------------

static const uint8_t WPC_PIN_COUNT = 16;

static const uint8_t wpcPins[WPC_PIN_COUNT] = {
  34, //  0 CD7
  35, //  1 CD6
  32, //  2 CD5
  33, //  3 CD4
  27, //  4 CD3
  14, //  5 CD2
  13, //  6 CD1
  12, //  7 CD0

  15, //  8 CA4
  4,  //  9 CA3
  25, // 10 CA2  *** VERIFIED ***
  21, // 11 CA1
  16, // 12 CA0
  17, // 13 WDEN
  36, // 14 R/W
  39  // 15 RESET
};

static const char* wpcNames[WPC_PIN_COUNT] = {
  "CD7","CD6","CD5","CD4","CD3","CD2","CD1","CD0",
  "CA4","CA3","CA2","CA1","CA0","WDEN","R/W","RESET"
};

static const uint8_t PIN_WDEN  = 17;
static const uint8_t PIN_RW    = 36;
static const uint8_t PIN_RESET = 39;

static const uint8_t SOUND_ADDR = 0x1C;

// ---------------------------------------------------------------------
// SD MAP
// ---------------------------------------------------------------------

static const uint8_t SD_CS   = 5;
static const uint8_t SD_SCK  = 18;
static const uint8_t SD_MISO = 19;
static const uint8_t SD_MOSI = 23;

SPIClass sdSPI(VSPI);

bool sdReady = false;
bool loggingEnabled = false;
File logFile;

// ---------------------------------------------------------------------
// DAC MAP
// ---------------------------------------------------------------------

static const uint8_t DAC_BCK = 26;
static const uint8_t DAC_LCK = 1;   // UART0 TX when Serial is active
static const uint8_t DAC_DIN = 22;

I2SClass I2S;

bool dacActive = false;
bool serialSuspendedForDAC = false;
File wavFile;

uint16_t wavChannels = 0;
uint16_t wavBits = 0;
uint32_t wavSampleRate = 0;
uint32_t wavDataBytesRemaining = 0;

// ---------------------------------------------------------------------
// LIVE PIN STATE + TRANSITION COUNTERS
//
// The loop samples all 16 inputs every pass.
// No INPUT_PULLUP or INPUT_PULLDOWN is used: the TXS/WPC bus defines state.
// ---------------------------------------------------------------------

uint8_t liveState[WPC_PIN_COUNT] = {0};
uint8_t previousState[WPC_PIN_COUNT] = {0};
uint32_t transitionCount[WPC_PIN_COUNT] = {0};

// ---------------------------------------------------------------------
// WDEN ISR RING BUFFER
//
// Every WDEN rising edge gets a timestamp + bus snapshot.
// The ISR stores only primitive values into a fixed ring buffer.
// ---------------------------------------------------------------------

struct BusEvent {
  uint32_t micros32;
  uint8_t address;
  uint8_t data;
  uint8_t rw;
  uint8_t reset;
};

static const uint16_t EVENT_RING_SIZE = 256;

volatile BusEvent eventRing[EVENT_RING_SIZE];
volatile uint16_t eventHead = 0;
volatile uint16_t eventTail = 0;
volatile uint32_t droppedEvents = 0;
volatile uint32_t totalWdenEdges = 0;

// Critical-section lock shared by ISR and loop.
portMUX_TYPE eventMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------
// RECENT EVENTS FOR WEB DISPLAY
//
// This is separate from the ISR ring. These are normal RAM objects,
// updated in loop() after safely copying events out of the ISR queue.
// ---------------------------------------------------------------------

struct RecentEvent {
  uint32_t micros32;
  uint8_t address;
  uint8_t data;
  uint8_t rw;
  uint8_t reset;
  bool isSoundWrite;
};

static const uint8_t RECENT_COUNT = 32;
RecentEvent recentEvents[RECENT_COUNT];
uint8_t recentUsed = 0;

// ---------------------------------------------------------------------
// ISR-SAFE BUS READ HELPERS
// ---------------------------------------------------------------------

static inline uint8_t IRAM_ATTR readDataBusISR() {
  // CD7..CD0 are stored in wpcPins[0..7].
  // Build normal 8-bit value: bit 7 = CD7, bit 0 = CD0.
  uint8_t value = 0;

  value |= gpio_get_level((gpio_num_t)34) ? 0x80 : 0;
  value |= gpio_get_level((gpio_num_t)35) ? 0x40 : 0;
  value |= gpio_get_level((gpio_num_t)32) ? 0x20 : 0;
  value |= gpio_get_level((gpio_num_t)33) ? 0x10 : 0;
  value |= gpio_get_level((gpio_num_t)27) ? 0x08 : 0;
  value |= gpio_get_level((gpio_num_t)14) ? 0x04 : 0;
  value |= gpio_get_level((gpio_num_t)13) ? 0x02 : 0;
  value |= gpio_get_level((gpio_num_t)12) ? 0x01 : 0;

  return value;
}

static inline uint8_t IRAM_ATTR readAddressBusISR() {
  // CA4..CA0 = 5-bit address.
  uint8_t value = 0;

  value |= gpio_get_level((gpio_num_t)15) ? 0x10 : 0; // CA4
  value |= gpio_get_level((gpio_num_t)4)  ? 0x08 : 0; // CA3
  value |= gpio_get_level((gpio_num_t)25) ? 0x04 : 0; // CA2 VERIFIED
  value |= gpio_get_level((gpio_num_t)21) ? 0x02 : 0; // CA1
  value |= gpio_get_level((gpio_num_t)16) ? 0x01 : 0; // CA0

  return value;
}

// ---------------------------------------------------------------------
// WDEN RISING-EDGE INTERRUPT
// ---------------------------------------------------------------------

void IRAM_ATTR onWdenRise() {
  BusEvent ev;

  ev.micros32 = micros();
  ev.address  = readAddressBusISR();
  ev.data     = readDataBusISR();
  ev.rw       = gpio_get_level((gpio_num_t)PIN_RW) ? 1 : 0;
  ev.reset    = gpio_get_level((gpio_num_t)PIN_RESET) ? 1 : 0;

  portENTER_CRITICAL_ISR(&eventMux);

  totalWdenEdges++;

  uint16_t next = (uint16_t)((eventHead + 1) % EVENT_RING_SIZE);

  if (next == eventTail) {
    // Ring is full. Drop this event rather than corrupt the queue.
    droppedEvents++;
  } else {
    eventRing[eventHead].micros32 = ev.micros32;
    eventRing[eventHead].address  = ev.address;
    eventRing[eventHead].data     = ev.data;
    eventRing[eventHead].rw       = ev.rw;
    eventRing[eventHead].reset    = ev.reset;
    eventHead = next;
  }

  portEXIT_CRITICAL_ISR(&eventMux);
}

// ---------------------------------------------------------------------
// COPY ONE ISR EVENT SAFELY INTO NORMAL CODE
// ---------------------------------------------------------------------

bool popBusEvent(BusEvent &out) {
  bool haveEvent = false;

  portENTER_CRITICAL(&eventMux);

  if (eventTail != eventHead) {
    out.micros32 = eventRing[eventTail].micros32;
    out.address  = eventRing[eventTail].address;
    out.data     = eventRing[eventTail].data;
    out.rw       = eventRing[eventTail].rw;
    out.reset    = eventRing[eventTail].reset;

    eventTail = (uint16_t)((eventTail + 1) % EVENT_RING_SIZE);
    haveEvent = true;
  }

  portEXIT_CRITICAL(&eventMux);

  return haveEvent;
}

// ---------------------------------------------------------------------
// LIVE PIN SAMPLING
// ---------------------------------------------------------------------

void sampleAllWpcPins() {
  for (uint8_t i = 0; i < WPC_PIN_COUNT; i++) {
    uint8_t now = digitalRead(wpcPins[i]) ? 1 : 0;
    liveState[i] = now;

    if (now != previousState[i]) {
      transitionCount[i]++;
      previousState[i] = now;
    }
  }
}

// ---------------------------------------------------------------------
// SIMPLE KNOWN-COMMAND LABELS
//
// We deliberately keep this tiny. The raw byte/address capture remains
// authoritative. More names can later come from a CSV mapping.
// ---------------------------------------------------------------------

const char* knownSoundName(uint8_t data) {
  switch (data) {
    case 0x07: return "Midnight";
    case 0x90: return "Tilt";
    case 0x93: return "Tilt Warning";
    case 0xB8: return "Trap Door Open";
    default:   return "";
  }
}

// ---------------------------------------------------------------------
// RECENT EVENT HISTORY
// ---------------------------------------------------------------------

void addRecentEvent(const BusEvent &ev) {
  // Shift older entries down. Only 32 items, so simplicity wins here.
  uint8_t top = recentUsed < RECENT_COUNT ? recentUsed : (RECENT_COUNT - 1);

  for (int i = top; i > 0; i--) {
    recentEvents[i] = recentEvents[i - 1];
  }

  RecentEvent &r = recentEvents[0];

  r.micros32 = ev.micros32;
  r.address = ev.address;
  r.data = ev.data;
  r.rw = ev.rw;
  r.reset = ev.reset;
  r.isSoundWrite = (ev.address == SOUND_ADDR && ev.rw == 0);

  if (recentUsed < RECENT_COUNT) recentUsed++;
}

// ---------------------------------------------------------------------
// SD LOGGING
// ---------------------------------------------------------------------

void startLogging() {
  if (!sdReady || loggingEnabled) return;

  logFile = SD.open("/buslog.csv", FILE_APPEND);

  if (!logFile) return;

  if (logFile.size() == 0) {
    logFile.println("micros,address_hex,data_hex,rw,reset,is_sound_write,name");
    logFile.flush();
  }

  loggingEnabled = true;
}

void stopLogging() {
  loggingEnabled = false;

  if (logFile) {
    logFile.flush();
    logFile.close();
  }
}

void logEventToSD(const BusEvent &ev) {
  if (!loggingEnabled || !logFile) return;

  bool soundWrite = (ev.address == SOUND_ADDR && ev.rw == 0);
  const char* name = soundWrite ? knownSoundName(ev.data) : "";

  char line[160];

  snprintf(
    line, sizeof(line),
    "%lu,%02X,%02X,%u,%u,%u,%s",
    (unsigned long)ev.micros32,
    ev.address,
    ev.data,
    ev.rw,
    ev.reset,
    soundWrite ? 1 : 0,
    name
  );

  logFile.println(line);

  // Flush occasionally for safety without flushing every single event.
  static uint16_t unflushed = 0;
  unflushed++;

  if (unflushed >= 16) {
    logFile.flush();
    unflushed = 0;
  }
}

// ---------------------------------------------------------------------
// PROCESS ALL CAPTURED WDEN EVENTS
// ---------------------------------------------------------------------

void processCapturedEvents() {
  BusEvent ev;

  while (popBusEvent(ev)) {
    addRecentEvent(ev);
    logEventToSD(ev);

    // Serial is deliberately silent while DAC owns GPIO1.
    if (!serialSuspendedForDAC) {
      bool soundWrite = (ev.address == SOUND_ADDR && ev.rw == 0);

      Serial.printf(
        "%10lu us  A=%02X D=%02X  %s  RW=%u RESET=%u%s",
        (unsigned long)ev.micros32,
        ev.address,
        ev.data,
        ev.rw ? "READ " : "WRITE",
        ev.rw,
        ev.reset,
        soundWrite ? "  SOUND" : ""
      );

      if (soundWrite) {
        const char* name = knownSoundName(ev.data);
        if (name[0]) {
          Serial.print("  ");
          Serial.print(name);
        }
      }

      Serial.println();
    }
  }
}

// ---------------------------------------------------------------------
// WAV PARSER
//
// Finds fmt and data chunks instead of assuming a fixed 44-byte header.
// Supports PCM, 16-bit, mono or stereo.
// ---------------------------------------------------------------------

bool readExact(File &f, void* dst, size_t bytes) {
  return f.read((uint8_t*)dst, bytes) == (int)bytes;
}

uint32_t readLE32(File &f, bool &ok) {
  uint8_t b[4];
  ok = readExact(f, b, 4);
  if (!ok) return 0;

  return (uint32_t)b[0]
       | ((uint32_t)b[1] << 8)
       | ((uint32_t)b[2] << 16)
       | ((uint32_t)b[3] << 24);
}

uint16_t readLE16(File &f, bool &ok) {
  uint8_t b[2];
  ok = readExact(f, b, 2);
  if (!ok) return 0;

  return (uint16_t)b[0]
       | ((uint16_t)b[1] << 8);
}

bool openAndParseWav(const char* path) {
  if (!sdReady) return false;

  wavFile = SD.open(path, FILE_READ);
  if (!wavFile) return false;

  char id[4];
  bool ok = true;

  if (!readExact(wavFile, id, 4) || memcmp(id, "RIFF", 4) != 0) {
    wavFile.close();
    return false;
  }

  (void)readLE32(wavFile, ok);
  if (!ok) {
    wavFile.close();
    return false;
  }

  if (!readExact(wavFile, id, 4) || memcmp(id, "WAVE", 4) != 0) {
    wavFile.close();
    return false;
  }

  bool gotFmt = false;
  bool gotData = false;
  uint16_t audioFormat = 0;

  while (wavFile.available()) {
    if (!readExact(wavFile, id, 4)) break;

    uint32_t chunkSize = readLE32(wavFile, ok);
    if (!ok) break;

    if (memcmp(id, "fmt ", 4) == 0) {
      audioFormat   = readLE16(wavFile, ok);
      wavChannels   = readLE16(wavFile, ok);
      wavSampleRate = readLE32(wavFile, ok);

      (void)readLE32(wavFile, ok); // byte rate
      (void)readLE16(wavFile, ok); // block align
      wavBits       = readLE16(wavFile, ok);

      if (!ok) break;

      if (chunkSize > 16) {
        wavFile.seek(wavFile.position() + (chunkSize - 16));
      }

      gotFmt = true;
    }
    else if (memcmp(id, "data", 4) == 0) {
      wavDataBytesRemaining = chunkSize;
      gotData = true;
      break;
    }
    else {
      // Skip any chunk we do not need.
      wavFile.seek(wavFile.position() + chunkSize);
    }

    // RIFF chunks are word-aligned.
    if (chunkSize & 1) {
      wavFile.seek(wavFile.position() + 1);
    }
  }

  if (!gotFmt || !gotData ||
      audioFormat != 1 ||
      wavBits != 16 ||
      (wavChannels != 1 && wavChannels != 2) ||
      wavSampleRate == 0) {

    wavFile.close();
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------
// DAC TEST START
//
// GPIO1/TX0 is shared with Serial.
// We shut Serial down BEFORE enabling I2S.
// ---------------------------------------------------------------------

bool startDacTest() {
  if (dacActive) return true;
  if (!sdReady) return false;

  if (!openAndParseWav("/test.wav")) {
    return false;
  }

  Serial.println();
  Serial.println("DAC TEST: /test.wav");
  Serial.printf("WAV: %lu Hz, %u-bit, %u channel(s)\n",
                (unsigned long)wavSampleRate,
                wavBits,
                wavChannels);
  Serial.println("Releasing GPIO1/TX0 from Serial for DAC LCK...");
  Serial.flush();
  delay(100);

  Serial.end();
  serialSuspendedForDAC = true;
  delay(20);

  I2S.setPins(
    DAC_BCK,
    DAC_LCK,
    DAC_DIN,
    -1,
    -1
  );

  if (!I2S.begin(
        I2S_MODE_STD,
        wavSampleRate,
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO)) {

    wavFile.close();

    serialSuspendedForDAC = false;
    Serial.begin(115200);
    delay(100);
    Serial.println("I2S START FAILED");

    return false;
  }

  dacActive = true;
  return true;
}

// ---------------------------------------------------------------------
// DAC PLAYBACK SERVICE
//
// Small chunks keep the web server responsive.
// ---------------------------------------------------------------------

void serviceDacPlayback() {
  if (!dacActive) return;

  static uint8_t raw[512];
  static int16_t stereo[256]; // 128 stereo frames

  if (wavDataBytesRemaining == 0 || !wavFile.available()) {
    wavFile.close();
    I2S.end();

    dacActive = false;

    // Restore Serial after GPIO1 is released by I2S.
    serialSuspendedForDAC = false;
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println("DAC playback finished. Serial restored.");
    return;
  }

  uint32_t want = sizeof(raw);
  if (want > wavDataBytesRemaining) want = wavDataBytesRemaining;

  int got = wavFile.read(raw, want);
  if (got <= 0) {
    wavDataBytesRemaining = 0;
    return;
  }

  wavDataBytesRemaining -= got;

  if (wavChannels == 2) {
    // Stereo PCM16 can be written directly.
    I2S.write(raw, got);
  }
  else {
    // Mono PCM16: duplicate each sample into left + right.
    int samples = got / 2;

    if (samples > 128) samples = 128;

    for (int i = 0; i < samples; i++) {
      int16_t s = (int16_t)(
        (uint16_t)raw[i * 2]
        | ((uint16_t)raw[i * 2 + 1] << 8)
      );

      stereo[i * 2]     = s;
      stereo[i * 2 + 1] = s;
    }

    I2S.write((uint8_t*)stereo, samples * 4);
  }
}

// ---------------------------------------------------------------------
// JSON HELPERS
// ---------------------------------------------------------------------

void sendJsonHeader() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
}

// ---------------------------------------------------------------------
// /api/state
//
// Current values for all 16 pins + transition counters.
// ---------------------------------------------------------------------

void handleApiState() {
  String json;
  json.reserve(2200);

  json += "{";

  json += "\"pins\":[";
  for (uint8_t i = 0; i < WPC_PIN_COUNT; i++) {
    if (i) json += ",";

    json += "{";
    json += "\"name\":\"";
    json += wpcNames[i];
    json += "\",\"gpio\":";
    json += wpcPins[i];
    json += ",\"state\":";
    json += liveState[i];
    json += ",\"changes\":";
    json += transitionCount[i];
    json += "}";
  }
  json += "],";

  uint32_t edges;
  uint32_t drops;

  portENTER_CRITICAL(&eventMux);
  edges = totalWdenEdges;
  drops = droppedEvents;
  portEXIT_CRITICAL(&eventMux);

  json += "\"wdenEdges\":";
  json += edges;

  json += ",\"dropped\":";
  json += drops;

  json += ",\"sdReady\":";
  json += sdReady ? "true" : "false";

  json += ",\"logging\":";
  json += loggingEnabled ? "true" : "false";

  json += ",\"dacActive\":";
  json += dacActive ? "true" : "false";

  json += "}";

  sendJsonHeader();
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------
// /api/events
// ---------------------------------------------------------------------

void handleApiEvents() {
  String json;
  json.reserve(5000);

  json += "[";

  for (uint8_t i = 0; i < recentUsed; i++) {
    if (i) json += ",";

    const RecentEvent &r = recentEvents[i];

    json += "{";
    json += "\"us\":";
    json += r.micros32;
    json += ",\"addr\":";
    json += r.address;
    json += ",\"data\":";
    json += r.data;
    json += ",\"rw\":";
    json += r.rw;
    json += ",\"reset\":";
    json += r.reset;
    json += ",\"sound\":";
    json += r.isSoundWrite ? "true" : "false";

    if (r.isSoundWrite) {
      const char* name = knownSoundName(r.data);
      json += ",\"name\":\"";
      json += name;
      json += "\"";
    }

    json += "}";
  }

  json += "]";

  sendJsonHeader();
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------
// WEB ACTIONS
// ---------------------------------------------------------------------

void handleStartLog() {
  if (!sdReady) {
    server.send(500, "text/plain", "SD not ready");
    return;
  }

  startLogging();

  server.send(
    loggingEnabled ? 200 : 500,
    "text/plain",
    loggingEnabled ? "Logging started" : "Could not open /buslog.csv"
  );
}

void handleStopLog() {
  stopLogging();
  server.send(200, "text/plain", "Logging stopped");
}

void handleClearLog() {
  stopLogging();

  if (!sdReady) {
    server.send(500, "text/plain", "SD not ready");
    return;
  }

  SD.remove("/buslog.csv");

  server.send(200, "text/plain", "Log cleared");
}

void handleResetCounters() {
  noInterrupts();

  for (uint8_t i = 0; i < WPC_PIN_COUNT; i++) {
    transitionCount[i] = 0;
  }

  interrupts();

  portENTER_CRITICAL(&eventMux);
  droppedEvents = 0;
  totalWdenEdges = 0;
  eventHead = 0;
  eventTail = 0;
  portEXIT_CRITICAL(&eventMux);

  recentUsed = 0;

  server.send(200, "text/plain", "Counters reset");
}

void handleDownloadLog() {
  if (!sdReady) {
    server.send(500, "text/plain", "SD not ready");
    return;
  }

  // Flush open log before download.
  if (loggingEnabled && logFile) {
    logFile.flush();
  }

  File f = SD.open("/buslog.csv", FILE_READ);

  if (!f) {
    server.send(404, "text/plain", "No /buslog.csv");
    return;
  }

  server.sendHeader(
    "Content-Disposition",
    "attachment; filename=\"buslog.csv\""
  );

  server.streamFile(f, "text/csv");
  f.close();
}

void handleDacTest() {
  if (dacActive) {
    server.send(409, "text/plain", "DAC already playing");
    return;
  }

  // Send the HTTP reply BEFORE Serial is shut down and playback starts.
  server.send(200, "text/plain",
              "Starting /test.wav. Serial will pause while DAC owns GPIO1.");

  delay(30);
  startDacTest();
}

// ---------------------------------------------------------------------
// MAIN WEB PAGE
// ---------------------------------------------------------------------

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FunHouse WPC Sniffer</title>

<style>
body{
  margin:0;
  font-family:Arial,Helvetica,sans-serif;
  background:#151515;
  color:#eee;
}
header{
  padding:14px 18px;
  background:#202020;
  border-bottom:1px solid #444;
}
h1{
  margin:0;
  font-size:22px;
}
.small{
  color:#aaa;
  font-size:13px;
  margin-top:4px;
}
.wrap{
  padding:14px;
  max-width:1200px;
  margin:auto;
}
.panel{
  background:#202020;
  border:1px solid #3b3b3b;
  border-radius:7px;
  padding:12px;
  margin-bottom:12px;
}
.grid{
  display:grid;
  grid-template-columns:repeat(4,minmax(120px,1fr));
  gap:8px;
}
.pin{
  border:1px solid #444;
  border-radius:6px;
  padding:9px;
  background:#181818;
}
.lamp{
  width:14px;
  height:14px;
  border-radius:50%;
  display:inline-block;
  margin-right:7px;
  vertical-align:-2px;
  background:#555;
}
.lamp.hi{
  background:#5cff72;
  box-shadow:0 0 10px #5cff72;
}
.name{
  font-weight:bold;
}
.meta{
  color:#aaa;
  font-size:12px;
  margin-top:5px;
}
button,a.button{
  display:inline-block;
  background:#333;
  color:#fff;
  border:1px solid #666;
  border-radius:5px;
  padding:8px 10px;
  margin:3px;
  text-decoration:none;
  cursor:pointer;
}
button:hover,a.button:hover{
  background:#444;
}
table{
  width:100%;
  border-collapse:collapse;
  font-family:Consolas,monospace;
  font-size:13px;
}
th,td{
  border-bottom:1px solid #333;
  padding:6px;
  text-align:left;
}
.sound{
  color:#7dff9b;
  font-weight:bold;
}
.bad{
  color:#ff7777;
}
.good{
  color:#7dff9b;
}
@media(max-width:700px){
  .grid{
    grid-template-columns:repeat(2,minmax(120px,1fr));
  }
}
</style>
</head>

<body>

<header>
  <h1>FunHouse WPC Sniffer</h1>
  <div class="small">
    Verified map • CA2 = GPIO25 • expected sound address = 0x1C • WDEN rising edge
  </div>
</header>

<div class="wrap">

<div class="panel">
  <b>System:</b>
  <span id="status">loading...</span>
</div>

<div class="panel">
  <h3>16 live WPC inputs</h3>
  <div id="pins" class="grid"></div>
</div>

<div class="panel">
  <h3>Controls</h3>

  <button onclick="action('/log/start')">START LOG</button>
  <button onclick="action('/log/stop')">STOP LOG</button>
  <button onclick="action('/log/clear')">CLEAR LOG</button>
  <a class="button" href="/log/download">DOWNLOAD CSV</a>
  <button onclick="action('/counters/reset')">RESET COUNTERS</button>
  <button onclick="action('/dac/test')">PLAY /test.wav</button>

  <div class="small">
    DAC uses GPIO1/TX0 for LCK. Serial pauses only while audio is playing.
  </div>
</div>

<div class="panel">
  <h3>Recent WDEN captures</h3>

  <table>
    <thead>
      <tr>
        <th>µs</th>
        <th>ADDR</th>
        <th>DATA</th>
        <th>R/W</th>
        <th>RESET</th>
        <th>TYPE / NAME</th>
      </tr>
    </thead>
    <tbody id="events"></tbody>
  </table>
</div>

</div>

<script>
function hex2(v){
  return Number(v).toString(16).toUpperCase().padStart(2,'0');
}

async function action(url){
  try{
    const r=await fetch(url,{method:'POST'});
    const t=await r.text();
    console.log(t);
  }catch(e){
    console.log(e);
  }
}

async function updateState(){
  try{
    const r=await fetch('/api/state',{cache:'no-store'});
    const j=await r.json();

    const p=document.getElementById('pins');
    p.innerHTML='';

    for(const pin of j.pins){
      const div=document.createElement('div');
      div.className='pin';

      div.innerHTML =
        '<span class="lamp '+(pin.state?'hi':'')+'"></span>'+
        '<span class="name">'+pin.name+'</span>'+
        '<div class="meta">GPIO'+pin.gpio+
        ' &nbsp; state='+pin.state+
        ' &nbsp; changes='+pin.changes+
        '</div>';

      p.appendChild(div);
    }

    document.getElementById('status').innerHTML =
      'WDEN edges: <b>'+j.wdenEdges+'</b>'+
      ' &nbsp; dropped: <b class="'+(j.dropped?'bad':'good')+'">'+j.dropped+'</b>'+
      ' &nbsp; SD: <b class="'+(j.sdReady?'good':'bad')+'">'+(j.sdReady?'OK':'FAIL')+'</b>'+
      ' &nbsp; logging: <b>'+(j.logging?'ON':'OFF')+'</b>'+
      ' &nbsp; DAC: <b>'+(j.dacActive?'PLAYING':'idle')+'</b>';

  }catch(e){}
}

async function updateEvents(){
  try{
    const r=await fetch('/api/events',{cache:'no-store'});
    const j=await r.json();

    let html='';

    for(const e of j){
      let type='';

      if(e.sound){
        type='<span class="sound">SOUND';
        if(e.name) type+=' — '+e.name;
        type+='</span>';
      }

      html += '<tr>'+
        '<td>'+e.us+'</td>'+
        '<td>0x'+hex2(e.addr)+'</td>'+
        '<td>0x'+hex2(e.data)+'</td>'+
        '<td>'+(e.rw?'R':'W')+'</td>'+
        '<td>'+e.reset+'</td>'+
        '<td>'+type+'</td>'+
        '</tr>';
    }

    document.getElementById('events').innerHTML=html;

  }catch(e){}
}

setInterval(updateState,200);
setInterval(updateEvents,500);

updateState();
updateEvents();
</script>

</body>
</html>
)HTML";

// ---------------------------------------------------------------------
// WEB SERVER SETUP
// ---------------------------------------------------------------------

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/events", HTTP_GET, handleApiEvents);

  server.on("/log/start", HTTP_POST, handleStartLog);
  server.on("/log/stop", HTTP_POST, handleStopLog);
  server.on("/log/clear", HTTP_POST, handleClearLog);
  server.on("/log/download", HTTP_GET, handleDownloadLog);

  server.on("/counters/reset", HTTP_POST, handleResetCounters);
  server.on("/dac/test", HTTP_POST, handleDacTest);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
}

// ---------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("==================================================");
  Serial.println("FUNHOUSE WPC WIFI SNIFFER");
  Serial.println("==================================================");
  Serial.println("Verified CA2 = GPIO25");
  Serial.println("Expected sound address = 0x1C");
  Serial.println("DAC LCK = GPIO1/TX0 only while DAC is active");
  Serial.println();

  // Configure all WPC pins as plain high-impedance inputs.
  for (uint8_t i = 0; i < WPC_PIN_COUNT; i++) {
    pinMode(wpcPins[i], INPUT);
    previousState[i] = digitalRead(wpcPins[i]) ? 1 : 0;
    liveState[i] = previousState[i];
  }

  // SD is optional for live sniffing. The app still runs if SD fails.
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  sdReady = SD.begin(SD_CS, sdSPI, 4000000);

  Serial.print("SD: ");
  Serial.println(sdReady ? "OK" : "NOT AVAILABLE");

  // Start our private Wi-Fi access point.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);

  Serial.print("Wi-Fi AP: ");
  Serial.println(WIFI_SSID);

  Serial.print("Web: http://");
  Serial.println(WiFi.softAPIP());

  setupWebServer();

  // Attach after pins are configured and the rest of setup is stable.
  attachInterrupt(
    digitalPinToInterrupt(PIN_WDEN),
    onWdenRise,
    RISING
  );

  Serial.println();
  Serial.println("WPC capture armed.");
  Serial.println("All 16 live inputs are being sampled.");
}

// ---------------------------------------------------------------------
// MAIN LOOP
// ---------------------------------------------------------------------

void loop() {
  // 1. Read all 16 pins continuously for the live display and counters.
  sampleAllWpcPins();

  // 2. Move captured WDEN events out of ISR buffer.
  processCapturedEvents();

  // 3. Keep HTTP UI responsive.
  server.handleClient();

  // 4. Feed WAV data to PCM5102A if a DAC test is active.
  serviceDacPlayback();

  // No long delay: we want fast live sampling.
  delay(1);
}