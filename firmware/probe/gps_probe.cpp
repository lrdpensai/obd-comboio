// Sonda: descobre em que pino/baud o modulo GPS esta mandando NMEA e mostra o estado do fix.
// Nao escreve nada no GPS (TX desligado).
#include <Arduino.h>

const int PINS[] = {16, 17, 4, 5, 13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39, 2, 15, 12};
const long BAUDS[] = {9600, 38400, 115200, 57600, 4800};

int foundPin = -1;
long foundBaud = 0;

bool tryPin(int pin, long baud, uint32_t ms) {
  Serial2.end();
  Serial2.begin(baud, SERIAL_8N1, pin, -1);
  String buf;
  uint32_t t = millis();
  while (millis() - t < ms) {
    while (Serial2.available()) {
      char c = Serial2.read();
      if (c == '\n') {
        if (buf.startsWith("$G") && buf.indexOf(',') > 0) return true;
        buf = "";
      } else if (buf.length() < 120) buf += c;
    }
    delay(2);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[gps-sonda] procurando NMEA...");
  for (long b : BAUDS) {
    for (int p : PINS) {
      if (tryPin(p, b, 1300)) {
        foundPin = p;
        foundBaud = b;
        Serial.printf("[gps-sonda] ACHEI: RX no GPIO%d a %ld baud\n", p, b);
        return;
      }
    }
    Serial.printf("[gps-sonda] nada a %ld baud\n", b);
  }
  Serial.println("[gps-sonda] NENHUM NMEA em nenhum pino");
}

void loop() {
  if (foundPin < 0) {
    delay(1000);
    return;
  }
  static String line;
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {
      if (line.startsWith("$GPGGA") || line.startsWith("$GNGGA") || line.startsWith("$GPRMC") ||
          line.startsWith("$GNRMC") || line.startsWith("$GPGSV") || line.startsWith("$GNGSA"))
        Serial.println(line);
      line = "";
    } else if (c != '\r' && line.length() < 160)
      line += c;
  }
}
