/**
 * DS18B20 → I²C-Bridge
 * ====================
 * Plattform : ESP32-C3
 * 1-Wire    : GPIO1  (DS18B20 Data, 4.7 kΩ Pull-Up nach 3.3 V)
 * Sensor-VCC : GPIO0  (OUTPUT HIGH → 3.3 V) — Strapping-Pin, muss HIGH bleiben!
 * Sensor-GND : GPIO2  (OUTPUT LOW  → GND)
 * Direktanschluss DS18B20: Pin0=3V3, Pin1=Data, Pin2=GND
 * I²C Slave : SDA = GPIO4, SCL = GPIO3
 * Onboard-LED: GPIO8 (blau) — 2× kurz blinken bei gültiger Messung
 *
 * Register-Layout (2 Byte, MSB first, LM75-kompatibel):
 *   Wert = int16_raw / 16.0  →  Auflösung 0.0625 °C
 *   z. B. 25.0 °C → raw = 400 = 0x0190, gesendet: 0x01 0x90
 *   Bei ungültigem Sensor → 0x8000 als Error-Marker
 *
 * Board-Manager : esp32 by Espressif  2.0.17  (IDF 4.x – I2C-Slave stabil!)
 * Board         : "ESP32C3 Dev Module"  (NICHT v3.x – I2C-Slave dort kaputt)
 * Libraries     : OneWire, DallasTemperature
 *
 * ── ESPHome Gegenseite ────────────────────────────────────────────────────────
 *
 *   i2c_device:
 *     - id: temp_bridge
 *       address: 0x48
 *       i2c_id: i2c_bus
 *
 *   sensor:
 *     - platform: template
 *       id: sensor_temp_becken
 *       name: "Temperatur Becken"
 *       unit_of_measurement: "°C"
 *       accuracy_decimals: 1
 *       device_class: temperature
 *       state_class: measurement
 *       update_interval: 1s
 *       lambda: |-
 *         // ESP32-C3 IDF 4.x Bug: Repeated-START (write-then-read) wird nicht ACKt.
 *         // Workaround: reiner Raw-Read ohne Register-Write davor.
 *         uint8_t buf[2];
 *         auto err = id(i2c_bus)->read(0x48, buf, 2);
 *         if (err != i2c::ERROR_OK) return {};
 *         int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
 *         if (raw == (int16_t)0x8000) return {};   // Error-Marker
 *         return raw / 16.0f;
 *       on_value:
 *         - lvgl.widget.refresh: lbl_temp_becken
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>

// ── Konfiguration ─────────────────────────────────────────────────────────────
#define ONE_WIRE_PIN    1        // DS18B20 Datenleitung
#define PIN_SENSOR_VCC  0        // OUTPUT HIGH → 3.3 V für Sensor (Strapping-Pin: MUSS HIGH bleiben!)
#define PIN_SENSOR_GND  2        // OUTPUT LOW  → GND für Sensor
#define I2C_SDA         4        // I²C Slave SDA
#define I2C_SCL         3        // I²C Slave SCL
#define I2C_ADDR        0x48     // Slave-Adresse (0x48–0x4F, Jumper-frei wählbar)
#define UPDATE_MS        750     // Messintervall [ms] – DS18B20 12-bit Wandlung ~750 ms
#define PIN_LED           8      // Onboard-LED (blau, active LOW)
// ─────────────────────────────────────────────────────────────────────────────

OneWire           oneWire(ONE_WIRE_PIN);
DallasTemperature ds18b20(&oneWire);

// Mutex für thread-sicheren Zugriff (Wire-CB läuft ggf. auf anderem Core)
portMUX_TYPE tempMux = portMUX_INITIALIZER_UNLOCKED;
int16_t      g_temp_raw = 0;     // Temperatur × 16  →  0.0625 °C / LSB
bool         g_valid    = false;

// Für Logging aus loop() statt aus dem Wire-Callback
volatile bool    g_req_fired = false;
volatile int16_t g_req_sent  = 0;
volatile bool    g_req_valid = false;

// ── I²C-Callbacks ─────────────────────────────────────────────────────────────

// Master schreibt ein Register-Byte vor dem Lesen – wir ignorieren es,
// da wir nur ein einziges Register haben.
void onReceive(int /*len*/) {
  while (Wire.available()) Wire.read();
}

