/* ============================================================================
 * loadcell_testbed.ino  --  the force pad for the arcade cabinet
 * ============================================================================
 * Hardware:
 *   ESP32-WROOM-32 devkit + HX711 amplifier + ONE LCS-1 S-type load cell
 *   (1000 kg, 2.0 mV/V, 350 ohm, 5 wires, already wired). An S-beam is
 *   loaded ALONG ITS AXIS through the threaded ends - side load is what
 *   damages it, and what makes it read wrong.
 *
 * The cabinet's game runs on a Raspberry Pi. This box does two things for it:
 *
 *   1. BLE HID GAMEPAD. The force is one axis - rest at 0.0, full scale at 1.0.
 *      BlueZ turns that into /dev/input/js*, so the game reads a joystick like
 *      any other and the game developer integrates nothing at all.
 *   2. WIFI ACCESS POINT + CAPTIVE-PORTAL WEB PAGE. This is how the pad is set
 *      up and diagnosed: three steps (amplifier and noise, zero, calibration),
 *      three live instruments, settings, and OTA firmware upload. It is the
 *      tool, and it is bilingual EN/FR.
 *
 * The gamepad starts first and is never stopped by anything the web side does.
 * The firmware reports FACTS - peak, impulse, rise, sample count - and never
 * scores. Scoring is game design and lives on the Pi, where it can be changed
 * without reflashing this board.
 *
 * USB serial remains a full bench fallback: single-letter commands for people,
 * and lines starting with '$' for machines, sharing the one port.
 *
 * Library dependency: "HX711 Arduino Library" by Bogdan Necula (bogde) >= 0.7.5
 * ========================================================================= */

#include <Arduino.h>
#include <HX711.h>
#include <Preferences.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Build-time configuration
 * ------------------------------------------------------------------------ */

// HX711 <-> ESP32. Plain GPIO, change here if you re-wire.
// GPIO5 is a boot strapping pin on most ESP32 modules (pulled up during reset),
// which is harmless for an output. Never put SCK on the input-only pins 34-39.
static const uint8_t PIN_HX711_DT  = 4;   // HX711 DOUT  -> ESP32 input
static const uint8_t PIN_HX711_SCK = 5;   // HX711 PD_SCK <- ESP32 output

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <DNSServer.h>
#include <BleGamepad.h>

/* --- the two things this box does for the outside world --------------------
 * 1. BLE HID gamepad  -> the Pi sees a joystick. One axis, the force.
 *    The game developer writes nothing; they read an axis like any other pad.
 * 2. WiFi AP + web page -> how WE set it up. Never touches (1).
 * The gamepad starts first and is never stopped by anything the web side does.
 */
// One name for the whole machine: it is both the wifi network and the gamepad
// the Pi pairs with. For a second cabinet, change this one string.
static const char* DEVICE_NAME = "esp32borne#1";
static const char* BLE_NAME = DEVICE_NAME;
static const char* AP_SSID  = DEVICE_NAME;

// Shipped default so the first connection can be made from a phone with no
// cable. 8 characters is the WPA2 minimum - it cannot be shorter than this
// without making the network open. The page nags until it is changed.
static const char* AP_PASS_DEFAULT = "borne123";

BleGamepad  gamepad(DEVICE_NAME, "loadcell", 100);
WebServer   server(80);
DNSServer   dns;
static bool gamepadUp = false;
static bool apUp      = false;
static char apPass[17] = {0};     // generated per device, stored in NVS

// Mirrors every byte of human output into a ring buffer so the web page can
// show the same console the serial port shows, without touching any of the
// ~350 existing print calls.
static const uint16_t LOGBUF = 3072;

class DualStream : public Stream {
 public:
  char     buf[LOGBUF];
  uint16_t head = 0;
  uint32_t seq  = 0;             // total bytes ever written

  // The firmware already prefixes every notable line with [OK] [WARN] [FAIL]
  // [ERROR] [NOTE] [PROBLEM] [REJECTED]. Catching them here means the web page
  // gets verdicts without parsing the console and without touching any of the
  // existing print calls.
  char     msg[120] = {0};
  char     cur[120] = {0};
  uint8_t  curLen = 0;
  bool     curIsMsg = false;

  void push(uint8_t c) {
    buf[head] = (char)c;
    head = (head + 1) % LOGBUF;
    seq++;

    if (c == '\n' || c == '\r') {
      if (curIsMsg && curLen > 2) { cur[curLen] = 0; strcpy(msg, cur); }
      curLen = 0; curIsMsg = false;
    } else {
      if (curLen == 0 && c == '[') curIsMsg = true;
      if (curIsMsg && curLen < sizeof(cur) - 1) cur[curLen++] = (char)c;
    }
  }
  size_t write(uint8_t c) override {
    push(c);
    return Serial.write(c);
  }
  size_t write(const uint8_t* b, size_t n) override {
    for (size_t i = 0; i < n; i++) push(b[i]);
    return Serial.write(b, n);
  }
  int available() override { return Serial.available(); }
  int read() override      { return Serial.read(); }
  int peek() override      { return Serial.peek(); }
  void flush() override    { Serial.flush(); }

  // Everything written since sequence number `since`, oldest first.
  String since(uint32_t s) {
    uint32_t have = (seq < LOGBUF) ? seq : LOGBUF;
    if (s > seq) s = seq;                       // client is confused, resync
    uint32_t want = seq - s;
    if (want > have) want = have;
    String out;
    out.reserve(want + 1);
    for (uint32_t i = 0; i < want; i++) {
      out += buf[(head + LOGBUF - want + i) % LOGBUF];
    }
    return out;
  }
};

static DualStream Both;

#define HUMAN_PORT    Both
#define MACHINE_PORT  Both

static const char* FW_VERSION    = "1.0.0";
static const int   PROTO_VERSION = 1;

// HX711 / ADC limits
static const uint32_t HX_TIMEOUT_MS = 1000;
static const long     HX_FULL_SCALE = 8388607L;  // 2^23 - 1
static const long     HX_SAT_LIMIT  = 8000000L;  // beyond this = railed
static const uint32_t HX_STALL_MS   = 3000;      // no sample this long = dead
static const float    HX_AVDD_V     = 4.3f;      // typical HX711 excitation

// Filtering / stability
// Filtering is tunable at run time (see Tune), so the ring is sized to the
// largest window anyone can ask for and only filterLen of it is used.
static const uint8_t  RING_MAX       = 32;
// Steadiness used to be fixed gram thresholds sized for the old 4 x 50 kg
// rig. On a 1000 kg cell they are unreachable - the reading never counted as
// steady, so drift cancelling never ran once. It is now a multiple of the
// noise floor STEP 1 ACTUALLY MEASURES.
static const uint32_t LIVE_PERIOD_MS = 500;

static const uint16_t AVG_SAMPLES    = 20;   // tare / calibration captures

// Auto zero tracking. A load cell's zero wanders with temperature and creep,
// so the zero is allowed to follow it - but ONLY while the plate is empty and
// the reading is steady, and only slowly. Anything real outruns it easily.

// Impact capture. The HX711 is a slow sigma-delta converter, so these settings
// are about catching that a hit happened and roughly how hard, not about
// resolving its waveform. See the note in enterHits().
// Defaults sized for the actual rig: one LCS-1 1000 kg cell at ~4.3 ADC counts
// per gram. A hand press lands in the kg range, so thresholds in the tens of
// grams would just trigger on noise.
static const uint16_t HIT_BASELINE_TC    = 64;     // EMA divisor when quiet
static const float    HIT_SPS_USABLE     = 40.0f;  // below this, warn loudly

// The gamepad axis. Rest at minimum, full-scale force at maximum.
static const int16_t BLE_AXIS_MIN = -32767;
static const int16_t BLE_AXIS_MAX =  32767;
// Full scale is the CELL's rating, not the force anyone expects to see, so a
// hard hit can never clip: every big hit would otherwise score identically.
// The axis then IS the fraction of full scale - axis 0.1 = 100 kg - and there
// is no agreed number for the game to be told.
static const float   AXIS_FS_DEFAULT_G = 1000000.0f;  // 1000 kg, the LCS-1 rating

static const uint16_t CFG_MAGIC   = 0x4C43;  // 'LC'

// The tuning blob lives under its own NVS key with its own version, so it
// can change shape freely. Config must NOT: cfgLoad() accepts stored bytes
// only when sizeof and version both match, so resizing Config throws away
// the calibration and the wifi password - and the only way back from a lost
// password is a USB reflash.
static const uint8_t  CAL_MAX_PTS  = 5;
static const uint16_t TUNE_MAGIC   = 0x544E;  // 'TN'
static const uint16_t TUNE_VERSION = 1;
static const uint16_t CFG_VERSION = 5;

/* ---------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------ */



struct Config {
  uint16_t magic;
  uint16_t version;
  float    countsPerGram;   // ADC counts per gram
  long     tareOffset;      // raw counts with no load
  uint16_t stagesDone;      // bit (stage-1) set once that stage has passed
  uint8_t  invert;          // software sign flip
  uint8_t  calibrated;
  uint8_t  retired1;        // was bridgeConfirmed; kept so sizeof() is stable
  uint8_t  uiStyle;         // 0 plain / 1 unicode
  uint8_t  reserved[2];
  float    axisFullScaleG;  // force that reads as a fully deflected axis
  char     apPass[16];      // wifi password
  char     retired2[16];    // was webPass; kept so sizeof() is stable
  uint8_t  passChanged;     // 0 while still on the shipped default
  uint8_t  reserved2[3];
  float    rowTop;          // top button row, fraction of screen height
  float    rowBot;          // bottom row. 0 = not measured yet
  uint8_t  useKg;           // display preference: 0 grams, 1 kilograms
  uint8_t  azt;             // auto zero tracking on/off
  // Which gamepad axis carries the force. Takes over a reserved byte, so
  // sizeof() is unchanged and every stored blob reads as 0 = both.
  uint8_t  axisMap;         // AXIS_BOTH / AXIS_X / AXIS_Y
  uint8_t  reserved3[1];
};
// cfgLoad() rejects stored bytes of any other size - and with them the
// calibration and the wifi password. Fail the build rather than find out.
static_assert(sizeof(Config) == 72, "Config layout changed - see cfgLoad()");

enum AxisMap : uint8_t { AXIS_BOTH = 0, AXIS_X = 1, AXIS_Y = 2 };

// The three setup stages. Which ones have passed is stored in NVS, so a reboot
// part-way through setup does not lose your place, and any stage can be
// repeated on its own from the menu.
// Everything that shapes how the sensor behaves. Separate blob - see above.
struct Tune {
  uint16_t magic;
  uint16_t version;
  // what counts as a hit
  float    hitThresh;       // grams above resting (counts if uncalibrated)
  float    hitReleaseFrac;  // fall below this x threshold to close the event
  uint32_t hitMaxMs;        // force-close an event stuck open this long
  uint8_t  hitReleaseN;     // consecutive samples below before closing
  // filtering and steadiness
  uint8_t  filterLen;       // ring window, <= RING_MAX
  uint8_t  trimCount;       // drop N lowest and N highest before averaging
  // Graph full scale: 0 = follow the last peak, 1..4 = that many quarters
  // of the axis full scale. Reuses a pad byte, so sizeof() is unchanged and
  // stored tuning survives.
  uint8_t  graphScale;
  float    steadySd;        // steady while sd  < noiseFloor * this
  float    steadyPp;        // steady while p-p < noiseFloor * this
  float    noiseFloor;      // counts, measured by step 1
  // calibration points: grams and the raw counts measured at that load.
  // Kept so they can be reviewed, added to and refitted without redoing the
  // whole calibration from scratch.
  float    calG[CAL_MAX_PTS];
  float    calRaw[CAL_MAX_PTS];
  float    calLinPct;       // worst deviation from the fitted line, %FS
  float    calLinG;         // the same, in grams
  uint8_t  calN;            // how many points are stored
  uint8_t  pad2[3];
  // drift cancelling (the on/off switch stays in Config as cfg.azt)
  float    aztRateGps;
  float    aztWindowG;
  uint32_t aztSettleMs;
};

enum Stage : uint8_t {
  STG_CHECK = 1,    // amplifier answers, reading sane, noise measured
  STG_ZERO,         // tare
  STG_CAL,          // calibrate against a known weight
  STG_MAX = STG_CAL
};

// A stable NAME for each state, sent to the page as "k" in /data.
// The page switches on this and never on the numeric state, so states can be
// added or removed without silently breaking the UI. Keep in step with State.
enum State : uint8_t {
  ST_IDLE = 0,
  ST_MENU,          // stage picker
  ST_W_CHECK,       // amplifier + noise
  ST_TARE,
  ST_CAL_KNOWN,     // ask for the known mass
  ST_CAL_PLACE,     // wait for the mass to be placed
  ST_HITS,          // impact capture - the game mechanic
  ST_MEASURE        // free monitoring with peak hold
};

/* ---------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------ */

HX711       hx;
Preferences prefs;

static Config cfg;
static Tune   tune;

static const char* stateKind(State s) {
  switch (s) {
    case ST_IDLE:      return "idle";
    case ST_MENU:      return "menu";
    case ST_W_CHECK:   return "amp";
    case ST_TARE:      return "zero";
    case ST_CAL_KNOWN: return "calw";
    case ST_CAL_PLACE: return "calp";
    case ST_HITS:      return "hit";
    case ST_MEASURE:   return "mon";
  }
  return "idle";
}

static State   state    = ST_IDLE;
static Stage   curStage = STG_CHECK;  // which stage the current state belongs to
static bool    runAll   = false;     // true = walk every stage in order
static bool    detail   = true;      // print this stage's long explanation
static bool    fromIdle = false;     // stage entered from the live readout

static long     ring[RING_MAX];
static uint8_t  ringCount   = 0;
static uint8_t  ringHead    = 0;
static uint32_t lastSampleMs = 0;
static bool     hxAlive     = false;

static float noiseSd       = 0.0f;   // counts, from the last sanity check
// NET movement of the zero since power-on, in grams. It was a sum of ABSOLUTE
// steps, which on a reading that jitters either side of zero just counted
// churn: it climbed at the rate cap forever and said nothing about drift.
static float    aztTotal   = 0.0f;
// Drift correction, in counts. NEVER stored: cfg.tareOffset is the zero a
// person set, this is how far the box has crept from it since. Keeping them
// apart is what stops an unrelated cfgSave() from baking drift into NVS,
// and stops a crept zero from landing in countsPerGram at calibration time.
static long     aztOffset  = 0;
static uint32_t aztLastMs  = 0;
static uint32_t aztSteadyMs = 0;
static uint32_t aztHeldMs   = 0;     // since the reading last moved at all
static const uint32_t AZT_REZERO_MS = 30000;

// Results the web page renders as fields instead of making you read console
// text. Filled by the stage that produces them.
static bool  hxOk = false, hxDtLow = false;      // stage 2
static long  hxSample = 0;
static bool  snDone = false;                     // stage 6
static float snMean = 0, snSd = 0;
static long  snMin = 0, snMax = 0;
static float lastHit[5]   = {0};   // peak, rise ms, width ms, samples, impulse

// The last HIT_LOG hits, so the Game tab can show a spread instead of a
// single number. RAM only - a bar would wear the flash out in a week.
static const uint8_t HIT_LOG = 50;
struct HitRec {
  float    peak;
  float    impulse;
  uint16_t rise;
  uint16_t width;
  uint16_t samples;
  uint32_t at;        // millis() when the hit closed
};
static HitRec hitLog[HIT_LOG];
static uint8_t  hitLogN    = 0;    // how many slots are filled (<= HIT_LOG)
static uint8_t  hitLogHead = 0;    // next slot to write
static uint32_t hitLogSeq  = 0;    // total ever recorded, survives wrapping

// Declared here on purpose: the Arduino build hoists generated prototypes
// above this struct, so a function returning HitRec* must be declared after
// it by hand - the generator then skips it.
static const HitRec* hitAt(uint8_t i);
static float calKnownGrams = 0.0f;

static bool     showRaw     = false; // live readout shows raw counts
static bool     quiet       = false; // suppress the periodic human readout
static uint32_t streamPeriod = 0;    // ms, 0 = machine streaming off
static uint32_t lastStreamMs = 0;
static uint32_t lastLiveMs   = 0;

// Impact capture state
static long     hitBaseline  = 0;
static bool     hitActive    = false;
static long     hitPeakRaw   = 0;
static uint32_t hitStartMs   = 0;
static uint32_t hitPeakMs    = 0;
static uint16_t hitSamples   = 0;
static uint8_t  hitBelow     = 0;
static uint16_t hitCount     = 0;
static float    hitMaxSoFar  = 0.0f;
static float    hitSps       = 0.0f;
static double   hitImpulse   = 0.0;    // gram-seconds, area under the event
static uint32_t hitLastMs    = 0;

static char    lineBuf[96];
static uint8_t lineLen = 0;
static char    mLineBuf[96];
static uint8_t mLineLen = 0;
static bool    portsShared = true;


static void enterState(State s);
static void enterStage(Stage s);
static void advance();
static void stagePassed(Stage s);
static void showMenu();

// The page's endpoints, also reachable over USB serial ($J, $SET, $AXIS).
// Declared by hand: they are defined below the page, further down than the
// machine-command handler that calls them.
static bool        axisStream = false;  // $AXIS,1: mirror the axes onto serial
static bool        axisDirty  = false;  // resend though the force is unchanged
static const char* setQuery   = nullptr; // null = settings come from HTTP
static String      jsonData();
static String      jsonInfo();
static String      jsonHits();
static String      textDoc();
static bool        applySet();
static const char* setReply(bool reboot);
static void        serialReply(const char* what, const String& body);

/* ---------------------------------------------------------------------------
 * Text helpers
 * ------------------------------------------------------------------------ */

/* --- display style ---------------------------------------------------------
 * ANSI colour was tried on the phone terminal and is not rendered, so there is
 * no colour layer. 'V' toggles the two that do work:
 *   0 PLAIN   plain ASCII, works in absolutely anything
 *   1 FANCY   unicode box drawing and block bars
 * Stored in NVS.
 */
enum UiStyle : uint8_t { UI_PLAIN = 0, UI_FANCY = 1 };
static uint8_t uiStyle = UI_FANCY;

#define uiCol(x)  ((void)0)
#define C_RESET   0
#define C_DIM     0
#define C_VAL     0
#define C_WARN    0
#define C_HEAD    0

static void hr() {
  uiCol(C_DIM);
  if (uiStyle >= UI_FANCY) {
    for (uint8_t i = 0; i < 52; i++) HUMAN_PORT.print(F("─"));
    HUMAN_PORT.println();
  } else {
    HUMAN_PORT.println(F("----------------------------------------------------"));
  }
  uiCol(C_RESET);
}

// A proportional bar. frac 0..1, clamped.
static void uiBar(float frac, uint8_t width) {
  if (!(frac > 0)) frac = 0;
  if (frac > 1.0f) frac = 1.0f;
  uint8_t on = (uint8_t)(frac * width + 0.5f);
  for (uint8_t i = 0; i < width; i++) {
    if (uiStyle >= UI_FANCY) HUMAN_PORT.print(i < on ? F("█") : F("·"));
    else                     HUMAN_PORT.print(i < on ? '#' : '.');
  }
  uiCol(C_RESET);
}

// One character of a sparkline, frac 0..1.
static void uiSpark(float frac) {
  if (!(frac > 0)) frac = 0;
  if (frac > 1.0f) frac = 1.0f;
  if (uiStyle >= UI_FANCY) {
    static const char* const LVL[8] = { "▁", "▂", "▃", "▄",
                                        "▅", "▆", "▇", "█" };
    HUMAN_PORT.print(LVL[(uint8_t)(frac * 7.99f)]);
  } else {
    HUMAN_PORT.print("._-=+*#@"[(uint8_t)(frac * 7.99f)]);
  }
}

static void title(const __FlashStringHelper* s) {
  HUMAN_PORT.println();
  hr();
  uiCol(C_HEAD);
  if (uiStyle >= UI_FANCY) HUMAN_PORT.print(F("  "));
  HUMAN_PORT.println(s);
  uiCol(C_RESET);
  hr();
}

// Every stage ends with the same footer, so the keys are predictable and the
// long explanations do not have to be on screen to know what to press.
static void footer(const __FlashStringHelper* keys) {
  HUMAN_PORT.println();
  HUMAN_PORT.print(keys);
  HUMAN_PORT.println(F("   [?] detail   [X] menu"));
}

// Tokenise a line into up to maxN floats. Commas/semicolons count as spaces.
// strtod is used instead of sscanf("%f") because float support in *scanf is not
// guaranteed by every newlib build shipped with the ESP32 core.
static int parseFloats(char* s, float* out, int maxN) {
  for (char* p = s; *p; ++p) {
    if (*p == ',' || *p == ';' || *p == '\t') *p = ' ';
  }
  int n = 0;
  char* p = s;
  while (n < maxN) {
    char* end = NULL;
    double v = strtod(p, &end);
    if (end == p) break;              // nothing left to parse
    out[n++] = (float)v;
    p = end;
  }
  return n;
}

static void trimLine(char* s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
  size_t lead = 0;
  while (s[lead] == ' ' || s[lead] == '\t') lead++;
  if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
}

static char upper1(const char* s) {
  return (strlen(s) == 1) ? (char)toupper((unsigned char)s[0]) : 0;
}

/* ---------------------------------------------------------------------------
 * Ring buffer + statistics
 * ------------------------------------------------------------------------ */

static void ringClear() { ringCount = 0; ringHead = 0; }

static void ringPush(long v) {
  ring[ringHead] = v;
  ringHead = (ringHead + 1) % tune.filterLen;
  if (ringCount < tune.filterLen) ringCount++;
}

static uint8_t ringCopySorted(long* dst) {
  uint8_t n = ringCount;
  for (uint8_t i = 0; i < n; i++) dst[i] = ring[i];
  for (uint8_t i = 1; i < n; i++) {           // insertion sort, n <= RING_MAX
    long key = dst[i];
    int8_t j = i - 1;
    while (j >= 0 && dst[j] > key) { dst[j + 1] = dst[j]; j--; }
    dst[j + 1] = key;
  }
  return n;
}

