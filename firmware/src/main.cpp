// Gravador OBD do comboio — ESP32 classico.
// ELM327 por Bluetooth classico (SPP) -> fila em LittleFS -> Azure Blob (append blob) via Wi-Fi.
// So leitura no carro: apenas comandos AT de configuracao do proprio ELM e mode 01.
//
// Linha gravada: boot,ms,epoch_ms,tipo,nome,valor
//   boot     = numero da inicializacao (NVS), separa sessoes
//   ms       = millis() desde o boot
//   epoch_ms = hora real (0 ate o NTP sincronizar; reconstruir pelo par ms/epoch da linha meta,ntp)
#include <Arduino.h>
#include <BluetoothSerial.h>
#include <esp_bt.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <sys/time.h>
#include "secrets.h"  // AZ_ACCOUNT, AZ_SAS, DEVICE_LABEL, OBD_NAME

#define LED 2

BluetoothSerial BT;
Preferences prefs;
WiFiMulti wifiMulti;
volatile bool wifiBusy = false;
int lastNet = 0;
SemaphoreHandle_t fsMux;

uint32_t bootNo = 0;
String macTag;
volatile bool btOk = false;
volatile uint32_t nLines = 0, nSent = 0, nFail = 0;
volatile bool ntpDone = false;
String uploading;  // arquivo em envio (nao apagar)
String denySfx;    // sufixo do dongle a ignorar (DONGLE_DENY ou comando remoto)

// ------------------------------------------------------------------ fila
File cur;
uint32_t curBytes = 0, lastRotate = 0, lastFlush = 0, qSeq = 0;

uint64_t epochMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

static String baseName(File& f) {
  String n = f.name();
  if (n.startsWith("/")) n = n.substring(1);
  return n;
}

// lista arquivos q*.csv; devolve o mais antigo e o maior numero
static String scanQueue(uint32_t* maxSeq, int* count) {
  String best;
  uint32_t mx = 0;
  int c = 0;
  File root = LittleFS.open("/");
  File f;
  while ((f = root.openNextFile())) {
    String n = baseName(f);
    f.close();
    if (n.startsWith("q") && n.endsWith(".csv")) {
      c++;
      uint32_t s = n.substring(1, n.length() - 4).toInt();
      if (s > mx) mx = s;
      if (best == "" || n < best) best = n;
    }
  }
  root.close();
  if (maxSeq) *maxSeq = mx;
  if (count) *count = c;
  return best.length() ? "/" + best : "";
}

static void qOpen() {
  cur = LittleFS.open("/cur.csv", FILE_APPEND);
  curBytes = cur ? cur.size() : 0;
}

static void rotateLocked() {
  if (!curBytes) return;
  cur.close();
  char nm[24];
  snprintf(nm, sizeof nm, "/q%06u.csv", (unsigned)++qSeq);
  LittleFS.rename("/cur.csv", nm);
  qOpen();
  lastRotate = millis();
}

static void dropOldestLocked() {
  String o = scanQueue(nullptr, nullptr);
  if (o.length() && o != uploading) {
    LittleFS.remove(o);
    Serial.printf("[fila] cheia, descartei %s\n", o.c_str());
  }
}

static void writeLine(const char* line, int n);

void record(const char* type, const char* name, double v, bool hasV = true) {
  char line[112];
  uint64_t e = epochMs();
  int n = hasV ? snprintf(line, sizeof line, "%u,%lu,%llu,%s,%s,%.3f\n", (unsigned)bootNo, millis(),
                          (unsigned long long)e, type, name, v)
               : snprintf(line, sizeof line, "%u,%lu,%llu,%s,%s,\n", (unsigned)bootNo, millis(),
                          (unsigned long long)e, type, name);
  writeLine(line, n);
}

// linha de GPS: boot,ms,epoch_ms,gps,lat,lon,kmh,alt_m,sats,hdop
void recordGps(double lat, double lon, double kmh, double alt, int sats, double hdop) {
  char line[128];
  int n = snprintf(line, sizeof line, "%u,%lu,%llu,gps,%.6f,%.6f,%.1f,%.1f,%d,%.1f\n", (unsigned)bootNo, millis(),
                   (unsigned long long)epochMs(), lat, lon, kmh, alt, sats, hdop);
  writeLine(line, n);
}

