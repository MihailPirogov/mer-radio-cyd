// Mari El Radio on FM 105.5 + online "now playing" with cover
// ESP32-2432S028 (CYD) + RRD-102 (RDA5807M), LVGL 8.3
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <TJpg_Decoder.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <RDA5807.h>
#include <vector>
#include <algorithm>
#include <Preferences.h>
#include <esp_wifi.h>
#include "secrets.h"
#include "logo.h"
#include "testjpg.h"

#define SCREEN_ROTATION 1      // 1 or 3 (180 deg flip)
#define FM_FREQ 10550          // 105.5 MHz, 10 kHz units
#define FM_VOLUME 8            // 0..15
#define I2C_SDA 27
#define I2C_SCL 22
#define COVER 90
#define C_ACCENT 0xF44336   // brand red (app colors.xml), used sparingly
#define C_BG_TOP 0x0E0E12
#define C_TEXT   0xFFFFFF
#define C_MUTED  0xB0B0BA
#define C_DIM    0x2C2C34   // dividers / inactive

// This CYD panel shows a colour NEGATIVE without INVON (user saw white bg, black text,
// red accents as cyan, photos negative). GRAM content was verified correct by readback,
// so the fix is panel inversion, not pixel format. Set 0 for panels that do not need it.
#define PANEL_INVERT 1
#define BOOT_TEST_MS 0         // colour test screen at boot (0 = off)
#define WIFI_SLEEP false     // test: true/false (flicker)

// Same sources as the Mari El Radio app (cr/s2 RadioService.java)
static const char *STATUS_URL = "https://mari-el-radio.ru:8010/status-json.xsl";
static const char *SITE_TEXT_URL = "https://tv.metr12.ru/schedules/mer.txt";
#define STATUS_POLL_MS 15000

LV_FONT_DECLARE(font_cyr_14);
LV_FONT_DECLARE(font_cyr_10);
LV_FONT_DECLARE(font_cyr_12);
LV_FONT_DECLARE(font_cyr_b14);
LV_FONT_DECLARE(font_cyr_b18);

TFT_eSPI tft;
RDA5807 rx;

static const uint16_t W = 320, H = 240;
static lv_disp_draw_buf_t drawBuf;
static lv_color_t buf1[W * 16];

lv_obj_t *lineMid, *icoLis, *lblLis, *lblLisSub, *capRaw, *capRds, *lineRds, *lblStation, *lblRaw, *lblPS, *lblRT, *badgeSt, *badgeRds, *lblWifi, *barSig, *lblNow, *lblArtist, *lblTitle, *imgCover, *sig[4];
bool tunerOk = false;
char ps[9] = "", rt[65] = "";

// ---- shared between net task (core 0) and UI (core 1) ----
SemaphoreHandle_t mtx;
static uint16_t *coverPix;  // heap, COVER*COVER
static lv_img_dsc_t coverDsc;
volatile bool songDirty = false, coverDirty = false, coverNone = false;
String sharedArtist, sharedTitle, sharedRaw;  // split exactly like the app
volatile bool netOk = false, forceDisconnect = false;
// ---- fault monitor (rear RGB LED: GPIO4 R, 16 G, 17 B, active LOW) ----
#define LED_R 4
#define LED_G 16
#define LED_B 17
volatile uint32_t wifiDownSince = 1;   // millis when Wi-Fi went down (0 = up)
volatile int netFailStreak = 0;
volatile bool tunerBad = false, simFault = false;
uint32_t lowRssiSince = 0, noRdsSince = 0;
void ledWrite(uint8_t r, uint8_t g, uint8_t b);
bool tuneVerified(const char *why);
volatile uint32_t statTracks = 0, statCovers = 0, statFails = 0, statReconnects = 0, statI2cRecover = 0;
volatile int wifiState = 0;  // 0 off, 1 connecting, 2 ok, 3 setup portal

// ================= text rules copied from the app =================
// RadioService.stripParentheses: remove (...) incl. nested, unclosed "(" cuts tail, collapse spaces
String stripParentheses(const String &in) {
  String s = in;
  bool changed = true;
  while (changed) {
    changed = false; int start = -1, depth = 0;
    for (int i = 0; i < (int)s.length(); i++) {
      char c = s[i];
      if (c == '(') { if (depth == 0) start = i; depth++; }
      else if (c == ')' && depth > 0) {
        depth--;
        if (depth == 0 && start >= 0) { s.remove(start, i + 1 - start); changed = true; break; }
      }
    }
    if (!changed && start >= 0) { s.remove(start); changed = true; }
  }
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  s.trim();
  return s;
}
// RadioService.applyLiveTrackText: last two " - " segments = artist + title
void splitTrack(const String &raw, String &artist, String &title) {
  String c = stripParentheses(raw);
  artist = ""; title = "";
  int idx = c.lastIndexOf(" - ");
  if (idx >= 0) {
    String before = c.substring(0, idx); before.trim();
    title = c.substring(idx + 3); title.trim();
    int idx2 = before.lastIndexOf(" - ");
    if (idx2 >= 0) { artist = before.substring(idx2 + 3); artist.trim(); } else artist = before;
  } else title = c;
}