// Trimmed mean: robust against the occasional HX711 spike without the lag of a
// long moving average.
static long ringFiltered() {
  long sorted[RING_MAX];
  uint8_t n = ringCopySorted(sorted);
  if (n == 0) return 0;
  uint8_t trim = (n >= (tune.trimCount * 2 + 2)) ? tune.trimCount : 0;
  double sum = 0;
  uint8_t used = 0;
  for (uint8_t i = trim; i < n - trim; i++) { sum += (double)sorted[i]; used++; }
  return (long)llround(sum / (double)used);
}

static void ringStats(float* mean, float* sd, long* mn, long* mx) {
  if (ringCount == 0) { *mean = 0; *sd = 0; *mn = 0; *mx = 0; return; }
  double sum = 0, sum2 = 0;
  long lo = ring[0], hi = ring[0];
  for (uint8_t i = 0; i < ringCount; i++) {
    double v = (double)ring[i];
    sum += v; sum2 += v * v;
    if (ring[i] < lo) lo = ring[i];
    if (ring[i] > hi) hi = ring[i];
  }
  double m = sum / ringCount;
  double var = (sum2 / ringCount) - (m * m);
  if (var < 0) var = 0;
  *mean = (float)m;
  *sd   = (float)sqrt(var);
  *mn   = lo;
  *mx   = hi;
}

/* ---------------------------------------------------------------------------
 * HX711 access
 * ------------------------------------------------------------------------ */

// bogde's read() calls wait_ready() internally, which blocks forever if the
// HX711 never asserts DOUT low. Always gate it behind wait_ready_timeout().
static bool hxReadRaw(long* out, uint32_t timeoutMs = HX_TIMEOUT_MS) {
  if (!hx.wait_ready_timeout(timeoutMs, 1)) return false;
  *out = hx.read();
  return true;
}

// Blocking capture used by tare, calibration and diagnostics.
static bool hxCapture(uint16_t n, float* mean, float* sd, long* mn, long* mx) {
  double sum = 0, sum2 = 0;
  long lo = 0, hi = 0;
  uint16_t got = 0;
  for (uint16_t i = 0; i < n; i++) {
    long v;
    if (!hxReadRaw(&v)) break;
    if (got == 0) { lo = hi = v; }
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    sum += (double)v; sum2 += (double)v * (double)v;
    got++;
    if ((i % 5) == 0) HUMAN_PORT.print('.');
  }
  HUMAN_PORT.println();
  if (got < n / 2) return false;
  double m = sum / got;
  double var = (sum2 / got) - (m * m);
  if (var < 0) var = 0;
  *mean = (float)m;
  *sd   = (float)sqrt(var);
  *mn   = lo;
  *mx   = hi;
  return true;
}

// Non-blocking: feed the ring buffer whenever the HX711 has a fresh sample.
static void gamepadPush(long raw);   // defined with the gamepad code
static void handlePage();            // defined with the web page
static void startAP();               // defined with the web code
static void hitsFeed(long raw);      // defined with the impact-capture code
static void measureFeed(long raw);   // defined with the measurement code

// Let the zero follow slow drift, but only with an empty steady plate and only
// at a crawl, so anything real outruns it.
// The zero in force right now: what was set, plus what drift has cancelled.
static long zeroNow() { return cfg.tareOffset + aztOffset; }

static void aztUpdate(long raw) {
  uint32_t now = millis();
  float dt = (now - aztLastMs) / 1000.0f;
  aztLastMs = now;
  if (!cfg.azt || !haveCal() || dt <= 0 || dt > 1.0f) return;

  float g = rawToGrams(raw);
  float sd, pp;
  bool steady = isStable(&sd, &pp);
  if (!steady) aztHeldMs = now;

  // Outside the window the zero used to be stuck for good. The cell's zero
  // moves about 200 g per 10 C, so a box calibrated in winter can sit outside
  // a 400 g window all summer - sending a constant force, or with a dead patch
  // at the bottom - until somebody re-zeroes it by hand. Nobody will. A punch
  // pad is never loaded dead still for half a minute, so a reading that IS
  // that still is the empty plate: take it as the zero.
  // Only from the live readout: a calibration weight sits still on purpose.
  if (fabsf(g) > tune.aztWindowG) {
    aztSteadyMs = now;
    if (steady && state == ST_IDLE && now - aztHeldMs >= AZT_REZERO_MS) {
      long z = ringFiltered() - cfg.tareOffset;
      aztTotal += (float)(z - aztOffset) / fabsf(cfg.countsPerGram);
      aztOffset = z;
      aztHeldMs = now;
    }
    return;
  }
  if (!steady) { aztSteadyMs = now; return; }
  if (now - aztSteadyMs < tune.aztSettleMs) return;

  // Work in counts so the sign convention and the invert flag cannot bite.
  float step = tune.aztRateGps * dt * fabsf(cfg.countsPerGram);
  float d = (float)(raw - zeroNow());
  if (d >  step) d =  step;
  if (d < -step) d = -step;
  aztOffset += (long)d;               // runtime only, never written to NVS
  aztTotal += d / fabsf(cfg.countsPerGram);   // signed: net shift, not churn
}

static void pollSample() {
  if (hx.is_ready()) {
    long v = hx.read();
    ringPush(v);
    aztUpdate(v);
    // Both modes want raw samples: the ring's trimmed mean exists to discard
    // exactly the transients they are trying to measure.
    gamepadPush(v);
    if (state == ST_HITS)         hitsFeed(v);
    else if (state == ST_MEASURE) measureFeed(v);
    lastSampleMs = millis();
    hxAlive = true;
  } else if (hxAlive && (millis() - lastSampleMs) > HX_STALL_MS) {
    hxAlive = false;
    HUMAN_PORT.println(F("[ERROR] HX711 stopped producing samples."));
    HUMAN_PORT.println(F("        Check DT/SCK/VCC/GND, then run 'S'."));
  }
}

/* ---------------------------------------------------------------------------
 * Persistent settings (ESP32 NVS via Preferences)
 * ------------------------------------------------------------------------ */

static void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic         = CFG_MAGIC;
  cfg.version       = CFG_VERSION;
  cfg.uiStyle       = UI_FANCY;   // memset above would otherwise mean PLAIN
  cfg.axisFullScaleG = AXIS_FS_DEFAULT_G;
  cfg.azt            = 1;   // drift cancelling on by default
  cfg.countsPerGram = 0.0f;
  cfg.tareOffset    = 0;
}

static void tuneDefaults() {
  memset(&tune, 0, sizeof(tune));
  tune.magic          = TUNE_MAGIC;
  tune.version        = TUNE_VERSION;
  tune.hitThresh      = 1000.0f;
  tune.hitReleaseFrac = 0.5f;
  tune.hitMaxMs       = 2000;
  tune.hitReleaseN    = 3;
  tune.filterLen      = 10;    // ~1 s at the HX711 default 10 SPS
  tune.trimCount      = 2;
  tune.steadySd       = 3.0f;  // multiples of the measured noise floor
  tune.steadyPp       = 8.0f;
  tune.noiseFloor     = 0.0f;  // 0 = step 1 has not run yet
  tune.aztRateGps     = 15.0f;
  tune.aztWindowG     = 400.0f;
  tune.aztSettleMs    = 1500;
  tune.calN           = 0;
  tune.graphScale     = 0;     // follow the last peak
}