static void writeLine(const char* line, int n) {
  xSemaphoreTake(fsMux, portMAX_DELAY);
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < 48000) dropOldestLocked();
  if (cur) {
    cur.write((const uint8_t*)line, n);
    curBytes += n;
  }
  if (millis() - lastFlush > 1000) {
    cur.flush();
    lastFlush = millis();
  }
  if (curBytes > 16000 || (curBytes && millis() - lastRotate > 120000)) rotateLocked();
  xSemaphoreGive(fsMux);
  nLines++;
}

// ------------------------------------------------------------------ Azure
String blobUrl() {
  return String("https://") + AZ_ACCOUNT + ".blob.core.windows.net/comboio/" + DEVICE_LABEL + "/boot" +
         bootNo + "-" + macTag + ".csv";
}

bool createBlob() {
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient h;
  h.setTimeout(15000);
  if (!h.begin(cli, blobUrl() + "?" + AZ_SAS)) return false;
  h.addHeader("x-ms-version", "2021-08-06");
  h.addHeader("x-ms-blob-type", "AppendBlob");
  h.addHeader("If-None-Match", "*");
  h.addHeader("Content-Length", "0");
  int code = h.sendRequest("PUT", (uint8_t*)nullptr, 0);
  h.end();
  Serial.printf("[azure] criar blob -> %d  heap=%u maior=%u\n", code, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return code == 201 || code == 409;
}

bool appendFile(const String& path) {
  File f = LittleFS.open(path, "r");
  if (!f) return true;
  size_t sz = f.size();
  if (!sz) {
    f.close();
    return true;
  }
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient h;
  h.setTimeout(20000);
  if (!h.begin(cli, blobUrl() + "?" + AZ_SAS + "&comp=appendblock")) {
    f.close();
    return false;
  }
  h.addHeader("x-ms-version", "2021-08-06");
  h.addHeader("Content-Type", "text/plain");
  int code = h.sendRequest("PUT", &f, sz);
  h.end();
  f.close();
  Serial.printf("[azure] %s (%u B) -> %d  heap=%u\n", path.c_str(), (unsigned)sz, code, ESP.getFreeHeap());
  return code == 201;
}

// Envia ate maxFiles arquivos da fila. Chamado com o Bluetooth DESLIGADO:
// com o BT classico ativo nao sobra memoria para o TLS do https.
bool blobCreated = false;
int uploadBatch(int maxFiles) {
  int sent = 0;
  if (!blobCreated) blobCreated = createBlob();
  if (!blobCreated) return -1;
  while (sent < maxFiles) {
    xSemaphoreTake(fsMux, portMAX_DELAY);
    if (curBytes) rotateLocked();
    String f = scanQueue(nullptr, nullptr);
    uploading = f;
    xSemaphoreGive(fsMux);
    if (!f.length()) break;
    esp_task_wdt_reset();
    if (!appendFile(f)) {
      nFail++;
      uploading = "";
      return sent ? sent : -1;
    }
    xSemaphoreTake(fsMux, portMAX_DELAY);
    LittleFS.remove(f);
    uploading = "";
    xSemaphoreGive(fsMux);
    nSent++;
    sent++;
  }
  return sent;
}

// ------------------------------------------------------------------ ELM327
#ifdef SIM_OBD
static float simT = 0;
static String hx(int v) {
  char b[4];
  snprintf(b, sizeof b, "%02X", v & 255);
  return b;
}
String elm(const char* c, uint32_t to = 1500) {
  delay(40);
  if (!strncmp(c, "AT", 2)) return "OK";
  String pid = String(c).substring(2);
  simT += 0.05;
  if (pid == "0C") { int r = (int)((1500 + 700 * sin(simT)) * 4); return "410C" + hx(r >> 8) + hx(r); }
  if (pid == "0D") return "410D" + hx(80 + (int)(20 * sin(simT / 3)));
  if (pid == "04") return "4104" + hx(90 + (int)(60 * sin(simT)));
  if (pid == "05") return "4105" + hx(128);
  if (pid == "0F") return "410F" + hx(70);
  if (pid == "33") return "4133" + hx(92);
  if (pid == "0100") return "4100BE3FA813";
  return "NO DATA";
}
bool btConnect() { return true; }
#else
String elm(const char* c, uint32_t to = 1500) {
  while (BT.available()) BT.read();
  BT.print(c);
  BT.print("\r");
  String r;
  uint32_t t = millis();
  while (millis() - t < to) {
    while (BT.available()) {
      char ch = BT.read();
      if (ch == '>') return r;
      r += ch;
    }
    delay(2);
  }
  return "TIMEOUT";
}

// Dongle ELM327 mini "OBDII": so Bluetooth classico (SPP), pareamento legado por PIN.
// No comboio ha dois dongles com o MESMO nome, um em cada carro. Regras:
//  - DONGLE_DENY (sufixo do endereco, ex. "22:33") nunca e usado: e o dongle do outro carro;
//  - entre os candidatos, tenta primeiro o de sinal mais forte (o do proprio carro);
//  - o primeiro que responder fica salvo; segurar o botao BOOT ao ligar esquece o salvo.
#ifndef DONGLE_DENY
#define DONGLE_DENY ""
#endif
static const char* PINS_BT[] = {"1234", "0000", "6789"};

static String addrStr(const uint8_t* a) {
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return b;
}
// Dongle da Amarok: 00:10:CC:4F:36:03 (classico + BLE). O da Hilux (AA:BB:CC:11:22:33) nunca.
#ifndef DONGLE_ADDR
#define DONGLE_ADDR "00:10:CC:4F:36:03"
#endif
static bool parseAddr(const char* s, uint8_t* out) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}
static bool denied(const uint8_t* a) {
  uint8_t only[6];
  if (parseAddr(DONGLE_ADDR, only) && memcmp(a, only, 6) != 0) return true;  // so o dongle da Amarok
  String d = denySfx;
  d.toUpperCase();
  return d.length() && addrStr(a).endsWith(d);
}
static bool tryAddr(uint8_t* a) {
  // 1o: SPP sem autenticacao (como o Car Scanner). Nao pareia, entao nao troca a unica
  // link key que o clone guarda — parear aqui quebraria o pareamento do celular com o dongle.
  esp_task_wdt_reset();
  Serial.printf("[bt] conectando %s sem pareamento...\n", addrStr(a).c_str());
  if (BT.connect(a, 0, ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER)) {
    char nm[20];
    snprintf(nm, sizeof nm, "dongle_%02X%02X", a[4], a[5]);
    record("evento", nm, 1);
    return true;
  }
  for (auto pin : PINS_BT) {
    esp_task_wdt_reset();
    BT.setPin(pin);
    Serial.printf("[bt] conectando %s (PIN %s)...\n", addrStr(a).c_str(), pin);
    if (BT.connect(a)) {
      char nm[20];
      snprintf(nm, sizeof nm, "dongle_%02X%02X", a[4], a[5]);
      record("evento", nm, 0, false);
      return true;
    }
  }
  return false;
}

