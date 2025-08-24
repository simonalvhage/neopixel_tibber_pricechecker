#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>

// ------------------- WIFI & TIBBER -------------------
#define WIFI_SSID     "SSID"
#define WIFI_PASS     "PASSWORD"
#define TIBBER_TOKEN  "TOKEN"
const char* TIBBER_URL = "https://api.tibber.com/v1-beta/gql";

// ------------------- NEOPIXEL -------------------
#define LED_PIN     D4
#define NUM_LEDS    12
#define DEFAULT_BRIGHTNESS 80

// 3 length spinning lights
#define SEG_LEN     3
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// fade for the latter 2 pixels
const float tailFade[SEG_LEN-1] = {0.35f, 0.12f};

ESP8266WebServer server(80);
enum PriceLevel { GOOD, OKAY, BAD, VERY_BAD, UNKNOWN };

struct Config {
  float goodMax      = 0.80f;       // <= GOOD
  float okMax        = 1.50f;       // <= OKAY
  float veryBadMin   = 2.50f;       // >= VERY_BAD
  uint32_t refreshMs = 5UL * 60UL * 1000UL; // 5 min
  uint16_t stepDelay = 80;          // ms per step
  uint8_t brightness = DEFAULT_BRIGHTNESS;
} cfg;

float currentPrice = NAN;
float pricePlus3h  = NAN;
PriceLevel currentLevel = UNKNOWN;
PriceLevel futureLevel  = UNKNOWN;

unsigned long lastPriceFetch = 0;

//Tibber gql query
const char* gqlBody =
  "{ \"query\": \"query { viewer { homes { currentSubscription { priceInfo { "
  "current { total startsAt currency level } "
  "today { total startsAt level } "
  "tomorrow { total startsAt level } "
  "} } } } }\" }";

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return strip.Color(r,g,b); }
uint32_t GREEN() { return rgb(0,255,0); }
uint32_t ORANGE(){ return rgb(255,100,0); }
uint32_t RED()   { return rgb(255,0,0); }

PriceLevel classifyPrice(float p) {
  if (isnan(p)) return UNKNOWN;
  if (p <= cfg.goodMax) return GOOD;
  if (p <= cfg.okMax)   return OKAY;
  return VERY_BAD;
}


void pickColors(PriceLevel nowLvl, PriceLevel plus3Lvl, uint32_t &headColor, uint32_t &tailColor) {
  switch (nowLvl) {
    case GOOD:     tailColor = GREEN();  break;
    case OKAY:     tailColor = ORANGE(); break;
    case BAD:
    case VERY_BAD: tailColor = RED();    break;
    default:       tailColor = ORANGE(); break;
  }
  headColor = tailColor;

  if (nowLvl == GOOD) {
    if (plus3Lvl == VERY_BAD) headColor = RED();
    else if (plus3Lvl == OKAY) headColor = ORANGE();
    else headColor = GREEN();
    return;
  }
  if (nowLvl == OKAY) {
    if (plus3Lvl == VERY_BAD) headColor = RED();
    else if (plus3Lvl == GOOD) headColor = GREEN();
    else headColor = ORANGE();
    return;
  }
  if (nowLvl == VERY_BAD || nowLvl == BAD) {
    if (plus3Lvl == GOOD) headColor = GREEN();
    else if (plus3Lvl == OKAY) headColor = ORANGE();
    else headColor = RED();
    return;
  }
}

inline void setPix(int idx, uint32_t c) {
  strip.setPixelColor(idx % NUM_LEDS, c);
}

void spinStepColors(uint32_t headColor, uint32_t tailColor) {
  for (int head = 0; head < NUM_LEDS; head++) {
    strip.clear();

    // head full intensity
    setPix(head, headColor);

    // tail with fade
    uint8_t tr = (tailColor >> 16) & 0xFF;
    uint8_t tg = (tailColor >> 8)  & 0xFF;
    uint8_t tb =  tailColor        & 0xFF;

    for (int t = 1; t <= 2; t++) {
      int idx = (head - t + NUM_LEDS) % NUM_LEDS;
      float f = tailFade[t-1];
      setPix(idx, rgb(uint8_t(tr * f), uint8_t(tg * f), uint8_t(tb * f)));
    }

    strip.show();
    delay(cfg.stepDelay);
  }
}

