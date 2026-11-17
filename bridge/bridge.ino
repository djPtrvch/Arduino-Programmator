// ESP32 Wi-Fi мост-программатор для AVR (ISP через SPI).
// iPad открывает веб-интерфейс моста и отправляет прошивку, мост пишет её во flash AVR.
//
// Подключение (ESP32 -> ICSP-разъём AVR):
//   GPIO23 (MOSI) -> ICSP 4 (MOSI)
//   GPIO19 (MISO) <- ICSP 1 (MISO)  через делитель 1 кОм / 2 кОм (5 В -> 3.3 В)
//   GPIO18 (SCK)  -> ICSP 3 (SCK)
//   GPIO5         -> ICSP 5 (RESET)
//   GND           -> ICSP 6 (GND)
//   VIN/5V        <- ICSP 2 (VCC)   мост питается от платы, а плата — от iPad
//
// Протокол загрузки (/api/flash, multipart): последовательность записей
//   [адрес страницы, 4 байта little-endian][данные страницы, pageSize байт]
// Записи готовит веб-интерфейс из .hex-файла (пустые страницы пропускаются).

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <SPI.h>
#include "web_ui.h"

#define BRIDGE_VERSION "1.0.0"

const int PIN_MOSI = 23;
const int PIN_MISO = 19;
const int PIN_SCK = 18;
const int PIN_RESET = 5;

const char *AP_SSID = "Arduino-Programmator";
const char *AP_PASS = "12345678";
const char *MDNS_NAME = "programmer";

WebServer server(80);
Preferences prefs;

// ---------------------------------------------------------------- ISP

uint32_t ispClock = 500000;  // < F_CPU/4 цели; 500 кГц безопасно для 8/16 МГц

uint8_t spi4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  SPI.transfer(a);
  SPI.transfer(b);
  SPI.transfer(c);
  return SPI.transfer(d);
}

void ispRelease() {
  SPI.endTransaction();
  SPI.end();
  pinMode(PIN_MOSI, INPUT);
  pinMode(PIN_MISO, INPUT);
  pinMode(PIN_SCK, INPUT);
  pinMode(PIN_RESET, INPUT);  // плата сама подтянет RESET и запустит программу
}

bool ispBegin() {
  pinMode(PIN_SCK, OUTPUT);
  digitalWrite(PIN_SCK, LOW);
  digitalWrite(PIN_RESET, HIGH);
  pinMode(PIN_RESET, OUTPUT);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  SPI.beginTransaction(SPISettings(ispClock, MSBFIRST, SPI_MODE0));
  delay(20);
  for (int attempt = 0; attempt < 8; attempt++) {
    digitalWrite(PIN_RESET, HIGH);
    delayMicroseconds(100);
    digitalWrite(PIN_RESET, LOW);
    delay(25);
    SPI.transfer(0xAC);
    SPI.transfer(0x53);
    uint8_t echo = SPI.transfer(0x00);
    SPI.transfer(0x00);
    if (echo == 0x53) return true;
  }
  ispRelease();
  return false;
}

bool ispWaitReady(uint32_t timeoutMs = 50) {
  uint32_t start = millis();
  while (spi4(0xF0, 0x00, 0x00, 0x00) & 0x01) {
    if (millis() - start > timeoutMs) return false;
  }
  return true;
}

uint32_t ispSignature() {
  uint32_t sig = 0;
  for (uint8_t i = 0; i < 3; i++) sig = (sig << 8) | spi4(0x30, 0x00, i, 0x00);
  return sig;
}

bool ispChipErase() {
  spi4(0xAC, 0x80, 0x00, 0x00);
  delay(20);
  return ispWaitReady(500);
}

int lastExtAddr = -1;

void ispExtAddr(uint32_t wordAddr) {
  int ext = (wordAddr >> 16) & 0xFF;
  if (ext != lastExtAddr) {
    spi4(0x4D, 0x00, ext, 0x00);
    lastExtAddr = ext;
  }
}