bool btConnect() {
  uint8_t addr[6];
  // endereco conhecido do dongle da Amarok: conecta direto, sem varredura
  if (parseAddr(DONGLE_ADDR, addr) && tryAddr(addr)) return true;
  // endereco salvo de uma conexao anterior: tenta direto
  if (prefs.getBytes("obdaddr", addr, 6) == 6 && !denied(addr)) {
    if (tryAddr(addr)) return true;
  }
  Serial.println("[bt] procurando dongle (10 s)...");
  BTScanResults* r = BT.discover(10000);
  if (!r) return false;
  // candidatos com nome de dongle, do sinal mais forte para o mais fraco
  int idx[16], rssi[16], n = 0;
  for (int i = 0; i < r->getCount() && n < 16; i++) {
    BTAdvertisedDevice* d = r->getDevice(i);
    String nmv = d->getName().c_str();
    String u = nmv;
    u.toUpperCase();
    int rs = d->haveRSSI() ? d->getRSSI() : -127;
    Serial.printf("[bt]  %s  %s  %d dBm\n", d->getAddress().toString().c_str(), nmv.c_str(), rs);
    if (u.indexOf("OBD") >= 0 || u.indexOf("ELM") >= 0 || u.indexOf("V-LINK") >= 0 || u.indexOf("VLINK") >= 0 ||
        nmv == OBD_NAME) {
      idx[n] = i;
      rssi[n] = rs;
      n++;
    }
  }
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      if (rssi[b] > rssi[a]) {
        std::swap(rssi[a], rssi[b]);
        std::swap(idx[a], idx[b]);
      }
  for (int k = 0; k < n; k++) {
    BTAddress ad = r->getDevice(idx[k])->getAddress();
    memcpy(addr, *ad.getNative(), 6);
    if (denied(addr)) {
      Serial.printf("[bt] %s e o dongle do outro carro, pulei\n", addrStr(addr).c_str());
      continue;
    }
    if (tryAddr(addr)) {
      prefs.putBytes("obdaddr", addr, 6);
      return true;
    }
  }
  return false;
}
#endif