bool wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool fetchTibberPrices(float &curPrice, float &plus3hPrice) {
  curPrice = NAN;
  plus3hPrice = NAN;

  if (!wifiEnsureConnected()) return false;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  if (!https.begin(*client, TIBBER_URL)) return false;

  https.addHeader("Authorization", String("Bearer ") + TIBBER_TOKEN);
  https.addHeader("Content-Type", "application/json");

  int code = https.POST((uint8_t*)gqlBody, strlen(gqlBody));
  if (code != HTTP_CODE_OK) {
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  JsonArray homes = doc["data"]["viewer"]["homes"].as<JsonArray>();
  if (homes.isNull() || homes.size() == 0) return false;

  JsonObject priceInfo = homes[0]["currentSubscription"]["priceInfo"];
  JsonObject current   = priceInfo["current"];
  JsonArray  today     = priceInfo["today"].as<JsonArray>();
  JsonArray  tomorrow  = priceInfo["tomorrow"].as<JsonArray>();

  if (current.isNull()) return false;

  String currentStart = current["startsAt"].as<String>();
  float currentTotal  = current["total"].is<float>() ? current["total"].as<float>()
                                                     : current["total"].as<double>();

  struct Slot { String startsAt; float total; };
  Slot slots[48];
  int n = 0;

  for (JsonObject t : today) {
    if (n < 48) {
      slots[n++] = { t["startsAt"].as<String>(),
                     t["total"].is<float>() ? t["total"].as<float>()
                                            : t["total"].as<double>() };
    }
  }
  for (JsonObject t : tomorrow) {
    if (n < 48) {
      slots[n++] = { t["startsAt"].as<String>(),
                     t["total"].is<float>() ? t["total"].as<float>()
                                            : t["total"].as<double>() };
    }
  }

  int curIdx = -1;
  for (int i = 0; i < n; i++) {
    if (slots[i].startsAt == currentStart) { curIdx = i; break; }
  }

  curPrice = currentTotal;
  if (curIdx >= 0 && (curIdx + 3) < n) {
    plus3hPrice = slots[curIdx + 3].total;
  }

  return true;
}

void printSerialSnapshot() {
  Serial.println(F("------ Tibber update ------"));
  Serial.print(F("Current price: "));
  Serial.print(currentPrice, 3);
  Serial.print(F(" kr/kWh -> "));
  switch(currentLevel) {
    case GOOD: Serial.println("GOOD"); break;
    case OKAY: Serial.println("OKAY"); break;
    case BAD: Serial.println("BAD"); break;
    case VERY_BAD: Serial.println("VERY_BAD"); break;
    default: Serial.println("UNKNOWN"); break;
  }

  Serial.print(F("Pris om 3h:    "));
  if (isnan(pricePlus3h)) {
    Serial.println("N/A");
  } else {
    Serial.print(pricePlus3h, 3);
    Serial.print(F(" kr/kWh -> "));
    switch(futureLevel) {
      case GOOD: Serial.println("GOOD"); break;
      case OKAY: Serial.println("OKAY"); break;
      case BAD: Serial.println("BAD"); break;
      case VERY_BAD: Serial.println("VERY_BAD"); break;
      default: Serial.println("UNKNOWN"); break;
    }
  }
  Serial.print(F("Refreshrate: "));
  Serial.print(cfg.refreshMs / 1000);
  Serial.println(F("s"));
  Serial.println(F("---------------------------"));
}

void updatePriceLevels(bool force = false) {
  if (!force && (millis() - lastPriceFetch < cfg.refreshMs)) return;

  float cur, p3;
  if (fetchTibberPrices(cur, p3)) {
    currentPrice = cur;
    pricePlus3h  = p3;
    currentLevel = classifyPrice(currentPrice);
    futureLevel  = classifyPrice(pricePlus3h);
    printSerialSnapshot();
  }
  lastPriceFetch = millis();
}

// littleFS config
bool saveConfig() {
  DynamicJsonDocument d(512);
  d["goodMax"]    = cfg.goodMax;
  d["okMax"]      = cfg.okMax;
  d["veryBadMin"] = cfg.veryBadMin;
  d["refreshMs"]  = cfg.refreshMs;
  d["stepDelay"]  = cfg.stepDelay;
  d["brightness"] = cfg.brightness;

  File f = LittleFS.open("/config.json", "w");
  if (!f) return false;
  serializeJsonPretty(d, f);
  f.close();
  return true;
}

bool loadConfig() {
  if (!LittleFS.exists("/config.json")) return saveConfig();
  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;
  DynamicJsonDocument d(512);
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) return false;

  cfg.goodMax    = d["goodMax"]   | cfg.goodMax;
  cfg.okMax      = d["okMax"]     | cfg.okMax;
  cfg.veryBadMin = d["veryBadMin"]| cfg.veryBadMin;
  cfg.refreshMs  = d["refreshMs"] | cfg.refreshMs;
  cfg.stepDelay  = d["stepDelay"] | cfg.stepDelay;
  cfg.brightness = d["brightness"]| cfg.brightness;
  return true;
}