// Master fordert Daten an → 2 Bytes senden (MSB first)
// ACHTUNG: Callback so kurz wie möglich halten – kein Serial, kein LED, kein delay!
// Serial.printf(%.4f) würde mehrere ms blockieren und den I²C-Timeout des Masters auslösen.
void onRequest() {
  int16_t snap;
  bool    valid;

  portENTER_CRITICAL_ISR(&tempMux);
    snap  = g_temp_raw;
    valid = g_valid;
  portEXIT_CRITICAL_ISR(&tempMux);

  if (!valid) {
    Wire.write((uint8_t)0x80);
    Wire.write((uint8_t)0x00);
    g_req_sent  = (int16_t)0x8000;
    g_req_valid = false;
  } else {
    Wire.write((uint8_t)((snap >> 8) & 0xFF));  // MSB
    Wire.write((uint8_t)( snap       & 0xFF));  // LSB
    g_req_sent  = snap;
    g_req_valid = true;
  }
  g_req_fired = true;  // loop() übernimmt Logging + LED
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║   DS18B20 → I²C-Bridge  (ESP32-C3)  ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.printf("  1-Wire  : GPIO%d\n",       ONE_WIRE_PIN);
  Serial.printf("  Sens-VCC: GPIO%d (HIGH)\n", PIN_SENSOR_VCC);
  Serial.printf("  Sens-GND: GPIO%d (LOW)\n",  PIN_SENSOR_GND);
  Serial.printf("  I²C SDA : GPIO%d\n",       I2C_SDA);
  Serial.printf("  I²C SCL : GPIO%d\n",       I2C_SCL);
  Serial.printf("  Adresse : 0x%02X\n",       I2C_ADDR);
  Serial.printf("  Intervall: %d ms\n\n",     UPDATE_MS);

  // Sensor-Versorgung per GPIO
  pinMode(PIN_SENSOR_GND, OUTPUT);
  digitalWrite(PIN_SENSOR_GND, LOW);
  pinMode(PIN_SENSOR_VCC, OUTPUT);
  digitalWrite(PIN_SENSOR_VCC, HIGH);
  delay(10);  // Sensor hochlaufen lassen

  // Onboard-LED (active LOW → HIGH = aus)
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  // 1-Wire initialisieren
  ds18b20.begin();
  ds18b20.setResolution(12);   // 12-bit → 0.0625 °C Auflösung

  int devCount = ds18b20.getDeviceCount();
  if (devCount == 0) {
    Serial.println("  WARNUNG: Kein DS18B20 gefunden – Pull-Up prüfen!");
  } else {
    Serial.printf("  DS18B20 gefunden: %d Sensor(en)\n", devCount);
    DeviceAddress addr;
    if (ds18b20.getAddress(addr, 0)) {
      Serial.printf("  ROM-Code: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
                    addr[0], addr[1], addr[2], addr[3],
                    addr[4], addr[5], addr[6], addr[7]);
    }
  }

  // I²C Slave starten
  // Hinweis: Wire.begin(addr, sda, scl) ist auf ESP32-C3 / Arduino v3.x unzuverlässig.
  // Korrekte Reihenfolge: setPins → onReceive/onRequest → begin(addr)
  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.onReceive(onReceive);
  Wire.onRequest(onRequest);
  bool ok = Wire.begin((uint8_t)I2C_ADDR);
  Serial.printf("  Wire.begin(0x%02X) -> %s\n", I2C_ADDR, ok ? "OK" : "FEHLER!");
  Serial.flush();

  Serial.println("\n  I²C-Slave aktiv – warte auf Anfragen …\n");
}

// ── Loop ───────────────────────────────────────────────────────────────────────
static uint32_t lastMs = 0;

void loop() {
  if (millis() - lastMs < UPDATE_MS) return;
  lastMs = millis();

  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);

  // 85.0 °C = Power-On-Reset-Wert → als Fehler behandeln
  if (t == DEVICE_DISCONNECTED_C || t == 85.0f) {
    portENTER_CRITICAL(&tempMux);
      g_valid = false;
    portEXIT_CRITICAL(&tempMux);
    Serial.println("  [ERR] Sensor nicht erreichbar oder Power-on-Default");
    return;
  }

  int16_t raw = (int16_t)roundf(t * 16.0f);

  portENTER_CRITICAL(&tempMux);
    g_temp_raw = raw;
    g_valid    = true;
  portEXIT_CRITICAL(&tempMux);

  // 2× kurz blinken (active LOW)
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_LED, LOW);  delay(40);
    digitalWrite(PIN_LED, HIGH); delay(80);
  }

  Serial.printf("  Temp: %+7.4f °C   raw=0x%04X (%d)\n",
                t, (uint16_t)raw, raw);

  // I²C-Request-Logging (verzögert aus Callback heraus – kein Float im ISR-Kontext)
  if (g_req_fired) {
    g_req_fired = false;
    if (!g_req_valid) {
      Serial.println("  [I2C] onRequest -> ERROR-Marker gesendet (kein gueltiger Wert)");
    } else {
      int16_t sent = g_req_sent;
      Serial.printf("  [I2C] onRequest -> gesendet: 0x%02X 0x%02X  (%.4f degC)\n",
                    (sent >> 8) & 0xFF, sent & 0xFF, sent / 16.0f);
      // 1× kurz blinken als I²C-Bestätigung
      digitalWrite(PIN_LED, LOW);  delay(20);
      digitalWrite(PIN_LED, HIGH);
    }
  }
}