// ---- ArtMatcher.java port (UTF-8 aware for Latin/Cyrillic) ----
static void putCp(String &o, uint32_t cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}
static std::vector<uint32_t> decodeUtf8(const String &s) {
  std::vector<uint32_t> v; const uint8_t *p = (const uint8_t *)s.c_str();
  while (*p) {
    uint32_t c = *p++;
    if (c >= 0xF0) { c = ((c & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (c >= 0xE0) { c = ((c & 15) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (c >= 0xC0) { c = ((c & 31) << 6) | (p[0] & 0x3F); p += 1; }
    v.push_back(c);
  }
  return v;
}
static uint32_t lowerCp(uint32_t c, bool &letter) {
  letter = true;
  if (c >= 'A' && c <= 'Z') return c + 32;
  if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return c;
  if (c == 0x401 || c == 0x451) return 0x435;           // ё -> е
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x430 && c <= 0x44F) return c;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  if (c >= 0x450 && c <= 0x45F) return c;
  if (c >= 0x460 && c <= 0x4FF) return (c & 1) ? c : c + 1;  // extended Cyrillic incl. Mari letters
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
  if (c >= 0xDF && c <= 0xFF && c != 0xF7) return c;
  letter = false; return ' ';
}
String normalizeArt(const String &in) {
  std::vector<uint32_t> v = decodeUtf8(in);
  String o; bool sp = true; int paren = 0, br = 0;
  for (uint32_t c : v) {
    if (c == '(') { paren++; c = ' '; } else if (c == ')' && paren) { paren--; c = ' '; }
    else if (c == '[') { br++; c = ' '; } else if (c == ']' && br) { br--; c = ' '; }
    else if (paren || br) continue;
    bool letter; uint32_t l = lowerCp(c, letter);
    if (!letter) { if (!sp) { o += ' '; sp = true; } continue; }
    putCp(o, l); sp = false;
  }
  o.trim(); return o;
}
static int cpLen(const String &s) { int n = 0; for (char c : s) if ((c & 0xC0) != 0x80) n++; return n; }
static std::vector<String> artWords(const String &s) {
  static const char *stop[] = {"feat", "ft", "featuring", "vs", "x", "and", "и", "with", "the"};
  std::vector<String> out; String n = normalizeArt(s) + " "; int st = 0;
  for (int i = 0; i < (int)n.length(); i++) if (n[i] == ' ') {
    String w = n.substring(st, i); st = i + 1;
    if (cpLen(w) < 2) continue;
    bool skip = false; for (auto sw : stop) if (w == sw) skip = true;
    bool dup = false; for (auto &x : out) if (x == w) dup = true;
    if (!skip && !dup) out.push_back(w);
  }
  return out;
}
bool artistMatchesStrict(const String &exp, const String &cand) {
  auto e = artWords(exp), c = artWords(cand);
  if (e.empty() || c.empty()) return false;
  for (auto &w : c) { bool f = false; for (auto &x : e) if (x == w) f = true; if (!f) return false; }
  return true;
}
bool titleEquals(const String &e0, const String &c0) { String e = normalizeArt(e0); return cpLen(e) >= 2 && e == normalizeArt(c0); }
bool titleMatches(const String &e0, const String &c0) {
  String e = normalizeArt(e0), c = normalizeArt(c0);
  if (cpLen(e) < 2 || cpLen(c) < 2) return false;
  if (e == c) return true;
  int mn = min(cpLen(e), cpLen(c));
  return mn >= 4 && (e.startsWith(c) || c.startsWith(e));
}
String urlEncode(const String &s) {
  String o; const char *hex = "0123456789ABCDEF";
  for (uint8_t c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// ================= networking (core 0) =================
bool httpGet(const String &url, String &body, size_t maxLen = 64000) {
  WiFiClientSecure cli; cli.setInsecure(); cli.setHandshakeTimeout(8);
  HTTPClient http; http.useHTTP10(true); http.setTimeout(6000); http.setConnectTimeout(6000); http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(cli, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    int len = http.getSize();
    if (len < 0 || (size_t)len <= maxLen) {
      body = ""; body.reserve(len > 0 ? len : 4096);
      WiFiClient *st = http.getStreamPtr(); uint32_t t0 = millis();
      while ((http.connected() || st->available()) && millis() - t0 < 10000 && body.length() < maxLen) {
        esp_task_wdt_reset();
        while (st->available() && body.length() < maxLen) body += (char)st->read();
        if (len > 0 && (int)body.length() >= len) break;
        delay(2);
      }
      ok = body.length() > 0;
    }
  }
  Serial.printf("GET %s -> %d (%u bytes) %s | heap %u maxAlloc %u\n", url.c_str(), code, body.length(), code < 0 ? http.errorToString(code).c_str() : "", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  http.end();
  return ok;
}
// stream an image into SPIFFS (/cover.jpg) so big photos do not need RAM
bool httpGetFile(const String &url, const char *path, size_t maxLen = 400000) {
  WiFiClientSecure cli; cli.setInsecure(); cli.setHandshakeTimeout(8);
  HTTPClient http; http.useHTTP10(true); http.setTimeout(6000); http.setConnectTimeout(6000); http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(cli, url)) return false;
  int code = http.GET(); int len = http.getSize(); size_t n = 0;
  if (code == 200 && len > 0 && (size_t)len <= maxLen) {
    File f = SPIFFS.open(path, FILE_WRITE);
    if (f) {
      static uint8_t chunk[1024]; WiFiClient *st = http.getStreamPtr(); uint32_t t0 = millis();
      while (n < (size_t)len && millis() - t0 < 15000) {
        int a = st->available();
        esp_task_wdt_reset();
        if (a) { int r = st->read(chunk, min((size_t)a, sizeof(chunk))); if (r > 0) { f.write(chunk, r); n += r; } }
        else delay(2);
      }
      f.close();
    }
  }
  Serial.printf("IMG %s -> %d len %d got %u\n", url.c_str(), code, len, n);
  http.end();
  return len > 0 && n == (size_t)len;
}

String fetchStatusTitle() {
  String body;
  if (!httpGet(STATUS_URL, body, 16000)) return "";
  JsonDocument doc;
  DeserializationError je = deserializeJson(doc, body);
  if (je) { Serial.printf("status JSON error: %s\n", je.c_str()); return ""; }
  JsonVariant src = doc["icestats"]["source"];
  String first, main;
  auto take = [&](JsonVariant s) {
    String t = s["title"] | ""; String u = s["listenurl"] | "";
    if (first.isEmpty()) first = t;
    if (u.endsWith("/stream") && main.isEmpty()) main = t;
  };
  if (src.is<JsonArray>()) for (JsonVariant s : src.as<JsonArray>()) take(s); else take(src);
  String t = main.length() ? main : first; t.trim(); return t;
}
// In-stream ICY metadata (what the app reads): GET /stream with Icy-MetaData:1,
// skip icy-metaint audio bytes, read StreamTitle, close. Plain HTTP, port 8011.
String fetchIcyTitle() {
  WiFiClient c; c.setTimeout(6000);
  if (!c.connect("mari-el-radio.ru", 8011, 6000)) { Serial.println("ICY connect failed"); return ""; }
  c.print("GET /stream HTTP/1.0\r\nHost: mari-el-radio.ru\r\nIcy-MetaData: 1\r\nUser-Agent: CYD-MER\r\n\r\n");
  int metaint = 0; uint32_t t0 = millis();
  while (millis() - t0 < 6000) {                      // headers
    String l = c.readStringUntil('\n'); l.trim();
    if (l.startsWith("icy-metaint:")) metaint = l.substring(12).toInt();
    if (l.length() == 0) break;
  }
  String title;
  if (metaint > 0 && metaint < 65536) {
    static uint8_t junk[512]; int left = metaint; t0 = millis();
    while (left > 0 && c.connected() && millis() - t0 < 8000) {
      esp_task_wdt_reset();
      int a = c.available(); if (!a) { delay(2); continue; }
      int r = c.read(junk, min(left, min(a, (int)sizeof(junk)))); if (r > 0) left -= r;
    }
    t0 = millis(); while (!c.available() && millis() - t0 < 3000) delay(2);
    int len = c.read() * 16;
    if (len > 0 && len < 4081) {
      String meta; meta.reserve(len); t0 = millis();
      while ((int)meta.length() < len && millis() - t0 < 3000) { if (c.available()) meta += (char)c.read(); else delay(2); }
      int a = meta.indexOf("StreamTitle='"), b = meta.indexOf("';", a + 13);
      if (a >= 0 && b > a) title = meta.substring(a + 13, b);
    }
  }
  c.stop();
  title.trim();
  Serial.printf("ICY metaint %d -> '%s'\n", metaint, title.c_str());
  return title;
}

String fetchSiteText() {
  String b;
  if (!httpGet(String(SITE_TEXT_URL) + "?v=" + millis(), b, 500)) return "";
  b.replace("\r", ""); b.replace("\n", " "); b.trim();
  if (b.startsWith("<")) return "";
  return b;
}

// ---- internet listeners: Mari El Icecast (all mounts) + METR relay (/mer) ----
volatile int lisMari = -1, lisMetr = -1; volatile bool lisDirty = false;
static int sumListeners(const char *url, const char *mustContain, const char *mustEnd) {
  String b; if (!httpGet(url, b, 16000)) { Serial.printf("LISTENERS fetch failed: %s\n", url); return -1; }
  JsonDocument d; if (deserializeJson(d, b)) return -1;
  JsonVariant src = d["icestats"]["source"]; int n = 0;
  auto take = [&](JsonVariant x) { String u = x["listenurl"] | "";
    if ((!mustContain || u.indexOf(mustContain) >= 0) && (!mustEnd || u.endsWith(mustEnd))) n += x["listeners"] | 0; };
  if (src.is<JsonArray>()) for (JsonVariant x : src.as<JsonArray>()) take(x); else take(src);
  return n;
}
void pollListeners() {
  int a = sumListeners(STATUS_URL, "mari-el-radio.ru", nullptr);
  int b = sumListeners("https://metr12.ru/play/status-json.xsl", nullptr, "/mer");
  if (a >= 0) lisMari = a; if (b >= 0) lisMetr = b; lisDirty = true;
  Serial.printf("LISTENERS mari-el %d + metr12 %d = %d\n", a, b, max(a, 0) + max(b, 0));
}

// ---- JPEG -> coverPix (center crop) ----
static uint16_t *decBuf; static int offX, offY;
static uint8_t g5[32], g6[64]; static bool gInit = false;
#define COVER_GAMMA 1.4f   // >1 darkens mid-tones (panel shows photos too bright)
static inline uint16_t gammaPx(uint16_t c) {
  if (!gInit) { for (int i = 0; i < 32; i++) g5[i] = (uint8_t)lroundf(31 * powf(i / 31.0f, COVER_GAMMA));
                for (int i = 0; i < 64; i++) g6[i] = (uint8_t)lroundf(63 * powf(i / 63.0f, COVER_GAMMA)); gInit = true; }
  return (g5[c >> 11] << 11) | (g6[(c >> 5) & 63] << 5) | g5[c & 31];
}
bool jpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
  for (int j = 0; j < h; j++) {
    int yy = y + j - offY; if (yy < 0 || yy >= COVER) continue;
    for (int i = 0; i < w; i++) { int xx = x + i - offX; if (xx >= 0 && xx < COVER) decBuf[yy * COVER + xx] = gammaPx(bmp[j * w + i]); }
  }
  return true;
}
bool loadCover(const String &url) {
  const char *path = "/cover.jpg";
  if (!httpGetFile(url, path)) return false;
  uint16_t w = 0, h = 0; bool ok = false;
  if (TJpgDec.getFsJpgSize(&w, &h, path, SPIFFS) == JDR_OK && w && h) {
    uint8_t sc = 1; while (sc < 8 && (w / sc > COVER || h / sc > COVER)) sc *= 2;   // fit whole image, no crop
    TJpgDec.setJpgScale(sc);
    int sw = w / sc, sh = h / sc; offX = (sw - COVER) / 2; offY = (sh - COVER) / 2;
    decBuf = (uint16_t *)malloc(COVER * COVER * 2);
    if (decBuf) {
      for (int i = 0; i < COVER * COVER; i++) decBuf[i] = lv_color_hex(C_BG_TOP).full;
      JRESULT jr = TJpgDec.drawFsJpg(0, 0, path, SPIFFS); ok = jr == JDR_OK;
      if (!ok) Serial.printf("TJpgDec result %d (3=progressive/unsupported)\n", jr);
      if (ok) { xSemaphoreTake(mtx, portMAX_DELAY); memcpy(coverPix, decBuf, COVER * COVER * 2); coverDirty = true; coverNone = false; xSemaphoreGive(mtx); }
      free(decBuf);
    }
  }
  if (ok) { uint32_t r = 0, g = 0, b = 0; for (int i = 0; i < COVER * COVER; i++) { uint16_t v = coverPix[i]; r += (v >> 11) << 3; g += ((v >> 5) & 63) << 2; b += (v & 31) << 3; }
    Serial.printf("cover avg RGB %u %u %u\n", r / (COVER * COVER), g / (COVER * COVER), b / (COVER * COVER)); }
  Serial.printf("cover %ux%u decode %s (maxAlloc %u)\n", w, h, ok ? "OK" : "FAIL", ESP.getMaxAllocHeap());
  return ok;
}
// Step 1 of the app: iTunes, only with verified artist+title match
String itunesCover(const String &artist, const String &title) {
  if (artist.isEmpty() || title.isEmpty()) return "";
  String body;
  if (!httpGet("https://itunes.apple.com/search?term=" + urlEncode(artist + " " + title) + "&media=music&limit=10", body, 60000)) return "";
  JsonDocument filter; filter["results"][0]["artistName"] = true; filter["results"][0]["trackName"] = true; filter["results"][0]["artworkUrl100"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return "";
  String chosen;
  for (JsonVariant c : doc["results"].as<JsonArray>()) {
    String art = c["artworkUrl100"] | ""; if (art.isEmpty()) continue;
    if (!artistMatchesStrict(artist, c["artistName"] | "")) continue;
    if (titleEquals(title, c["trackName"] | "")) { chosen = art; break; }
    if (chosen.isEmpty() && titleMatches(title, c["trackName"] | "")) chosen = art;
  }
  chosen.replace("100x100bb", "90x90bb");  // app uses 300x300bb; 90 = our cover size
  return chosen;
}
// Step 2 of the app: mari-el-radio.ru/save.php?find=<raw on-air text>
String siteCover(const String &raw) {
  String b;
  String q = urlEncode(raw);
  if (!httpGet("https://mari-el-radio.ru/save.php?find=" + q, b, 1000)) return "";
  int nl = b.indexOf('\n'); if (nl >= 0) b = b.substring(0, nl);
  b.trim();
  if (b.isEmpty() || b == "none" || b.startsWith("<")) return "";
  int sl = b.lastIndexOf('/');
  return "https://mari-el-radio.ru/" + b.substring(0, sl + 1) + urlEncode(b.substring(sl + 1));
}

static bool waitWifi(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { esp_task_wdt_reset(); if (WiFi.status() == WL_CONNECTED) return true; vTaskDelay(250 / portTICK_PERIOD_MS); }
  return WiFi.status() == WL_CONNECTED;
}
// ---- known networks: our own NVS list (namespace "mernets", up to 5, newest first) + secrets.h default ----
#define MAX_NETS 5
static Preferences netPrefs;
struct Net { String ssid, pass; };
static std::vector<Net> loadNets() {
  std::vector<Net> v; netPrefs.begin("mernets", true);
  int n = netPrefs.getInt("n", 0);
  for (int i = 0; i < n && i < MAX_NETS; i++) { Net x{ netPrefs.getString(("s" + String(i)).c_str(), ""), netPrefs.getString(("p" + String(i)).c_str(), "") }; if (x.ssid.length()) v.push_back(x); }
  netPrefs.end(); return v;
}
static void saveNet(const String &ssid, const String &pass) {
  if (!ssid.length()) return;
  auto v = loadNets(); if (v.size() && v[0].ssid == ssid && v[0].pass == pass) return;   // already newest: no flash write
  std::vector<Net> o{ { ssid, pass } };
  for (auto &x : v) if (x.ssid != ssid && o.size() < MAX_NETS) o.push_back(x);
  netPrefs.begin("mernets", false);
  netPrefs.putInt("n", o.size());
  for (size_t i = 0; i < o.size(); i++) { netPrefs.putString(("s" + String(i)).c_str(), o[i].ssid); netPrefs.putString(("p" + String(i)).c_str(), o[i].pass); }
  netPrefs.end();
  Serial.printf("WiFi: saved '%s' to NVS (%u known)\n", ssid.c_str(), (unsigned)o.size());
}
static void forgetNet(const String &ssid) {
  auto v = loadNets(); netPrefs.begin("mernets", false); int k = 0;
  for (auto &x : v) if (x.ssid != ssid) { netPrefs.putString(("s" + String(k)).c_str(), x.ssid); netPrefs.putString(("p" + String(k)).c_str(), x.pass); k++; }
  netPrefs.putInt("n", k); netPrefs.end(); Serial.printf("WiFi: forgot '%s'\n", ssid.c_str());
}
void logNets() {
  auto v = loadNets(); Serial.printf("WiFi: %u saved network(s):", (unsigned)v.size());
  for (auto &x : v) Serial.printf(" '%s'", x.ssid.c_str()); Serial.printf(" + default '%s'\n", WIFI_SSID);
}
void wifiInitNets() {                       // once at boot: migrate legacy ESP-IDF creds into our list
  WiFi.mode(WIFI_STA); WiFi.persistent(false); WiFi.setAutoReconnect(true);
  wifi_config_t c; if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0]) {
    String ss = String((char *)c.sta.ssid), pp = String((char *)c.sta.password);
    bool have = false; for (auto &x : loadNets()) if (x.ssid == ss) have = true;
    if (!have) saveNet(ss, pp);
  }
  logNets();
}
// one attempt: scan, try every known network that is visible (strongest first); portal if none worked
static bool everConnected = false;
static bool connectWifiOnce(int attempt) {
  wifiState = 1;
  auto nets = loadNets();
  bool haveDef = false; for (auto &x : nets) if (x.ssid == WIFI_SSID) haveDef = true;
  if (!haveDef) nets.push_back({ WIFI_SSID, WIFI_PASS });
  int n = WiFi.scanNetworks(false, false); esp_task_wdt_reset();
  std::vector<std::pair<int, int>> cand;   // (rssi, idx)
  for (size_t k = 0; k < nets.size(); k++) { int best = -999; for (int i = 0; i < n; i++) if (WiFi.SSID(i) == nets[k].ssid) best = max(best, (int)WiFi.RSSI(i)); if (best > -999) cand.push_back({ best, (int)k }); }
  WiFi.scanDelete();
  std::sort(cand.begin(), cand.end(), [](auto &a, auto &b) { return a.first > b.first; });
  Serial.printf("WiFi: scan %d APs, %u known visible\n", n, (unsigned)cand.size());
  for (auto &c : cand) {
    Net &x = nets[c.second]; Serial.printf("WiFi: trying '%s' (%d dBm)\n", x.ssid.c_str(), c.first);
    WiFi.begin(x.ssid.c_str(), x.pass.c_str());
    if (waitWifi(15000)) { saveNet(x.ssid, x.pass); return true; }
    WiFi.disconnect(false, false);          // drop this attempt only; never erase stored config
  }
  if (attempt >= (everConnected ? 3 : 1)) {   // none of the known networks reachable -> portal
    Serial.println("WiFi: no known network reachable -> portal MariRadio-Setup (192.168.4.1)");
    wifiState = 3;
    esp_task_wdt_delete(NULL);  // portal blocks up to 300 s
    WiFiManager wm; wm.setDebugOutput(false); wm.setConfigPortalTimeout(300); wm.setConnectTimeout(20); wm.setBreakAfterConfig(true);
    String newS, newP;
    wm.setSaveConfigCallback([&]() { newS = wm.getWiFiSSID(false); newP = wm.getWiFiPass(false); });
    bool ok = wm.startConfigPortal("MariRadio-Setup");
    esp_task_wdt_add(NULL);
    if (WiFi.status() == WL_CONNECTED) { newS = WiFi.SSID(); newP = WiFi.psk(); }
    if (newS.length()) saveNet(newS, newP);
    Serial.println(ok ? "WiFi: portal connected" : "WiFi: portal timeout/failed");
    WiFi.mode(WIFI_STA); wifiState = 1;
    return WiFi.status() == WL_CONNECTED;
  }
  return false;
}


// ---- demo mode for screenshots: 'D1'..'D5' inject a track (real cover lookup), 'D0' back to live ----
struct Demo { const char *icy, *full, *ps, *rt; };
static const Demo DEMOS[] = {
  { "", "", "", "" },
  { "Смирнов Иван - Эх, могае пиалем!", "Смирнов Иван - Эх, могае пиалем!", "MARRADIO", "SMIRNOV IVAN - EKH, MOGAE PIALEM! | MARIJ EL RADIO 105.5 FM YOLA" },
  { "Богданова Наталья - Кертат чыла сенен", "Богданова Наталья - Кертат чыла сенен (Ефимов П.)", "MARRADIO", "BOGDANOVA NATALYA - KERTAT CHYLA SENEN" },
  { "Речкин Александр - Йӧндымсӧ рапсодий", "Речкин Александр - Йӧндымсӧ рапсодий (муз. П. Ефимов, сл. С. Иванов, исп. ансамбль «Марий Эл», запись 2019 г.)", "MARRADIO", "Марий Эл Радио: Речкин Александр - Йӧндымсӧ рапсодий, 2019 г. !!" },
  { "Смирнов Иван - Эх, могае пиалем!", "Смирнов Иван - Эх, могае пиалем!", "MARRADIO", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW" },   // RT worst case
  { "Данилова Ирина - Вычы, вычы", "Данилова Ирина - Вычы, вычы", "MARRADIO", "DANILOVA IRINA - VYCHY, VYCHY" },
};
volatile int demoReq = -1; volatile bool demoMode = false;
static void applyTrack(const String &raw, String &lastKey) {
  String artist, title; splitTrack(raw, artist, title);
  if (artist.isEmpty() && title.isEmpty()) return;
  String a = artist.length() ? artist : "Неизвестный исполнитель";
  String t = title.length() ? title : "Неизвестный трек";
  xSemaphoreTake(mtx, portMAX_DELAY); sharedArtist = a; sharedTitle = t; songDirty = true; xSemaphoreGive(mtx);
  String key = a + "|" + t;
  if (key == lastKey) { Serial.println("same track, cover kept"); return; }   // no cover refetch
  lastKey = key; statTracks++;
  Serial.printf("NOW: raw='%s' artist='%s' title='%s'\n", raw.c_str(), artist.c_str(), title.c_str());
  bool got = false;
  if (artist.length() && title.length()) { String u = itunesCover(artist, title); if (u.length()) { Serial.println("iTunes: " + u); got = loadCover(u); } }
  if (!got) { String u = siteCover(raw); if (u.length()) { Serial.println("site photo: " + u);
      got = loadCover("https://images.weserv.nl/?url=" + urlEncode(u) + "&w=" + COVER + "&h=" + COVER + "&fit=contain&cbg=0e0e12&output=jpg&q=88");
      if (!got) got = loadCover(u); } }
  if (got) statCovers++;
  else { xSemaphoreTake(mtx, portMAX_DELAY); coverNone = true; coverDirty = true; xSemaphoreGive(mtx); Serial.println("cover: logo"); }
}

void netTask(void *) {
  esp_task_wdt_add(NULL);
  String lastRaw, lastKey; uint32_t lastPoll = 0, nextTry = 0, backoff = 5000; int attempt = 0; int fails = 0;
  for (;;) {
    esp_task_wdt_reset();
    if (forceDisconnect) { forceDisconnect = false; Serial.println("TEST: WiFi.disconnect()"); WiFi.disconnect(); vTaskDelay(500 / portTICK_PERIOD_MS); }
    if (WiFi.status() != WL_CONNECTED) {
      netOk = false;
      if (!wifiDownSince) wifiDownSince = millis() | 1;
      if (wifiState == 2) { Serial.println("WiFi lost"); wifiState = 1; }
      if ((int32_t)(millis() - nextTry) >= 0) {
        attempt++;
        if (connectWifiOnce(attempt)) { attempt = 0; backoff = 5000; }
        else { nextTry = millis() + backoff; Serial.printf("WiFi: retry in %lu s\n", backoff / 1000); backoff = min<uint32_t>(backoff * 2, 120000); }
      }
      vTaskDelay(500 / portTICK_PERIOD_MS); continue;
    }
    wifiDownSince = 0;
    if (wifiState != 2) {
      wifiState = 2; if (everConnected) statReconnects++; everConnected = true;
      Serial.printf("WiFi OK %s RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI()); lastPoll = 0;
    }
    if (demoReq >= 0) { int d = demoReq; demoReq = -1;
      if (d == 0) { demoMode = false; lastRaw = ""; lastKey = ""; lastPoll = 0; Serial.println("DEMO off -> live"); }
      else { demoMode = true; Serial.printf("DEMO %d\n", d); applyTrack(DEMOS[d].icy, lastKey);
        xSemaphoreTake(mtx, portMAX_DELAY); sharedRaw = DEMOS[d].full; songDirty = true; xSemaphoreGive(mtx); Serial.println("DEMO ready"); } }
    if (!demoMode && (lastPoll == 0 || millis() - lastPoll > STATUS_POLL_MS)) {
      lastPoll = millis();
      String raw = fetchIcyTitle();                       // primary: in-stream ICY, as the app
      if (raw.isEmpty()) raw = fetchStatusTitle();        // Icecast status-json.xsl
      String site = fetchSiteText();                      // mer.txt: full line incl. "(...)" -> raw field
      if (raw.isEmpty()) raw = fetchSiteText();           // mer.txt (Mari letters replaced)
      if (raw.isEmpty()) { fails++; statFails++; netFailStreak = fails; if (fails >= 2) netOk = false; }   // stream/server down: keep last data
      else {
        fails = 0; netFailStreak = 0; netOk = true;
        if (raw.length() > 1000) raw = raw.substring(0, 1000);   // memory guard only
        if (raw != lastRaw) { lastRaw = raw; applyTrack(raw, lastKey); }
        static uint32_t lisT = 0; if (lisT == 0 || millis() - lisT > 30000) { lisT = millis(); pollListeners(); }
        String show = site.length() ? site : raw;
        if (show.length() > 1000) show = show.substring(0, 1000);
        static String lastShow;
        if (show != lastShow) { lastShow = show; Serial.println("RAW field (" + String(site.length() ? "mer.txt" : "ICY") + "): '" + show + "'");
          xSemaphoreTake(mtx, portMAX_DELAY); sharedRaw = show; songDirty = true; xSemaphoreGive(mtx); }
      }
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ================= display =================
// Brand: app theme red #F44336 (res/values/colors.xml), dark purple from res/drawable/bg_image.webp

void flushCb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *c) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  tft.startWrite(); tft.setAddrWindow(a->x1, a->y1, w, h);
  tft.pushColors((uint16_t *)&c->full, w * h, true); tft.endWrite();
  lv_disp_flush_ready(d);
}
// set text/color only on change -> no needless redraws
void layoutAll();
void setTextIf(lv_obj_t *l, const char *t) { const char *cur = lv_label_get_text(l); if (!cur || strcmp(cur, t)) lv_label_set_text(l, t); }
void setColorIf(lv_obj_t *o, uint32_t c) { if (lv_obj_get_style_text_color(o, 0).full != lv_color_hex(c).full) lv_obj_set_style_text_color(o, lv_color_hex(c), 0); }
void setBgIf(lv_obj_t *o, uint32_t c) { if (lv_obj_get_style_bg_color(o, 0).full != lv_color_hex(c).full) lv_obj_set_style_bg_color(o, lv_color_hex(c), 0); }

// label: wrapped (no ellipsis); size/font re-fitted by layoutAll()
lv_obj_t *mkLabel(lv_obj_t *p, const lv_font_t *f, uint32_t col, int x, int y, int w = 0, int h = 0) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_pos(l, x, y);
  if (w && h) { lv_obj_set_size(l, w, h); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); }
  else if (w) { lv_obj_set_width(l, w); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); }
  lv_label_set_text(l, ""); return l;
}
lv_obj_t *mkBadge(lv_obj_t *p, const char *t, int x) {
  lv_obj_t *b = lv_label_create(p);
  lv_obj_set_style_text_font(b, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(b, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(b, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_radius(b, 7, 0); lv_obj_set_style_pad_hor(b, 5, 0); lv_obj_set_style_pad_ver(b, 1, 0);
  lv_obj_set_pos(b, x, 5); lv_label_set_text(b, t); return b;
}

// Boot colour test: LVGL-native rects + raw RGB565 buffer + decoded JPEG, side by side
static uint16_t *testPix;
void showColourTest() {
  lv_obj_t *t = lv_obj_create(lv_layer_top()); lv_obj_remove_style_all(t);
  lv_obj_set_size(t, W, H); lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(t, lv_color_hex(0x000000), 0);
  const uint32_t cols[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
  const char *names[4] = {"КРАСНЫЙ", "ЗЕЛЁНЫЙ", "СИНИЙ", "БЕЛЫЙ"};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *r = lv_obj_create(t); lv_obj_remove_style_all(r); lv_obj_set_size(r, 70, 50); lv_obj_set_pos(r, 8 + i * 78, 8);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(r, lv_color_hex(cols[i]), 0);
    lv_obj_t *l = lv_label_create(t); lv_obj_set_style_text_font(l, &font_cyr_14, 0); lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(l, names[i]); lv_obj_set_pos(l, 8 + i * 78, 60);
  }
  lv_obj_t *cap = lv_label_create(t); lv_obj_set_style_text_font(cap, &font_cyr_14, 0); lv_obj_set_style_text_color(cap, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(cap, 150, 90); lv_obj_set_width(cap, 165); lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
  lv_label_set_text(cap, "Тест цвета. Слева JPEG: красный, зелёный / синий, белый. Фон должен быть ЧЁРНЫМ.");
  // decoded JPEG (same path as covers)
  static lv_img_dsc_t d; d.header.always_zero = 0; d.header.w = COVER; d.header.h = COVER; d.header.cf = LV_IMG_CF_TRUE_COLOR;
  d.data_size = COVER * COVER * 2; d.data = (const uint8_t *)testPix;
  lv_obj_t *im = lv_img_create(t); lv_img_set_src(im, &d); lv_obj_set_pos(im, 20, 100);
  lv_timer_handler(); uint32_t t0 = millis(); while (millis() - t0 < BOOT_TEST_MS) { lv_timer_handler(); delay(10); }
  lv_obj_del(t); free(testPix); testPix = nullptr;
}

lv_obj_t *mkCard(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(p); lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, x, y); lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(c, lv_color_hex(0x1C1620), 0);
  lv_obj_set_style_border_width(c, 1, 0); lv_obj_set_style_border_color(c, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_border_opa(c, LV_OPA_70, 0); lv_obj_set_style_radius(c, 10, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}
lv_obj_t *mkCaption(lv_obj_t *p, const lv_font_t *f, const char *t, int x, int y) {
  lv_obj_t *l = mkLabel(p, f, C_ACCENT, x, y); lv_label_set_text(l, t); lv_obj_set_style_text_letter_space(l, 1, 0); return l;
}

lv_obj_t *mkLine(lv_obj_t *p, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(p); lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(o, lv_color_hex(C_DIM), 0);
  return o;
}
lv_obj_t *mkCap(lv_obj_t *p, const char *t, int x, int y, int w) {
  lv_obj_t *l = mkLabel(p, &font_cyr_10, C_MUTED, x, y, w, 13); lv_label_set_text(l, t);
  lv_obj_set_style_text_letter_space(l, 2, 0); return l;
}

// Minimal layout, 320x240 pixel budget (side margin 10):
//  status    y 0..19, divider y 20
//  now play  y 28..125 : cover 90x90 @ (10,30), text column x 110..309
//  divider   y 132
//  bottom    y 140..235: ICECAST x 10..151 | vline x 160 | RDS x 170..309
void buildUi(lv_obj_t *s) {
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG_TOP), 0);
  lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_NONE, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);

  // status line: frequency, station, icons (Montserrat carries the LVGL symbols), RSSI bar
  lv_obj_t *fq = mkLabel(s, &font_cyr_b14, C_TEXT, 10, 1, 80, 18);
  lv_label_set_text_fmt(fq, "%u.%u FM", FM_FREQ / 100, (FM_FREQ % 100) / 10);
  lblStation = mkCap(s, "", 88, 4, 124); lv_obj_set_style_text_letter_space(lblStation, 1, 0);   // fault text only
  lv_obj_add_flag(lblStation, LV_OBJ_FLAG_HIDDEN);
  // internet listeners: headphones + total, tiny per-source breakdown (Mari El Icecast + METR relay)
  // headphones glyph drawn from primitives (LVGL 8 symbols have none): arc band + two cups, 13x12 at (92,4)
  icoLis = lv_obj_create(s); lv_obj_remove_style_all(icoLis); lv_obj_set_size(icoLis, 13, 12); lv_obj_set_pos(icoLis, 92, 4);
  lv_obj_clear_flag(icoLis, LV_OBJ_FLAG_SCROLLABLE);
  { lv_obj_t *band = lv_arc_create(icoLis); lv_obj_remove_style_all(band); lv_obj_set_size(band, 13, 13); lv_obj_set_pos(band, 0, 0);
    lv_arc_set_bg_angles(band, 180, 360); lv_arc_set_angles(band, 180, 360); lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(band, 2, LV_PART_INDICATOR); lv_obj_set_style_arc_color(band, lv_color_hex(C_MUTED), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(band, true, LV_PART_INDICATOR);
    for (int i = 0; i < 2; i++) { lv_obj_t *c = mkLine(icoLis, i ? 9 : 0, 6, 4, 6); lv_obj_set_style_bg_color(c, lv_color_hex(C_MUTED), 0); lv_obj_set_style_radius(c, 1, 0); } }
  lblLis = mkLabel(s, &font_cyr_b14, C_TEXT, 110, 1, 36, 18); lv_label_set_text(lblLis, "–");
  lblLisSub = mkLabel(s, &font_cyr_10, C_MUTED, 146, 4, 66, 14); lv_label_set_text(lblLisSub, ""); lv_obj_add_flag(lblLisSub, LV_OBJ_FLAG_HIDDEN);
  badgeSt  = mkLabel(s, &lv_font_montserrat_12, C_MUTED, 216, 3, 16, 14); lv_label_set_text(badgeSt, LV_SYMBOL_VOLUME_MAX);   // stereo
  badgeRds = mkLabel(s, &lv_font_montserrat_12, C_MUTED, 234, 3, 14, 14); lv_label_set_text(badgeRds, LV_SYMBOL_LIST);        // RDS data
  lblWifi  = mkLabel(s, &lv_font_montserrat_12, C_MUTED, 250, 3, 16, 14); lv_label_set_text(lblWifi, LV_SYMBOL_WIFI);
  barSig = lv_bar_create(s); lv_obj_remove_style_all(barSig);
  lv_obj_set_size(barSig, 40, 4); lv_obj_set_pos(barSig, 270, 8);
  lv_obj_set_style_bg_opa(barSig, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(barSig, lv_color_hex(C_DIM), 0); lv_obj_set_style_radius(barSig, 2, 0);
  lv_obj_set_style_bg_opa(barSig, LV_OPA_COVER, LV_PART_INDICATOR); lv_obj_set_style_bg_color(barSig, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
  lv_obj_set_style_radius(barSig, 2, LV_PART_INDICATOR);
  lv_bar_set_range(barSig, 0, 63);
  mkLine(s, 10, 20, 300, 1);

  // now playing
  coverDsc.header.always_zero = 0; coverDsc.header.w = COVER; coverDsc.header.h = COVER;
  coverDsc.header.cf = LV_IMG_CF_TRUE_COLOR; coverDsc.data_size = COVER * COVER * 2; coverDsc.data = (const uint8_t *)coverPix;
  imgCover = lv_img_create(s); lv_img_set_src(imgCover, &img_logo_big); lv_obj_set_pos(imgCover, 10, 32);
  lv_obj_set_style_radius(imgCover, 10, 0); lv_obj_set_style_clip_corner(imgCover, true, 0);
  lblNow = mkCap(s, "СЕЙЧАС В ЭФИРЕ", 112, 30, 198);
  lv_obj_t *note = mkLabel(s, &lv_font_montserrat_14, C_ACCENT, 112, 49, 16, 18); lv_label_set_text(note, LV_SYMBOL_AUDIO);
  lblArtist = mkLabel(s, &font_cyr_b18, C_TEXT, 130, 46, 180, 42); lv_label_set_text(lblArtist, "Марий Эл Радио");  // 2 x 21
  lblTitle = mkLabel(s, &font_cyr_14, C_MUTED, 112, 91, 198, 36);                                              // 2 lines
  lineMid = mkLine(s, 10, 131, 300, 1);

  // ICECAST (raw, untouched) | RDS
  capRaw = mkCap(s, "ПОЛНОЕ НАЗВАНИЕ", 10, 137, 300);
  lblRaw = mkLabel(s, &font_cyr_12, C_TEXT, 10, 152, 300, 48);
  lineRds = mkLine(s, 10, 202, 300, 1);
  capRds = mkCap(s, "RDS", 10, 210, 30);
  lblPS = mkLabel(s, &font_cyr_b14, C_TEXT, 170, 155, 140, 18); lv_label_set_text(lblPS, "---");
  lblRT = mkLabel(s, &font_cyr_12, C_MUTED, 170, 175, 140, 32);    // 2 lines x 15 px
  layoutAll();
}


// ---- auto-fit: largest font whose wrapped text fits; sections share height by content ----
static int txtH(const char *t, const lv_font_t *f, int w) {
  lv_point_t sz; lv_txt_get_size(&sz, t, f, 0, 0, w, LV_TEXT_FLAG_NONE); return sz.y;
}
static void placeL(lv_obj_t *l, const lv_font_t *f, int x, int y, int w, int h) {
  if (lv_obj_get_style_text_font(l, 0) != f) lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_pos(l, x, y); lv_obj_set_size(l, w, h);
}
// stack two labels in [y0, y0+H): top gets its natural height, bottom the rest
static void fitPair(lv_obj_t *a, const lv_font_t **fa, int na, int xa, int wa,
                    lv_obj_t *b, const lv_font_t **fb, int nb, int xb, int wb, int y0, int H, int gap) {
  const char *ta = lv_label_get_text(a), *tb = lv_label_get_text(b);
  for (int sum = 0; sum <= na + nb - 2; sum++)
    for (int i = 0; i < na; i++) { int j = sum - i; if (j < 0 || j >= nb) continue;
      int ha = *ta ? txtH(ta, fa[i], wa) : 0, hb = *tb ? txtH(tb, fb[j], wb) : 0;
      if (ha + hb + gap <= H) { placeL(a, fa[i], xa, y0, wa, ha ? ha : 1); placeL(b, fb[j], xb, y0 + ha + gap, wb, H - ha - gap); return; }
    }
  int ha = txtH(ta, fa[na-1], wa); if (ha > H - 14) ha = H - 14;
  placeL(a, fa[na-1], xa, y0, wa, ha); placeL(b, fb[nb-1], xb, y0 + ha + gap, wb, H - ha - gap);
}
static void fitOne(lv_obj_t *l, const lv_font_t **f, int n, int x, int y, int w, int h) {
  const char *t = lv_label_get_text(l);
  for (int i = 0; i < n; i++) if (txtH(t, f[i], w) <= h || i == n - 1) { placeL(l, f[i], x, y, w, h); return; }
}
void layoutAll() {
  static const lv_font_t *FB[] = { &font_cyr_b18, &font_cyr_b14, &font_cyr_12, &font_cyr_10 };
  static const lv_font_t *FR[] = { &font_cyr_14, &font_cyr_12, &font_cyr_10 };
  // --- bottom block measured bottom-up: RDS row (PS | RT up to 4 lines), then full title above it ---
  const char *rtT = lv_label_get_text(lblRT), *psT = lv_label_get_text(lblPS), *rawT = lv_label_get_text(lblRaw);
  int rk = -1, rtH = 0;
  for (int i = 0; i < 3 && rk < 0; i++) { int h = txtH(rtT, FR[i], 164); if (h <= 32) { rk = i; rtH = h; } }        // side-by-side if RT <= 2 lines
  bool stack = rk < 0;                       // long RT: "RDS" caption / PS line / RT full width below
  int pk = 3, psH = 0, rdsH;
  if (stack) {
    pk = 1; psH = txtH(psT, FB[pk], 300);
    rk = 0; rtH = txtH(rtT, FR[0], 300); rdsH = 15 + psH + 1 + rtH;   // final RT font chosen below with the raw block
  } else {
    for (int i = 0; i < 4; i++) if (txtH(psT, FB[i], 100) <= max(rtH, 21)) { pk = i; break; }
    rdsH = max(max(rtH, txtH(psT, FB[pk], 100)), 18);
  }
  const int BOT = 238, TOP0 = 137, TOPMIN = 126;          // borrow up to 11 px from now-playing (cover ends at y122)
  bool cap = true, rdsOn = true; int wk = 2, rawH = 0, top = TOP0;
  auto need = [&](int rh, bool c, bool r) { return (c ? 15 : 0) + rh + (r ? 7 + rdsH : 0); };
  bool ok = false;
  for (int mode = 0; mode < 3 && !ok; mode++) {           // 0: all; 1: drop full-title caption; 2: hide RDS row
    cap = mode == 0; rdsOn = mode < 2;
    for (int r = 0; r < (stack && rdsOn ? 3 : 1) && !ok; r++) {     // stacked RT: biggest font that lets everything fit (<= 4 lines)
      if (stack) { rk = r; rtH = txtH(rtT, FR[r], 300); if (rtH > 4 * lv_font_get_line_height(FR[r]) && r < 2) continue; rdsH = 15 + psH + 1 + rtH; }
      for (int i = 0; i < 3 && !ok; i++) { int h = txtH(rawT, FR[i], 300); if (need(h, cap, rdsOn) <= BOT - TOPMIN) { ok = true; wk = i; rawH = h; } }
    }
  }
  if (!ok) { wk = 2; rawH = BOT - TOPMIN - 15; cap = true; rdsOn = false; }
  top = min(TOP0, BOT - need(rawH, cap, rdsOn)); if (top < TOPMIN) top = TOPMIN;
  // --- now-playing gets everything above the borrowed line ---
  lv_obj_set_y(lineMid, top - 5);
  fitPair(lblArtist, FB, 4, 130, 180, lblTitle, FR, 3, 112, 198, 46, top - 5 - 46 - 1, 3);
  int y = top;
  if (cap) { lv_obj_clear_flag(capRaw, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(capRaw, y); y += 15; } else lv_obj_add_flag(capRaw, LV_OBJ_FLAG_HIDDEN);
  placeL(lblRaw, FR[wk], 10, y, 300, rdsOn ? rawH : BOT - y); y += rawH;
  lv_obj_t *rdsObjs[] = { lineRds, capRds, lblPS, lblRT };
  for (auto o : rdsObjs) { if (rdsOn) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
  if (rdsOn) {
    int ry = BOT - rdsH; lv_obj_set_y(lineRds, ry - 4);
    if (stack) {
      lv_obj_set_y(capRds, ry + 1);
      placeL(lblPS, FB[pk], 10, ry + 15, 300, psH);
      placeL(lblRT, FR[rk], 10, ry + 15 + psH + 1, 300, rtH);
    } else {
      lv_obj_set_y(capRds, ry + 3);
      placeL(lblPS, FB[pk], 40, ry, 100, rdsH);
      placeL(lblRT, FR[rk], 146, ry, 164, rdsH);
    }
  }
  Serial.printf("LAYOUT top=%d raw:f%d h%d cap%d rds:%d stack%d RT f%d h%d PS f%d\n", top, wk, rawH, cap, rdsOn, stack, rk, rtH, pk);
}

void showRds() { setTextIf(lblPS, *ps ? ps : "---"); setTextIf(lblRT, rt); layoutAll(); }

static void cleanAscii(char *dst, const char *src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i]; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : ' ';
  dst[i] = 0;
  while (i > 0 && dst[i - 1] == ' ') dst[--i] = 0;
}

// RDS: apply PS / RadioText only after they stayed unchanged for a while (complete, not partial)
void pollRadio() {
  static uint32_t tR = 0, tS = 0, tLog = 0, psSince = 0, rtSince = 0;
  static char psCand[9] = "", rtCand[65] = "";
  uint32_t now = millis();
  if (now - tR >= 200) {
    tR = now;
    if (rx.getRdsReady()) {
      char *p = rx.getRdsStationName();
      if (p && *p) { char b[9]; cleanAscii(b, p, 8); if (strcmp(b, psCand)) { strcpy(psCand, b); psSince = now; } }
      char *t = rx.getRdsProgramInformation();
      if (t && *t) { char b[65]; cleanAscii(b, t, 64); if (strcmp(b, rtCand)) { strcpy(rtCand, b); rtSince = now; } }
    }
    if (!demoMode && *psCand && now - psSince > 1500 && strcmp(psCand, ps)) { strcpy(ps, psCand); showRds(); Serial.printf("PS: '%s'\n", ps); }
    if (!demoMode && *rtCand && now - rtSince > 4000 && strcmp(rtCand, rt)) { strcpy(rt, rtCand); showRds(); Serial.printf("RT: '%s'\n", rt); }
  }
  if (now - tS >= 1000) {
    tS = now;
    int r = rx.getRssi(); bool st = rx.isStereo(); bool rds = rx.getRdsReady() || *ps;
    if (r < 15) { if (!lowRssiSince) lowRssiSince = now | 1; } else lowRssiSince = 0;
    if (!rx.getRdsReady()) { if (!noRdsSince) noRdsSince = now | 1; } else noRdsSince = 0;
    int lvl = r >= 50 ? 4 : r >= 38 ? 3 : r >= 26 ? 2 : r >= 14 ? 1 : 0;
    static float ema = -1; ema = ema < 0 ? r : ema * 0.7f + r * 0.3f;   // smoothed RSSI 0..63
    static int shown = -1; int v = (int)(ema + 0.5f);
    if (v != shown) { shown = v; lv_bar_set_value(barSig, v, LV_ANIM_OFF); }
    (void)lvl;
    setColorIf(badgeSt, st ? C_ACCENT : C_MUTED);
    setColorIf(badgeRds, rds ? C_ACCENT : C_MUTED);
    if (now - tLog > 60000) { tLog = now; Serial.printf("RSSI %d %s RDS:%d heap %u up %lus\n", r, st ? "ST" : "MONO", rds, ESP.getFreeHeap(), now / 1000); }
  }
}

void pollNet() {
  static int lastWifi = -1; static bool lastNet = false;
  if (netOk != lastNet) { lastNet = netOk; setColorIf(lblWifi, (wifiState == 2 && netOk) ? C_MUTED : C_ACCENT); setTextIf(lblWifi, netOk ? LV_SYMBOL_WIFI : LV_SYMBOL_WARNING); }
  if (wifiState != lastWifi) {
    lastWifi = wifiState;
    setColorIf(lblWifi, (wifiState == 2 && netOk) ? C_MUTED : C_ACCENT);
    if (wifiState == 3) { setTextIf(lblNow, "НАСТРОЙКА WI-FI"); setTextIf(lblArtist, "MariRadio-Setup"); setTextIf(lblTitle, "Подключитесь к этой сети и откройте 192.168.4.1"); layoutAll(); }
    else if (lastWifi == 2) { setTextIf(lblNow, "СЕЙЧАС В ЭФИРЕ"); songDirty = true; }
  }
  if (songDirty || coverDirty) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (lisDirty) { lisDirty = false; int a = lisMari, b = lisMetr; char t[12], u[24];
      if (a < 0 && b < 0) strcpy(t, "–"); else snprintf(t, sizeof t, "%d", max(a, 0) + max(b, 0));
      snprintf(u, sizeof u, "%s+%s", a < 0 ? "?" : String(a).c_str(), b < 0 ? "?" : String(b).c_str());
      setTextIf(lblLis, t); }
    if (songDirty && wifiState != 3) { if (sharedArtist.length()) { setTextIf(lblArtist, sharedArtist.c_str()); setTextIf(lblTitle, sharedTitle.c_str()); setTextIf(lblRaw, sharedRaw.c_str()); layoutAll(); } songDirty = false; }
    if (coverDirty) {
      if (coverNone) lv_img_set_src(imgCover, &img_logo_big);
      else { lv_img_cache_invalidate_src(&coverDsc); lv_img_set_src(imgCover, &coverDsc); lv_obj_invalidate(imgCover); }
      coverDirty = false;
    }
    xSemaphoreGive(mtx);
  }
}

// Serial 'S' -> dump the screen (readback from ILI9341) as hex lines for a real preview PNG
TaskHandle_t netTaskH = nullptr;
void dumpScreen() {
  static uint16_t line[W];
  bool susp = netTaskH && eTaskGetState(netTaskH) != eDeleted;   // keep other tasks' logs out of the dump
  bool wd = susp && esp_task_wdt_status(netTaskH) == ESP_OK;
  if (susp) { if (wd) esp_task_wdt_delete(netTaskH); vTaskSuspend(netTaskH); }
  Serial.flush(); Serial.println("SCREEN-BEGIN 320 240");
  for (int y = 0; y < H; y++) {
    tft.readRect(0, y, W, 1, line);
    esp_task_wdt_reset(); Serial.printf("%03d:", y);
    for (int x = 0; x < W; x++) Serial.printf("%04X", line[x]);
    Serial.println();
  }
  Serial.println("SCREEN-END");
  if (susp) { vTaskResume(netTaskH); if (wd) esp_task_wdt_add(netTaskH); }
}

// ---- boot splash (~3.5 s): logo zoom/fade, titles, accent line, progress of boot steps ----
static lv_obj_t *spBar, *spStep;
static void animZoom(void *o, int32_t v) { lv_img_set_zoom((lv_obj_t *)o, v); }
static void animOpa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); }
static void animW(void *o, int32_t v) { lv_obj_set_width((lv_obj_t *)o, v); lv_obj_set_x((lv_obj_t *)o, 160 - v / 2); }
static void runAnim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t a, int32_t b, uint32_t ms, uint32_t delay, lv_anim_path_cb_t path = lv_anim_path_ease_out) {
  lv_anim_t an; lv_anim_init(&an); lv_anim_set_var(&an, o); lv_anim_set_exec_cb(&an, cb); lv_anim_set_values(&an, a, b);
  lv_anim_set_time(&an, ms); lv_anim_set_delay(&an, delay); lv_anim_set_path_cb(&an, path); lv_anim_start(&an);
}
static lv_obj_t *spText(lv_obj_t *s, const lv_font_t *f, uint32_t col, int y, const char *t, uint32_t delay) {
  lv_obj_t *l = lv_label_create(s); lv_obj_set_style_text_font(l, f, 0); lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_width(l, 300); lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_pos(l, 10, y); lv_label_set_text(l, t);
  lv_obj_set_style_opa(l, 0, 0); runAnim(l, animOpa, 0, 255, 450, delay); return l;
}
lv_obj_t *buildSplash(lv_obj_t *s) {
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG_TOP), 0); lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0); lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *logo = lv_img_create(s); lv_img_set_src(logo, &img_logo_big); lv_obj_set_pos(logo, 115, 18);
  lv_obj_set_style_radius(logo, 14, 0); lv_obj_set_style_clip_corner(logo, true, 0);
  lv_img_set_zoom(logo, 160); lv_obj_set_style_opa(logo, 0, 0);
  runAnim(logo, animZoom, 160, 256, 700, 0, lv_anim_path_overshoot); runAnim(logo, animOpa, 0, 255, 500, 0);
  spText(s, &font_cyr_b18, C_TEXT, 120, "Марий Эл Радио", 350);
  spText(s, &font_cyr_12, C_MUTED, 143, "FM · RDS · онлайн-монитор", 550);
  lv_obj_t *ln = lv_obj_create(s); lv_obj_remove_style_all(ln); lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(ln, lv_color_hex(C_ACCENT), 0); lv_obj_set_style_radius(ln, 1, 0); lv_obj_set_size(ln, 0, 2); lv_obj_set_pos(ln, 160, 166);
  runAnim(ln, animW, 0, 140, 700, 800);
  spText(s, &font_cyr_14, C_TEXT, 174, "Разработчик: Михаил Пирогов", 1000);
  spText(s, &font_cyr_10, C_ACCENT, 194, "проект PirogovLab", 1250);
  spStep = spText(s, &font_cyr_10, C_MUTED, 212, "", 1400);
  spBar = lv_bar_create(s); lv_obj_remove_style_all(spBar); lv_obj_set_size(spBar, 160, 3); lv_obj_set_pos(spBar, 80, 230);
  lv_obj_set_style_bg_opa(spBar, LV_OPA_COVER, 0); lv_obj_set_style_bg_color(spBar, lv_color_hex(C_DIM), 0); lv_obj_set_style_radius(spBar, 1, 0);
  lv_obj_set_style_bg_opa(spBar, LV_OPA_COVER, LV_PART_INDICATOR); lv_obj_set_style_bg_color(spBar, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
  lv_obj_set_style_radius(spBar, 1, LV_PART_INDICATOR); lv_bar_set_range(spBar, 0, 100); lv_bar_set_value(spBar, 5, LV_ANIM_OFF);
  return s;
}
// 'Pnn': replay the splash, dump the frame at nn*100 ms, then return to the main screen
void replaySplash(uint32_t atMs) {
  lv_obj_t *old = lv_scr_act(); lv_obj_t *sp = lv_obj_create(NULL);
  buildSplash(sp); lv_scr_load(sp);
  uint32_t t0 = millis();
  while (millis() - t0 < atMs) {
    uint32_t el = millis() - t0;
    lv_bar_set_value(spBar, min<uint32_t>(10 + el / 30, 100), LV_ANIM_OFF);
    setTextIf(spStep, el < 1500 ? "Тюнер 105.5 FM…" : (el < 2600 ? "Подключение к Wi-Fi…" : "Готово"));
    lv_timer_handler(); delay(5); esp_task_wdt_reset();
  }
  lv_refr_now(NULL);
  Serial.printf("SPLASH FRAME %u ms\n", (unsigned)atMs); dumpScreen();
  lv_scr_load(old); lv_obj_del(sp); lv_obj_invalidate(old);
}
static volatile bool tunerDone = false;
static void tunerInitTask(void *) {        // runs in parallel with the splash and Wi-Fi
  if (tunerOk) {
    rx.setup(); Wire.setClock(50000); rx.setVolume(FM_VOLUME);
    rx.setRDS(true); rx.setRdsFifo(true);
    tuneVerified("boot");
    Serial.printf("Tuned %u.%02u MHz, volume %d\n", rx.getRealFrequency() / 100, rx.getRealFrequency() % 100, FM_VOLUME);
  }
  tunerDone = true; vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200); delay(300);
  Serial.printf("\nMER radio CYD start, reset reason %d\n", (int)esp_reset_reason());
  ledWrite(0, 0, 0);
  mtx = xSemaphoreCreateMutex();
  coverPix = (uint16_t *)calloc(COVER * COVER, 2);
  Wire.begin(I2C_SDA, I2C_SCL); Wire.setClock(50000);   // 50 kHz: more margin on noisy wiring
  for (uint8_t a = 0x10; a <= 0x11; a++) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) tunerOk = true; }
  Serial.println(tunerOk ? "RDA5807 OK" : "RDA5807 NOT FOUND");

  tft.begin(); tft.setRotation(SCREEN_ROTATION); if (PANEL_INVERT) tft.invertDisplay(true); tft.fillScreen(TFT_BLACK);
  if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");
  TJpgDec.setSwapBytes(false); TJpgDec.setCallback(jpgOut);

  // colour self-test: decode embedded R/G/B/W JPEG and print quadrant pixels (expect F800 07E0 001F FFFF)
  { decBuf = (uint16_t *)malloc(COVER * COVER * 2); offX = offY = 0; TJpgDec.setJpgScale(1);
    JRESULT jr = TJpgDec.drawJpg(0, 0, test_jpg, sizeof(test_jpg));
    Serial.printf("JPEG self-test %d: TL %04X TR %04X BL %04X BR %04X (expect F800 07E0 001F FFFF)\n", jr,
      decBuf[22 * COVER + 22], decBuf[22 * COVER + 67], decBuf[67 * COVER + 22], decBuf[67 * COVER + 67]);
    Serial.printf("LVGL lv_color_hex: red %04X green %04X blue %04X (LV_COLOR_DEPTH %d, 16_SWAP %d, PANEL_INVERT %d)\n",
      lv_color_hex(0xFF0000).full, lv_color_hex(0x00FF00).full, lv_color_hex(0x0000FF).full, LV_COLOR_DEPTH, LV_COLOR_16_SWAP, PANEL_INVERT);
    testPix = decBuf; }
  lv_init();
  lv_disp_draw_buf_init(&drawBuf, buf1, NULL, W * 16);
  static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
  dd.hor_res = W; dd.ver_res = H; dd.flush_cb = flushCb; dd.draw_buf = &drawBuf; dd.full_refresh = 0;
  lv_disp_drv_register(&dd);
  if (BOOT_TEST_MS) showColourTest(); else if (testPix) { free(testPix); testPix = nullptr; }
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);   // backlight full brightness (no PWM dimming)
  buildSplash(lv_scr_act());
  WiFi.setSleep(WIFI_SLEEP);                 // modem-sleep causes current pulses -> backlight flicker
  // task watchdog 30 s for loop + net task (TLS handshakes are bounded to 8 s)
  esp_task_wdt_config_t wcfg = { .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true };
  if (esp_task_wdt_reconfigure(&wcfg) != ESP_OK) esp_task_wdt_init(&wcfg);
  wifiInitNets();
  xTaskCreatePinnedToCore(tunerInitTask, "tune", 4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, &netTaskH, 0);
  { uint32_t t0 = millis(); bool dumped = false;
    while (true) {
      uint32_t el = millis() - t0; bool wOk = WiFi.status() == WL_CONNECTED;
      int pct = 10 + (tunerDone ? 40 : 0) + (wOk ? 40 : 0) + min<uint32_t>(el / 350, 10);
      lv_bar_set_value(spBar, pct, LV_ANIM_ON);
      const char *st = !tunerDone ? "Тюнер 105.5 FM…" : (wOk ? "Готово" : (wifiState == 3 ? "Wi-Fi: портал MariRadio-Setup" : "Подключение к Wi-Fi…"));
      setTextIf(spStep, st);
      lv_timer_handler(); delay(5);
      if (!dumped && el > 2600 && Serial.available() && Serial.peek() == 'S') { Serial.read(); dumped = true; Serial.println("SPLASH DUMP"); dumpScreen(); t0 = millis() - 2600; }
      if (el > 3500 && tunerDone && (wOk || el > 5000)) break;
      if (el > 9000) break;
    }
  }
  lv_obj_t *mainScr = lv_obj_create(NULL);
  buildUi(mainScr);
  if (!tunerOk) { lv_label_set_text(lblPS, "Tuner not found"); layoutAll(); }
  lv_scr_load_anim(mainScr, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, true);
  esp_task_wdt_add(NULL);
}