// Least-squares fit through the stored points: raw = slope*grams + intercept.
// With one point it degenerates to exactly what single-point calibration did,
// so the three-step flow is unchanged for anyone who does not want more.
// Returns false if the points cannot produce a usable slope.
static bool calFit(float* slope, float* intercept, float* worstG) {
  uint8_t n = tune.calN;
  if (n == 0) return false;
  if (n == 1) {
    if (tune.calG[0] == 0.0f) return false;
    *intercept = (float)zeroNow();
    *slope = (tune.calRaw[0] - *intercept) / tune.calG[0];
    *worstG = 0.0f;                       // one point defines the line
    return (*slope != 0.0f);
  }
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (uint8_t i = 0; i < n; i++) {
    double x = tune.calG[i], y = tune.calRaw[i];
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  double den = n * sxx - sx * sx;
  if (fabs(den) < 1e-9) return false;     // every point at the same load
  double m = (n * sxy - sx * sy) / den;
  double b = (sy - m * sx) / n;
  if (m == 0.0) return false;
  *slope = (float)m;
  *intercept = (float)b;
  // Worst residual, expressed in grams - that is the number that says
  // whether a single-point calibration was good enough.
  double worst = 0;
  for (uint8_t i = 0; i < n; i++) {
    double predicted = m * tune.calG[i] + b;
    double dg = (tune.calRaw[i] - predicted) / m;
    if (fabs(dg) > fabs(worst)) worst = dg;
  }
  *worstG = (float)fabs(worst);
  return true;
}

// Apply the fit to the live calibration. Everything downstream keeps
// reading cfg.countsPerGram / cfg.tareOffset and never knows the difference.
static bool calApplyFit() {
  float m, b, worstG;
  if (!calFit(&m, &b, &worstG)) return false;
  cfg.tareOffset    = (long)llround(b);
  aztOffset         = 0;              // the fit defines the zero from here
  cfg.countsPerGram = m;
  cfg.invert        = (cfg.countsPerGram < 0) ? 1 : 0;
  if (cfg.invert) cfg.countsPerGram = -cfg.countsPerGram;
  cfg.calibrated    = 1;
  cfgSave();
  tune.calLinG   = worstG;
  tune.calLinPct = (cfg.axisFullScaleG > 0)
                 ? (worstG / cfg.axisFullScaleG * 100.0f) : 0.0f;
  tuneSave();
  return true;
}

static void tuneSave() { prefs.putBytes("tune", &tune, sizeof(tune)); }

static void tuneLoad() {
  tuneDefaults();
  Tune tmp;
  if (prefs.getBytesLength("tune") == sizeof(tmp) &&
      prefs.getBytes("tune", &tmp, sizeof(tmp)) == sizeof(tmp) &&
      tmp.magic == TUNE_MAGIC && tmp.version == TUNE_VERSION) {
    tune = tmp;
  }
  // Never trust stored numbers to be sane - a bad filterLen would index
  // off the end of the ring.
  if (tune.filterLen < 3 || tune.filterLen > RING_MAX) tune.filterLen = 10;
  if (tune.trimCount * 2 + 2 > tune.filterLen) tune.trimCount = 0;
  if (tune.hitReleaseN < 1) tune.hitReleaseN = 1;
  if (!(tune.hitReleaseFrac > 0.0f) || tune.hitReleaseFrac >= 1.0f) tune.hitReleaseFrac = 0.5f;
  if (!(tune.hitThresh > 0.0f))   tune.hitThresh = 1000.0f;
  if (!(tune.steadySd  > 0.0f))   tune.steadySd  = 3.0f;
  if (!(tune.steadyPp  > 0.0f))   tune.steadyPp  = 8.0f;
  if (!(tune.aztRateGps > 0.0f))  tune.aztRateGps = 15.0f;
  if (!(tune.aztWindowG > 0.0f))  tune.aztWindowG = 400.0f;
  if (tune.aztSettleMs > 60000)   tune.aztSettleMs = 1500;
  if (tune.hitMaxMs < 100 || tune.hitMaxMs > 60000) tune.hitMaxMs = 2000;
  if (tune.calN > CAL_MAX_PTS) tune.calN = 0;
  if (tune.graphScale > 4) tune.graphScale = 0;
}

static void cfgSave() {
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
}

static void cfgLoad() {
  cfgDefaults();
  Config tmp;
  if (prefs.getBytesLength("cfg") == sizeof(tmp) &&
      prefs.getBytes("cfg", &tmp, sizeof(tmp)) == sizeof(tmp) &&
      tmp.magic == CFG_MAGIC && tmp.version == CFG_VERSION) {
    cfg = tmp;
  }
  if (cfg.axisMap > AXIS_Y) cfg.axisMap = AXIS_BOTH;
}

// Erase what measurement produced - the calibration and the zero - and keep
// what the installer chose: wifi password, row heights, full scale, unit,
// language. Wiping those too would take the wifi password with them, and the
// only way back from a lost password is a USB reflash.
static void cfgEraseCalibration() {
  // The stored points describe the calibration being erased, so they go too.
  tune.calN = 0; tune.calLinG = 0; tune.calLinPct = 0;
  tuneSave();
  float rt = cfg.rowTop, rb = cfg.rowBot, fs = cfg.axisFullScaleG;
  char ap[16];
  strncpy(ap, cfg.apPass, sizeof(ap));
  uint8_t pc = cfg.passChanged, ui = cfg.uiStyle, kg = cfg.useKg, az = cfg.azt;
  uint8_t am = cfg.axisMap;

  prefs.clear();
  cfgDefaults();
  cfg.rowTop = rt; cfg.rowBot = rb; cfg.axisFullScaleG = fs;
  strncpy(cfg.apPass, ap, sizeof(cfg.apPass));
  cfg.passChanged = pc; cfg.uiStyle = ui; cfg.useKg = kg; cfg.azt = az;
  cfg.axisMap = am;
  cfgSave();
  HUMAN_PORT.println(F("[OK] Calibration and zero erased. Settings kept."));
}

/* --- stage progress, persisted so a reboot keeps your place --- */

static uint16_t stageBit(Stage s) { return (uint16_t)1 << (s - 1); }

static bool stageIsDone(Stage s) { return (cfg.stagesDone & stageBit(s)) != 0; }

// Re-doing a stage invalidates everything downstream of it: a fresh zero makes
// the old calibration meaningless, so it is dropped rather than left to lie.
static void stageInvalidateFrom(Stage s) {
  for (uint8_t i = s; i <= STG_MAX; i++) cfg.stagesDone &= ~stageBit((Stage)i);
  if (s <= STG_CAL)    cfg.calibrated = 0;
}

static void markStage(Stage s) {
  cfg.stagesDone |= stageBit(s);
  cfgSave();
}

static bool setupComplete() {
  for (uint8_t i = STG_CHECK; i <= STG_MAX; i++) {
    if (!stageIsDone((Stage)i)) return false;
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * Weight conversion
 * ------------------------------------------------------------------------ */

static bool haveCal() { return cfg.calibrated && fabsf(cfg.countsPerGram) > 1e-6f; }

static float rawToGrams(long raw) {
  if (!haveCal()) return NAN;
  float g = (float)(raw - zeroNow()) / cfg.countsPerGram;
  return cfg.invert ? -g : g;
}

// Stability from the live ring buffer. sdOut/ppOut are grams when calibrated,
// raw counts otherwise.
// The noise floor to judge steadiness against, in COUNTS. Step 1 measures it
// and it is stored; before that has ever run, fall back to the ring's own
// spread so a fresh board is not stuck reporting "settling" forever.
static float noiseFloorCounts(float liveSd) {
  if (tune.noiseFloor > 0.0f) return tune.noiseFloor;
  if (noiseSd > 0.0f)         return noiseSd;
  return (liveSd > 1.0f) ? liveSd : 1.0f;
}

// Steadiness is a MULTIPLE OF MEASURED NOISE, not a fixed number of grams.
// It used to be sd < 2 g and p-p < 5 g, sized for the old 4 x 50 kg rig. On
// a 1000 kg cell at ~5 counts/g those are unreachable, so nothing was ever
// "steady" and drift cancelling - which is gated on it - never ran at all.
static bool isStable(float* sdOut, float* ppOut) {
  float mean, sd; long mn, mx;
  ringStats(&mean, &sd, &mn, &mx);
  float pp = (float)(mx - mn);
  // Report in the display unit, but decide in counts.
  float k = haveCal() ? fabsf(cfg.countsPerGram) : 1.0f;
  if (k <= 0.0f) k = 1.0f;
  *sdOut = sd / k;
  *ppOut = pp / k;
  if (ringCount < tune.filterLen) return false;
  float floorC = noiseFloorCounts(sd);
  return (sd < floorC * tune.steadySd) && (pp < floorC * tune.steadyPp);
}

/* ---------------------------------------------------------------------------
 * Machine protocol (every line starts with '$')
 * ------------------------------------------------------------------------ */

static void emitOk(const char* what)  { MACHINE_PORT.print(F("$OK,"));  MACHINE_PORT.println(what); }

static void emitErr(const char* what, const char* why) {
  MACHINE_PORT.print(F("$ERR,")); MACHINE_PORT.print(what);
  MACHINE_PORT.print(','); MACHINE_PORT.println(why);
}

static void emitId() {
  MACHINE_PORT.print(F("$ID,LOADCELL,fw="));
  MACHINE_PORT.print(FW_VERSION);
  MACHINE_PORT.print(F(",proto="));
  MACHINE_PORT.println(PROTO_VERSION);
}

// Set one tuning value by name, with the same clamps /set uses. Returns
// false for an unknown name or an out-of-range value - never stores either.
static bool tuneSet(const char* n, float v) {
  if (!strcmp(n, "ht"))      { if (v <= 0) return false; tune.hitThresh = v; }
  else if (!strcmp(n, "hrf")) { if (v <= 0.05f || v >= 0.95f) return false; tune.hitReleaseFrac = v; }
  else if (!strcmp(n, "hrn")) { if (v < 1 || v > 20) return false; tune.hitReleaseN = (uint8_t)v; }
  else if (!strcmp(n, "hmax")) { if (v < 100 || v > 60000) return false; tune.hitMaxMs = (uint32_t)v; }
  else if (!strcmp(n, "flen")) {
    if (v < 3 || v > RING_MAX) return false;
    tune.filterLen = (uint8_t)v;
    if (tune.trimCount * 2 + 2 > tune.filterLen) tune.trimCount = 0;
    ringClear();
  }
  else if (!strcmp(n, "trim")) { if (v < 0 || v * 2 + 2 > tune.filterLen) return false; tune.trimCount = (uint8_t)v; }
  else if (!strcmp(n, "ssd")) { if (v <= 0.5f || v > 50.0f) return false; tune.steadySd = v; }
  else if (!strcmp(n, "spp")) { if (v <= 0.5f || v > 100.0f) return false; tune.steadyPp = v; }
  else if (!strcmp(n, "arate")) { if (v <= 0 || v > 1000.0f) return false; tune.aztRateGps = v; }
  else if (!strcmp(n, "awin")) { if (v <= 0) return false; tune.aztWindowG = v; }
  else if (!strcmp(n, "asettle")) { if (v < 0 || v > 60000) return false; tune.aztSettleMs = (uint32_t)v; }
  else if (!strcmp(n, "gs")) { if (v < 0 || v > 4) return false; tune.graphScale = (uint8_t)v; }
  else return false;
  return true;
}

static void emitTune() {
  MACHINE_PORT.print(F("$TUNE,ht="));      MACHINE_PORT.print(tune.hitThresh, 1);
  MACHINE_PORT.print(F(",hrf="));          MACHINE_PORT.print(tune.hitReleaseFrac, 2);
  MACHINE_PORT.print(F(",hrn="));          MACHINE_PORT.print(tune.hitReleaseN);
  MACHINE_PORT.print(F(",hmax="));         MACHINE_PORT.print(tune.hitMaxMs);
  MACHINE_PORT.print(F(",flen="));         MACHINE_PORT.print(tune.filterLen);
  MACHINE_PORT.print(F(",trim="));         MACHINE_PORT.print(tune.trimCount);
  MACHINE_PORT.print(F(",ssd="));          MACHINE_PORT.print(tune.steadySd, 2);
  MACHINE_PORT.print(F(",spp="));          MACHINE_PORT.print(tune.steadyPp, 2);
  MACHINE_PORT.print(F(",noise="));        MACHINE_PORT.print(tune.noiseFloor, 1);
  MACHINE_PORT.print(F(",arate="));        MACHINE_PORT.print(tune.aztRateGps, 1);
  MACHINE_PORT.print(F(",awin="));         MACHINE_PORT.print(tune.aztWindowG, 0);
  MACHINE_PORT.print(F(",asettle="));      MACHINE_PORT.print(tune.aztSettleMs);
  MACHINE_PORT.print(F(",gs="));           MACHINE_PORT.println(tune.graphScale);
}

static void emitCfg() {
  MACHINE_PORT.print(F("$CFG,"));
  MACHINE_PORT.print(cfg.countsPerGram, 4); MACHINE_PORT.print(',');
  MACHINE_PORT.print(cfg.tareOffset);       MACHINE_PORT.print(',');
  MACHINE_PORT.print(cfg.invert ? 1 : 0);   MACHINE_PORT.print(',');
  MACHINE_PORT.print(haveCal() ? 1 : 0);    MACHINE_PORT.print(',');
  MACHINE_PORT.print(cfg.axisFullScaleG, 0); MACHINE_PORT.print(',');
  MACHINE_PORT.println(cfg.azt ? 1 : 0);
}

static void emitWeight() {
  long raw = ringFiltered();
  float sd, pp;
  bool stable = isStable(&sd, &pp);
  if (!hxAlive) { emitErr("WEIGHT", "NO_ADC"); return; }
  if (!haveCal()) { emitErr("WEIGHT", "NOT_CALIBRATED"); return; }
  MACHINE_PORT.print(F("$WEIGHT,"));
  MACHINE_PORT.print(rawToGrams(raw), 1);   MACHINE_PORT.print(',');
  MACHINE_PORT.print(stable ? 1 : 0);       MACHINE_PORT.print(',');
  MACHINE_PORT.println(raw);
}

static void emitRaw() {
  if (!hxAlive) { emitErr("RAW", "NO_ADC"); return; }
  MACHINE_PORT.print(F("$RAW,")); MACHINE_PORT.println(ringFiltered());
}

static void emitStat() {
  float mean, sd; long mn, mx;
  ringStats(&mean, &sd, &mn, &mx);
  MACHINE_PORT.print(F("$STAT,"));
  MACHINE_PORT.print(mean, 1);     MACHINE_PORT.print(',');
  MACHINE_PORT.print(sd, 1);       MACHINE_PORT.print(',');
  MACHINE_PORT.print(mx - mn);     MACHINE_PORT.print(',');
  MACHINE_PORT.println(ringCount);
}

/* ---------------------------------------------------------------------------
 * Step 1: HX711 <-> ESP32 wiring
 * ------------------------------------------------------------------------ */


/* ---------------------------------------------------------------------------
 * Step 1: HX711 detection / test
 * ------------------------------------------------------------------------ */

static bool hxSelfTest() {
  title(F("STEP 1/3  AMPLIFIER"));

  HUMAN_PORT.print(F("DT pin idle level: "));
  pinMode(PIN_HX711_DT, INPUT);
  HUMAN_PORT.println(digitalRead(PIN_HX711_DT) ? F("HIGH (busy/not ready)") : F("LOW (ready)"));

  HUMAN_PORT.print(F("Waiting for the HX711 to report data ready ... "));
  if (!hx.wait_ready_timeout(2000, 1)) {
    HUMAN_PORT.println(F("TIMEOUT"));
    HUMAN_PORT.println();
    HUMAN_PORT.println(F("[ERROR] No response from the HX711. Check, in this order:"));
    HUMAN_PORT.println(F("  1. VCC and GND actually reach the module (measure 3.3V)."));
    HUMAN_PORT.println(F("  2. DT and SCK are not swapped."));
    HUMAN_PORT.println(F("  3. DT/SCK are on the GPIOs printed in stage 1."));
    HUMAN_PORT.println(F("  4. Grounds are common between ESP32 and HX711."));
    HUMAN_PORT.println(F("  5. The module is not a dead/counterfeit board: DT should"));
    HUMAN_PORT.println(F("     pulse low ~10 times a second once powered."));
    HUMAN_PORT.println();
    HUMAN_PORT.println(F("[R] retry   [X] abort"));
    return false;
  }
  HUMAN_PORT.println(F("OK"));

  // Power-cycle test: a real HX711 goes quiet on power down and comes back.
  HUMAN_PORT.print(F("Power-down / power-up response ... "));
  hx.power_down();
  delay(100);
  bool wentQuiet = !hx.is_ready();
  hx.power_up();
  delay(100);
  bool cameBack = hx.wait_ready_timeout(2000, 1);
  if (cameBack && wentQuiet) {
    HUMAN_PORT.println(F("OK"));
  } else if (cameBack) {
    HUMAN_PORT.println(F("partial (did not go quiet - odd, but usable)"));
  } else {
    HUMAN_PORT.println(F("FAILED (did not come back)"));
    HUMAN_PORT.println(F("[ERROR] The HX711 did not recover from power down."));
    HUMAN_PORT.println(F("        Suspect an unstable supply or a bad SCK connection."));
    HUMAN_PORT.println(F("[R] retry   [X] abort"));
    return false;
  }

  long v;
  HUMAN_PORT.print(F("Sample read: "));
  if (!hxReadRaw(&v)) {
    HUMAN_PORT.println(F("FAILED"));
    HUMAN_PORT.println(F("[R] retry   [X] abort"));
    return false;
  }
  HUMAN_PORT.println(v);
  hxSample = v;
  hxDtLow  = (digitalRead(PIN_HX711_DT) == LOW);
  hxOk     = true;
  hxAlive = true;
  lastSampleMs = millis();
  ringClear();

  if (labs(v) > HX_SAT_LIMIT) {
    HUMAN_PORT.println(F("At the rail, which is EXPECTED with A+/A- unconnected."));
  }
  HUMAN_PORT.println(F("HX711 communication works."));
  markStage(STG_CHECK);
  footer(F("[ENTER] continue   [R] test again"));
  return true;
}



/* ---------------------------------------------------------------------------
 * Step 1: electrical sanity checks and the noise measurement
 * ------------------------------------------------------------------------ */

static void runSanity(bool asStage) {
  title(F("STEP 1/3  AMPLIFIER AND NOISE"));
  HUMAN_PORT.println(F("Do not touch or load the setup during this test."));
  HUMAN_PORT.print(F("Sampling"));

  float mean, sd; long mn, mx;
  if (!hxCapture(AVG_SAMPLES, &mean, &sd, &mn, &mx)) {
    HUMAN_PORT.println(F("[ERROR] Could not collect samples from the HX711."));
    HUMAN_PORT.println(F("        The ADC answered earlier, so suspect the supply or a"));
    HUMAN_PORT.println(F("        loose DT/SCK wire. [R] retry   [X] abort"));
    return;
  }

  noiseSd = sd;
  snMean = mean; snSd = sd; snMin = mn; snMax = mx; snDone = true;

  // This is what "steady" is measured against from now on, so it has to
  // outlive the reboot. Everything gated on steadiness depends on it.
  // Only STEP 1 sets it: D runs the same measurement to report, and a
  // diagnostic read should not quietly rewrite a stored setting - least of
  // all one taken while someone is leaning on the bench.
  if (asStage) {
    tune.noiseFloor = sd;
    tuneSave();
  }
  HUMAN_PORT.print(F("mean ")); HUMAN_PORT.print(mean, 0);
  HUMAN_PORT.print(F("  sd ")); HUMAN_PORT.print(sd, 1);
  HUMAN_PORT.print(F("  min ")); HUMAN_PORT.print(mn);
  HUMAN_PORT.print(F("  max ")); HUMAN_PORT.print(mx);
  HUMAN_PORT.print(F("  p-p ")); HUMAN_PORT.println(mx - mn);
  HUMAN_PORT.print(F("Full-scale use: "));
  HUMAN_PORT.print((fabsf(mean) / (float)HX_FULL_SCALE) * 100.0f, 1);
  HUMAN_PORT.println('%');
  HUMAN_PORT.println();

  bool bad = false;

  if (fabsf(mean) > (float)HX_SAT_LIMIT) {
    bad = true;
    HUMAN_PORT.println(F("[FAIL] The ADC is saturated (pinned at full scale)."));
    HUMAN_PORT.println(F("       Typical causes, most likely first:"));
    HUMAN_PORT.println(F("         - A+ or A- not actually landed on the HX711."));
    HUMAN_PORT.println(F("         - E+ / E- swapped with A+ / A-. Note that A- comes"));
    HUMAN_PORT.println(F("           BEFORE A+ on the green board - wire by label."));
    HUMAN_PORT.println(F("         - A broken cell lead, or a damaged bridge."));
    HUMAN_PORT.println(F("       Check the five cell wires at the HX711 before going on."));
  } else if (fabsf(mean) > (float)HX_FULL_SCALE * 0.80f) {
    HUMAN_PORT.println(F("[WARN] Resting value uses >80% of full scale. The bridge is"));
    HUMAN_PORT.println(F("       far off balance, so you have little range left for"));
    HUMAN_PORT.println(F("       actual weight. Most likely the cell is mechanically"));
    HUMAN_PORT.println(F("       pre-loaded - something is resting on it, or it is"));
    HUMAN_PORT.println(F("       bolted down in a way that stops it flexing freely."));
  }

  if (mx == mn) {
    bad = true;
    HUMAN_PORT.println(F("[FAIL] Every sample is byte-identical. A real bridge always"));
    HUMAN_PORT.println(F("       dithers by a few counts. Suspect DT stuck high/low, a"));
    HUMAN_PORT.println(F("       floating DT line, or a dead HX711."));
  }

  if (fabsf(mean) < 100.0f && sd < 5.0f) {
    bad = true;
    HUMAN_PORT.println(F("[FAIL] Reading is essentially a hard zero with no noise."));
    HUMAN_PORT.println(F("       That is not what a live cell looks like; check that"));
    HUMAN_PORT.println(F("       E+ / E- really reach the cell and that its leads are"));
    HUMAN_PORT.println(F("       not shorted together."));
  }

  if (sd > 5000.0f) {
    HUMAN_PORT.println(F("[WARN] Very noisy (sd > 5000 counts). Look at:"));
    HUMAN_PORT.println(F("         - a long or unshielded cell cable, or its shield"));
    HUMAN_PORT.println(F("           left unconnected,"));
    HUMAN_PORT.println(F("         - a noisy USB/5V supply (try a different source),"));
    HUMAN_PORT.println(F("         - missing common ground between HX711 and ESP32,"));
    HUMAN_PORT.println(F("         - loose screw terminals or cold solder joints,"));
    HUMAN_PORT.println(F("         - draughts and vibration on the platform."));
  } else if (sd > 500.0f) {
    HUMAN_PORT.println(F("[NOTE] Moderate noise (sd > 500 counts). Workable, but the"));
    HUMAN_PORT.println(F("       resolution you can trust will be limited."));
  } else {
    HUMAN_PORT.println(F("[OK]   Noise level looks reasonable."));
  }

  if (!bad) {
    HUMAN_PORT.println(F("[OK]   Electrical behaviour is consistent with a live bridge."));
  }
  if (asStage) {
    if (!bad) markStage(STG_CHECK);   // a failing bridge does not pass the stage
    footer(F("[ENTER] continue   [R] re-run"));
  } else {
    HUMAN_PORT.println();
    HUMAN_PORT.println(F("Diagnostics done. [H] for help."));
  }
}


/* ---------------------------------------------------------------------------
 * Step 2: zero   /   Step 3: calibration
 * ------------------------------------------------------------------------ */

static void tarePrompt() {
  title(F("STEP 2/3  ZERO"));
  HUMAN_PORT.println(F("Remove everything from the platform. Let it settle."));
  footer(F("[ENTER] capture zero"));
}

static void tareCapture(bool uiAdvance) {
  HUMAN_PORT.print(F("Capturing zero"));
  float mean, sd; long mn, mx;
  if (!hxCapture(AVG_SAMPLES, &mean, &sd, &mn, &mx)) {
    HUMAN_PORT.println(F("[ERROR] No samples. Tare not changed. [ENTER] to retry."));
    emitErr("TARE", "NO_ADC");
    return;
  }
  if (fabsf(mean) > (float)HX_SAT_LIMIT) {
    HUMAN_PORT.println(F("[ERROR] ADC saturated - refusing to tare on a railed reading."));
    HUMAN_PORT.println(F("        Run 'D' diagnostics and fix that first."));
    emitErr("TARE", "SATURATED");
    return;
  }
  cfg.tareOffset = (long)llround(mean);
  aztOffset = 0;                      // a fresh zero spends the drift
  aztTotal  = 0.0f;
  cfgSave();
  HUMAN_PORT.print(F("Tare offset = ")); HUMAN_PORT.print(cfg.tareOffset);
  HUMAN_PORT.print(F("  (noise sd ")); HUMAN_PORT.print(sd, 1);
  HUMAN_PORT.println(F(" counts, saved to NVS)"));
  noiseSd = sd;
  emitOk("TARE");
  markStage(STG_ZERO);
  if (uiAdvance) advance();
}

static void calKnownPrompt() {
  title(F("STEP 3/3  CALIBRATION"));
  if (detail) {
    HUMAN_PORT.println(F("You need one object of known mass. Heavier is better -"));
    HUMAN_PORT.println(F("aim for at least a fifth of the range you care about."));
  }
  HUMAN_PORT.println(F("Mass in grams, e.g. 500     [?] detail   [X] menu"));
  HUMAN_PORT.print(F("> "));
}

static void calPlacePrompt() {
  HUMAN_PORT.print(F("Place the "));
  HUMAN_PORT.print(calKnownGrams, 1);
  HUMAN_PORT.println(F(" g mass in the CENTRE of the platform."));
  footer(F("[ENTER] capture"));
}

static void calCapture(bool uiAdvance) {
  HUMAN_PORT.print(F("Capturing loaded value"));
  float mean, sd; long mn, mx;
  if (!hxCapture(AVG_SAMPLES, &mean, &sd, &mn, &mx)) {
    HUMAN_PORT.println(F("[ERROR] No samples. Calibration unchanged."));
    emitErr("CAL", "NO_ADC");
    return;
  }
  if (fabsf(mean) > (float)HX_SAT_LIMIT) {
    HUMAN_PORT.println(F("[ERROR] ADC saturated under load. Calibration unchanged."));
    HUMAN_PORT.println(F("        The cell has no headroom left - re-run step 1."));
    emitErr("CAL", "SATURATED");
    return;
  }

  float span = mean - (float)zeroNow();
  HUMAN_PORT.print(F("Span for ")); HUMAN_PORT.print(calKnownGrams, 1);
  HUMAN_PORT.print(F(" g = ")); HUMAN_PORT.print(span, 0); HUMAN_PORT.println(F(" counts"));

  // A span that is not clearly above the noise floor cannot be calibrated.
  float floorCounts = (noiseSd > 1.0f) ? noiseSd * 20.0f : 200.0f;
  if (fabsf(span) < floorCounts) {
    HUMAN_PORT.println(F("[ERROR] The mass barely changed the reading."));
    HUMAN_PORT.println(F("        Check, in this order:"));
    HUMAN_PORT.println(F("          1. The mass is really on the cell's platform."));
    HUMAN_PORT.println(F("          2. A+/A- are connected. Note A- comes BEFORE A+"));
    HUMAN_PORT.println(F("             on the green board - wire by label."));
    HUMAN_PORT.println(F("          3. The load actually reaches the cell along its"));
    HUMAN_PORT.println(F("             axis. An S-beam ignores most of a side load."));
    HUMAN_PORT.println(F("          4. Use a heavier reference mass."));
    HUMAN_PORT.println(F("        Calibration NOT saved. [ENTER] to retry."));
    emitErr("CAL", "NO_SPAN");
    return;
  }

  // Record this weight as a calibration point and refit. One point behaves
  // exactly as before; more points give a real line and a linearity figure.
  // Re-weighing the same mass replaces its point rather than adding a
  // duplicate, so repeating step 3 does not fill the table with one load.
  uint8_t slot = tune.calN;
  for (uint8_t i = 0; i < tune.calN; i++) {
    if (fabsf(tune.calG[i] - calKnownGrams) < calKnownGrams * 0.02f) { slot = i; break; }
  }
  if (slot >= CAL_MAX_PTS) {
    // Table full: drop the oldest point to make room for this one.
    for (uint8_t i = 1; i < CAL_MAX_PTS; i++) {
      tune.calG[i - 1] = tune.calG[i]; tune.calRaw[i - 1] = tune.calRaw[i];
    }
    slot = CAL_MAX_PTS - 1;
  } else if (slot == tune.calN) {
    tune.calN++;
  }
  tune.calG[slot]   = calKnownGrams;
  tune.calRaw[slot] = mean;
  tuneSave();

  if (!calApplyFit()) {
    HUMAN_PORT.println(F("[ERROR] Those points do not make a usable line."));
    HUMAN_PORT.println(F("        Use masses that differ, then try again."));
    emitErr("CAL", "NO_FIT");
    return;
  }

  HUMAN_PORT.print(F("Points: "));
  HUMAN_PORT.print(tune.calN);
  HUMAN_PORT.print(F(" of "));
  HUMAN_PORT.println(CAL_MAX_PTS);
  if (tune.calN >= 2) {
    HUMAN_PORT.print(F("Linearity: worst point is "));
    HUMAN_PORT.print(tune.calLinG, 1);
    HUMAN_PORT.print(F(" g off the line ("));
    HUMAN_PORT.print(tune.calLinPct, 3);
    HUMAN_PORT.println(F(" %FS)"));
  } else {
    HUMAN_PORT.println(F("Linearity: needs a second weight to mean anything."));
  }

  HUMAN_PORT.print(F("Calibration factor = "));
  HUMAN_PORT.print(cfg.countsPerGram, 4);
  HUMAN_PORT.println(F(" counts/gram  (saved to NVS)"));
  if (cfg.invert) {
    HUMAN_PORT.println(F("Sign inversion enabled automatically (load reduced the raw"));
    HUMAN_PORT.println(F("value). Toggle it any time with 'I'."));
  }
  float resolution = 1.0f / cfg.countsPerGram;
  HUMAN_PORT.print(F("One ADC count = ")); HUMAN_PORT.print(resolution, 4);
  HUMAN_PORT.println(F(" g (theoretical, before noise)"));
  if (noiseSd > 1.0f) {
    HUMAN_PORT.print(F("Realistic resolution from measured noise: about "));
    HUMAN_PORT.print(noiseSd * 2.0f / cfg.countsPerGram, 2);
    HUMAN_PORT.println(F(" g"));
  }
  emitOk("CAL");
  emitCfg();
  markStage(STG_CAL);
  if (uiAdvance) advance();
}

/* ---------------------------------------------------------------------------
 * Impact / peak capture ("hits mode")
 *
 * One cell under the plate measures the TOTAL force arriving on it. It cannot
 * tell you where the hit landed - that would need several cells and an
 * amplifier each, which the game does not ask for.
 * ------------------------------------------------------------------------ */

// Deviation in grams when calibrated, raw counts otherwise.
static float devToUnits(long dev) {
  if (!haveCal()) return (float)dev;
  return (float)dev / cfg.countsPerGram;
}

static const __FlashStringHelper* hitUnit() {
  return haveCal() ? F(" g") : F(" counts");
}

// Count how many samples the HX711 actually delivers in one second. This is the
// hard limit on impact capture and depends on the module's RATE pin, not on us.
static float measureSps() {
  uint32_t t0 = millis();
  uint16_t n = 0;
  while ((millis() - t0) < 1000) {
    if (hx.is_ready()) { hx.read(); n++; }
  }
  return (float)n;
}

static void hitsReport() {
  long  dev    = hitPeakRaw - hitBaseline;
  float peak   = fabsf(devToUnits(dev));
  uint32_t rise  = hitPeakMs - hitStartMs;
  uint32_t width = millis() - hitStartMs;

  hitCount++;
  if (peak > hitMaxSoFar) hitMaxSoFar = peak;
  lastHit[0] = peak; lastHit[1] = (float)rise; lastHit[2] = (float)width;
  lastHit[3] = (float)hitSamples; lastHit[4] = (float)hitImpulse;

  HitRec& r = hitLog[hitLogHead];
  r.peak    = peak;
  r.impulse = (float)hitImpulse;
  r.rise    = (uint16_t)((rise  > 65535) ? 65535 : rise);
  r.width   = (uint16_t)((width > 65535) ? 65535 : width);
  r.samples = hitSamples;
  r.at      = millis();
  hitLogHead = (uint8_t)((hitLogHead + 1) % HIT_LOG);
  if (hitLogN < HIT_LOG) hitLogN++;
  hitLogSeq++;

  HUMAN_PORT.print(F("HIT "));
  HUMAN_PORT.print(hitCount);
  HUMAN_PORT.print(F("   peak "));
  HUMAN_PORT.print(peak, 1);
  HUMAN_PORT.print(hitUnit());
  HUMAN_PORT.print(F("   rise "));
  HUMAN_PORT.print(rise);
  HUMAN_PORT.print(F(" ms   width "));
  HUMAN_PORT.print(width);
  HUMAN_PORT.print(F(" ms   samples "));
  HUMAN_PORT.print(hitSamples);
  HUMAN_PORT.println();
  HUMAN_PORT.print(F("        impulse "));
  HUMAN_PORT.print((float)hitImpulse, 2);
  HUMAN_PORT.print(haveCal() ? F(" g.s") : F(" cts.s"));
  if (hitSamples < 3) {
    HUMAN_PORT.print(F("   (peak is a guess at this rate,"));
    HUMAN_PORT.print(F(" impulse is not)"));
  }
  HUMAN_PORT.println();

  MACHINE_PORT.print(F("$HIT,"));
  MACHINE_PORT.print(hitStartMs); MACHINE_PORT.print(',');
  MACHINE_PORT.print(peak, 1);   MACHINE_PORT.print(',');
  MACHINE_PORT.print(rise);      MACHINE_PORT.print(',');
  MACHINE_PORT.print(width);     MACHINE_PORT.print(',');
  MACHINE_PORT.print(hitSamples); MACHINE_PORT.print(',');
  MACHINE_PORT.println((float)hitImpulse, 3);

  hitActive = false;
  hitBelow  = 0;
}

// Fed one RAW sample at a time from pollSample(). Deliberately not the filtered
// ring buffer - the trimmed mean is designed to throw spikes away, which is
// exactly the wrong thing here.
static void hitsFeed(long raw) {
  long  dev   = raw - hitBaseline;
  float units = fabsf(devToUnits(dev));

  if (!hitActive) {
    if (units >= tune.hitThresh) {
      hitActive  = true;
      hitStartMs = millis();
      hitPeakMs  = hitStartMs;
      hitPeakRaw = raw;
      hitSamples = 1;
      hitBelow   = 0;
      hitImpulse = 0.0;
      hitLastMs  = hitStartMs;
    } else {
      // Track slow drift only while genuinely quiet, so the baseline never
      // creeps up into a hit.
      if (units < tune.hitThresh * 0.25f) {
        hitBaseline += dev / (long)HIT_BASELINE_TC;
      }
    }
    return;
  }

  hitSamples++;
  // Area under the curve. A sigma-delta ADC integrates over its conversion
  // window, so a spike shorter than the window loses its HEIGHT but keeps most
  // of its AREA. At 10 SPS the impulse is far more repeatable than the peak.
  uint32_t nowMs = millis();
  hitImpulse += (double)units * (double)(nowMs - hitLastMs) / 1000.0;
  hitLastMs = nowMs;

  if (labs(dev) > labs(hitPeakRaw - hitBaseline)) {
    hitPeakRaw = raw;
    hitPeakMs  = nowMs;
  }
  if (units < tune.hitThresh * tune.hitReleaseFrac) {
    if (++hitBelow >= tune.hitReleaseN) hitsReport();
  } else {
    hitBelow = 0;
    // Something is sitting on the plate rather than hitting it.
    if ((millis() - hitStartMs) > tune.hitMaxMs) {
      HUMAN_PORT.println(F("(load held for 2 s - closing the event)"));
      hitsReport();
    }
  }
}

static void enterHits() {
  title(F("HITS MODE  (impact / peak capture)"));

  HUMAN_PORT.print(F("Measuring the HX711 sample rate ... "));
  hitSps = measureSps();
  HUMAN_PORT.print(hitSps, 0);
  HUMAN_PORT.println(F(" samples/s"));

  if (hitSps < HIT_SPS_USABLE) {
    HUMAN_PORT.println();
    HUMAN_PORT.println(F("[WARNING] That is too slow for real impact measurement."));
    HUMAN_PORT.print(F("          One sample every "));
    HUMAN_PORT.print(hitSps > 0 ? (1000.0f / hitSps) : 0.0f, 0);
    HUMAN_PORT.println(F(" ms, so a short hit is easily"));
    HUMAN_PORT.println(F("          missed entirely, and a peak you do catch is"));
    HUMAN_PORT.println(F("          whatever the converter happened to be averaging."));
    HUMAN_PORT.println(F("          Treat readings as 'a hit happened, roughly this"));
    HUMAN_PORT.println(F("          hard' - not as peak force."));
    HUMAN_PORT.println();
    HUMAN_PORT.println(F("          To get 80 SPS you must change the HX711 module:"));
    HUMAN_PORT.println(F("          its RATE pin is tied to GND on most boards. Lift it"));
    HUMAN_PORT.println(F("          and tie it to VCC (some boards have a jumper). That"));
    HUMAN_PORT.println(F("          is a hardware change, nothing here can do it."));
    HUMAN_PORT.println(F("          Even 80 SPS gives only ~12.5 ms resolution; a load"));
    HUMAN_PORT.println(F("          cell plus HX711 is a weighing chain, not an impact"));
    HUMAN_PORT.println(F("          sensor. A piezo disc or accelerometer suits hits."));
  }

  HUMAN_PORT.println();
  HUMAN_PORT.println(F("Keep the plate still - taking a resting baseline."));
  float mean, sd; long mn, mx;
  if (hxCapture(AVG_SAMPLES, &mean, &sd, &mn, &mx)) {
    hitBaseline = (long)llround(mean);
    noiseSd = sd;
    // A threshold under the noise floor would trigger on nothing at all.
    // DERIVED, not configured. What counts as a hit is the game's call;
    // all the box needs is a floor high enough that its own noise cannot
    // trip it. Six sigma of the resting noise, measured just now.
    float floorUnits = fabsf(devToUnits((long)(sd * 6.0f)));
    if (floorUnits < 1.0f) floorUnits = 1.0f;
    tune.hitThresh = floorUnits;   // live only - not written to NVS
    HUMAN_PORT.print(F("Trigger "));
    HUMAN_PORT.print(tune.hitThresh, 1);
    HUMAN_PORT.print(hitUnit());
    HUMAN_PORT.println(F(" (6 sigma of the measured noise)."));
  } else {
    HUMAN_PORT.println(F("[ERROR] No samples from the HX711 - cannot arm."));
    enterState(ST_IDLE);
    return;
  }

  hitActive   = false;
  hitSamples  = 0;
  hitBelow    = 0;
  hitCount    = 0;
  hitMaxSoFar = 0.0f;

  HUMAN_PORT.print(F("Baseline "));
  HUMAN_PORT.print(hitBaseline);
  HUMAN_PORT.print(F("   trigger at "));
  HUMAN_PORT.print(tune.hitThresh, 1);
  HUMAN_PORT.print(hitUnit());
  if (!haveCal()) HUMAN_PORT.print(F("  (not calibrated - raw counts)"));
  HUMAN_PORT.println();
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("  THRESHOLD is how big a change counts as a hit."));
  HUMAN_PORT.println(F("  Too low and noise triggers it, too high and soft"));
  HUMAN_PORT.println(F("  hits are ignored. Type a number to change it."));
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("  Each hit reports:"));
  HUMAN_PORT.println(F("    peak     biggest force seen during the hit"));
  HUMAN_PORT.println(F("    rise     ms from start of hit to that peak"));
  HUMAN_PORT.println(F("    width    ms the whole hit lasted"));
  HUMAN_PORT.println(F("    samples  how many readings it was built from -"));
  HUMAN_PORT.println(F("             under 3 and the peak is a guess"));
  HUMAN_PORT.println(F("    impulse  area under the whole event."));
  HUMAN_PORT.println(F("             THIS is the number to score on at a"));
  HUMAN_PORT.println(F("             slow sample rate. The converter averages"));
  HUMAN_PORT.println(F("             over its window, so a short hit loses"));
  HUMAN_PORT.println(F("             its height but keeps its area. Two"));
  HUMAN_PORT.println(F("             identical hits give wandering peaks but"));
  HUMAN_PORT.println(F("             similar impulses."));
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("  ARMED. Hit the plate."));
  HUMAN_PORT.println(F("    <number> threshold   [T] re-zero   [Z] reset"));
  HUMAN_PORT.println(F("    [X] exit"));
}