static String clean(String s) {
  String o;
  for (char ch : s)
    if (ch != ' ' && ch != '\r' && ch != '\n') o += (char)toupper(ch);
  return o;
}

struct Pid {
  const char* id;
  const char* name;
  bool two;
  bool fast;
  float mul, add;  // valor = (A ou 256A+B) * mul + add
};
Pid PIDS[] = {
    {"0C", "rpm", true, true, 0.25f, 0},       {"0D", "vel_obd", false, true, 1, 0},
    {"04", "carga", false, true, 100 / 255.f, 0}, {"0B", "map", false, true, 1, 0},
    {"10", "maf", true, true, 0.01f, 0},       {"49", "pedal", false, true, 100 / 255.f, 0},
    {"11", "borboleta", false, false, 100 / 255.f, 0}, {"5E", "vazao_comb", true, false, 0.05f, 0},
    {"05", "arrefec", false, false, 1, -40},   {"0F", "admissao", false, false, 1, -40},
    {"46", "ambiente", false, false, 1, -40},  {"5C", "oleo", false, false, 1, -40},
    {"33", "baro", false, false, 1, 0},        {"23", "rail", true, false, 10, 0},
    {"2F", "tanque", false, false, 100 / 255.f, 0}, {"42", "tensao", true, false, 0.001f, 0},
};
const int NP = sizeof(PIDS) / sizeof(PIDS[0]);
bool sup[NP];
int fastIdx[NP], nFast = 0, slowIdx[NP], nSlow = 0, slowRR = 0;

bool parse(const Pid& p, const String& raw, double* out) {
  String s = clean(raw);
  int i = s.indexOf(String("41") + p.id);
  if (i < 0) return false;
  String hex = s.substring(i + 4);
  if ((int)hex.length() < (p.two ? 4 : 2)) return false;
  int a = strtol(hex.substring(0, 2).c_str(), nullptr, 16);
  int b = p.two ? strtol(hex.substring(2, 4).c_str(), nullptr, 16) : 0;
  double raw_v = p.two ? 256.0 * a + b : a;
  *out = raw_v * p.mul + p.add;
  return true;
}

bool elmInit() {
  const char* cmds[] = {"ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATAT1", "ATSP0"};
  for (auto c : cmds) {
    String r = elm(c, strcmp(c, "ATZ") ? 2000 : 4000);
    Serial.printf("[elm] %s -> %s\n", c, clean(r).c_str());
  }
  String r = elm("0100", 12000);
  Serial.printf("[elm] 0100 -> %s\n", clean(r).c_str());
  return clean(r).indexOf("4100") >= 0;
}