// explicit band/space, then setFrequency + read-back verification (up to 5 tries)
bool tuneVerified(const char *why) {
  for (int i = 1; i <= 5; i++) {
    rx.setBand(RDA_FM_BAND_USA_EU);   // 87.5-108 MHz
    rx.setSpace(0);                   // 100 kHz
    rx.setFrequency(FM_FREQ);
    delay(60);
    uint16_t f = rx.getRealFrequency();
    Serial.printf("TUNE[%s] try %d: set %u -> read %u.%02u MHz\n", why, i, FM_FREQ, f / 100, f % 100);
    if (f == FM_FREQ) return true;
    delay(50);
  }
  Serial.println("TUNE: FAILED to verify 105.5 after 5 tries");
  return false;
}
void checkFreq() {
  static uint32_t t = 0;
  if (millis() - t < 10000) return; t = millis();
  uint16_t f = rx.getRealFrequency();
  if (f != FM_FREQ) { Serial.printf("FREQ drift: read %u -> retune\n", f); tuneVerified("drift"); }
}
void bootRadioLog() {
  static uint32_t t = 0;
  if (millis() > 120000 || millis() - t < 5000) return; t = millis();
  uint16_t f = rx.getRealFrequency();
  Serial.printf("RADIO t=%lus f=%u.%02u RSSI %d %s RDS:%d PS='%s'\n", millis() / 1000, f / 100, f % 100, rx.getRssi(), rx.isStereo() ? "ST" : "MONO", rx.getRdsReady(), ps);
}