bool ispWritePage(uint32_t byteAddr, const uint8_t *data, uint16_t pageSize) {
  uint32_t wordAddr = byteAddr >> 1;
  ispExtAddr(wordAddr);
  for (uint16_t i = 0; i < pageSize / 2; i++) {
    uint32_t w = wordAddr + i;
    spi4(0x40, (w >> 8) & 0xFF, w & 0xFF, data[i * 2]);
    spi4(0x48, (w >> 8) & 0xFF, w & 0xFF, data[i * 2 + 1]);
  }
  spi4(0x4C, (wordAddr >> 8) & 0xFF, wordAddr & 0xFF, 0x00);
  return ispWaitReady(50);
}

uint16_t ispVerifyPage(uint32_t byteAddr, const uint8_t *data, uint16_t pageSize) {
  uint32_t wordAddr = byteAddr >> 1;
  ispExtAddr(wordAddr);
  uint16_t errors = 0;
  for (uint16_t i = 0; i < pageSize / 2; i++) {
    uint32_t w = wordAddr + i;
    if (spi4(0x20, (w >> 8) & 0xFF, w & 0xFF, 0x00) != data[i * 2]) errors++;
    if (spi4(0x28, (w >> 8) & 0xFF, w & 0xFF, 0x00) != data[i * 2 + 1]) errors++;
  }
  return errors;
}

// ---------------------------------------------------------------- HTTP helpers

String jsonEscape(const String &s) {
  String out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') { out += "\\n"; continue; }
    out += c;
  }
  return out;
}

void sendJson(int code, const String &body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", body);
}

void sendError(int code, const String &msg) {
  sendJson(code, "{\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}");
}

String hex2(uint8_t v) {
  char b[3];
  snprintf(b, sizeof b, "%02x", v);
  return String(b);
}

// ---------------------------------------------------------------- handlers

void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", (const char *)WEB_UI_GZ, WEB_UI_GZ_LEN);
}

void handleStatus() {
  String body = "{\"ok\":true,\"bridge\":\"" BRIDGE_VERSION "\"";
  body += ",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
  body += ",\"staConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  body += ",\"staSsid\":\"" + jsonEscape(prefs.getString("ssid", "")) + "\"";
  body += ",\"staIp\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\"";
  body += "}";
  sendJson(200, body);
}

void handleChipInfo() {
  if (server.hasArg("clock")) ispClock = server.arg("clock").toInt();
  if (!ispBegin()) {
    sendError(500, "Чип не отвечает. Проверьте провода ICSP, питание и общий GND.");
    return;
  }
  uint32_t sig = ispSignature();
  uint8_t lfuse = spi4(0x50, 0x00, 0x00, 0x00);
  uint8_t hfuse = spi4(0x58, 0x08, 0x00, 0x00);
  uint8_t efuse = spi4(0x50, 0x08, 0x00, 0x00);
  uint8_t lock = spi4(0x58, 0x00, 0x00, 0x00);
  ispRelease();
  char sigStr[7];
  snprintf(sigStr, sizeof sigStr, "%06lx", (unsigned long)sig);
  sendJson(200, String("{\"ok\":true,\"signature\":\"") + sigStr + "\",\"lfuse\":\"" + hex2(lfuse) +
                    "\",\"hfuse\":\"" + hex2(hfuse) + "\",\"efuse\":\"" + hex2(efuse) +
                    "\",\"lock\":\"" + hex2(lock) + "\"}");
}

// Состояние текущей прошивки
struct FlashJob {
  bool active = false;
  String error;
  uint16_t pageSize = 128;
  uint32_t flashSize = 32768;
  uint8_t *record = nullptr;  // 4 байта адреса + страница
  size_t filled = 0;
  uint32_t pages = 0;
  uint32_t verifyErrors = 0;
  uint32_t startedAt = 0;
} job;

void jobFail(const String &msg) {
  if (job.error.isEmpty()) job.error = msg;
  if (job.active) ispRelease();
  job.active = false;
}