// Newest first. i = 0 is the most recent hit.
static const HitRec* hitAt(uint8_t i) {
  if (i >= hitLogN) return NULL;
  uint8_t idx = (uint8_t)((hitLogHead + HIT_LOG - 1 - i) % HIT_LOG);
  return &hitLog[idx];
}

static void hitLogClear() {
  hitLogN = 0; hitLogHead = 0; hitLogSeq = 0;
}

static void hitsStats() {
  HUMAN_PORT.print(F("hits "));
  HUMAN_PORT.print(hitCount);
  HUMAN_PORT.print(F("   hardest "));
  HUMAN_PORT.print(hitMaxSoFar, 1);
  HUMAN_PORT.print(hitUnit());
  HUMAN_PORT.print(F("   at "));
  HUMAN_PORT.print(hitSps, 0);
  HUMAN_PORT.println(F(" SPS"));
}

/* ---------------------------------------------------------------------------
 * Free measurement mode
 *
 * Just show me what the thing is doing: live value, peak hold, min/max, noise
 * and a short history. Works before calibration too, in raw counts - which is
 * when you most want it, while wiring and mounting.
 * ------------------------------------------------------------------------ */

static const uint8_t SPARK_LEN = 24;

static float   mNow = 0, mPeak = 0, mMin = 0, mMax = 0;
static bool    mHave = false;
static long    mZero = 0;
static uint32_t mCount = 0;
static float   mSpark[SPARK_LEN];
static uint8_t mSparkN = 0, mSparkHead = 0;
static uint32_t mLastPrint = 0;
static bool    mPanel = false;     // full panel on the next refresh

static const __FlashStringHelper* mUnit() {
  return haveCal() ? F(" g") : F(" cts");
}

static void measureReset() {
  mHave = false;
  mPeak = mMin = mMax = mNow = 0;
  mCount = 0;
  mSparkN = mSparkHead = 0;
}

static void measureFeed(long raw) {
  float v = haveCal() ? rawToGrams(raw) : (float)(raw - mZero);
  mNow = v;
  mCount++;
  if (!mHave) { mHave = true; mMin = mMax = mPeak = v; }
  if (v < mMin) mMin = v;
  if (v > mMax) mMax = v;
  if (fabsf(v) > fabsf(mPeak)) mPeak = v;

  mSpark[mSparkHead] = v;
  mSparkHead = (mSparkHead + 1) % SPARK_LEN;
  if (mSparkN < SPARK_LEN) mSparkN++;
}

static void measureSparkline() {
  if (mSparkN < 2) return;
  float lo = mSpark[0], hi = mSpark[0];
  for (uint8_t i = 0; i < mSparkN; i++) {
    if (mSpark[i] < lo) lo = mSpark[i];
    if (mSpark[i] > hi) hi = mSpark[i];
  }
  float span = hi - lo;
  if (span < 1e-6f) span = 1.0f;
  uint8_t start = (mSparkN < SPARK_LEN) ? 0 : mSparkHead;
  for (uint8_t i = 0; i < mSparkN; i++) {
    uiSpark((mSpark[(start + i) % SPARK_LEN] - lo) / span);
  }
}

static void measureLine() {
  uiCol(C_VAL);
  HUMAN_PORT.print(mNow, haveCal() ? 1 : 0);
  HUMAN_PORT.print(mUnit());
  uiCol(C_RESET);
  uiCol(C_DIM); HUMAN_PORT.print(F("   peak ")); uiCol(C_RESET);
  HUMAN_PORT.print(mPeak, haveCal() ? 1 : 0);
  uiCol(C_DIM); HUMAN_PORT.print(F("   ")); uiCol(C_RESET);
  measureSparkline();
  HUMAN_PORT.println();
}

static void measurePanel() {
  title(F("MONITOR"));
  if (!mHave) { HUMAN_PORT.println(F("  waiting for the HX711 ...")); return; }

  HUMAN_PORT.print(F("    "));
  uiCol(C_VAL);
  HUMAN_PORT.print(mNow, haveCal() ? 1 : 0);
  HUMAN_PORT.print(mUnit());
  uiCol(C_RESET);
  if (haveCal()) {
    HUMAN_PORT.print(F("        "));
    HUMAN_PORT.print(mNow / 1000.0f, 4);
    HUMAN_PORT.print(F(" kg"));
  }
  HUMAN_PORT.println();
  HUMAN_PORT.println();

  // Bar is relative to the largest thing seen this session, so it is useful
  // whatever scale you happen to be working at.
  float ref = fabsf(mMax) > fabsf(mMin) ? fabsf(mMax) : fabsf(mMin);
  if (ref < 1e-6f) ref = 1.0f;
  HUMAN_PORT.print(F("    "));
  uiBar(fabsf(mNow) / ref, 30);
  HUMAN_PORT.println();
  HUMAN_PORT.println();

  HUMAN_PORT.print(F("    peak  ")); HUMAN_PORT.print(mPeak, haveCal() ? 1 : 0);
  HUMAN_PORT.print(F("      min  ")); HUMAN_PORT.println(mMin, haveCal() ? 1 : 0);
  HUMAN_PORT.print(F("    max   ")); HUMAN_PORT.print(mMax, haveCal() ? 1 : 0);
  HUMAN_PORT.print(F("      p-p  ")); HUMAN_PORT.println(mMax - mMin, haveCal() ? 1 : 0);
  HUMAN_PORT.print(F("    samples ")); HUMAN_PORT.println(mCount);
  HUMAN_PORT.println();
  HUMAN_PORT.print(F("    "));
  measureSparkline();
  HUMAN_PORT.println();
  if (!haveCal()) {
    HUMAN_PORT.println();
    uiCol(C_DIM);
    HUMAN_PORT.println(F("    not calibrated - values are raw ADC counts"));
    uiCol(C_RESET);
  }
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("    [Z] reset   [T] zero here   [ENTER] panel   [X] exit"));
}

static void enterMeasure() {
  measureReset();
  mZero = ringCount ? ringFiltered() : 0;
  title(F("MONITOR"));
  HUMAN_PORT.println(F("  Live reading with peak hold. Nothing is saved,"));
  HUMAN_PORT.println(F("  nothing is changed - safe to sit in while wiring."));
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("  What you get:"));
  HUMAN_PORT.println(F("    now     the reading right this second"));
  HUMAN_PORT.println(F("    peak    largest reading since the last reset"));
  HUMAN_PORT.println(F("    min/max lowest and highest seen"));
  HUMAN_PORT.println(F("    p-p     max minus min = how much it wanders"));
  HUMAN_PORT.println(F("            this IS your noise floor. Untouched, it"));
  HUMAN_PORT.println(F("            should be small and steady."));
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("    [Z] reset stats   [T] call this zero"));
  HUMAN_PORT.println(F("    [ENTER] full panel   [V] plain/unicode   [X] exit"));
  HUMAN_PORT.println();
  mLastPrint = 0;
}

/* ---------------------------------------------------------------------------
 * Idle readout, help, diagnostics
 * ------------------------------------------------------------------------ */

static void printLive() {
  if (!hxAlive || ringCount == 0) {
    HUMAN_PORT.println(F("no data from HX711 - run 'S'"));
    return;
  }
  long raw = ringFiltered();
  float sd, pp;
  bool stable = isStable(&sd, &pp);

  if (showRaw || !haveCal()) {
    HUMAN_PORT.print(F("raw "));
    HUMAN_PORT.print(raw);
    HUMAN_PORT.print(F("   sd "));
    HUMAN_PORT.print(sd, 1);
    HUMAN_PORT.print(F("   p-p "));
    HUMAN_PORT.print(pp, 0);
    if (!haveCal()) HUMAN_PORT.print(F("   [not calibrated: run 'C']"));
    HUMAN_PORT.println();
    return;
  }
  float g = rawToGrams(raw);
  HUMAN_PORT.print(g, 1);
  HUMAN_PORT.print(F(" g   "));
  HUMAN_PORT.print(g / 1000.0f, 4);
  HUMAN_PORT.print(F(" kg   "));
  HUMAN_PORT.print(stable ? F("[stable]") : F("[settling]"));
  HUMAN_PORT.print(F("   sd "));
  HUMAN_PORT.print(sd, 2);
  HUMAN_PORT.print(F(" g   raw "));
  HUMAN_PORT.println(raw);
}

static void printHelp() {
  title(F("COMMANDS"));
  HUMAN_PORT.println(F("  S  setup menu (pick or repeat any step)"));
  HUMAN_PORT.println(F("  T  tare / zero the scale"));
  HUMAN_PORT.println(F("  C  calibrate with a known mass"));
  HUMAN_PORT.println(F("  M  monitor: live value, peak hold, min/max"));
  HUMAN_PORT.println(F("  V  display style (plain / unicode / colour)"));
  HUMAN_PORT.println(F("  K  impact mode: capture hits - THIS is the game mechanic"));
  HUMAN_PORT.println(F("  R  toggle raw-count readout"));
  HUMAN_PORT.println(F("  D  diagnostics (noise, saturation, stored settings)"));
  HUMAN_PORT.println(F("  I  invert the weight sign (saved)"));
  HUMAN_PORT.println(F("  Q  quiet: pause/resume the live readout"));
  HUMAN_PORT.println(F("  E  erase the calibration and zero (settings kept)"));
  HUMAN_PORT.println(F("  H  this help"));
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("Inside a stage:"));
  HUMAN_PORT.println(F("  ?  show that stage's full explanation again"));
  HUMAN_PORT.println(F("  X  leave the stage, back to the menu"));
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("Progress is stored, so a reboot part-way through setup"));
  HUMAN_PORT.println(F("comes back to the menu with the finished stages ticked."));
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("Drift cancelling is a page setting ($AZT,0 / $AZT,1)."));
  HUMAN_PORT.println(F("Tuning lives on the Sensor tab. $TUNE lists it, $TUNE,flen,16 sets."));
  HUMAN_PORT.println(F("Host protocol: lines starting with '$' (try $ID, $CFG, $W)."));
  hr();
}

static void printStored() {
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("Stored settings (NVS):"));
  HUMAN_PORT.print(F("  calibrated      : ")); HUMAN_PORT.println(haveCal() ? F("yes") : F("no"));
  HUMAN_PORT.print(F("  counts per gram : ")); HUMAN_PORT.println(cfg.countsPerGram, 4);
  HUMAN_PORT.print(F("  axis full scale : "));
  HUMAN_PORT.print(cfg.axisFullScaleG / 1000.0f, 1); HUMAN_PORT.println(F(" kg"));
  HUMAN_PORT.print(F("  cal points      : "));
  HUMAN_PORT.print(tune.calN);
  if (tune.calN >= 2) {
    HUMAN_PORT.print(F("   worst off line: "));
    HUMAN_PORT.print(tune.calLinG, 1); HUMAN_PORT.print(F(" g ("));
    HUMAN_PORT.print(tune.calLinPct, 3); HUMAN_PORT.print(F(" %FS)"));
  }
  HUMAN_PORT.println();
  HUMAN_PORT.print(F("  noise floor     : "));
  if (tune.noiseFloor > 0) {
    HUMAN_PORT.print(tune.noiseFloor, 1); HUMAN_PORT.println(F(" counts"));
  } else {
    HUMAN_PORT.println(F("not measured - run step 1"));
  }
  HUMAN_PORT.print(F("  filter / trim   : "));
  HUMAN_PORT.print(tune.filterLen); HUMAN_PORT.print(F(" / "));
  HUMAN_PORT.println(tune.trimCount);
  HUMAN_PORT.print(F("  hit trigger     : "));
  HUMAN_PORT.print(tune.hitThresh, 1); HUMAN_PORT.println(F(" (display unit)"));
  HUMAN_PORT.print(F("  drift cancelling: "));
  HUMAN_PORT.print(cfg.azt ? F("on") : F("off"));
  HUMAN_PORT.print(F("   cancelled so far: "));
  HUMAN_PORT.print(aztTotal, 1); HUMAN_PORT.println(F(" g"));
  HUMAN_PORT.print(F("  tare offset     : ")); HUMAN_PORT.print(cfg.tareOffset);
  if (aztOffset) {
    HUMAN_PORT.print(F("   + drift ")); HUMAN_PORT.print(aztOffset);
    HUMAN_PORT.print(F(" = ")); HUMAN_PORT.print(zeroNow());
  }
  HUMAN_PORT.println();
  HUMAN_PORT.print(F("  sign inverted   : ")); HUMAN_PORT.println(cfg.invert ? F("yes") : F("no"));
  HUMAN_PORT.print(F("  stages passed   : "));
  for (uint8_t i = STG_CHECK; i <= STG_MAX; i++) {
    if (stageIsDone((Stage)i)) { HUMAN_PORT.print(i); HUMAN_PORT.print(' '); }
  }
  if (cfg.stagesDone == 0) HUMAN_PORT.print(F("none"));
  HUMAN_PORT.println();
}

static void runDiagnostics() {
  printStored();
  runSanity(false);
  float sd, pp;
  bool stable = isStable(&sd, &pp);
  HUMAN_PORT.print(F("Live stability: "));
  HUMAN_PORT.print(stable ? F("stable") : F("not stable"));
  HUMAN_PORT.print(F("  sd ")); HUMAN_PORT.print(sd, 2);
  HUMAN_PORT.print(F("  p-p ")); HUMAN_PORT.print(pp, 2);
  HUMAN_PORT.println(haveCal() ? F(" (grams)") : F(" (counts)"));
}

/* ---------------------------------------------------------------------------
 * State machine
 * ------------------------------------------------------------------------ */

// The stage picker. This is the hub: every stage returns here unless it was
// reached by "run all", and the [ok]/[--] column is read straight out of NVS so
// it is still right after a reboot.
static void showMenu() {
  static const char* const NAMES[STG_MAX] = {
    "Amplifier and noise",
    "Zero",
    "Calibration"
  };
  title(F("SETUP MENU"));
  for (uint8_t i = STG_CHECK; i <= STG_MAX; i++) {
    HUMAN_PORT.print(F("  "));
    HUMAN_PORT.print(i);
    HUMAN_PORT.print(stageIsDone((Stage)i) ? F("  [ok]  ") : F("  [--]  "));
    HUMAN_PORT.print(NAMES[i - 1]);
    // A couple of stages can show what they actually stored.
    if (i == STG_CAL && haveCal()) {
      HUMAN_PORT.print(F("             "));
      HUMAN_PORT.print(cfg.countsPerGram, 2);
      HUMAN_PORT.print(F(" counts/g"));
    }
    HUMAN_PORT.println();
  }
  HUMAN_PORT.println();
  HUMAN_PORT.println(F("  1-3  run that stage      A  run all from stage 1"));
  HUMAN_PORT.println(F("  X    live readout        H  help"));
  HUMAN_PORT.print(F("> "));
}

static void enterState(State s) {
  state = s;
  switch (s) {
    case ST_IDLE:
      runAll = false;
      HUMAN_PORT.println();
      HUMAN_PORT.println(F("Live readout."));
      HUMAN_PORT.println(F("  K impact   M monitor"));
      HUMAN_PORT.println(F("  T zero   C calibrate   D diagnostics"));
      HUMAN_PORT.println(F("  S setup menu   H help"));
      break;
    case ST_MENU:         runAll = false; showMenu(); break;
    case ST_W_CHECK:   runSanity(true); break;
    case ST_TARE:      tarePrompt(); break;
    case ST_CAL_KNOWN: calKnownPrompt(); break;
    case ST_CAL_PLACE: calPlacePrompt(); break;
    case ST_HITS:      enterHits(); break;
    case ST_MEASURE:   enterMeasure(); break;
  }
}

// Enter a stage by number. The long explanation is printed automatically the
// first time a stage is visited and on demand after that.
static void enterStage(Stage s) {
  // Where did we come from? Only a stage started from the live readout (a
  // one-off T or C) should return there. Anything started from the menu, or
  // chained from the previous stage, must walk FORWARD - otherwise every step
  // dead-ends back at the menu and the wizard never flows.
  fromIdle = (state == ST_IDLE);
  curStage = s;
  detail   = !stageIsDone(s);   // long explanation on the first visit only
  switch (s) {
    case STG_CHECK: enterState(ST_W_CHECK); break;
    case STG_ZERO:  enterState(ST_TARE); break;
    case STG_CAL:   enterState(ST_CAL_KNOWN); break;
    default:        enterState(ST_MENU); break;
  }
}

// Where to go when a stage finishes.
static void advance() {
  if (fromIdle) { enterState(ST_IDLE); return; }     // one-off tare/calibrate
  if (curStage < STG_MAX) { enterStage((Stage)(curStage + 1)); return; }
  runAll = false;
  title(F("SETUP COMPLETE"));
  HUMAN_PORT.println(F("Calibration is stored in NVS. It survives reboots and"));
  HUMAN_PORT.println(F("re-flashing; only 'E' or a full flash erase clears it."));
  HUMAN_PORT.println(F("Verify it: put a DIFFERENT known mass on the platform and"));
  HUMAN_PORT.println(F("check the reading before you trust this setup."));
  enterState(ST_IDLE);
}

static void stagePassed(Stage s) {
  markStage(s);
  advance();
}

static void startRunAll() {
  runAll = true;
  enterStage(STG_CHECK);
}