// html
String htmlPage() {
  String nowStr = isnan(currentPrice) ? "N/A" : String(currentPrice, 3);
  String p3Str  = isnan(pricePlus3h)  ? "N/A" : String(pricePlus3h, 3);

  String h = F(
    "<!doctype html><html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>NeoPixel & Tibber</title>"
    "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;max-width:720px}"
    "h1{font-size:1.4rem} label{display:block;margin-top:12px;font-weight:600}"
    "input{padding:.5rem;width:100%;max-width:220px}"
    "button{padding:.6rem 1rem;margin-top:16px;cursor:pointer}"
    ".row{display:flex;gap:16px;flex-wrap:wrap}"
    ".card{border:1px solid #ddd;border-radius:12px;padding:16px;margin-top:16px}"
    ".kv{display:grid;grid-template-columns:160px 1fr;gap:8px}"
    "</style></head><body>"
  );
  h += F("<h1>NeoPixel & Tibber – Konfiguration</h1>");

  h += "<div class='card'><div class='kv'>"
       "<div>Pris nu:</div><div>" + nowStr + " kr/kWh (" +
       (currentLevel==GOOD?"GOOD":currentLevel==OKAY?"OKAY":currentLevel==BAD?"BAD":currentLevel==VERY_BAD?"VERY_BAD":"UNKNOWN")
       + ")</div>"
       "<div>Pris +3h:</div><div>" + p3Str + " kr/kWh (" +
       (futureLevel==GOOD?"GOOD":futureLevel==OKAY?"OKAY":futureLevel==BAD?"BAD":futureLevel==VERY_BAD?"VERY_BAD":"UNKNOWN")
       + ")</div>"
       "<div>Uppdateringsintervall:</div><div>" + String(cfg.refreshMs/1000) + " s</div>"
       "</div>"
       "<form class='row' action='/refresh' method='post'><button>Hämta pris nu</button></form>"
       "</div>";

  h += F("<div class='card'><form method='post' action='/save'>");
  h += "<label>GOOD ≤ (kr/kWh)<br/><input type='number' step='0.01' name='goodMax' value='"+String(cfg.goodMax,2)+"'></label>";
  h += "<label>OKAY ≤ (kr/kWh)<br/><input type='number' step='0.01' name='okMax' value='"+String(cfg.okMax,2)+"'></label>";
  h += "<label>VERY_BAD ≥ (kr/kWh)<br/><input type='number' step='0.01' name='veryBadMin' value='"+String(cfg.veryBadMin,2)+"'></label>";

  h += "<label>Uppdateringsintervall (sek)<br/><input type='number' step='1' min='5' name='refreshSec' value='"+String(cfg.refreshMs/1000)+"'></label>";
  h += "<label>Snurrhastighet STEP_DELAY (ms)<br/><input type='number' step='1' min='0' name='stepDelay' value='"+String(cfg.stepDelay)+"'></label>";
  h += "<label>Ljusstyrka (0–255)<br/><input type='number' step='1' min='0' max='255' name='brightness' value='"+String(cfg.brightness)+"'></label>";

  h += F("<button type='submit'>Spara</button></form></div>");

  h += F("<p style='margin-top:16px;color:#777'>Tips: GOOD ≤ OKAY ≤ VERY_BAD (håll rimlig ordning). Ändringar sparas till LittleFS.</p>");
  h += F("</body></html>");
  return h;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
}

void handleRefreshNow() {
  updatePriceLevels(true);
  server.sendHeader("Location", "/");
  server.send(303);
}

float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

void handleSave() {
  if (server.hasArg("goodMax"))    cfg.goodMax    = server.arg("goodMax").toFloat();
  if (server.hasArg("okMax"))      cfg.okMax      = server.arg("okMax").toFloat();
  if (server.hasArg("veryBadMin")) cfg.veryBadMin = server.arg("veryBadMin").toFloat();

  if (server.hasArg("refreshSec")) {
    long s = server.arg("refreshSec").toInt();
    if (s < 5) s = 5;
    cfg.refreshMs = (uint32_t)s * 1000UL;
  }
  if (server.hasArg("stepDelay")) {
    int sd = server.arg("stepDelay").toInt();
    if (sd < 0) sd = 0;
    cfg.stepDelay = (uint16_t)sd;
  }
  if (server.hasArg("brightness")) {
    int b = server.arg("brightness").toInt();
    b = (int)clampf(b, 0, 255);
    cfg.brightness = (uint8_t)b;
    strip.setBrightness(cfg.brightness);
  }

  if (cfg.okMax < cfg.goodMax)    cfg.okMax = cfg.goodMax;
  if (cfg.veryBadMin < cfg.okMax) cfg.veryBadMin = cfg.okMax;

  saveConfig();
  // update
  currentLevel = classifyPrice(currentPrice);
  futureLevel  = classifyPrice(pricePlus3h);
  server.sendHeader("Location", "/");
  server.send(303);
}


void setup() {
  Serial.begin(115200);
  delay(200);

  // FS
  if (!LittleFS.begin()) {
    Serial.println(F("LittleFS mount misslyckades; försöker formatera..."));
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("Ansluter WiFi"));
  for (int i=0; i<60 && WiFi.status()!=WL_CONNECTED; ++i) {
    delay(250); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status()==WL_CONNECTED) {
    Serial.print(F("IP: ")); Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("WiFi ej anslutet, fortsätter ändå (försöker vid fetch)"));
  }

  // LEDs
  strip.begin();
  strip.setBrightness(cfg.brightness);
  strip.show();

  // Web
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/refresh", HTTP_POST, handleRefreshNow);
  server.begin();
  Serial.println(F("Webserver startad på port 80"));

  // first price update
  updatePriceLevels(true);
}

void loop() {
  server.handleClient();

  // update price each interval
  updatePriceLevels();

  // choose colors
  uint32_t headC = ORANGE();
  uint32_t tailC = ORANGE();
  pickColors(currentLevel, futureLevel, headC, tailC);
  spinStepColors(headC, tailC);
}