// returns current fault reason ("" = healthy)
const char *faultReason() {
  uint32_t now = millis();
  if (simFault) return "TEST FAULT";
  if (!tunerOk || tunerBad) return "TUNER I2C";
  if (wifiDownSince && now - wifiDownSince > 30000) return "WI-FI DOWN";
  if (netFailStreak >= 3) return "ICECAST FAIL";
  if (lowRssiSince && now - lowRssiSince > 60000) return "LOW SIGNAL";
  if (noRdsSince && now - noRdsSince > 120000) return "NO RDS";
  return "";
}
#define LED_OK_GREEN 40   // 0..255, moderate solid green when healthy
// PWM via LEDC (analogWrite), active LOW -> duty inverted
void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
  static int lr = -1, lg = -1, lb = -1;
  if (r != lr) { analogWrite(LED_R, 255 - r); lr = r; }
  if (g != lg) { analogWrite(LED_G, 255 - g); lg = g; }
  if (b != lb) { analogWrite(LED_B, 255 - b); lb = b; }
}
// police pattern: red double flash, blue double flash (1 s cycle), non-blocking
void updateFault() {
  static const char *last = nullptr; static uint32_t tChk = 0;
  uint32_t now = millis();
  const char *f = faultReason();
  if (f != last) {
    last = f;
    lv_obj_t *lis[] = { icoLis, lblLis };
    if (*f) { Serial.printf("FAULT: %s\n", f); setTextIf(lblStation, f); setColorIf(lblStation, C_ACCENT);
              lv_obj_clear_flag(lblStation, LV_OBJ_FLAG_HIDDEN); for (auto o : lis) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
    else { Serial.println("FAULT cleared"); lv_obj_add_flag(lblStation, LV_OBJ_FLAG_HIDDEN); for (auto o : lis) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); }
  }
  if (!*f) { ledWrite(0, LED_OK_GREEN, 0); return; }
  uint32_t ph = now % 1000;
  bool r = (ph < 70) || (ph >= 140 && ph < 210);
  bool b = (ph >= 500 && ph < 570) || (ph >= 640 && ph < 710);
  ledWrite(r ? 255 : 0, 0, b ? 255 : 0);
}