// Single-letter commands that work in any state.
static bool handleGlobal(char c) {
  switch (c) {
    case 'H': printHelp(); return true;
    case 'S': enterState(ST_MENU); return true;
    case 'K': enterState(ST_HITS); return true;
    case 'M': enterState(ST_MEASURE); return true;
    case 'V':
      uiStyle = (uiStyle == UI_PLAIN) ? UI_FANCY : UI_PLAIN;
      cfg.uiStyle = uiStyle;
      cfgSave();
      HUMAN_PORT.print(F("Display: "));
      HUMAN_PORT.println(uiStyle == UI_PLAIN ? F("plain ASCII") : F("unicode"));
      hr();
      return true;
    case 'X':
      // From a stage, X drops back to the menu; from a mode or the menu,
      // straight to the live readout.
      if (state != ST_IDLE && state != ST_MENU && state != ST_HITS &&
          state != ST_MEASURE) {
        runAll = false;
        enterState(ST_MENU);
      } else {
        enterState(ST_IDLE);
      }
      return true;
    case '?':
      // Re-show the current stage with its full explanation.
      if (state != ST_IDLE && state != ST_MENU) { detail = true; enterState(state); }
      else printHelp();
      return true;
    case 'D': runDiagnostics(); return true;
    case 'T': runAll = false; enterStage(STG_ZERO); return true;
    case 'C': runAll = false; enterStage(STG_CAL); return true;
    case 'R':
      showRaw = !showRaw;
      HUMAN_PORT.print(F("Raw readout ")); HUMAN_PORT.println(showRaw ? F("ON") : F("OFF"));
      return true;
    case 'I':
      cfg.invert = !cfg.invert;
      cfgSave();
      HUMAN_PORT.print(F("Sign inversion ")); HUMAN_PORT.println(cfg.invert ? F("ON") : F("OFF"));
      emitCfg();
      return true;
    case 'Q':
      quiet = !quiet;
      HUMAN_PORT.print(F("Live readout ")); HUMAN_PORT.println(quiet ? F("paused") : F("resumed"));
      return true;
    case 'E': cfgEraseCalibration(); return true;
    default: return false;
  }
}

static void handleMachineLine(char* s) {
  // s starts with '$'
  char* cmd = s + 1;
  char* arg = strpbrk(cmd, " ,");
  if (arg) { *arg = 0; arg++; }
  for (char* p = cmd; *p; ++p) *p = (char)toupper((unsigned char)*p);

  if (!strcmp(cmd, "ID"))        { emitId(); }
  else if (!strcmp(cmd, "W"))    { emitWeight(); }
  else if (!strcmp(cmd, "RAW"))  { emitRaw(); }
  else if (!strcmp(cmd, "STAT")) { emitStat(); }
  else if (!strcmp(cmd, "CFG"))  { emitCfg(); }
  else if (!strcmp(cmd, "TARE")) {
    // Non-interactive tare: the host is responsible for an empty platform.
    // uiAdvance=false so a host command never drags the human UI to another stage.
    tareCapture(false);
  }
  else if (!strcmp(cmd, "CAL")) {
    float g[1];
    if (!arg || parseFloats(arg, g, 1) != 1 || g[0] <= 0) { emitErr("CAL", "BAD_ARG"); return; }
    calKnownGrams = g[0];
    calCapture(false);
  }
  else if (!strcmp(cmd, "INVERT")) {
    float v[1];
    if (!arg || parseFloats(arg, v, 1) != 1) { emitErr("INVERT", "BAD_ARG"); return; }
    cfg.invert = (v[0] != 0) ? 1 : 0;
    cfgSave();
    emitOk("INVERT");
  }
  else if (!strcmp(cmd, "HITLOG")) {
    if (arg && !strcmp(arg, "clear")) { hitLogClear(); emitOk("HITLOG"); return; }
    MACHINE_PORT.print(F("$HITLOG,")); MACHINE_PORT.println(hitLogN);
    for (uint8_t i = 0; i < hitLogN; i++) {
      const HitRec* r = hitAt(i);
      if (!r) break;
      MACHINE_PORT.print(F("$HIT#,"));  MACHINE_PORT.print(i);
      MACHINE_PORT.print(','); MACHINE_PORT.print(r->peak, 1);
      MACHINE_PORT.print(','); MACHINE_PORT.print(r->impulse, 2);
      MACHINE_PORT.print(','); MACHINE_PORT.print(r->rise);
      MACHINE_PORT.print(','); MACHINE_PORT.print(r->width);
      MACHINE_PORT.print(','); MACHINE_PORT.println(r->samples);
    }
    emitOk("HITLOG");
  }
  else if (!strcmp(cmd, "CALPTS")) {
    // No arg lists the points; "clear" empties the table.
    if (arg && !strcmp(arg, "clear")) {
      tune.calN = 0; tune.calLinG = 0; tune.calLinPct = 0;
      tuneSave();
      emitOk("CALPTS");
      return;
    }
    MACHINE_PORT.print(F("$CALPTS,"));
    MACHINE_PORT.print(tune.calN);
    for (uint8_t i = 0; i < tune.calN; i++) {
      MACHINE_PORT.print(','); MACHINE_PORT.print(tune.calG[i], 0);
      MACHINE_PORT.print(':'); MACHINE_PORT.print(tune.calRaw[i], 0);
    }
    MACHINE_PORT.print(F(",lin=")); MACHINE_PORT.print(tune.calLinG, 1);
    MACHINE_PORT.print(F(",linpct=")); MACHINE_PORT.println(tune.calLinPct, 3);
  }
  else if (!strcmp(cmd, "TUNE")) {
    // No argument: report. With one, set by name - $TUNE,flen,16
    if (!arg || !*arg) { emitTune(); return; }
    char name[12]; float v[1];
    char* comma = strchr(arg, ',');   // parseFloats rewrites separators in place
    if (!comma) { emitErr("TUNE", "BAD_ARG"); return; }
    size_t nlen = (size_t)(comma - arg);
    if (nlen == 0 || nlen >= sizeof(name)) { emitErr("TUNE", "BAD_ARG"); return; }
    memcpy(name, arg, nlen); name[nlen] = 0;
    if (parseFloats(comma + 1, v, 1) != 1) { emitErr("TUNE", "BAD_ARG"); return; }
    if (!tuneSet(name, v[0])) { emitErr("TUNE", "BAD_ARG"); return; }
    tuneSave();
    emitTune();
    emitOk("TUNE");
  }
  else if (!strcmp(cmd, "AZT")) {
    float v[1];
    if (!arg || parseFloats(arg, v, 1) != 1) { emitErr("AZT", "BAD_ARG"); return; }
    cfg.azt = (v[0] != 0) ? 1 : 0;
    cfgSave();
    emitOk("AZT");
  }
  else if (!strcmp(cmd, "STREAM")) {
    float hz[1];
    if (!arg || parseFloats(arg, hz, 1) != 1 || hz[0] < 0) { emitErr("STREAM", "BAD_ARG"); return; }
    streamPeriod = (hz[0] <= 0) ? 0 : (uint32_t)(1000.0f / hz[0]);
    emitOk("STREAM");
  }
  else if (!strcmp(cmd, "ERASE")) {
    // There used to be a second mode that preserved "wiring knowledge" from
    // the four-sensor wizard. There is no wiring to know about now, so any
    // argument is ignored and there is one behaviour.
    cfgEraseCalibration();
    emitOk("ERASE");
  }
  else if (!strcmp(cmd, "RESETPROG")) {
    // Clears which stages have passed, so setup starts again. Deliberately
    // keeps the calibration - use E to wipe that.
    cfg.stagesDone = 0;
    cfgSave();
    HUMAN_PORT.println(F("[OK] Setup progress cleared. Calibration kept."));
    emitOk("RESETPROG");
  }
  else if (!strcmp(cmd, "PING")) {
    // The host uses this to estimate the ESP32/host clock offset, so press
    // timestamps mean something. Answer immediately, before anything else.
    MACHINE_PORT.print(F("$PONG,"));
    MACHINE_PORT.println(millis());
  }
  else if (!strcmp(cmd, "HITS")) {
    float v[1];
    bool on = (arg && parseFloats(arg, v, 1) == 1) ? (v[0] != 0) : true;
    enterState(on ? ST_HITS : ST_IDLE);
    emitOk("HITS");
  }
  else if (!strcmp(cmd, "HITTHRESH")) {
    float v[1];
    if (!arg || parseFloats(arg, v, 1) != 1 || v[0] <= 0) { emitErr("HITTHRESH", "BAD_ARG"); return; }
    tune.hitThresh = v[0];
    tuneSave();
    emitOk("HITTHRESH");
  }
  else if (!strcmp(cmd, "QUIET")) {
    float v[1];
    quiet = (arg && parseFloats(arg, v, 1) == 1) ? (v[0] != 0) : true;
    emitOk("QUIET");
  }
  else if (!strcmp(cmd, "AXIS")) {
    // $AXIS,1 mirrors the gamepad axes onto this port as "$A,<x>,<y>" each
    // time they change - what pi/pad.py turns into a joystick. No $OK: the
    // answer is the $A line itself, sent with the next sample, so a bridge
    // can ask again as often as it likes without filling the console.
    float v[1];
    axisStream = (arg && parseFloats(arg, v, 1) == 1) ? (v[0] != 0) : true;
    axisDirty = true;
  }
  else if (!strcmp(cmd, "J")) {
    if (!arg) arg = (char*)"";
    if (!strcmp(arg, "data"))      serialReply("data", jsonData());
    else if (!strcmp(arg, "info")) serialReply("info", jsonInfo());
    else if (!strcmp(arg, "hits")) serialReply("hits", jsonHits());
    else if (!strcmp(arg, "doc"))  serialReply("doc",  textDoc());
    else if (!strncmp(arg, "log", 3)) {
      uint32_t since = (arg[3] == ',') ? strtoul(arg + 4, NULL, 10) : 0;
      serialReply("log", Both.since(since));
    }
    else emitErr("J", "BAD_ARG");
  }
  else if (!strcmp(cmd, "SET")) {
    // The page's /set, over the cable: $SET,fs=50000&ax=0
    setQuery = arg ? arg : "";
    bool reboot = applySet();
    setQuery = nullptr;
    serialReply("set", setReply(reboot));
    if (reboot) { delay(400); ESP.restart(); }
  }
  else {
    emitErr(cmd, "UNKNOWN");
  }
}

static void handleLine(char* s) {
  trimLine(s);

  if (s[0] == '$') { handleMachineLine(s); return; }

  char c = upper1(s);

  // Some letters mean something stage-local and must not reach the global
  // handler: 'B' (back), 'N' (skip), 'P' (repeat), and 'R' where a stage offers
  // "retry" rather than the global raw-readout toggle.
  bool stageLocal = (state == ST_W_CHECK && c == 'R')
                 // In the capture modes T re-zeroes rather than running a tare.
                 || (state == ST_HITS       && (c == 'T' || c == 'Z'))
                 || (state == ST_MEASURE    && (c == 'T' || c == 'Z'));
  if (c && !stageLocal) {
    if (c != 'Y' && handleGlobal(c)) return;
  }

  switch (state) {
    case ST_IDLE:
      if (s[0]) HUMAN_PORT.println(F("Unknown input. 'H' for help."));
      break;

    case ST_MENU: {
      if (c == 'A') { startRunAll(); break; }
      float n[1];
      int pick = (parseFloats(s, n, 1) == 1) ? (int)n[0] : 0;
      if (pick >= (int)STG_CHECK && pick <= (int)STG_MAX) {
        enterStage((Stage)pick);
      } else {
        HUMAN_PORT.println(F("Pick a stage 1-3, [A] all, [X] readout, [H] help."));
        HUMAN_PORT.print(F("> "));
      }
      break;
    }

    case ST_W_CHECK:
      if (c == 'R') runSanity(true);
      else if (s[0] == 0) advance();   // runSanity() marks the stage itself
      break;

    case ST_TARE:
      if (s[0] == 0) tareCapture(true);
      break;

    case ST_CAL_KNOWN: {
      float v[1];
      if (parseFloats(s, v, 1) == 1 && v[0] > 0) {
        calKnownGrams = v[0];
        enterState(ST_CAL_PLACE);
      } else {
        HUMAN_PORT.println(F("Enter a positive mass in grams, e.g. 500"));
        HUMAN_PORT.print(F("> "));
      }
      break;
    }

    case ST_CAL_PLACE:
      if (s[0] == 0) calCapture(true);
      break;

    case ST_HITS: {
      if (c == 'T') {
        HUMAN_PORT.print(F("Re-baselining"));
        float mean, sd; long mn, mx;
        if (hxCapture(AVG_SAMPLES, &mean, &sd, &mn, &mx)) {
          hitBaseline = (long)llround(mean);
          hitActive = false;
          HUMAN_PORT.print(F("Baseline "));
          HUMAN_PORT.println(hitBaseline);
        } else {
          HUMAN_PORT.println(F("[ERROR] no samples"));
        }
        break;
      }
      if (c == 'Z') {
        hitCount = 0;
        hitMaxSoFar = 0.0f;
        hitLogClear();
        HUMAN_PORT.println(F("Stats and hit history reset."));
        break;
      }
      float v[1];
      if (parseFloats(s, v, 1) == 1 && v[0] > 0) {
        tune.hitThresh = v[0];
        tuneSave();
        HUMAN_PORT.print(F("Trigger threshold "));
        HUMAN_PORT.print(tune.hitThresh, 1);
        HUMAN_PORT.println(hitUnit());
      } else if (s[0] == 0) {
        hitsStats();
      }
      break;
    }

    case ST_MEASURE:
      if (c == 'Z')      { measureReset(); HUMAN_PORT.println(F("  reset")); }
      else if (c == 'T') { mZero = ringFiltered(); measureReset();
                           HUMAN_PORT.println(F("  zeroed here")); }
      else if (s[0] == 0) mPanel = true;
      break;

    default:
      break;
  }
}

/* ---------------------------------------------------------------------------
 * Serial line readers (non-blocking)
 * ------------------------------------------------------------------------ */

static void pollPort(Stream& port, char* buf, uint8_t* len, uint8_t cap) {
  while (port.available()) {
    char c = (char)port.read();
    if (c == '\n' || c == '\r') {
      if (c == '\r' && port.peek() == '\n') port.read();
      buf[*len] = 0;
      char tmp[96];
      strncpy(tmp, buf, sizeof(tmp) - 1);
      tmp[sizeof(tmp) - 1] = 0;
      *len = 0;
      handleLine(tmp);
    } else if (*len < cap - 1) {
      buf[(*len)++] = c;
    }
  }
}

/* ---------------------------------------------------------------------------
 * Gamepad output and the web interface
 * ------------------------------------------------------------------------ */

// Force -> axis. Rest sits at the axis minimum, full-scale force at maximum,
// so the Pi reads "how hard is it being pressed" with no scaling knowledge
// beyond the one full-scale number.
// Rest sits at the axis CENTRE, not the minimum, so a host reading the axis as
// -1..+1 sees 0.0 with nothing on the plate and 1.0 at full scale. The axis
// value is then simply the fraction of full scale, with no offset to undo.
//
// An untouched pad must read EXACTLY 0, for years, with nobody looking after
// it. So anything inside the sensor's own noise is sent as 0, and so is any
// negative reading: the game never sees jitter or a zero that has wandered.
// The band is a multiple of the noise step 1 measured, not a number of grams,
// so it stays right if the cell or the amplifier is ever changed.
static const float AXIS_DEADBAND_SIGMA = 6.0f;

static float axisDeadbandG() {
  if (!haveCal()) return 0.0f;
  return AXIS_DEADBAND_SIGMA * noiseFloorCounts(0.0f) / fabsf(cfg.countsPerGram);
}

static int16_t axisFromGrams(float g) {
  float fs = cfg.axisFullScaleG > 0 ? cfg.axisFullScaleG : AXIS_FS_DEFAULT_G;
  if (!(g > axisDeadbandG())) g = 0.0f;       // also catches NaN
  float f = g / fs;
  if (f > 1) f = 1;
  return (int16_t)(f * (float)BLE_AXIS_MAX);
}

static int16_t lastAxis = 0;

// The axis is worked out whether or not anything is listening, so the page
// shows what a host WOULD read. It used to stop here with no Bluetooth host,
// which left the page's gauge frozen at zero on a cabinet wired by USB.
static void gamepadPush(long raw) {
  int16_t a = axisFromGrams(haveCal() ? rawToGrams(raw) : 0.0f);
  if (a == lastAxis && !axisDirty) return;   // nothing changed, do not spam
  lastAxis = a;
  axisDirty = false;
  int16_t x = (cfg.axisMap != AXIS_Y) ? a : 0;
  int16_t y = (cfg.axisMap != AXIS_X) ? a : 0;
  if (gamepadUp && gamepad.isConnected()) {
    gamepad.setX(x);
    gamepad.setY(y);
    gamepad.sendReport();
  }
  // Straight to Serial, not through Both: at ten lines a second this would
  // push everything else out of the page's console.
  if (axisStream) {
    Serial.print(F("$A,")); Serial.print(x);
    Serial.print(',');      Serial.println(y);
  }
}

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name=viewport content="width=device-width,initial-scale=1">
<title>esp32borne#1</title><style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:#0b0d10;color:#eceef1;
 font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:540px;margin:0 auto;padding:6px 16px 48px}

.hero{padding:14px 0 6px}
.val{font-size:72px;font-weight:600;letter-spacing:-3px;line-height:1;
 font-variant-numeric:tabular-nums}