void probe() {
  nFast = nSlow = 0;
  for (int i = 0; i < NP; i++) {
    double v;
    String r = elm((String("01") + PIDS[i].id).c_str(), 2500);
    sup[i] = parse(PIDS[i], r, &v);
    Serial.printf("[elm] 01%s %s -> %s\n", PIDS[i].id, PIDS[i].name, sup[i] ? String(v, 1).c_str() : "nao responde");
    if (sup[i]) {
      if (PIDS[i].fast) fastIdx[nFast++] = i;
      else slowIdx[nSlow++] = i;
      record("meta", (String("sup_") + PIDS[i].name).c_str(), v);
    }
  }
  record("meta", "sinais", nFast + nSlow);
}

// ------------------------------------------------------------------ GPS (LC29H em RX2/GPIO16, 9600)
#define GPS_RX 16
#define GPS_TX 17
volatile int gpsSats = 0;
volatile bool gpsFix = false;
volatile uint32_t nGps = 0;

static bool nmeaOk(const String& s) {  // confere o checksum *hh
  int star = s.lastIndexOf('*');
  if (star < 1 || star + 3 > (int)s.length()) return false;
  uint8_t c = 0;
  for (int i = 1; i < star; i++) c ^= (uint8_t)s[i];
  return c == (uint8_t)strtol(s.substring(star + 1, star + 3).c_str(), nullptr, 16);
}
static int fields(const String& s, String* f, int max) {
  int n = 0, start = 0;
  for (int i = 0; i <= (int)s.length() && n < max; i++)
    if (i == (int)s.length() || s[i] == ',' || s[i] == '*') {
      f[n++] = s.substring(start, i);
      start = i + 1;
      if (i < (int)s.length() && s[i] == '*') break;
    }
  return n;
}
static double degMin(const String& v, const String& hemi) {  // ddmm.mmmm -> graus
  if (!v.length()) return 0;
  double x = v.toDouble();
  int d = (int)(x / 100);
  double g = d + (x - d * 100) / 60.0;
  return (hemi == "S" || hemi == "W") ? -g : g;
}

