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
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <sys/time.h>
#include "secrets.h"  // AZ_ACCOUNT, AZ_SAS, DEVICE_LABEL, OBD_NAME

#define LED 2

BluetoothSerial BT;
Preferences prefs;
WiFiManager wm;
SemaphoreHandle_t fsMux;

uint32_t bootNo = 0;
String macTag;
volatile bool btOk = false;
volatile uint32_t nLines = 0, nSent = 0, nFail = 0;
volatile bool ntpDone = false;
String uploading;  // arquivo em envio (nao apagar)

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

void record(const char* type, const char* name, double v, bool hasV = true) {
  char line[112];
  uint64_t e = epochMs();
  int n = hasV ? snprintf(line, sizeof line, "%u,%lu,%llu,%s,%s,%.3f\n", (unsigned)bootNo, millis(),
                          (unsigned long long)e, type, name, v)
               : snprintf(line, sizeof line, "%u,%lu,%llu,%s,%s,\n", (unsigned)bootNo, millis(),
                          (unsigned long long)e, type, name);
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

bool btConnect() {
  uint8_t addr[6];
  // endereco salvo de uma conexao anterior: tenta direto
  if (prefs.getBytes("obdaddr", addr, 6) == 6) {
    Serial.println("[bt] conectando ao dongle salvo...");
    if (BT.connect(addr)) return true;
  }
  Serial.println("[bt] procurando dongle (8 s)...");
  BTScanResults* r = BT.discover(8000);
  if (r) {
    for (int i = 0; i < r->getCount(); i++) {
      BTAdvertisedDevice* d = r->getDevice(i);
      String n = d->getName().c_str();
      String u = n;
      u.toUpperCase();
      Serial.printf("[bt]  %s  %s\n", d->getAddress().toString().c_str(), n.c_str());
      if (u.indexOf("OBD") >= 0 || u.indexOf("ELM") >= 0 || u.indexOf("V-LINK") >= 0 ||
          u.indexOf("VLINK") >= 0 || u.indexOf("VGATE") >= 0 || n == OBD_NAME) {
        BTAddress a = d->getAddress();
        memcpy(addr, *a.getNative(), 6);
        if (BT.connect(addr)) {
          prefs.putBytes("obdaddr", addr, 6);
          return true;
        }
      }
    }
  }
  Serial.printf("[bt] tentando pelo nome %s...\n", OBD_NAME);
  return BT.connect(String(OBD_NAME));
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
void wifiTask(void*) {
  for (;;) {
    wm.process();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  fsMux = xSemaphoreCreateMutex();
  prefs.begin("comboio", false);
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

  // Wi-Fi: credenciais salvas; sem elas, abre o portal "comboio-esp" (senha comboio123)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(20);
  wm.autoConnect("comboio-esp", "comboio123");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  xTaskCreatePinnedToCore(wifiTask, "wifi", 6144, nullptr, 1, nullptr, 0);

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
const uint32_t UPLOAD_EVERY_MS = UPLOAD_EVERY_S * 1000UL;  // pausa o BT e envia

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
  btStart_();
  btOk = wasOk && quickResume();
  record("evento", "retoma", n);
  record("meta", "pausa_ms", millis() - t0);
  Serial.printf("[envio] %d arquivos, pausa de %lu ms, obd=%d\n", n, millis() - t0, (int)btOk);
}

void loop() {
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
  if (millis() - lastMeta > 60000) {
    lastMeta = millis();
    record("meta", "heap", ESP.getFreeHeap());
    record("meta", "wifi_rssi", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  }
  if (millis() - lastStatus > 10000) {
    lastStatus = millis();
    int qc;
    xSemaphoreTake(fsMux, portMAX_DELAY);
    scanQueue(nullptr, &qc);
    xSemaphoreGive(fsMux);
    Serial.printf("[status] linhas=%u enviados=%u falhas=%u fila=%d wifi=%s ntp=%d heap=%u\n", (unsigned)nLines,
                  (unsigned)nSent, (unsigned)nFail, qc, WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "-",
                  (int)ntpDone, ESP.getFreeHeap());
  }
}