.val small{font-size:22px;color:#707885;margin-left:8px;letter-spacing:0;font-weight:400}
.sub{display:flex;gap:20px;margin-top:10px;color:#707885;font-size:13px;
 font-variant-numeric:tabular-nums}
.sub b{color:#c3c9d2;font-weight:600}
canvas{width:100%;height:132px;display:block;margin:14px 0 6px}

.chips{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:18px}
.chip{font-size:12px;padding:5px 11px;border-radius:99px;background:#181c22;color:#707885}
.chip.on{background:#13301d;color:#74dd9b}.chip.off{background:#331a1a;color:#ff9c9c}

.steps{display:flex;gap:5px;margin:2px 0 8px}
.step{flex:1;height:34px;border-radius:8px;background:#181c22;color:#5c6470;
 border:0;padding:0;cursor:pointer;font:600 13px inherit}
.step.done{background:#16301f;color:#74dd9b}
.step.now{background:#4c8dff;color:#fff}
.prog{color:#5c6470;font-size:12px;margin:0 0 20px;text-align:center}

.h{font-size:22px;font-weight:600;margin:0 0 4px;letter-spacing:-.3px}
.sd{color:#707885;font-size:14px;margin:0 0 18px}

.act{width:100%;font:600 17px inherit;color:#fff;background:#4c8dff;border:0;
 border-radius:12px;padding:17px;cursor:pointer;margin-bottom:10px}
.act:active{background:#3f79dd}
.act.ghost{background:#181c22;color:#eceef1}
.more{display:flex;gap:18px;justify-content:center;flex-wrap:wrap;margin-top:14px}
.more button{background:none;border:0;color:#707885;font:500 14px inherit;
 padding:6px 0;cursor:pointer}
.more button:active{color:#eceef1}
body.busy .act,body.busy .more button{opacity:.4;pointer-events:none}

dl{margin:0 0 18px}
dl div{display:flex;justify-content:space-between;align-items:baseline;
 padding:11px 0;border-bottom:1px solid #191d23}
dl dt{color:#707885;font-size:14px;margin:0}
dl dd{margin:0;font-size:17px;font-weight:600;font-variant-numeric:tabular-nums}
dl dd.dim{color:#707885;font-weight:400;font-size:15px}

table{width:100%;border-collapse:collapse;font-size:14px;margin:0 0 18px}
th{text-align:left;color:#5c6470;font-size:11px;text-transform:uppercase;
 letter-spacing:.07em;padding:0 8px 8px 0;font-weight:600}
td{padding:9px 8px 9px 0;border-top:1px solid #191d23;font-variant-numeric:tabular-nums}
td.k{font-weight:600}
.RED{color:#ff8080}.WHITE{color:#eceef1}.BLACK{color:#8d95a3}

.f{display:flex;align-items:center;gap:10px;margin-bottom:10px}
.f label{flex:0 0 130px;color:#707885;font-size:14px;font-weight:500}
input{flex:1;font:16px inherit;color:#eceef1;background:#14171c;
 border:1px solid #232830;border-radius:11px;padding:14px 13px;min-width:0}
input:focus{outline:0;border-color:#4c8dff}
/* One page: h2 and <summary> head its sections, h3 heads a group inside one. */
h2,summary{font-size:13px;text-transform:uppercase;letter-spacing:.1em;color:#8d95a3;
 font-weight:600}
h2{margin:26px 0 14px;padding-top:18px;border-top:1px solid #191d23}
h3{font-size:11px;text-transform:uppercase;letter-spacing:.1em;color:#5c6470;
 margin:24px 0 10px;font-weight:600}
details{border-top:1px solid #191d23;margin-top:26px;padding-bottom:2px}
details.in{border-top:0;margin-top:6px}
summary{list-style:none;cursor:pointer;display:flex;justify-content:space-between;
 align-items:center;padding:17px 0}
details.in summary{font-size:11px;color:#707885;padding:12px 0}
summary::-webkit-details-marker{display:none}
summary::after{content:'+';font-size:20px;line-height:1;color:#5c6470;font-weight:400;
 letter-spacing:0}
details[open]>summary::after{content:'\2212'}
.two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.three{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}
.three input{width:100%}
.lb{display:block;color:#707885;font-size:13px;font-weight:500;margin:0 0 6px}
.f .seg{flex:1;margin:0}
.sv{flex:0 0 auto;font:600 15px inherit;color:#eceef1;background:#181c22;border:0;
 border-radius:11px;padding:14px 18px;cursor:pointer}
.sv.ok{background:#16301f;color:#74dd9b}.sv.bad{background:#331a1a;color:#ff9c9c}
.h:empty,.sd:empty,.n:empty,.prog:empty{display:none}
textarea{width:100%;background:#0b0d10;color:#c3c9d2;border:1px solid #232830;
 border-radius:11px;padding:12px;font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;
 resize:vertical}
pre{background:#14171c;border-radius:12px;padding:13px;font-size:12px;
 line-height:1.55;height:44vh;overflow:auto;white-space:pre-wrap;margin:0}
.seg{display:flex;background:#14171c;border-radius:11px;padding:3px;margin:0 0 14px}
.seg button{flex:1;border:0;background:none;color:#707885;font:600 13px inherit;
 padding:10px 2px;border-radius:9px;cursor:pointer}
.seg button.on{background:#272d36;color:#eceef1}
.mode{background:#14171c;border-radius:12px;padding:14px;margin-bottom:16px}
.mode h4{margin:0 0 10px;font-size:12px;text-transform:uppercase;letter-spacing:.08em;
 color:#5c6470;font-weight:600}
.mode dl{margin:0}
.mode dl div:last-child{border-bottom:0;padding-bottom:0}
/* instrument components */
.inst{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:16px}
.inst.one{grid-template-columns:1fr}
.rd{background:#14171c;border-radius:11px;padding:12px 13px}
.rd k{display:block;color:#5c6470;font-size:10px;text-transform:uppercase;
 letter-spacing:.09em;font-weight:700}
.rd v{display:block;font:600 26px/1.1 ui-monospace,SFMono-Regular,Menlo,monospace;
 font-variant-numeric:tabular-nums;margin-top:5px;letter-spacing:-.5px}
.rd v n{font-size:13px;color:#707885;margin-left:4px;font-weight:400}
.rd d{display:block;color:#5c6470;font-size:11px;margin-top:3px;line-height:1.35}
.rd.bad v{color:#ff8080}.rd.warn v{color:#f0c674}.rd.ok v{color:#74dd9b}
.gt{height:6px;background:#0b0d10;border-radius:99px;margin-top:9px;position:relative;
 overflow:hidden}
.gf{height:100%;background:#4c8dff;border-radius:99px}
.gf.warn{background:#f0c674}.gf.bad{background:#ff8080}.gf.ok{background:#74dd9b}
.gm{position:absolute;top:-2px;width:2px;height:10px;background:#5c6470;
 display:block;text-decoration:none}
.led{display:flex;align-items:center;gap:9px;padding:11px 13px;background:#14171c;
 border-radius:11px}
.led u{width:9px;height:9px;border-radius:99px;background:#39404a;flex:0 0 9px;
 display:block;text-decoration:none}
.led.ok u{background:#74dd9b;box-shadow:0 0 7px #74dd9b90}
.led.bad u{background:#ff8080;box-shadow:0 0 7px #ff808090}
.led.warn u{background:#f0c674;box-shadow:0 0 7px #f0c67490}
.led span{font-size:13px;color:#c3c9d2}
.dv{margin-bottom:9px}
.dv .l{display:flex;justify-content:space-between;font-size:12px;color:#707885;
 margin-bottom:4px;font-variant-numeric:tabular-nums}
.dv .t{height:22px;background:#14171c;border-radius:6px;position:relative}
.dv .c{position:absolute;left:50%;top:0;bottom:0;width:1px;background:#39404a}
.dv .b{position:absolute;top:4px;bottom:4px;border-radius:3px}
.cb{display:flex;gap:10px;align-items:flex-start;margin-top:12px;cursor:pointer}
.cb input{flex:0 0 20px;width:20px;height:20px;margin:1px 0 0;accent-color:#4c8dff}
.cb span{font-size:13px;color:#707885;line-height:1.4}
.pick{position:fixed;inset:0;background:#0b0d10;z-index:99;display:flex;
 flex-direction:column;justify-content:center;align-items:center;gap:14px;padding:24px}
.pick h1{font-size:20px;font-weight:600;margin:0 0 6px;text-align:center}
.pick button{width:100%;max-width:300px;font:600 18px inherit;color:#fff;
 background:#4c8dff;border:0;border-radius:12px;padding:20px;cursor:pointer}
.pick button.b{background:#181c22;color:#eceef1}
.n{font-size:13px;color:#707885;margin:14px 0 0}
.n.bad{color:#ff9c9c;background:#2a1414;border-radius:9px;padding:11px 13px;
 margin-top:16px}
</style></head><body><div class=pick id=langpick>
  <h1>Language / Langue</h1>
  <button onclick="setLang('en')">English</button>
  <button class=b onclick="setLang('fr')">Français</button>
</div>

<div class=wrap>

<!-- ONE page, read top to bottom: the reading, what the game is sent, the hit
     test, then setup. Settings and the console fold away underneath. -->
<div class=hero>
  <div class=val><span id=g>--</span><small id=u onclick=flipUnit()
 style="cursor:pointer;text-decoration:underline dotted">g</small></div>
  <div class=sub>
    <span onclick=resetPeak() style=cursor:pointer><d data-t=peak></d> <b id=pk>--</b> &#8634;</span>
    <span><d data-t=noise></d> <b id=sd>--</b></span>
    <span><d data-t=pp></d> <b id=pp>--</b></span>
  </div>
</div>
<canvas id=c></canvas>
<div class=chips>
  <span class=chip id=pcal></span>
  <span class=chip id=pst></span>
  <span class=chip id=ppad></span>
</div>

<h2 data-t=outH></h2>
<div id=axg></div>
<div class=seg>
  <button id=ax0 onclick="setAx(0)" data-t=axBoth></button>
  <button id=ax1 onclick="setAx(1)" data-t=axX></button>
  <button id=ax2 onclick="setAx(2)" data-t=axY></button>
</div>
<label class=lb id=lfs for=fs></label>
<div class=f><input id=fs inputmode=decimal autocomplete=off>
  <button class=sv id=fsb onclick="saveFs()" data-t=save></button></div>
<p class=n id=fsn style=margin-top:0></p>

<h2 data-t=setupH></h2>
<div class=steps id=steps></div>
<p class=prog id=prog></p>
<p class=h id=ct></p>
<p class=sd id=cd></p>
<div id=tbl></div>
<div id=cc></div>
<p class=n id=note></p>
<div class=two style=margin-top:14px>
  <button class="act ghost" onclick="tareNow()" data-t=zero></button>
  <button class="act ghost" onclick="cmd('C')" data-t=calib></button>
</div>
<details class=in>
  <summary data-t=calH></summary>
  <div id=calpts></div>
  <div id=invrow></div>
  <dl id=det></dl>
</details>

<details id=dset>
  <summary data-t=tSet></summary>
  <div class=f><label data-t=langH></label><div class=seg>
    <button id=lg0 onclick="setLang('en')">English</button>
    <button id=lg1 onclick="setLang('fr')">Français</button></div></div>
  <div class=f><label data-t=units></label><div class=seg>
    <button id=u0 onclick="setUnit(0)" data-t=grams></button>
    <button id=u1 onclick="setUnit(1)" data-t=kilos></button></div></div>
  <h3 data-t=gScaleH></h3>
  <div class=seg id=gseg></div>

  <h3 data-t=wifi></h3>
  <div class=f><input id=ap autocomplete=off autocapitalize=off data-p=passPh>
    <button class=sv id=apb onclick="saveWifi()" data-t=save></button></div>

  <h3 data-t=rowsH></h3>
  <p class=n data-t=rowsHint style="margin:0 0 10px"></p>
  <div class=three>
    <div><label class=lb data-t=screenH></label><input id=sh inputmode=decimal></div>
    <div><label class=lb data-t=topRow></label><input id=rt inputmode=decimal></div>
    <div><label class=lb data-t=botRow></label><input id=rb inputmode=decimal></div>
  </div>
  <div class=f style=margin-top:10px>
    <span class=n style="flex:1;margin:0" id=rowsnow>--</span>
    <button class=sv id=rwb onclick="saveRows()" data-t=save></button></div>

  <h3 data-t=docH></h3>
  <textarea id=doc readonly rows=8></textarea>
  <div class=two style=margin-top:10px>
    <button class="act ghost" onclick="copyDoc()" data-t=copy></button>
    <a class="act ghost" style="display:block;text-align:center;text-decoration:none"
       href="/doc?dl=1" download="POUR_LE_DEV.md" data-t=dl></a>
  </div>

  <h3 data-t=fwH></h3>
  <form method=POST action="/update" enctype="multipart/form-data">
   <input type=file name=f accept=".bin" style="padding:11px;width:100%">
   <button class="act ghost" type=submit style=margin-top:10px data-t=fwBtn></button>
  </form>

  <h3 data-t=resetH></h3>
  <div class=two>
    <button class="act ghost" onclick="resetProg()" data-t=resetProg></button>
    <button class="act ghost" onclick="wipe()" data-t=erase></button>
  </div>
  <p class=n data-t=resetHint style=margin-top:0></p>
</details>

<details id=dcon>
  <summary data-t=tCon></summary>
  <pre id=log></pre>
  <div class=f style=margin-top:10px>
    <input id=t data-p=typeCmd autocomplete=off autocapitalize=off>
    <button class=sv onclick="send()" data-t=send></button>
  </div>
  <div class=more><button onclick="cmd('H')" data-t=helpBtn></button></div>
</details>

</div><script>
// Two languages, chosen on first connection and remembered per phone.
// Keys are terse; the English text is the reference wording.
var LG={
en:{tCon:'Console',tSet:'Settings',peak:'peak',noise:'noise',
 pp:'p-p',cCal:'calibrated',cNot:'not calibrated',cPad:'Bluetooth linked',
 cWait:'Bluetooth idle',cSteady:'steady',cSettle:'settling',
 zero:'Zero',calib:'Calibrate',
 outH:'Output to the game',axBoth:'X + Y',axX:'X only',axY:'Y only',
 axOn:'live',axOff:'off',axis:'axis',fsNote:'Axis 1.0 =',dbNote:'reads 0 below',
 setupH:'Setup',passPh:'new password, 8+ characters, reboots',
 stepsDone:'of 3 steps done',allDone:'All three steps done',send:'Send',
 typeCmd:'type a command',units:'Units',grams:'grams',kilos:'kilograms',
 fullScale:'Full scale',
 wifi:'WiFi password',save:'Save',rowsH:'Button rows',
 rowsHint:'Measure the cabinet with a tape. Any unit, as long as all three match. Top and bottom row centres, measured from the top of the screen.',
 screenH:'Screen',topRow:'Top row',botRow:'Bottom row',
 docH:'Handoff document',
 copy:'Copy',dl:'Download',resetH:'Reset',resetProg:'Reset progress',
 erase:'Erase all',
 resetHint:'Reset progress clears the three steps and keeps your calibration. Erase removes the calibration and the zero - your wifi password, row heights, unit and full scale are kept.',
 signH:'Weight sign',signNorm:'normal',signInv:'inverted',
 signHint:'Use this if pressing makes the reading go down',signDo:'Invert the sign',
 signUndo:'Set back to normal',
 helpBtn:'Print the command reference',fwH:'Firmware',
 fwBtn:'Upload app .bin',langH:'Language',
 confErase:'Erase the calibration and the zero? Your settings are kept.',
 confProg:'Clear setup progress? Calibration is kept.',
 // stage titles and one-liners, indexed by firmware state
 s_idle:['Ready','The gamepad axis is live. Setup is three steps.'],
 s_menu:['Choose a step','Tap a number above, or run all three.'],
 s_amp:['Amplifier and noise','Hands off while it samples.'],
 s_zero:['Zero','Nothing on the plate.'],s_calw:['Reference weight','How heavy is it?'],
 s_calp:['Place the weight','On the plate, then weigh it.'],
 s_hit:['Impact','Recording hits.'],
 s_mon:['Monitor','Live reading with peak hold.'],aCont:'Continue',aZero:'Take the zero',
 aWeigh:'Weigh it',aRunAll:'Run the whole sequence',aStart:'Start setup',
 aUseWeight:'Use this weight',mMenu:'Menu',
 mRunAgain:'Run again',mRezero:'Re-zero',mReset:'Reset',
 mZeroHere:'Zero here',hxData:'Data line',hxReady:'ready',hxBusy:'busy',hxAns:'Amplifier',
 hxAnsY:'answering',hxAnsN:'silent',hxSample:'Raw sample',
 hxRail:'At the rail. Correct with A+/A- unconnected.',hxOk:'Within range',
 rest:'Resting value',restSub:'where the bridge sits',ptp:'Peak to peak',
 ptpSub:'how much it wanders',noisy:'very noisy',workable:'workable',good:'good',
 fsUsed:'Full scale used',fsLow:'little headroom left',fsOk:'plenty of headroom',
 curZero:'Current zero',curZeroSub:'what counts as nothing',noiseSub:'smaller is better',
 cpg:'Counts per gram',sens:'sensitivity',usable:'Usable resolution',
 usableSub:'from the measured noise, not the theoretical figure',lastPeak:'Last peak',
 impulse:'Impulse',
 impSub:'area, trust this one',pkSub:'height, unreliable at a low sample rate',
 peakHeld:'Peak held',
 sinceReset:'since reset',spread:'Spread',
 dCal:'Calibrated',dYes:'yes',dNo:'no',dZeroOff:'Zero offset',
 padH:'Gamepad',dLinked:'linked',dWaiting:'waiting',
 aztOff:'off',
 
 driftH:'Drift cancelled',
 calH:'Calibration details',
 calNone:'No points yet. Run step 3 with a known weight.',
 calOne:'One point. Calibrate again with a different weight to measure linearity.',
 cWeight:'weight',
 cRaw:'raw counts',
 linH:'Linearity',
 linSub:'worst point off the fitted line',
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 gScaleH:'Graph full scale',
 gsPeak:'fit',
 aStop:'Stop',
 
 setupOk:'Setup complete',
 runAgain:'Run setup again'
},
fr:{tCon:'Console',tSet:'Paramètres',peak:'crête',
 noise:'bruit',pp:'c-à-c',cCal:'étalonné',cNot:'non étalonné',
 cPad:'Bluetooth lié',cWait:'Bluetooth inactif',cSteady:'stable',cSettle:'en cours',
 zero:'Zéro',calib:'Étalonner',
 outH:'Sortie vers le jeu',axBoth:'X + Y',axX:'X seul',axY:'Y seul',
 axOn:'actif',axOff:'inactif',axis:'axe',fsNote:'Axe 1.0 =',dbNote:'0 en dessous de',
 setupH:'Réglage',
 passPh:'nouveau mot de passe, 8+ caractères, redémarre',
 stepsDone:'des 3 etapes faites',allDone:'Les trois etapes sont faites',send:'Envoyer',
 typeCmd:'tapez une commande',units:'Unités',grams:'grammes',kilos:'kilogrammes',
 fullScale:'Pleine échelle',
 wifi:'Mot de passe WiFi',save:'OK',rowsH:'Rangées de boutons',
 rowsHint:"Mesurez la borne au mètre. Peu importe l'unité, tant que les trois correspondent. Centres des rangées haute et basse, depuis le haut de l'écran.",
 screenH:'Écran',topRow:'Haute',botRow:'Basse',
 docH:'Document de transmission',
 copy:'Copier',dl:'Télécharger',resetH:'Réinitialisation',
 resetProg:'Progression',erase:'Tout effacer',
 resetHint:"Effacer la progression remet les trois etapes a zero et garde l\'etalonnage. Tout effacer supprime l\'etalonnage et le zero - le mot de passe wifi, les hauteurs de rangees, l\'unite et la pleine echelle sont gardes.",
 signH:'Signe du poids',signNorm:'normal',signInv:'inversé',
 signHint:"À utiliser si appuyer fait descendre la valeur",signDo:'Inverser le signe',
 signUndo:'Revenir au normal',helpBtn:'Afficher la liste des commandes',
 fwH:'Micrologiciel',fwBtn:'Envoyer le .bin',langH:'Langue',
 confErase:"Effacer l\'etalonnage et le zero ? Vos reglages sont gardes.",
 confProg:"Effacer la progression ? L'étalonnage est gardé.",
 // stage titles and one-liners, indexed by firmware state
 s_idle:['Prêt',"L'axe manette est actif. Le réglage tient en trois étapes."],
 s_menu:['Choisissez une étape','Touchez un numéro, ou lancez les trois.'],
 s_amp:['Amplificateur et bruit',"Ne touchez à rien pendant l'échantillonnage."],
 s_zero:['Zéro','Rien sur le plateau.'],s_calw:['Poids de référence','Quel est son poids ?'],
 s_calp:['Posez le poids','Sur le plateau, puis pesez.'],
 s_hit:["Impact","Enregistre les coups."],
 s_mon:['Suivi','Lecture directe avec maintien de crete.'],aCont:'Continuer',aZero:'Prendre le zéro',
 aWeigh:'Peser',aRunAll:'Lancer toute la séquence',aStart:'Démarrer le réglage',
 aUseWeight:'Utiliser ce poids',
 mMenu:'Menu',mRunAgain:'Relancer',mRezero:'Re-zéro',mReset:'Remise à zéro',
 mZeroHere:'Zéro ici',hxData:'Ligne de données',hxReady:'prête',
 hxBusy:'occupée',hxAns:'Amplificateur',hxAnsY:'répond',hxAnsN:'muet',
 hxSample:'Mesure brute',hxRail:'En butée. Normal tant que A+/A- ne sont pas branchés.',
 hxOk:'Dans la plage',rest:'Valeur au repos',restSub:'où se place le pont',
 ptp:'Crête à crête',ptpSub:'amplitude des variations',noisy:'très bruité',
 workable:'exploitable',good:'bon',fsUsed:'Pleine échelle utilisée',
 fsLow:'peu de marge restante',fsOk:'marge confortable',curZero:'Zéro actuel',
 curZeroSub:'ce qui compte comme rien',noiseSub:'plus petit est mieux',
 cpg:'Points par gramme',sens:'sensibilité',usable:'Résolution utile',
 usableSub:'à partir du bruit mesuré, pas du chiffre théorique',lastPeak:'Dernière crête',
 
 impulse:'Impulsion',impSub:"l'aire, c'est celle-ci qui est fiable",
 pkSub:'la hauteur, peu fiable a faible cadence',
 peakHeld:'Crête retenue',
 sinceReset:'depuis la remise à zéro',spread:'Amplitude',
 dCal:'Etalonne',dYes:'oui',dNo:'non',dZeroOff:'Decalage du zero',
 padH:'Manette',dLinked:'liee',dWaiting:'en attente',
 aztOff:'desactive',
 
 driftH:'Derive compensee',
 calH:'Details de l\'etalonnage',
 calNone:'Aucun point. Lancez l\'etape 3 avec une masse connue.',
 calOne:'Un seul point. Etalonnez avec une autre masse pour mesurer la linearite.',
 cWeight:'masse',
 cRaw:'points bruts',
 linH:'Linearite',
 linSub:'point le plus eloigne de la droite',
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 gScaleH:'Echelle du graphique',
 gsPeak:'ajuste',
 aStop:'Arreter',
 
 setupOk:'Reglage termine',
 runAgain:'Relancer le reglage'
}};
var lang=null;
try{lang=localStorage.getItem('lang');}catch(e){}
function t(k){var d=LG[lang]||LG.en;return d[k]!==undefined?d[k]:(LG.en[k]||k);}
function setLang(l){
  lang=l;
  try{localStorage.setItem('lang',l);}catch(e){}
  var p=document.getElementById('langpick');
  if(p)p.style.display='none';
  paint();
}

var seq=0,hist=[],fs=1000000,peak=0,last='',lastDone=-1,dat={},detI=null;
var lastInfo=0,lastSteps='',lastNote='';
var c=document.getElementById('c'),x=c.getContext('2d');
function E(i){return document.getElementById(i);}
// Units in one place. Everything that shows a force goes through these.
function U(){return dat.kg?'kg':'g';}
// Big forces read in kg whatever the unit setting - '1000000.0 g' is not a
// number anyone can parse at a glance.
function Vbig(v){
  var n=parseFloat(v); if(isNaN(n))return '--';
  if(Math.abs(n)>=10000)return (n/1000).toFixed(0)+' kg';
  return V(n)+' '+U();
}
function V(g,dp){
  var n=parseFloat(g);
  if(isNaN(n))return g;
  return dat.kg?(n/1000).toFixed(dp==null?3:dp+2):n.toFixed(dp==null?1:dp);
}
// A French phone keyboard types 12,5 - parseFloat alone would read that as 12.
function num(v){return parseFloat(String(v).replace(',','.'));}
function toG(v){return dat.kg?num(v)*1000:num(v);}
function setUnit(k){
  document.body.className='busy';
  fetch('/set?kg='+(k?1:0)).then(function(){document.body.className='';})
    .catch(function(e){document.body.className='';});
}
function flipUnit(){setUnit(dat.kg?0:1);}
// Only touch the DOM when the value actually changed. Rewriting every 250ms is
// what made the screen flicker.
function T(id,v){var e=E(id);if(e&&e.textContent!=v)e.textContent=v;}
function H(id,v){var e=E(id);if(e&&e.dataset.h!=v){e.dataset.h=v;e.innerHTML=v;}}
function fit(){c.width=c.clientWidth*devicePixelRatio;c.height=c.clientHeight*devicePixelRatio;}
addEventListener('resize',function(){fit();draw();});

// Which language and which unit are active, plus the current values in the
// boxes - otherwise you cannot tell what is set without changing it.
// Graph full scale: four fixed quarters of the axis full scale, plus one
// option that follows the last peak. Labelled with the real force so it
// stays meaningful if the axis full scale is changed.
var gsShown='';
function gscaleUI(d){
  var gs=(d.tn&&d.tn.gs)|0;
  var sig=gs+'|'+d.fs+'|'+(d.kg?1:0)+'|'+lang;
  if(sig==gsShown)return;
  gsShown=sig;
  var h='';
  // Always kg here. These are quarters of a 1000 kg full scale, so in
  // grams they read 250000.0 and tell you nothing at a glance.
  for(var i=1;i<=4;i++)
    h+='<button class='+(gs==i?'on':'')+' onclick="setGs('+i+')">'+
       Math.round(d.fs*i/4000)+' kg</button>';
  h+='<button class='+(gs==0?'on':'')+' onclick="setGs(0)">'+t('gsPeak')+'</button>';
  H('gseg',h);
}
function setGs(v){gsShown='';fetch('/set?gs='+v).then(tick);}

function settingsUI(d){
  var uk=d.kg?1:0;
  E('u0').className=uk?'':'on';
  E('u1').className=uk?'on':'';
  gscaleUI(d);
  E('lg0').className=(lang=='fr')?'':'on';
  E('lg1').className=(lang=='fr')?'on':'';
  var r=E('rowsnow');
  if(r){
    if(d.rt>0&&d.rb>d.rt){
      var out=[];
      for(var i=0;i<5;i++)out.push((d.rt+(d.rb-d.rt)*(i/4)).toFixed(4));
      T('rowsnow',out.join('   '));
    } else T('rowsnow','--');
  }
}
// --- what the game is sent --------------------------------------------------
// The tick rewrites the full-scale box from the stored value. It must leave
// the box alone while it holds an unsaved edit - not just while it has focus.
// Tapping Save takes the focus away first, so the old focus-only test put the
// stored number back a moment before Save read the box, and nothing typed on
// a phone ever stuck.
function dirty(e){return e.dataset.dirty=='1';}
function fsUI(d){
  T('lfs',t('fullScale')+' ('+U()+')');
  var f=E('fs');
  if(!dirty(f)&&document.activeElement!==f){
    var want=String(d.kg?+(d.fs/1000).toFixed(3):+(+d.fs).toFixed(1));
    if(f.value!==want)f.value=want;
  }
  T('fsn',t('fsNote')+' '+(+(d.fs/1000).toFixed(3))+' kg'+
    (d.db>0?'  ·  '+t('dbNote')+' '+Math.round(d.db)+' g':''));
}
// Both gauges always show, so it is obvious which axis is carrying the force
// and which is parked. The number is the axis as a host reads it: 0 to 1.
function axisUI(d){
  var a=Math.max(0,d.axis/32767),m=d.ax|0;
  function one(name,idx,on){
    return GAUGE(name+' &middot; '+t('axis')+' '+idx,on?a.toFixed(3):'0.000','',0,1,
                 t(on?'axOn':'axOff'),on?'ok':'');
  }
  H('axg',I2(one('X',0,m!=2),one('Y',1,m!=1)));
  for(var i=0;i<3;i++){var e=E('ax'+i),cl=(i==m?'on':'');if(e.className!=cl)e.className=cl;}
}
function setAx(v){fetch('/set?ax='+v).then(tick);}
// A save button says what happened to it: green if the box took it, red if not.
function flash(id,ok){
  var b=E(id);if(!b)return;
  b.className='sv '+(ok?'ok':'bad');
  setTimeout(function(){b.className='sv';},1300);
}
function saveFs(){
  var f=E('fs'),g=toG(f.value);
  if(!(g>0)){flash('fsb',0);return;}
  return fetch('/set?fs='+encodeURIComponent(g)).then(function(r){
    if(r.ok)f.dataset.dirty='';
    flash('fsb',r.ok);
    return tick();
  }).catch(function(e){flash('fsb',0);});
}
function resetPeak(){peak=0;T('pk','--');}

function paint(){
  var n=document.querySelectorAll('[data-t]'),i;
  for(i=0;i<n.length;i++)n[i].textContent=t(n[i].getAttribute('data-t'));
  n=document.querySelectorAll('[data-p]');
  for(i=0;i<n.length;i++)n[i].placeholder=t(n[i].getAttribute('data-p'));
  last='';lastSteps='';                   // force the card and strip to redraw
  tick();
}

// EVERYTHING below keys off the state KIND string from /data, never off the
// numeric state. Renumbering the firmware enum used to silently blank whole
// panels; with names it cannot.
var KINDS=['idle','menu','amp','zero','calw','calp','hit','mon'];
// The steps of the setup sequence, in order, and the live instruments.
var SETUP=['amp','zero','calw','calp'];
var LIVEK=['hit','mon'];
function isSetup(k){return SETUP.indexOf(k)>=0;}
function isLive(k){return LIVEK.indexOf(k)>=0;}

function ST(k){return t('s_'+k)||['',''];}

// One primary action per state, everything else demoted to a text link.
// Impact ('hit') has none here: its controls are the Hit test section.
var A={menu:['aRunAll','A'],idle:['aStart','S'],
amp:['aCont',''],zero:['aZero',''],calw:['aUseWeight',''],calp:['aWeigh',''],
hit:['aStop','X'],mon:['aStop','X']};
var M={idle:[],menu:[],
amp:[['mRunAgain','R'],['mMenu','X']],
zero:[['mMenu','X']],calw:[['mMenu','X']],calp:[['mMenu','X']],
hit:[['mRezero','T'],['mReset','Z']],
mon:[['mReset','Z'],['mZeroHere','T']]};

function links(m){
  var h='<div class=more>';
  for(var i=0;i<m.length;i++)
    h+='<button onclick="cmd(\''+m[i][1]+'\')">'+t(m[i][0])+'</button>';
  return h+'</div>';
}
function controls(k){
  var h='';
  if(k=='calw')
    h='<div class=f><input id=kg inputmode=decimal placeholder="'+U()+'">'+
      '<button class=act style="flex:0 0 auto;width:auto;margin:0" onclick="sendK()">'+
      t('aUseWeight')+'</button></div>';
  else if(A[k])
    h='<button class=act onclick="cmd(\''+A[k][1].trim()+'\')">'+t(A[k][0])+'</button>';
  var m=M[k];
  if(m&&m.length)h+=links(m);
  return h;
}

function sendK(){var v=E('kg');if(v&&v.value)cmd(String(toG(v.value)));}
function goStage(n){cmd('S').then(function(){return cmd(''+n);});}
// --- instrument components ------------------------------------------------
function READ(k,v,unit,sub,cls){
  return '<div class="rd '+(cls||'')+'"><k>'+k+'</k><v>'+v+
         (unit?'<n>'+unit+'</n>':'')+'</v>'+(sub?'<d>'+sub+'</d>':'')+'</div>';
}
function GAUGE(k,v,unit,min,max,sub,cls,mark){
  var f=Math.max(0,Math.min(1,(v-min)/(max-min||1)));
  var m='';
  if(mark!=null){
    var mf=Math.max(0,Math.min(1,(mark-min)/(max-min||1)));
    m='<u class=gm style="left:'+(mf*100).toFixed(1)+'%"></u>';
  }
  return '<div class="rd '+(cls||'')+'"><k>'+k+'</k><v>'+v+
         (unit?'<n>'+unit+'</n>':'')+'</v>'+
         '<div class=gt><div class="gf '+(cls||'')+'" style="width:'+
         (f*100).toFixed(1)+'%"></div>'+m+'</div>'+
         (sub?'<d>'+sub+'</d>':'')+'</div>';
}
function LED(label,state){
  return '<div class="led '+(state||'')+'"><u></u><span>'+label+'</span></div>';
}
// Direction IS the reading here, so draw it either side of a centre line.
function DIVBAR(label,v,max){
  var f=Math.max(-1,Math.min(1,v/(max||1)));
  var w=Math.abs(f)*50, left=f<0?50-w:50;
  var col=v>0?'#74dd9b':v<0?'#ff8080':'#39404a';
  return '<div class=dv><div class=l><span>'+label+'</span><span>'+
    (v?(v>0?'+':'')+v:'waiting')+'</span></div>'+
    '<div class=t><div class=c></div>'+
    (v?'<div class=b style="left:'+left+'%;width:'+w+'%;background:'+col+'"></div>':'')+
    '</div></div>';
}
function I2(a,b){return '<div class=inst>'+a+b+'</div>';}
function I1(a){return '<div class="inst one">'+a+'</div>';}

function DL(list){
  var h='';
  for(var i=0;i<list.length;i++)
    h+='<div><dt>'+list[i][0]+'</dt><dd'+(list[i][2]?' class=dim':'')+'>'+list[i][1]+'</dd></div>';
  return h;
}

function tables(k,inf){
  if(!inf)return '';
  if(k=='amp'&&inf.hx&&inf.sn){
    var rail=Math.abs(inf.hx.s)>8000000, sd=parseFloat(inf.sn.sd), pct=parseFloat(inf.sn.fs);
    var h=I2(LED(t('hxData')+' '+t(inf.hx.dt?'hxReady':'hxBusy'),inf.hx.dt?'ok':'warn'),
             LED(t('hxAns')+' '+t(inf.hx.ok?'hxAnsY':'hxAnsN'),inf.hx.ok?'ok':'bad'));
    if(inf.sn.ok)
      h+=I2(READ(t('rest'),inf.sn.mean,'counts',t('restSub')),
            READ(t('ptp'),inf.sn.max-inf.sn.min,'counts',t('ptpSub')))+
         I2(GAUGE(t('noise'),inf.sn.sd,'sd',0,5000,
              t(sd>5000?'noisy':sd>500?'workable':'good'),
              sd>5000?'bad':sd>500?'warn':'ok',500),
            GAUGE(t('fsUsed'),inf.sn.fs,'%',0,100,
              t(pct>80?'fsLow':'fsOk'),pct>80?'warn':'ok',80));
    else h+=I1(READ(t('hxSample'),inf.hx.s,'counts',t(rail?'hxRail':'hxOk')));
    return h;
  }
  if(k=='zero') return I2(READ(t('curZero'),dat.tare,'counts',t('curZeroSub')),
                      READ(t('noise'),dat.sd,'sd',t('noiseSub')));
  if(k=='calw'||k=='calp'){
    var cpg=parseFloat(dat.cpg),res=cpg>0?(parseFloat(dat.sd)*2/cpg):0;
    return I2(READ(t('cpg'),dat.cpg,'',t('sens')),
              READ(t('curZero'),dat.tare,'counts',''))+
      (cpg>0?I1(READ(t('usable'),res.toFixed(2),'g',t('usableSub'))):'');
  }
  if(k=='hit'&&inf.hit) return I2(READ(t('lastPeak'),V(inf.hit[0]),U(),t('pkSub')),
                               READ(t('impulse'),V(inf.hit[4],3),U()+'.s',t('impSub')));
  if(k=='mon') return I2(READ(t('peakHeld'),dat.cal?V(peak):'--',U(),t('sinceReset')),
                         READ(t('spread'),dat.pp,'counts',t('ptp')));
  return '';
}


// Graph full scale. 0 follows the biggest reading so the trace fills the
// height; 1..4 are fixed quarters of the axis full scale, so two traces
// taken at the same setting are directly comparable.
// The visible window, [lo,hi] in display units.
// gs 1..4 are fixed quarters of full scale, anchored at zero, so two traces
// taken at the same setting are directly comparable.
// gs 0 FITS THE TRACE: both ends follow the data.
// The Y range, and nothing else. gs 1..4 are the fixed scales chosen in
// Settings; gs 0 follows the peak hold, which is what 'fit' means here.
function graphTop(){
  var gs=(dat.tn&&dat.tn.gs)|0;
  if(gs>0)return fs*gs/4;
  return Math.max(peak*1.25,parseFloat(dat.pp)*4||0,1);
}
function draw(){
  var w=c.width,h=c.height,p=6*devicePixelRatio;
  if(!w||!h)return;
  x.clearRect(0,0,w,h);
  x.strokeStyle='#191d23';x.lineWidth=devicePixelRatio;
  for(var i=1;i<4;i++){var y=h*i/4;x.beginPath();x.moveTo(0,y);x.lineTo(w,y);x.stroke();}
  if(hist.length<2)return;
  var top=graphTop();
  var X=function(i){return p+(w-2*p)*i/(hist.length-1);};
  var Y=function(v){return h-p-(h-2*p)*Math.min(v/top,1);};
  x.beginPath();x.moveTo(X(0),h);
  for(var i=0;i<hist.length;i++)x.lineTo(X(i),Y(hist[i]));
  x.lineTo(X(hist.length-1),h);x.closePath();
  var gr=x.createLinearGradient(0,0,0,h);
  gr.addColorStop(0,'rgba(76,141,255,.42)');gr.addColorStop(1,'rgba(76,141,255,0)');
  x.fillStyle=gr;x.fill();
  x.beginPath();
  for(var i=0;i<hist.length;i++){if(i)x.lineTo(X(i),Y(hist[i]));else x.moveTo(X(i),Y(hist[i]));}
  x.strokeStyle='#4c8dff';x.lineWidth=2*devicePixelRatio;x.stroke();
  if(peak>0){var py=Y(peak);x.beginPath();
    x.setLineDash([3*devicePixelRatio,4*devicePixelRatio]);
    x.moveTo(0,py);x.lineTo(w,py);x.strokeStyle='#f0c674';
    x.lineWidth=devicePixelRatio;x.stroke();x.setLineDash([]);}
}
function chip(id,on,txt){
  var e=E(id),cl='chip '+(on?'on':'off');
  if(e.className!=cl)e.className=cl;
  if(e.textContent!=txt)e.textContent=txt;
}
function steps(done,stage,k){
  var key=done+'|'+stage+'|'+isSetup(k);
  if(key==lastSteps)return;
  lastSteps=key;
  var h='';
  for(var i=1;i<=3;i++){
    var dn=(done>>(i-1))&1,now=(isSetup(k)&&stage==i);
    h+='<button class="step'+(dn?' done':'')+(now?' now':'')+
       '" onclick="goStage('+i+')">'+i+'</button>';
  }
  E('steps').innerHTML=h;
  var n=0;
  for(var j=1;j<=3;j++)if((done>>(j-1))&1)n++;
  // Finished and out of the flow: hide the strip and say it once, quietly.
  var out=!isSetup(k)&&k!='menu';
  E('steps').style.display=(n==3&&out)?'none':'flex';
  T('prog',n==3?(out?t('setupOk'):t('allDone')):n+' '+t('stepsDone'));
}
function details(d){
  var i=detI||{};
  H('det',DL([[t('dCal'),t(d.cal?'dYes':'dNo')],[t('cpg'),d.cpg],
    [t('dZeroOff'),d.tare],[t('noise'),d.sd,1],[t('ptp'),d.pp,1],
    [t('fullScale'),Vbig(d.fs),1],
    [t('padH'),t(d.pad?'dLinked':'dWaiting'),1],
    [t('driftH'),d.azt?(i.drift!=null?V(i.drift)+' '+U():'--'):t('aztOff'),1]]));
}
function invToggle(){cmd('I');}

// --- Sensor tab: tuning -----------------------------------------------------
// One row per value. Everything is clamped again in the firmware, so a silly
// entry here is rejected there rather than stored.
// [key, label, step, isUnit] - isUnit means it is a force and goes through the
// gram/kilo conversion on the way in and out.
// The Sensor Tuning form is deliberately NOT here.
//
// It carried "what counts as a hit" - trigger threshold, release fraction,
// release samples, max event - and that is a GAME decision. The Pi reads a
// joystick axis and decides what a hit is; the box reports facts and never
// scores. It also exposed filter length, trim, steadiness and drift
// internals on a page anyone in a bar can open, where a wrong value quietly
// wrecks the measurement.
//
// All of it is still reachable over serial for the bench: $TUNE lists it,
// $TUNE,<name>,<value> sets it. The trigger is derived from the measured
// noise floor and needs no setting at all.

// --- Sensor tab: calibration points ----------------------------------------
function calUI(d){
  var n=d.caln||0,h='';
  if(!n){H('calpts','<p class=n>'+t('calNone')+'</p>');return;}
  h='<table><tr><th>'+t('cWeight')+'</th><th>'+t('cRaw')+'</th></tr>';
  for(var i=0;i<(d.pts||[]).length;i++)
    h+='<tr><td>'+V(d.pts[i][0])+' '+U()+'</td><td>'+d.pts[i][1]+'</td></tr>';
  h+='</table>';
  if(n>=2)
    h+=I1(READ(t('linH'),V(d.ling),U(),t('linSub')+'  ('+d.linpct+' %FS)',
          parseFloat(d.linpct)>0.5?'warn':'ok'));
  else h+='<p class=n>'+t('calOne')+'</p>';
  H('calpts',h);
}

function info(k){
  return fetch('/info').then(function(r){return r.json();}).then(function(v){
    detI=v;
    if(k)H('tbl',tables(k,v));
  }).catch(function(e){});
}
function tick(){
  return fetch('/data').then(function(r){return r.json();}).then(function(d){
    fs=d.fs||1000000;dat=d;
    T('g',d.cal?V(d.g):d.raw);
    T('u',d.cal?U():'raw');
    T('sd',d.sd);T('pp',d.pp);
    if(d.cal&&d.g>peak)peak=d.g;
    T('pk',d.cal?V(peak):'--');
    chip('pcal',d.cal,t(d.cal?'cCal':'cNot'));
    chip('pst',d.stable,t(d.stable?'cSteady':'cSettle'));
    // Grey, not red, when no Bluetooth host is linked: on a cabinet wired by
    // USB that is the normal state, not a fault.
    var pe=E('ppad'),pc='chip'+(d.pad?' on':'');
    if(pe.className!=pc)pe.className=pc;
    T('ppad',t(d.pad?'cPad':'cWait'));
    hist.push(d.cal?d.g:0);if(hist.length>140)hist.shift();
    // One page, so every section paints on every tick. T() and H() only
    // touch the DOM when the text actually changed.
    draw();
    axisUI(d);fsUI(d);
    calUI(d.tn||{});details(d);   // the points travel inside the tuning block
    H('invrow',READ(t('signH'),t(d.inv?'signInv':'signNorm'),'',t('signHint'))+
      '<button class="act ghost" style=margin-top:10px onclick=invToggle()>'+
      t(d.inv?'signUndo':'signDo')+'</button>');
    settingsUI(d);
    steps(d.done,d.stage,d.k);

    var n=E('note'),m=d.msg||'';
    if(m!=lastNote){
      lastNote=m;
      var fail=/FAIL|ERROR|PROBLEM|REJECTED/.test(m);
      n.textContent=fail?m.replace(/^\[[A-Z]+\]\s*/,''):'';
      n.className=fail?'n bad':'n';
    }

    var now=Date.now();
    if(d.k!=last||(isLive(d.k)&&now-lastInfo>900)){
      lastInfo=now;info(d.k);
    }

    if(d.k!=last||lastDone!=d.done){
      last=d.k;lastDone=d.done;
      // Finished and idle, or recording hits: one quiet link. Not a heading
      // telling you to set up a box that is already set up, and not a second
      // blue button competing with the one in Hit test.
      var fin=((d.done|0)&7)==7;
      if(d.k=='idle'&&fin){
        T('ct','');
        T('cd','');
        H('cc',fin?'<div class=more style=margin-top:0><button onclick="cmd(\'S\')">'+
          t('runAgain')+'</button></div>':'');
      }else{
        var s=ST(d.k)||['',''];
        T('ct',s[0]);
        T('cd',s[1]);
        H('cc',controls(d.k));
      }
    }
  }).catch(function(e){});
}
function pullLog(){
  return fetch('/log?since='+seq).then(function(r){return r.text();}).then(function(t){
    if(t){var L=E('log');L.textContent+=t;seq+=t.length;
      if(L.textContent.length>12000)L.textContent=L.textContent.slice(-12000);
      if(E('dcon').open)L.scrollTop=L.scrollHeight;}
  }).catch(function(e){});
}
function cmd(s){
  document.body.className='busy';
  return fetch('/cmd?c='+encodeURIComponent(s)).then(pullLog).then(tick)
    .then(function(){document.body.className='';})
    .catch(function(e){document.body.className='';});
}
function tareNow(){
  document.body.className='busy';
  return fetch('/cmd?c=T').then(function(){return fetch('/cmd?c=');})
    .then(pullLog).then(tick)
    .then(function(){document.body.className='';})
    .catch(function(e){document.body.className='';});
}
function send(){var t=E('t');cmd(t.value);t.value='';}
E('t').addEventListener('keydown',function(e){if(e.key=='Enter')send();});
function resetProg(){
  if(confirm(t('confProg')))cmd('$RESETPROG');
}
function wipe(){
  if(confirm(t('confErase')))cmd('$ERASE');
}
function loadDoc(){
  fetch('/doc').then(function(r){return r.text();}).then(function(t){
    var e=E('doc'); if(e.value!=t)e.value=t;
  }).catch(function(e){});
}
function copyDoc(){
  var e=E('doc'); e.select(); e.setSelectionRange(0,999999);
  try{document.execCommand('copy');}catch(err){}
}
// The box only keeps rows that make sense (bottom below top, both on the
// screen), so check the same thing here and say so rather than appear to save.
function saveRows(){
  var h=num(E('sh').value),a=num(E('rt').value),b=num(E('rb').value);
  if(!(h>0&&a>=0&&b>a&&b<=h)){flash('rwb',0);return;}
  fetch('/set?sh='+h+'&rt='+a+'&rb='+b).then(function(r){
    flash('rwb',r.ok);loadDoc();return tick();
  }).catch(function(e){flash('rwb',0);});
}
function saveWifi(){
  var a=E('ap');
  if(a.value.length<8||a.value.length>15){flash('apb',0);return;}
  fetch('/set?wifi='+encodeURIComponent(a.value)).then(function(r){
    flash('apb',r.ok);a.value='';
  }).catch(function(e){flash('apb',0);});
}
E('fs').addEventListener('input',function(){this.dataset.dirty='1';});
E('fs').addEventListener('keydown',function(e){if(e.key=='Enter')saveFs();});
E('dset').addEventListener('toggle',function(){if(this.open)loadDoc();});
E('dcon').addEventListener('toggle',function(){
  var L=E('log');if(this.open)L.scrollTop=L.scrollHeight;});
if(lang){document.getElementById('langpick').style.display='none';}
fit();          // opening the Sensor tab used to do this; there are no tabs now
paint();
setInterval(tick,250);setInterval(pullLog,900);tick();
</script></body></html>)HTML";

static void handlePage() {
  server.send_P(200, "text/html", PAGE);
}


// No page password. Getting onto the wifi at all needs WPA2, and that is the
// gate - a second password on top only meant two things to lose.
static String jsonData() {
  long raw = ringFiltered();
  float sd, pp;
  bool stable = isStable(&sd, &pp);
  String j = "{";
  j.reserve(1100);
  j += "\"g\":";      j += String(haveCal() ? rawToGrams(raw) : 0.0f, 1);
  j += ",\"raw\":";   j += String(raw);
  j += ",\"axis\":";  j += String(lastAxis);
  j += ",\"ax\":";    j += String(cfg.axisMap);
  j += ",\"db\":";    j += String(axisDeadbandG(), 0);
  j += ",\"fs\":";    j += String(cfg.axisFullScaleG, 0);
  j += ",\"cal\":";   j += (haveCal() ? "1" : "0");
  j += ",\"stable\":";j += (stable ? "1" : "0");
  j += ",\"pad\":";   j += (gamepadUp && gamepad.isConnected() ? "1" : "0");
  j += ",\"seq\":";   j += String(Both.seq);
  j += ",\"defpw\":"; j += (cfg.passChanged ? "0" : "1");
  // Structured state, so the page can render the right controls rather than
  // making you read console text and guess what it wants.
  j += ",\"st\":";    j += String((int)state);
  j += ",\"k\":\"";   j += stateKind(state); j += "\"";
  j += ",\"stage\":"; j += String((int)curStage);
  j += ",\"done\":";  j += String(cfg.stagesDone);
  j += ",\"cpg\":";   j += String(cfg.countsPerGram, 3);
  j += ",\"tare\":";  j += String(zeroNow());
  j += ",\"sd\":";    j += String(sd, 1);
  j += ",\"pp\":";    j += String(pp, 1);
  j += ",\"inv\":";   j += (cfg.invert ? "1" : "0");
  j += ",\"kg\":";    j += (cfg.useKg ? "1" : "0");
  j += ",\"azt\":";   j += (cfg.azt ? "1" : "0");
  // The tuning block, so the Sensor tab can paint its form from the tick it
  // already runs rather than needing a second fetch.
  j += ",\"tn\":{\"ht\":";  j += String(tune.hitThresh, 1);
  j += ",\"hrf\":";  j += String(tune.hitReleaseFrac, 2);
  j += ",\"hrn\":";  j += String(tune.hitReleaseN);
  j += ",\"hmax\":"; j += String(tune.hitMaxMs);
  j += ",\"flen\":"; j += String(tune.filterLen);
  j += ",\"trim\":"; j += String(tune.trimCount);
  j += ",\"ssd\":";  j += String(tune.steadySd, 2);
  j += ",\"spp\":";  j += String(tune.steadyPp, 2);
  j += ",\"noise\":";j += String(tune.noiseFloor, 1);
  j += ",\"arate\":";j += String(tune.aztRateGps, 1);
  j += ",\"awin\":"; j += String(tune.aztWindowG, 0);
  j += ",\"asettle\":"; j += String(tune.aztSettleMs);
  j += ",\"gs\":";    j += String(tune.graphScale);
  j += ",\"caln\":";  j += String(tune.calN);
  j += ",\"linpct\":";j += String(tune.calLinPct, 3);
  j += ",\"ling\":";  j += String(tune.calLinG, 1);
  j += ",\"pts\":[";
  for (uint8_t i = 0; i < tune.calN; i++) {
    if (i) j += ",";
    j += "["; j += String(tune.calG[i], 0);
    j += ","; j += String(tune.calRaw[i], 0); j += "]";
  }
  j += "]";
  j += "}";
  j += ",\"rt\":";    j += String(cfg.rowTop, 4);
  j += ",\"rb\":";    j += String(cfg.rowBot, 4);
  j += ",\"msg\":\"";
  for (const char* m = Both.msg; *m; m++) {       // JSON-safe
    char ch = *m;
    if (ch == '"' || ch == '\\') ch = ' ';
    j += ch;
  }
  j += "\"";
  j += "}";
  return j;
}

static void handleData() { server.send(200, "application/json", jsonData()); }

// Everything the console shows as a table, as data instead: what each sensor
// was identified as, the exact bridge wiring for those wires, and how each
// sensor responded to the polarity press. This is what makes the page usable
// without ever reading the console.
static String jsonInfo() {
  String j = "{\"hx\":{\"ok\":";
  j += (hxOk ? "1" : "0");
  j += ",\"dt\":";  j += (hxDtLow ? "1" : "0");
  j += ",\"s\":";   j += String(hxSample);
  j += "},\"sn\":{\"ok\":"; j += (snDone ? "1" : "0");
  j += ",\"mean\":"; j += String(snMean, 0);
  j += ",\"sd\":";   j += String(snSd, 1);
  j += ",\"min\":";  j += String(snMin);
  j += ",\"max\":";  j += String(snMax);
  j += ",\"fs\":";   j += String(fabsf(snMean) / (float)HX_FULL_SCALE * 100.0f, 1);
  j += "},\"hit\":[";
  for (uint8_t i = 0; i < 5; i++) { if (i) j += ","; j += String(lastHit[i], 1); }
  j += "]";
  j += ",\"hits\":";     j += String(hitCount);
  j += ",\"drift\":";    j += String(aztTotal, 1);
  j += ",\"azt\":";      j += (cfg.azt ? "1" : "0");
  j += "}";
  return j;
}

static void handleInfo() { server.send(200, "application/json", jsonInfo()); }

// The hit history, newest first. The page turns this into a list and a
// spread; the firmware deliberately does not score any of it.
static String jsonHits() {
  String j = "{\"n\":";
  j += String(hitLogN);
  j += ",\"seq\":"; j += String(hitLogSeq);
  j += ",\"cal\":";  j += (haveCal() ? "1" : "0");
  j += ",\"h\":[";
  for (uint8_t i = 0; i < hitLogN; i++) {
    const HitRec* r = hitAt(i);
    if (!r) break;
    if (i) j += ",";
    j += "[";  j += String(r->peak, 1);
    j += ",";  j += String(r->impulse, 2);
    j += ",";  j += String(r->rise);
    j += ",";  j += String(r->width);
    j += ",";  j += String(r->samples);
    j += ",";  j += String((millis() - r->at) / 1000);   // seconds ago
    j += "]";
  }
  j += "]}";
  return j;
}

static void handleHits() {
  if (server.hasArg("clear")) hitLogClear();
  server.send(200, "application/json", jsonHits());
}

static void handleLog() {
  uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), NULL, 10) : 0;
  server.send(200, "text/plain", Both.since(since));
}

static void handleCmd() {
  String c = server.arg("c");
  char line[96];
  strncpy(line, c.c_str(), sizeof(line) - 1);
  line[sizeof(line) - 1] = 0;
  handleLine(line);                 // exactly what the serial port does
  server.send(200, "text/plain", "ok");
}

// Settings arrive two ways: as an HTTP query from the page over wifi, or as
// "$SET,fs=50000&kg=1" over USB serial. One body serves both; these read
// whichever is active (setQuery, declared with the other globals).
static const char* setFind(const char* name, size_t* len) {
  size_t n = strlen(name);
  for (const char* p = setQuery; p && *p; ) {
    const char* amp = strchr(p, '&');
    if (!strncmp(p, name, n) && p[n] == '=') {
      const char* v = p + n + 1;
      *len = amp ? (size_t)(amp - v) : strlen(v);
      return v;
    }
    p = amp ? amp + 1 : nullptr;
  }
  return nullptr;
}

static bool setHas(const char* name) {
  if (!setQuery) return server.hasArg(name);
  size_t len;
  return setFind(name, &len) != nullptr;
}

static String setArg(const char* name) {
  if (!setQuery) return server.arg(name);
  size_t len = 0;
  const char* v = setFind(name, &len);
  String out;
  for (size_t i = 0; v && i < len; i++) out += v[i];
  return out;
}

static bool applySet() {
  if (setHas("fs")) {
    float v = setArg("fs").toFloat();
    if (v > 0) cfg.axisFullScaleG = v;
  }
  if (setHas("ax")) {                 // which axis carries the force
    int v = setArg("ax").toInt();
    if (v >= AXIS_BOTH && v <= AXIS_Y) { cfg.axisMap = (uint8_t)v; axisDirty = true; }
  }
  bool reboot = false;
  // Rows are entered as three measurements in ANY consistent unit - mm off a
  // tape measure is easiest. We only keep the two fractions.
  if (setHas("kg")) {
    cfg.useKg = (setArg("kg").toInt() != 0) ? 1 : 0;
  }
  if (setHas("azt")) {
    cfg.azt = (setArg("azt").toInt() != 0) ? 1 : 0;
  }
  // Tuning. Each value is clamped where it arrives; nothing out of range
  // is ever written, so a typo cannot leave the box in a state where the
  // ring indexes off its end or the hit detector never fires.
  bool tuned = false;
  if (setHas("ht")) {          // hit trigger threshold, display unit
    float v = setArg("ht").toFloat();
    if (v > 0) { tune.hitThresh = v; tuned = true; }
  }
  if (setHas("hrf")) {         // release fraction
    float v = setArg("hrf").toFloat();
    if (v > 0.05f && v < 0.95f) { tune.hitReleaseFrac = v; tuned = true; }
  }
  if (setHas("hrn")) {         // release samples
    int v = setArg("hrn").toInt();
    if (v >= 1 && v <= 20) { tune.hitReleaseN = (uint8_t)v; tuned = true; }
  }
  if (setHas("hmax")) {        // max event length, ms
    long v = setArg("hmax").toInt();
    if (v >= 100 && v <= 60000) { tune.hitMaxMs = (uint32_t)v; tuned = true; }
  }
  if (setHas("flen")) {        // filter window
    int v = setArg("flen").toInt();
    if (v >= 3 && v <= RING_MAX) {
      tune.filterLen = (uint8_t)v;
      if (tune.trimCount * 2 + 2 > tune.filterLen) tune.trimCount = 0;
      ringClear();                    // stale samples are not this window
      tuned = true;
    }
  }
  if (setHas("trim")) {
    int v = setArg("trim").toInt();
    if (v >= 0 && v * 2 + 2 <= tune.filterLen) { tune.trimCount = (uint8_t)v; tuned = true; }
  }
  if (setHas("ssd")) {         // steady: sd < noise * this
    float v = setArg("ssd").toFloat();
    if (v > 0.5f && v <= 50.0f) { tune.steadySd = v; tuned = true; }
  }
  if (setHas("spp")) {
    float v = setArg("spp").toFloat();
    if (v > 0.5f && v <= 100.0f) { tune.steadyPp = v; tuned = true; }
  }
  if (setHas("arate")) {       // drift rate, g/s
    float v = setArg("arate").toFloat();
    if (v > 0 && v <= 1000.0f) { tune.aztRateGps = v; tuned = true; }
  }
  if (setHas("awin")) {        // empty-plate window, g
    float v = setArg("awin").toFloat();
    if (v > 0) { tune.aztWindowG = v; tuned = true; }
  }
  if (setHas("gs")) {          // graph full scale, 0..4
    int v = setArg("gs").toInt();
    if (v >= 0 && v <= 4) { tune.graphScale = (uint8_t)v; tuned = true; }
  }
  if (setHas("asettle")) {
    long v = setArg("asettle").toInt();
    if (v >= 0 && v <= 60000) { tune.aztSettleMs = (uint32_t)v; tuned = true; }
  }
  if (tuned) tuneSave();
  if (setHas("sh") && setHas("rt") && setHas("rb")) {
    float h = setArg("sh").toFloat();
    float t = setArg("rt").toFloat();
    float b = setArg("rb").toFloat();
    if (h > 0 && t >= 0 && b > t && b <= h) {
      cfg.rowTop = t / h;
      cfg.rowBot = b / h;
    }
  }
  if (setHas("wifi") && setArg("wifi").length() >= 8) {
    strncpy(cfg.apPass, setArg("wifi").c_str(), sizeof(cfg.apPass) - 1);
    cfg.apPass[sizeof(cfg.apPass) - 1] = 0;
    cfg.passChanged = 1;
    reboot = true;                    // the AP has to restart to apply it
  }
  cfgSave();
  return reboot;
}

static const char* setReply(bool reboot) {
  return reboot ? "saved - rebooting, rejoin the wifi with the new password"
                : "saved";
}

static void handleSet() {
  bool reboot = applySet();
  server.send(200, "text/plain", setReply(reboot));
  if (reboot) { delay(400); ESP.restart(); }       // the AP restarts to apply it
}

// The page's endpoints over USB serial, for a PC or a Pi on the cable instead
// of the wifi:  $J,data  $J,info  $J,hits  $J,doc  $J,log,<since>
// The answer is "$R,<what>,<bytes>" on one line, then exactly that many bytes.
// Written to Serial directly so these bodies never enter the page's console.
static void serialReply(const char* what, const String& body) {
  Serial.print(F("$R,")); Serial.print(what);
  Serial.print(',');      Serial.println(body.length());
  Serial.print(body);
  Serial.println();
}

// Firmware update straight from the phone: pick a .bin, it writes to the spare
// OTA slot and reboots. This is what makes the box genuinely cable-free after
// the first flash. The app binary (NOT the 4 MB merged image) is what goes here.
static void handleUpdateDone() {
  bool ok = !Update.hasError();
  server.send(200, "text/plain", ok
    ? "updated - rebooting"
    : "FAILED - board is unchanged, old firmware still running");
  if (ok) { delay(500); ESP.restart(); }
}

static void handleUpdateData() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

// The handoff document for the game developer, built here from the values this
// box is actually holding, so the numbers can never be stale or invented.
// Served as plain text to read, copy or download. Raw string literals, so the
// text below is exactly what comes out - no escape sequences to get wrong.
static const char DOC_HEAD[] PROGMEM = R"DOC(# Interface

## Hauteurs des rangees

)DOC";

static const char DOC_TAIL[] PROGMEM = R"DOC(
Fraction de la hauteur d'ecran :

    y_pixels = hauteur * hauteur_ecran

Les boutons sont hors ecran : seule leur hauteur est definie. Espacement
regulier entre rangees, marges haut et bas differentes.

## La force

Le plateau est une MANETTE (joystick). La force est UN AXE, lineaire.
)DOC";

static const char DOC_MID[] PROGMEM = R"DOC(
Rien a installer, rien de specifique a ecrire : c'est une manette comme une
autre, lue par pygame, SDL, jstest ou n'importe quoi d'autre.

Deux choses a savoir :

)DOC";

static const char DOC_END[] PROGMEM = R"DOC(
- La valeur est la force TOTALE sur le plateau, sans indication de l'endroit
  ou l'on appuie.

Les boutons de la borne sont un peripherique USB separe, vu nativement.
)DOC";

static String textDoc() {
  String d;
  d.reserve(2800);

  d += FPSTR(DOC_HEAD);
  if (cfg.rowBot > cfg.rowTop) {
    d += "| rangee | boutons | hauteur |\n|---|---|---|\n";
    for (uint8_t i = 0; i < 5; i++) {
      float y = cfg.rowTop + (cfg.rowBot - cfg.rowTop) * (i / 4.0f);
      d += "| ";  d += (i + 1);
      d += " | L"; d += (i + 1);
      d += " R";   d += (i + 1);
      d += " | ";  d += String(y, 4);
      d += " |\n";
    }
  } else {
    d += "> Rangees pas encore mesurees.\n";
  }
  d += FPSTR(DOC_TAIL);

  // Full scale is the cell's own rating, so a hard hit can never clip. Every
  // figure below comes from the stored setting - it cannot go stale.
  float fsKg = cfg.axisFullScaleG / 1000.0f;
  d += "\n    axe 0.0  = rien sur le plateau\n";
  d += "    axe 1.0  = "; d += String(fsKg, 0); d += " kg\n";
  d += "\nDonc :\n\n    force_kg = axe * "; d += String(fsKg, 0); d += "\n";
  d += "\nExemple : axe 0.1 = "; d += String(fsKg / 10.0f, 0); d += " kg.\n\n";
  d += "La force est envoyee sur ";
  d += (cfg.axisMap == AXIS_X) ? "l'axe X (axe 0) seulement"
     : (cfg.axisMap == AXIS_Y) ? "l'axe Y (axe 1) seulement"
     : "les axes X et Y (axes 0 et 1), meme valeur sur les deux";
  d += ".\n";

  d += FPSTR(DOC_MID);

  // The HX711's rate is set by its RATE pin, not by anything here, so report
  // what was actually measured rather than repeating a number from a datasheet.
  d += "- Le capteur mesure ";
  if (hitSps > 0) {
    d += String(hitSps, 0);
    d += " fois par seconde (cadence mesuree).\n";
  } else {
    d += "environ 10 fois par seconde.\n";
  }
  d += "  L'axe ne change pas plus vite que ca, quelle que soit la frequence\n";
  d += "  a laquelle vous le lisez.\n";

  d += FPSTR(DOC_END);
  return d;
}

static void handleDoc() {
  if (server.hasArg("dl")) {
    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"POUR_LE_DEV.md\"");
  }
  server.send(200, "text/plain; charset=utf-8", textDoc());
}

static void startAP() {
  if (cfg.apPass[0] == 0) {           // first boot on this board
    strncpy(cfg.apPass, AP_PASS_DEFAULT, sizeof(cfg.apPass) - 1);
    cfg.passChanged = 0;
    cfgSave();
  }
  strncpy(apPass, cfg.apPass, sizeof(apPass) - 1);

  WiFi.mode(WIFI_AP);
  // ssid, password, channel, HIDDEN, max clients
  apUp = WiFi.softAP(AP_SSID, apPass, 1, 1, 4);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/",     handlePage);
  server.on("/data", handleData);
  server.on("/log",  handleLog);
  server.on("/info", handleInfo);
  server.on("/hits", handleHits);
  server.on("/doc",  handleDoc);
  server.on("/cmd",  handleCmd);
  server.on("/set",  handleSet);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateData);

  // Captive portal: every DNS lookup resolves here, and anything we do not
  // recognise redirects to the page. That is what makes phones pop up the
  // "sign in to network" sheet the moment they join, so nobody types an IP.
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

/* ---------------------------------------------------------------------------
 * setup() / loop()
 * ------------------------------------------------------------------------ */

void setup() {
  Serial.begin(115200);
  delay(300);

  // Via pointers so the compiler does not flag a self-comparison while both
  // macros still resolve to Serial. It stops being trivially true as soon as
  // MACHINE_PORT is pointed at Serial2.
  Stream* hp = &HUMAN_PORT;
  Stream* mp = &MACHINE_PORT;
  portsShared = (hp == mp);

  prefs.begin("loadcell", false);
  cfgLoad();
  tuneLoad();

  if (cfg.uiStyle <= UI_FANCY) uiStyle = cfg.uiStyle;

  // The axis has spanned 20 kg, then the old sensors' 200 kg, and now the
  // cell's own 1000 kg rating. Carry an UNTOUCHED old default across rather
  // than making anyone re-enter it; a value the user actually chose sits
  // outside both windows and is left alone.
  bool oldDefault =
      (cfg.axisFullScaleG > 19999.0f  && cfg.axisFullScaleG < 20001.0f) ||
      (cfg.axisFullScaleG > 199999.0f && cfg.axisFullScaleG < 200001.0f);
  if (oldDefault) {
    cfg.axisFullScaleG = AXIS_FS_DEFAULT_G;
    cfgSave();
  }

  // The gamepad comes up FIRST and is never stopped afterwards. Whatever else
  // fails - wifi, the web server, a bad setting - the Pi keeps seeing a pad.
  gamepad.begin();
  gamepadUp = true;

  startAP();

  hx.begin(PIN_HX711_DT, PIN_HX711_SCK);   // gain 128 on channel A by default

  HUMAN_PORT.println();
  hr();
  HUMAN_PORT.print(F("ESP32 load-cell testbed  fw "));
  HUMAN_PORT.println(FW_VERSION);
HUMAN_PORT.print(F("Gamepad : "));
  HUMAN_PORT.print('"'); HUMAN_PORT.print(BLE_NAME); HUMAN_PORT.println('"');
  HUMAN_PORT.print(F("WiFi    : "));
  if (apUp) {
    HUMAN_PORT.print(F("hidden network \"")); HUMAN_PORT.print(AP_SSID);
    HUMAN_PORT.print(F("\"  password ")); HUMAN_PORT.println(apPass);
    HUMAN_PORT.println(F("          http://192.168.4.1"));
  } else {
    HUMAN_PORT.println(F("failed to start"));
  }
  HUMAN_PORT.print(F("Free RAM: "));
  HUMAN_PORT.print(ESP.getFreeHeap());
  HUMAN_PORT.println(F(" bytes"));
  hr();
  emitId();
  printStored();

  HUMAN_PORT.println();
  if (setupComplete() && haveCal()) {
    HUMAN_PORT.println(F("Setup complete - going straight to readings."));
    HUMAN_PORT.println(F("'S' for the setup menu, 'H' for help."));
    enterState(ST_IDLE);
  } else if (cfg.stagesDone != 0) {
    // Part-way through: back to the menu rather than restarting at stage 1.
    HUMAN_PORT.println(F("Setup is unfinished - picking up where you left off."));
    enterState(ST_MENU);
  } else {
    HUMAN_PORT.println(F("Nothing stored yet. Starting setup at stage 1."));
    HUMAN_PORT.println(F("('X' leaves a stage, 'S' reopens the menu.)"));
    startRunAll();
  }
}

void loop() {
  pollSample();
  if (apUp) { dns.processNextRequest(); server.handleClient(); }
  pollPort(HUMAN_PORT, lineBuf, &lineLen, sizeof(lineBuf));
  if (!portsShared) pollPort(MACHINE_PORT, mLineBuf, &mLineLen, sizeof(mLineBuf));


  uint32_t now = millis();

  if (streamPeriod && (now - lastStreamMs) >= streamPeriod) {
    lastStreamMs = now;
    emitWeight();
  }

  // The live human readout only runs when no stage is waiting for input.
  if (state == ST_IDLE && !quiet && (now - lastLiveMs) >= LIVE_PERIOD_MS) {
    lastLiveMs = now;
    printLive();
  }

  if (state == ST_MEASURE && !quiet && (now - mLastPrint) >= 500) {
    mLastPrint = now;
    if (mPanel) { mPanel = false; measurePanel(); }
    else if (mHave) measureLine();
  }

}