void processRecord() {
  const uint8_t *r = job.record;
  uint32_t addr = r[0] | (r[1] << 8) | (r[2] << 16) | ((uint32_t)r[3] << 24);
  if (addr % job.pageSize || addr + job.pageSize > job.flashSize) {
    jobFail("Адрес страницы вне памяти: 0x" + String(addr, HEX));
    return;
  }
  if (!ispWritePage(addr, r + 4, job.pageSize)) {
    jobFail("Тайм-аут записи страницы 0x" + String(addr, HEX));
    return;
  }
  job.verifyErrors += ispVerifyPage(addr, r + 4, job.pageSize);
  job.pages++;
}

void handleFlashUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    job = FlashJob();
    job.startedAt = millis();
    job.pageSize = server.hasArg("page") ? server.arg("page").toInt() : 128;
    job.flashSize = server.hasArg("flash") ? server.arg("flash").toInt() : 32768;
    if (server.hasArg("clock")) ispClock = server.arg("clock").toInt();
    if (job.pageSize < 32 || job.pageSize > 512 || job.pageSize % 2) {
      job.error = "Неверный размер страницы";
      return;
    }
    job.record = (uint8_t *)malloc(4 + job.pageSize);
    if (!job.record) {
      job.error = "Нет памяти";
      return;
    }
    lastExtAddr = -1;
    if (!ispBegin()) {
      job.error = "Чип не отвечает. Проверьте провода ICSP, питание и общий GND.";
      return;
    }
    job.active = true;
    uint32_t sig = ispSignature();
    if (server.hasArg("sig")) {
      uint32_t expected = strtoul(server.arg("sig").c_str(), nullptr, 16);
      if (sig != expected) {
        char msg[96];
        snprintf(msg, sizeof msg, "Сигнатура чипа %06lx, а выбранная плата ожидает %06lx",
                 (unsigned long)sig, (unsigned long)expected);
        jobFail(msg);
        return;
      }
    }
    if (!ispChipErase()) jobFail("Не удалось стереть чип");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!job.active) return;
    size_t recLen = 4 + job.pageSize;
    for (size_t i = 0; i < up.currentSize && job.active; i++) {
      job.record[job.filled++] = up.buf[i];
      if (job.filled == recLen) {
        processRecord();
        job.filled = 0;
      }
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (job.active) {
      if (job.filled != 0) jobFail("Обрезанные данные прошивки");
      else {
        ispRelease();
        job.active = false;
      }
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    jobFail("Загрузка прервана");
  }
}

void handleFlashDone() {
  if (job.record) {
    free(job.record);
    job.record = nullptr;
  }
  if (!job.error.isEmpty()) {
    sendError(500, job.error);
    return;
  }
  if (job.verifyErrors) {
    sendError(500, "Ошибки проверки: " + String(job.verifyErrors) + " байт не совпали");
    return;
  }
  sendJson(200, "{\"ok\":true,\"pages\":" + String(job.pages) + ",\"ms\":" +
                    String(millis() - job.startedAt) + "}");
}

// Обновление прошивки самого моста
String otaError;

void handleOtaUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaError = "";
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaError.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize)
      otaError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaError.isEmpty() && !Update.end(true)) otaError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaError = "Загрузка прервана";
  }
}

void handleOtaDone() {
  if (!otaError.isEmpty()) {
    sendError(500, otaError);
    return;
  }
  sendJson(200, "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

void handleWifi() {
  prefs.putString("ssid", server.arg("ssid"));
  prefs.putString("pass", server.arg("pass"));
  sendJson(200, "{\"ok\":true}");
  delay(500);
  ESP.restart();
}

// ---------------------------------------------------------------- setup

void setup() {
  Serial.begin(115200);
  pinMode(PIN_RESET, INPUT);
  prefs.begin("bridge", false);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  String ssid = prefs.getString("ssid", "");
  if (!ssid.isEmpty()) {
    WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  }
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/chip", HTTP_GET, handleChipInfo);
  server.on("/api/flash", HTTP_POST, handleFlashDone, handleFlashUpload);
  server.on("/api/bridge-update", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
  server.begin();

  Serial.printf("AP %s (%s), STA %s\n", AP_SSID, WiFi.softAPIP().toString().c_str(),
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-");
}

void loop() {
  server.handleClient();
}