void gpsTask(void*) {
  String line, f[16];
  double kmh = 0;
  for (;;) {
    while (Serial2.available()) {
      char c = Serial2.read();
      if (c == '\r') continue;
      if (c != '\n') {
        if (line.length() < 120) line += c;
        continue;
      }
      if (line.length() > 6 && line[0] == '$' && nmeaOk(line)) {
        String t = line.substring(3, 6);
        int n = fields(line, f, 16);
        if (t == "RMC" && n >= 10) {
          kmh = f[7].toDouble() * 1.852;
          // relogio pelo GPS quando ainda nao ha internet
          if (f[2] == "A" && f[9].length() == 6 && f[1].length() >= 6 && !epochMs()) {
            struct tm tm = {};
            tm.tm_mday = f[9].substring(0, 2).toInt();
            tm.tm_mon = f[9].substring(2, 4).toInt() - 1;
            tm.tm_year = f[9].substring(4, 6).toInt() + 100;
            tm.tm_hour = f[1].substring(0, 2).toInt();
            tm.tm_min = f[1].substring(2, 4).toInt();
            tm.tm_sec = f[1].substring(4, 6).toInt();
            setenv("TZ", "UTC0", 1);
            tzset();
            struct timeval tv = {mktime(&tm), 0};
            settimeofday(&tv, nullptr);
            record("meta", "relogio_gps", (double)tv.tv_sec);
          }
        } else if (t == "GGA" && n >= 10) {
          int q = f[6].toInt();
          gpsSats = f[7].toInt();
          gpsFix = q > 0;
          if (gpsFix) {
            recordGps(degMin(f[2], f[3]), degMin(f[4], f[5]), kmh, f[9].toDouble(), gpsSats, f[8].toDouble());
            nGps++;
          }
        }
      }
      line = "";
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ------------------------------------------------------------------ pilha Bluetooth
void btStart_() {
#ifndef SIM_OBD
  BT.begin("comboio-esp", true);
  BT.setPin("1234");
#endif
}
void btStop_() {
#ifndef SIM_OBD
  BT.disconnect();
  BT.end();
  delay(300);
#endif
}

// ------------------------------------------------------------------ setup/loop
// registra troca de rede (wifi_1 / wifi_2, sem o nome da rede nos dados)
void logWifi() {
  int idx = 0;
  if (WiFi.status() == WL_CONNECTED)
    for (int i = 0; i < (int)(sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0])); i++)
      if (WiFi.SSID() == WIFI_NETS[i].ssid) idx = i + 1;
  if (idx != lastNet) {
    lastNet = idx;
    Serial.printf("[wifi] rede %d %s %d dBm\n", idx, idx ? WiFi.SSID().c_str() : "-", idx ? WiFi.RSSI() : 0);
    char nm[12];
    snprintf(nm, sizeof nm, "wifi_%d", idx);
    record("evento", nm, idx ? WiFi.RSSI() : 0);
  }
}

// reconecta sozinho na melhor rede conhecida (maior sinal)
void wifiTask(void*) {
  for (;;) {
    if (!wifiBusy && WiFi.status() != WL_CONNECTED) wifiMulti.run(8000);
    if (!wifiBusy) logWifi();
    // sem rede: varre a cada 30 s (a varredura atrapalha o Bluetooth do dongle)
    vTaskDelay(pdMS_TO_TICKS(WiFi.status() == WL_CONNECTED ? 4000 : 30000));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  fsMux = xSemaphoreCreateMutex();
  prefs.begin("comboio", false);
  // botao BOOT (GPIO0) segurado ao ligar: esquece o dongle salvo
  pinMode(0, INPUT_PULLUP);
  delay(50);
  if (digitalRead(0) == LOW) {
    prefs.remove("obdaddr");
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED, i % 2);
      delay(100);
    }
    Serial.println("[bt] dongle salvo esquecido (BOOT)");
  }
  bootNo = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootNo);
  uint8_t m[6];
  WiFi.macAddress(m);
  char t[8];
  snprintf(t, sizeof t, "%02x%02x%02x", m[3], m[4], m[5]);
  macTag = t;

  if (!LittleFS.begin(true)) Serial.println("[fs] falha ao montar LittleFS");
  scanQueue(&qSeq, nullptr);
  // sobra de um boot anterior: vira fila
  if (LittleFS.exists("/cur.csv")) {
    File c = LittleFS.open("/cur.csv", "r");
    size_t s = c.size();
    c.close();
    if (s) {
      char nm[24];
      snprintf(nm, sizeof nm, "/q%06u.csv", (unsigned)++qSeq);
      LittleFS.rename("/cur.csv", nm);
    }
  }
  qOpen();
  lastRotate = millis();
  Serial.printf("\n[comboio] %s boot %u mac %s  fs %u/%u KB\n", DEVICE_LABEL, (unsigned)bootNo, macTag.c_str(),
                (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  record("meta", "inicio", bootNo);

  // Wi-Fi: redes de secrets.h; conecta na de maior sinal e troca quando enfraquece
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  for (auto& n : WIFI_NETS) wifiMulti.addAP(n.ssid, n.pass[0] ? n.pass : nullptr);
  wifiMulti.run(10000);
  logWifi();
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  xTaskCreatePinnedToCore(wifiTask, "wifi", 6144, nullptr, 1, nullptr, 0);
  // vigia: o laco principal tem 240 s para dar sinal de vida; travou -> reinicia (a fila fica na flash)
  esp_task_wdt_init(240, true);
  esp_task_wdt_add(nullptr);
  Serial2.setRxBufferSize(1024);
  Serial2.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  xTaskCreatePinnedToCore(gpsTask, "gps", 6144, nullptr, 2, nullptr, 0);

#ifndef SIM_OBD
  // so Bluetooth classico: devolve a memoria do controlador BLE
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
#endif
  btStart_();
  Serial.printf("[mem] heap livre %u, maior bloco %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

uint32_t lastStatus = 0, lastMeta = 0, lastGood = 0, lastUpload = 0;
int timeouts = 0;
#ifndef UPLOAD_EVERY_S
#define UPLOAD_EVERY_S 180
#endif
uint32_t UPLOAD_EVERY_MS = UPLOAD_EVERY_S * 1000UL;  // pausa o BT e envia (comando remoto "every")

// ------------------------------------------------------------------ comandos remotos
// O ESP le cmd/<mac>.txt no Azure a cada envio (saida pela Starlink, sem porta aberta).
// Formato: primeira linha "id=N" (so executa N maior que o ultimo), depois um comando por linha:
//   every <s>   intervalo de envio em segundos (30..900), fica salvo
//   deny <xx:yy>  sufixo de endereco de dongle a ignorar, fica salvo ("deny -" limpa)
//   forget      esquece o dongle salvo
//   reboot      reinicia
void fetchCommands() {
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient h;
  h.setTimeout(10000);
  String url = String("https://") + AZ_ACCOUNT + ".blob.core.windows.net/comboio/cmd/" + macTag + ".txt?" + AZ_CMD_SAS;
  if (!h.begin(cli, url)) return;
  h.addHeader("x-ms-version", "2021-08-06");
  int code = h.GET();
  String body = code == 200 ? h.getString() : "";
  h.end();
  if (code != 200) return;  // 404 = nenhum comando
  int nl = body.indexOf('\n');
  String first = nl < 0 ? body : body.substring(0, nl);
  first.trim();
  if (!first.startsWith("id=")) return;
  uint32_t id = first.substring(3).toInt();
  if (id <= prefs.getUInt("cmdid", 0)) return;
  prefs.putUInt("cmdid", id);
  bool doReboot = false;
  int pos = nl < 0 ? body.length() : nl + 1;
  while (pos < (int)body.length()) {
    int e = body.indexOf('\n', pos);
    if (e < 0) e = body.length();
    String c = body.substring(pos, e);
    c.trim();
    pos = e + 1;
    if (!c.length()) continue;
    Serial.printf("[cmd] %u: %s\n", (unsigned)id, c.c_str());
    if (c.startsWith("every ")) {
      uint32_t s = constrain(c.substring(6).toInt(), 30, 900);
      prefs.putUInt("every", s);
      UPLOAD_EVERY_MS = s * 1000UL;
      record("cmd", "every", s);
    } else if (c.startsWith("deny ")) {
      String d = c.substring(5);
      d.trim();
      if (d == "-") d = "";
      prefs.putString("deny", d);
      denySfx = d;
      record("cmd", "deny", d.length());
    } else if (c == "forget") {
      prefs.remove("obdaddr");
      record("cmd", "forget", 0, false);
    } else if (c == "reboot") {
      doReboot = true;
    } else {
      record("cmd", "desconhecido", 0, false);
    }
  }
  record("evento", "cmd_ok", id);
  if (doReboot) {
    record("cmd", "reboot", 0, false);
    xSemaphoreTake(fsMux, portMAX_DELAY);
    cur.flush();
    xSemaphoreGive(fsMux);
    ESP.restart();
  }
}

// reconexao rapida depois da pausa: o ELM continua alimentado e configurado
bool quickResume() {
  if (!btConnect()) return false;
  const char* cmds[] = {"ATE0", "ATL0", "ATS0", "ATH0"};
  for (auto c : cmds) elm(c, 1500);
  double v;
  for (int i = 0; i < NP; i++)
    if (sup[i]) return parse(PIDS[i], elm((String("01") + PIDS[i].id).c_str(), 3000), &v) || elmInit();
  return elmInit();
}

void pauseAndUpload() {
  lastUpload = millis();
  // handoff: sem rede ou sinal fraco -> varre e fica com a melhor rede conhecida
  if (WiFi.status() != WL_CONNECTED || WiFi.RSSI() < -75) {
    wifiBusy = true;
    if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(false);
    wifiMulti.run(8000);
    wifiBusy = false;
    logWifi();
  }
  if (WiFi.status() != WL_CONNECTED) return;
  int qc;
  xSemaphoreTake(fsMux, portMAX_DELAY);
  scanQueue(nullptr, &qc);
  bool pendente = qc || curBytes;
  xSemaphoreGive(fsMux);
  if (!pendente) return;
  bool wasOk = btOk;
  uint32_t t0 = millis();
  record("evento", "pausa_envio", qc);
  btStop_();
  Serial.printf("[mem] BT parado: heap %u, maior bloco %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (ESP.getMaxAllocHeap() < 30000) {  // memoria fragmentada: reinicia (a fila fica na flash)
    record("evento", "reinicio_memoria", ESP.getMaxAllocHeap());
    xSemaphoreTake(fsMux, portMAX_DELAY);
    cur.flush();
    xSemaphoreGive(fsMux);
    ESP.restart();
  }
  int n = uploadBatch(40);
  if (n < 0) {  // rede sem internet (ex.: portal cativo): solta para tentar outra na proxima
    record("evento", "envio_falhou", 0, false);
    WiFi.disconnect(false);
  } else {
    fetchCommands();
  }
  btStart_();
  btOk = wasOk && quickResume();
  record("evento", "retoma", n);
  record("meta", "pausa_ms", millis() - t0);
  Serial.printf("[envio] %d arquivos, pausa de %lu ms, obd=%d\n", n, millis() - t0, (int)btOk);
}

void printStatus() {
  if (millis() - lastStatus < 10000) return;
  lastStatus = millis();
  int qc;
  xSemaphoreTake(fsMux, portMAX_DELAY);
  scanQueue(nullptr, &qc);
  xSemaphoreGive(fsMux);
  Serial.printf("[status] obd=%d gps=%d sat=%d pts=%u linhas=%u enviados=%u falhas=%u fila=%d wifi=%s hora=%d heap=%u\n",
                (int)btOk, (int)gpsFix, gpsSats, (unsigned)nGps, (unsigned)nLines, (unsigned)nSent, (unsigned)nFail, qc,
                WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "-", (int)(epochMs() != 0),
                ESP.getFreeHeap());
}

void loop() {
  static bool cfgLoaded = false;
  if (!cfgLoaded) {  // ajustes salvos por comando remoto
    cfgLoaded = true;
    uint32_t s = prefs.getUInt("every", 0);
    if (s) UPLOAD_EVERY_MS = s * 1000UL;
    denySfx = prefs.isKey("deny") ? prefs.getString("deny") : String(DONGLE_DENY);
    Serial.printf("[cfg] envio a cada %lu s, dongle ignorado: '%s'\n", UPLOAD_EVERY_MS / 1000, denySfx.c_str());
  }
  printStatus();
  esp_task_wdt_reset();
  // batimento a cada minuto, com ou sem dongle: mostra que o laco esta vivo
  if (millis() - lastMeta > 60000) {
    lastMeta = millis();
    record("meta", "heap", ESP.getFreeHeap());
    record("meta", "wifi_rssi", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  }
  if (!ntpDone && epochMs()) {
    ntpDone = true;
    record("meta", "ntp", (double)(epochMs() / 1000));
  }
  if (millis() - lastUpload > UPLOAD_EVERY_MS) pauseAndUpload();
  if (!btOk) {
    digitalWrite(LED, (millis() / 500) % 2);
    if (btConnect() && elmInit()) {
      btOk = true;
      timeouts = 0;
      record("evento", "obd_ok", 0, false);
      probe();
      lastGood = millis();
    } else {
      record("evento", "obd_falhou", 0, false);
#ifndef SIM_OBD
      BT.disconnect();
#endif
      delay(5000);
      return;
    }
  }

  int list[NP + 1], n = 0;
  for (int i = 0; i < nFast; i++) list[n++] = fastIdx[i];
  if (nSlow) list[n++] = slowIdx[slowRR++ % nSlow];
  for (int k = 0; k < n; k++) {
    const Pid& p = PIDS[list[k]];
    String r = elm((String("01") + p.id).c_str(), 1500);
    double v;
    if (parse(p, r, &v)) {
      record("obd", p.name, v);
      timeouts = 0;
      lastGood = millis();
    } else if (r == "TIMEOUT") {
      timeouts++;
    }
  }
  digitalWrite(LED, !digitalRead(LED));

  // dongle caiu: muitos timeouts seguidos ou nada valido em 60 s
  if (timeouts > 8 || millis() - lastGood > 60000) {
    record("evento", "obd_caiu", timeouts);
    btOk = false;
#ifndef SIM_OBD
    BT.disconnect();
#endif
  }
  printStatus();
}