// tuner watchdog: re-init RDA5807 if it stops answering on I2C
void checkTuner() {
  static uint32_t t = 0; static int bad = 0;
  if (millis() - t < 5000) return; t = millis();
  Wire.beginTransmission(0x11);
  if (Wire.endTransmission() == 0) { bad = 0; tunerBad = false; return; }
  tunerBad = true;
  if (++bad < 3) return;
  bad = 0; statI2cRecover++;
  Serial.println("I2C: RDA5807 not answering -> bus reset + re-init");
  Wire.end(); delay(10); Wire.begin(I2C_SDA, I2C_SCL); Wire.setClock(50000);
  rx.setup(); Wire.setClock(50000); rx.setVolume(FM_VOLUME); rx.setRDS(true); rx.setRdsFifo(true); tuneVerified("i2c-recover");
}
void logHealth() {
  static uint32_t t = 0;
  if (millis() - t < 60000) return; t = millis();
  Serial.printf("HEALTH up %lus heap %u min %u maxblk %u wifi %d net %d tracks %lu covers %lu fails %lu reconn %lu i2c %lu\n",
    millis() / 1000, ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(), wifiState, netOk,
    statTracks, statCovers, statFails, statReconnects, statI2cRecover);
}

void loop() {
  lv_timer_handler();
  if (tunerOk) pollRadio();
  pollNet();
  if (Serial.available()) { int c = Serial.read(); if (c == 'S') dumpScreen(); else if (c == 'W') forceDisconnect = true; else if (c == 'L') logNets(); else if (c == 'D') { uint32_t w = millis(); while (!Serial.available() && millis() - w < 500) delay(1);
      int d = Serial.read() - '0'; if (d >= 0 && d < (int)(sizeof(DEMOS) / sizeof(DEMOS[0]))) { demoReq = d;
        if (d) { demoMode = true; setTextIf(lblPS, DEMOS[d].ps); setTextIf(lblRT, DEMOS[d].rt); layoutAll(); } else { demoMode = false; showRds(); } } } else if (c == 'P') { uint32_t w = millis(); while (Serial.available() < 2 && millis() - w < 500) delay(1); int f = (Serial.read() - '0') * 10; f += Serial.read() - '0'; replaySplash(f * 100); } else if (c == 'Y') saveNet("MER-NVS-TEST", "testpass1"); else if (c == 'Z') { forgetNet("MER-NVS-TEST"); logNets(); } else if (c == 'T') { static int tn = 0; const char *tt[] = { "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", "ЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖЖ", "Марий Эл Радио - Mari El Radio 105.5 FM: Йошкар-Ола, ты лучшая!!" }; lv_label_set_text(lblRT, tt[tn % 3]); Serial.printf("TEST RT #%d len %d\n", tn % 3, (int)strlen(tt[tn % 3])); tn++; layoutAll(); } else if (c == 'F') { simFault = !simFault; Serial.printf("TEST: simulated fault %s\n", simFault ? "ON" : "OFF"); } }
  updateFault();
  checkTuner(); logHealth();
  if (tunerOk) { checkFreq(); bootRadioLog(); }
  esp_task_wdt_reset();
  delay(5);
}
