#include <WiFi.h>
#include <WiFiManager.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <stdio.h>
#include <math.h>

#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "secrets.h"

// ======================================================
// PIN LCD
// ======================================================

#define LCD_SDA_PIN 21
#define LCD_SCL_PIN 22

// ======================================================
// PIN INA219
// ======================================================

#define INA_SDA_PIN 2
#define INA_SCL_PIN 4

// ======================================================
// PIN ACS
// ======================================================

#define ACS_PIN 34
const float ACS_SENSITIVITY = 0.185;  // 185 mV/A untuk varian 5A
const float ACS_DIVIDER_GAIN = 1.5;   // Pembagi tegangan 10k dan 20k

// Debounce status ACS: pin floating kadang terlihat normal sesaat,
// jadi status butuh pembacaan beruntun sebelum berubah
const int ACS_GOOD_CONFIRM_READS = 3;  // beruntun valid -> connected
const int ACS_BAD_CONFIRM_READS = 5;   // beruntun buruk -> tinggalkan connected

float acsZeroVoltage = 2.5;
float acsVoltage = 0;
float acsCurrent = 0;
int acsRaw = 0;
bool acsValid = false;  // true hanya jika pembacaan ACS712 lolos validasi

// Status ACS hasil validasi nyata: connected / error / disconnected
const char *acsStatus = "disconnected";
String lastAcsStatus = "";

// ======================================================
// LCD
// ======================================================

#define LCD_ADDRESS 0x27

// ======================================================
// RELAY
// ======================================================

const int RELAY1_PIN = 23;
const int RELAY2_PIN = 19;

// ======================================================
// I2C
// ======================================================

TwoWire I2C_LCD = TwoWire(0);
TwoWire I2C_INA = TwoWire(1);

// ======================================================
// INA219
// ======================================================

Adafruit_INA219 ina219(0x40);

// Status sensor hasil pemeriksaan nyata, dikirim ke Firebase
const char *inaStatus = "disconnected";
String lastInaStatus = "";

// ======================================================
// SENSOR ASLI
// ======================================================

float voltage = 0;
float current = 0;
float power = 0;

// ======================================================
// ACS CONFIG
// ======================================================

const float ACS_VCC = 3.3;

// ======================================================
// INTERVAL
// ======================================================

const unsigned long RELAY_CHECK_INTERVAL_MS = 100;
const unsigned long STATUS_PRINT_INTERVAL_MS = 1000;
const unsigned long LCD_UPDATE_INTERVAL_MS = 500;
const unsigned long SENSOR_UPLOAD_INTERVAL_MS = 1000;
const unsigned long FIREBASE_READ_INTERVAL_MS = 1000;

const unsigned long FIREBASE_LOGIN_TIMEOUT_MS = 15000;

// ======================================================
// FIREBASE
// ======================================================

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ======================================================
// WIFI MANAGER
// ======================================================

WiFiManager wifiManager;

// ======================================================
// TIMER
// ======================================================

unsigned long lastRelayCheckMillis = 0;
unsigned long lastStatusPrintMillis = 0;
unsigned long lastLcdUpdateMillis = 0;
unsigned long lastSensorUploadMillis = 0;
unsigned long lastFirebaseReadMillis = 0;

// ======================================================
// STATE
// ======================================================

bool relay1State = false;
bool relay2State = false;

bool wifiWasConnected = false;
bool firebaseWasReady = false;
bool ina219Ready = false;

// ======================================================
// LCD BACKLIGHT
// ======================================================

uint8_t lcdBacklight = 0x08;

// ======================================================
// DATA FIREBASE UNTUK LCD
// ======================================================

float firebaseVoltage = 0;
float firebaseCurrent = 0;
float firebasePower = 0;

float firebaseAcsCurrent = 0;
float firebaseAcsVoltage = 0;

// ======================================================
// LCD RAW
// ======================================================

void lcdWriteRaw(uint8_t value) {
  I2C_LCD.beginTransmission(LCD_ADDRESS);
  I2C_LCD.write(value | lcdBacklight);
  I2C_LCD.endTransmission();

  delayMicroseconds(50);
}

// ======================================================
// LCD ENABLE
// ======================================================

void lcdPulseEnable(uint8_t value) {
  lcdWriteRaw(value | 0x04);

  delayMicroseconds(1);

  lcdWriteRaw(value & ~0x04);

  delayMicroseconds(50);
}

// ======================================================
// LCD 4 BIT
// ======================================================

void lcdWrite4Bits(uint8_t value) {
  lcdWriteRaw(value);
  lcdPulseEnable(value);
}

// ======================================================
// LCD SEND
// ======================================================

void lcdSend(uint8_t value, uint8_t mode) {
  uint8_t highNibble = value & 0xF0;
  uint8_t lowNibble = (value << 4) & 0xF0;

  lcdWrite4Bits(highNibble | mode);
  lcdWrite4Bits(lowNibble | mode);
}

// ======================================================
// LCD COMMAND
// ======================================================

void lcdCommand(uint8_t command) {
  lcdSend(command, 0x00);
}

// ======================================================
// LCD DATA
// ======================================================

void lcdData(uint8_t data) {
  lcdSend(data, 0x01);
}

// ======================================================
// LCD INIT
// ======================================================

void lcdInit() {
  delay(50);

  lcdWrite4Bits(0x30);
  delay(5);

  lcdWrite4Bits(0x30);
  delayMicroseconds(150);

  lcdWrite4Bits(0x30);
  delayMicroseconds(150);

  lcdWrite4Bits(0x20);

  lcdCommand(0x28);
  lcdCommand(0x08);

  lcdCommand(0x01);
  delay(2);

  lcdCommand(0x06);
  lcdCommand(0x0C);
}

// ======================================================
// LCD CLEAR
// ======================================================

void lcdClear() {
  lcdCommand(0x01);
  delay(2);
}

// ======================================================
// LCD CURSOR
// ======================================================

void lcdSetCursor(uint8_t col, uint8_t row) {
  static const uint8_t rowOffsets[] = {
    0x00,
    0x40,
    0x14,
    0x54
  };

  if (row > 3)
    row = 3;

  lcdCommand(0x80 | (col + rowOffsets[row]));
}

// ======================================================
// LCD PRINT
// ======================================================

void lcdPrint(const String &text) {
  for (size_t i = 0; i < text.length(); i++)
    lcdData(text[i]);
}

// ======================================================
// LCD LINE
// ======================================================

void lcdLine(int row, const String &text) {
  String line = text;

  if (line.length() > 20)
    line = line.substring(0, 20);

  while (line.length() < 20)
    line += ' ';

  lcdSetCursor(0, row);
  lcdPrint(line);
}

// ======================================================
// CENTER TEXT
// ======================================================

String centerText(const String &txt, int width = 20) {
  int pad = width - (int)txt.length();

  if (pad <= 0)
    return txt.substring(0, width);

  int left = pad / 2;
  int right = pad - left;

  String out = "";

  for (int k = 0; k < left; k++)
    out += ' ';

  out += txt;

  for (int k = 0; k < right; k++)
    out += ' ';

  return out;
}

// ======================================================
// LCD OPENING
// ======================================================

void lcdOpening() {
  lcdClear();

  lcdLine(0, "====================");
  lcdLine(1, centerText("MICROCELL"));
  lcdLine(2, centerText("Smart Monitoring"));
  lcdLine(3, "====================");

  delay(1500);
}

// ======================================================
// WIFI MANAGER
// ======================================================

bool connectWiFi() {
  Serial.println();
  Serial.println("================================");
  Serial.println("      MICROCELL WIFI MANAGER");
  Serial.println("================================");

  lcdClear();

  lcdLine(0, "[ WiFi Manager ]");
  lcdLine(1, "Mencari WiFi...");

  // --------------------------------------------------
  // BATAS WAKTU PORTAL 3 MENIT
  // --------------------------------------------------

  wifiManager.setConfigPortalTimeout(180);

  // --------------------------------------------------
  // ACCESS POINT KONFIGURASI
  // --------------------------------------------------

  // SSID : MICROCELL-SETUP
  // PASSWORD : microcell123

  bool connected = wifiManager.autoConnect(
    "MICROCELL-SETUP",
    "microcell123");

  if (!connected) {
    Serial.println();
    Serial.println("WiFi gagal terhubung.");

    lcdLine(2, "WiFi: GAGAL");
    lcdLine(3, "Restart ESP32...");

    delay(2000);

    ESP.restart();

    return false;
  }

  Serial.println();
  Serial.println("WiFi Connected");

  Serial.print("SSID : ");
  Serial.println(WiFi.SSID());

  Serial.print("IP   : ");
  Serial.println(WiFi.localIP());

  Serial.print("RSSI : ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  lcdClear();

  lcdLine(0, "[ WiFi Terhubung ]");
  lcdLine(1, "SSID: " + WiFi.SSID());
  lcdLine(2, "Status: Connected");
  lcdLine(3, "IP: " + WiFi.localIP().toString());

  delay(2000);

  return true;
}

// ======================================================
// FIREBASE CONNECTION
// ======================================================

bool connectFirebase() {
  lcdClear();

  lcdLine(0, "[ Firebase Login ]");
  lcdLine(1, "Menghubungkan...");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Waiting Firebase Login...");

  unsigned long start = millis();
  int dotCount = 0;

  while (
    !Firebase.ready() && millis() - start < FIREBASE_LOGIN_TIMEOUT_MS) {
    String dots = "";

    for (int d = 0; d < (dotCount % 4) + 1; d++)
      dots += '.';

    lcdLine(2, "Autentikasi" + dots);

    dotCount++;

    delay(500);
  }

  if (Firebase.ready()) {
    Serial.println();
    Serial.println("Firebase Connected");

    lcdLine(2, "Status: Ready!");
    lcdLine(3, "Memulai sistem...");

    delay(1500);

    return true;
  }

  Serial.println();
  Serial.println("Firebase Login Failed");

  lcdLine(2, "Status: GAGAL!");
  lcdLine(3, "Cek Kredensial");

  delay(1500);

  return false;
}

// ======================================================
// SYSTEM READY
// ======================================================

void lcdSystemReady() {
  lcdClear();

  lcdLine(0, "====================");
  lcdLine(1, centerText("SISTEM SIAP"));
  lcdLine(2, centerText("Memuat dashboard"));
  lcdLine(3, "====================");

  delay(1000);
}

// ======================================================
// WIFI LOST
// ======================================================

void lcdWifiLost() {
  lcdClear();

  lcdLine(0, "[ WiFi Terputus ]");
  lcdLine(1, "Mencoba reconnect...");
}

// ======================================================
// FIREBASE LOST
// ======================================================

void lcdFirebaseLost() {
  lcdClear();

  lcdLine(0, "[ Firebase Offline ]");
  lcdLine(1, "Menunggu koneksi...");
}

// ======================================================
// LCD MAIN SCREEN
// MENGAMBIL DATA DARI FIREBASE
// ======================================================

void displayMainScreen() {
  char buf[21];

  // ==================================================
  // BARIS 1
  // ==================================================

  snprintf(
    buf,
    sizeof(buf),
    "V:%6.2fV I:%5.0fmA",
    firebaseVoltage,
    firebaseCurrent);

  lcdLine(0, buf);

  // ==================================================
  // BARIS 2
  // ==================================================

  snprintf(
    buf,
    sizeof(buf),
    "P:%7.0fmW",
    firebasePower);

  lcdLine(1, buf);

  // ==================================================
  // BARIS 3
  // ==================================================

  snprintf(
    buf,
    sizeof(buf),
    "ACS:%5.2fA",
    firebaseAcsCurrent);

  lcdLine(2, buf);

  // ==================================================
  // BARIS 4
  // ==================================================

  snprintf(
    buf,
    sizeof(buf),
    "R1:%-3s R2:%-3s",
    relay1State ? "ON" : "OFF",
    relay2State ? "ON" : "OFF");

  lcdLine(3, buf);
}

// ======================================================
// READ SENSOR ASLI
// ======================================================

void readSensor() {
  // Probe I2C: cek apakah INA219 (0x40) masih merespon.
  // Keberadaan device di bus, bukan nilai current 0,
  // yang jadi indikator terputus
  I2C_INA.beginTransmission(0x40);

  bool present = (I2C_INA.endTransmission() == 0);

  if (!present) {
    if (ina219Ready) {
      ina219Ready = false;

      Serial.println("INA219 SENSOR ERROR / DISCONNECTED");
    }

    inaStatus = "disconnected";

    // Jangan perbarui voltage/current/power saat sensor tiada
    return;
  }

  // Sensor terdeteksi: inisialisasi ulang tanpa restart
  if (!ina219Ready) {
    ina219Ready = ina219.begin(&I2C_INA);

    if (!ina219Ready) {
      // Terdeteksi di bus tapi inisialisasi gagal
      inaStatus = "error";

      return;
    }

    Serial.println("INA219 terhubung kembali");
  }

  inaStatus = "connected";

  voltage = ina219.getBusVoltage_V();

  current = ina219.getCurrent_mA() / 1000.0;

  power = voltage * current;
}

// ======================================================
// READ ACS ASLI
// ======================================================

float readACSVoltage() {
  const int samples = 100;
  uint32_t totalMilliVolts = 0;

  for (int i = 0; i < samples; i++) {
    totalMilliVolts += analogReadMilliVolts(ACS_PIN);
    delayMicroseconds(200);
  }

  // Tegangan aktual pada pin ADC (Volt)
  float adcVoltage =
    (totalMilliVolts / (float)samples) / 1000.0;

  // Rekonstruksi tegangan output asli ACS712
  return adcVoltage * ACS_DIVIDER_GAIN;
}

// ======================================================
// CALIBRATION ACS712
// Jalankan ketika tidak ada arus pada jalur sensor
// ======================================================

void calibrateACSZero() {
  const int samples = 500;
  float totalVoltage = 0;

  delay(500);

  for (int i = 0; i < samples; i++) {
    totalVoltage += readACSVoltage();
    delay(2);
  }

  float measuredZero = totalVoltage / samples;

  // Pin floating saat OUT terlepas memberi nilai di luar rentang
  // fisik output ACS712 (suplai 5V) - offset tidak diadopsi
  if (measuredZero > 0.05 && measuredZero < 4.7) {
    acsZeroVoltage = measuredZero;

    Serial.println("===== ACS712 CALIBRATION =====");
    Serial.print("Zero Current Voltage: ");
    Serial.print(acsZeroVoltage, 3);
    Serial.println(" V");
    Serial.println("==============================");
  } else {
    Serial.println("ACS712 SENSOR ERROR / OUTPUT INVALID");
    Serial.println("Kalibrasi offset dibatalkan, memakai default 2.5V");
  }
}

// ======================================================
// READ ACS712
// ======================================================

void readACS() {
  const int samples = 3;
  float totalVoltage = 0;
  float minVoltage = 999.0;
  float maxVoltage = -999.0;

  // Beberapa pengambilan terpisah untuk mengukur kestabilan sinyal
  for (int i = 0; i < samples; i++) {
    float v = readACSVoltage();

    totalVoltage += v;

    if (v < minVoltage)
      minVoltage = v;

    if (v > maxVoltage)
      maxVoltage = v;
  }

  float meanVoltage = totalVoltage / samples;

  acsRaw = analogRead(ACS_PIN);

  // ==================================================
  // VALIDASI PEMBACAAN
  // 1. Rentang fisik: output ACS712 bersuplai 5V tidak
  //    mungkin di bawah ~0V atau di atas ~4.7V.
  //    Pin ADC floating cenderung menempel ke rail ADC
  //    (derekonstruksi > 4.7V) atau liar mendekati 0V.
  // 2. Kestabilan: kabel terpasang memberi bacaan yang
  //    konsisten antar sampel; pin floating loncat-loncat.
  // ==================================================

  bool inRange = (meanVoltage > 0.05 && meanVoltage < 4.7);
  bool stable = ((maxVoltage - minVoltage) < 0.15);
  bool valid = inRange && stable;

  // ==================================================
  // DEBOUNCE STATUS
  // Satu pembacaan tidak boleh langsung mengubah status;
  // pin floating kadang terlihat normal beberapa saat,
  // jadi perlu pembacaan beruntun untuk berpindah status.
  // Nilai global hanya diperbarui saat status confirmed.
  // ==================================================

  static int goodStreak = 0;
  static int badStreak = 0;
  const char *prevStatus = acsStatus;

  if (valid) {
    goodStreak++;
    badStreak = 0;

    if (goodStreak >= ACS_GOOD_CONFIRM_READS) {
      acsStatus = "connected";
      acsVoltage = meanVoltage;

      // Hitung arus berdasarkan tegangan offset
      acsCurrent =
        (acsVoltage - acsZeroVoltage) / ACS_SENSITIVITY;

      // Hilangkan noise arus yang sangat kecil
      if (fabs(acsCurrent) < 0.05) {
        acsCurrent = 0;
      }
    }
  } else {
    badStreak++;
    goodStreak = 0;

    if (badStreak >= ACS_BAD_CONFIRM_READS) {
      if (inRange) {
        // Masih dalam rentang fisik tapi sinyal tidak stabil
        acsStatus = "error";
      } else {
        // Di luar rentang fisik: OUT terlepas / pin floating
        acsStatus = "disconnected";
      }
    }
  }

  acsValid = (strcmp(acsStatus, "connected") == 0);

  if (strcmp(acsStatus, prevStatus) != 0) {
    if (strcmp(prevStatus, "connected") == 0) {
      Serial.println("ACS712 SENSOR ERROR / OUTPUT INVALID");

      Serial.print("  Voltage: ");
      Serial.print(meanVoltage, 3);
      Serial.print(" V (min ");
      Serial.print(minVoltage, 3);
      Serial.print(" / max ");
      Serial.print(maxVoltage, 3);
      Serial.println(" V)");
    }

    Serial.print("ACS712 status: ");
    Serial.print(prevStatus);
    Serial.print(" -> ");
    Serial.println(acsStatus);
  }
}


// ======================================================
// UPLOAD DATA SENSOR KE FIREBASE
// ======================================================

void uploadSensor() {
  if (
    millis() - lastSensorUploadMillis < SENSOR_UPLOAD_INTERVAL_MS) {
    return;
  }

  lastSensorUploadMillis = millis();

  if (!Firebase.ready())
    return;

  // Baca sensor asli terlebih dahulu
  readSensor();
  readACS();

  // ==================================================
  // UPLOAD INA219
  // Nilai aktual hanya saat connected. Saat disconnected,
  // field nilai dihapus (null) dan status diperbarui.
  // Status hanya ditulis saat berubah.
  // ==================================================

  if (strcmp(inaStatus, "connected") == 0) {
    if (
      !Firebase.RTDB.setFloat(
        &fbdo,
        "/devices/main/sensors/ina1/voltage",
        voltage)) {
      Serial.print("Upload Voltage Error: ");
      Serial.println(fbdo.errorReason());
    }

    if (
      !Firebase.RTDB.setFloat(
        &fbdo,
        "/devices/main/sensors/ina1/current",
        current * 1000.0)) {
      Serial.print("Upload Current Error: ");
      Serial.println(fbdo.errorReason());
    }

    if (
      !Firebase.RTDB.setFloat(
        &fbdo,
        "/devices/main/sensors/ina1/power",
        power * 1000.0)) {
      Serial.print("Upload Power Error: ");
      Serial.println(fbdo.errorReason());
    }
  }

  if (lastInaStatus != inaStatus) {
    if (strcmp(inaStatus, "disconnected") == 0) {
      if (
        !Firebase.RTDB.deleteNode(
          &fbdo,
          "/devices/main/sensors/ina1/voltage")) {
        Serial.print("Delete INA Voltage Error: ");
        Serial.println(fbdo.errorReason());
      }

      if (
        !Firebase.RTDB.deleteNode(
          &fbdo,
          "/devices/main/sensors/ina1/current")) {
        Serial.print("Delete INA Current Error: ");
        Serial.println(fbdo.errorReason());
      }

      if (
        !Firebase.RTDB.deleteNode(
          &fbdo,
          "/devices/main/sensors/ina1/power")) {
        Serial.print("Delete INA Power Error: ");
        Serial.println(fbdo.errorReason());
      }
    }

    if (
      !Firebase.RTDB.setString(
        &fbdo,
        "/devices/main/sensors/ina1/status",
        inaStatus)) {
      Serial.print("Upload INA Status Error: ");
      Serial.println(fbdo.errorReason());
    }

    lastInaStatus = inaStatus;
  }

  // ==================================================
  // UPLOAD ACS
  // Nilai aktual hanya saat connected. Saat disconnected,
  // field nilai dihapus (null) dan status diperbarui.
  // Status hanya ditulis saat berubah.
  // ==================================================

  if (acsValid) {
    if (
      !Firebase.RTDB.setFloat(
        &fbdo,
        "/devices/main/sensors/acs1/current",
        acsCurrent)) {
      Serial.print("Upload ACS Current Error: ");
      Serial.println(fbdo.errorReason());
    }

    if (
      !Firebase.RTDB.setFloat(
        &fbdo,
        "/devices/main/sensors/acs1/voltage",
        acsVoltage)) {
      Serial.print("Upload ACS Voltage Error: ");
      Serial.println(fbdo.errorReason());
    }
  }

  if (lastAcsStatus != acsStatus) {
    if (strcmp(acsStatus, "disconnected") == 0) {
      if (
        !Firebase.RTDB.deleteNode(
          &fbdo,
          "/devices/main/sensors/acs1/current")) {
        Serial.print("Delete ACS Current Error: ");
        Serial.println(fbdo.errorReason());
      }

      if (
        !Firebase.RTDB.deleteNode(
          &fbdo,
          "/devices/main/sensors/acs1/voltage")) {
        Serial.print("Delete ACS Voltage Error: ");
        Serial.println(fbdo.errorReason());
      }
    }

    if (
      !Firebase.RTDB.setString(
        &fbdo,
        "/devices/main/sensors/acs1/status",
        acsStatus)) {
      Serial.print("Upload ACS Status Error: ");
      Serial.println(fbdo.errorReason());
    }

    lastAcsStatus = acsStatus;
  }
}

// ======================================================
// READ DATA DARI FIREBASE
// DATA INI DIGUNAKAN LCD
// ======================================================

void readFirebaseSensor() {
  if (
    millis() - lastFirebaseReadMillis < FIREBASE_READ_INTERVAL_MS) {
    return;
  }

  lastFirebaseReadMillis = millis();

  if (!Firebase.ready())
    return;

  // Node nilai yang sudah di-null-kan (sensor disconnected)
  // tidak dibaca, agar tidak spam error ke Serial
  bool readIna = (strcmp(inaStatus, "connected") == 0);
  bool readAcs = (strcmp(acsStatus, "connected") == 0);

  // ==================================================
  // VOLTAGE
  // ==================================================

  if (readIna) {
    if (
      Firebase.RTDB.getFloat(
        &fbdo,
        "/devices/main/sensors/ina1/voltage")) {
      firebaseVoltage = fbdo.floatData();
    } else {
      Serial.print("Read Voltage Error: ");
      Serial.println(fbdo.errorReason());
    }
  }

  // ==================================================
  // CURRENT
  // ==================================================

  if (readIna) {
    if (
      Firebase.RTDB.getFloat(
        &fbdo,
        "/devices/main/sensors/ina1/current")) {
      firebaseCurrent = fbdo.floatData();
    } else {
      Serial.print("Read Current Error: ");
      Serial.println(fbdo.errorReason());
    }
  }

  // ==================================================
  // POWER
  // ==================================================

  if (readIna) {
    if (
      Firebase.RTDB.getFloat(
        &fbdo,
        "/devices/main/sensors/ina1/power")) {
      firebasePower = fbdo.floatData();
    } else {
      Serial.print("Read Power Error: ");
      Serial.println(fbdo.errorReason());
    }
  }

  // ==================================================
  // ACS CURRENT
  // ==================================================

  if (readAcs) {
    if (
      Firebase.RTDB.getFloat(
        &fbdo,
        "/devices/main/sensors/acs1/current")) {
      firebaseAcsCurrent = fbdo.floatData();
    } else {
      Serial.print("Read ACS Current Error: ");
      Serial.println(fbdo.errorReason());
    }
  }

  // ==================================================
  // ACS VOLTAGE
  // ==================================================

  if (readAcs) {
    if (
      Firebase.RTDB.getFloat(
        &fbdo,
        "/devices/main/sensors/acs1/voltage")) {
      firebaseAcsVoltage = fbdo.floatData();
    } else {
      Serial.print("Read ACS Voltage Error: ");
      Serial.println(fbdo.errorReason());
    }
  }
}

// ======================================================
// SERIAL STATUS
// ======================================================

void printConnectionIndicator() {
  if (
    millis() - lastStatusPrintMillis < STATUS_PRINT_INTERVAL_MS) {
    return;
  }

  lastStatusPrintMillis = millis();

  bool wifiOk =
    WiFi.status() == WL_CONNECTED;

  bool fbOk =
    Firebase.ready();

  Serial.print("[STATUS] WiFi: ");
  Serial.print(
    wifiOk ? "CONNECTED" : "DISCONNECTED");

  Serial.print(" | SSID: ");

  if (wifiOk)
    Serial.print(WiFi.SSID());
  else
    Serial.print("-");

  Serial.print(" | Firebase: ");
  Serial.print(
    fbOk ? "CONNECTED" : "DISCONNECTED");

  Serial.print(" | INA219: ");
  Serial.println(
    ina219Ready ? "OK" : "ERROR");

  // ==================================================
  // SENSOR ASLI
  // ==================================================

  Serial.print("Sensor Voltage: ");
  Serial.print(voltage, 3);

  Serial.print(" V | Sensor Current: ");
  Serial.print(current * 1000.0, 2);

  Serial.print(" mA | Sensor Power: ");
  Serial.print(power * 1000.0, 2);

  Serial.println(" mW");

  Serial.print("ACS RAW: ");
  Serial.print(acsRaw);

  Serial.print(" | ACS Voltage: ");
  Serial.print(acsVoltage, 3);

  Serial.print(" V | ACS Current: ");
  Serial.print(acsCurrent, 3);

  Serial.println(" A");

  // ==================================================
  // DATA FIREBASE
  // ==================================================

  Serial.print("Firebase Voltage: ");
  Serial.print(firebaseVoltage, 3);

  Serial.print(" V | Firebase Current: ");
  Serial.print(firebaseCurrent, 2);

  Serial.print(" mA | Firebase Power: ");
  Serial.print(firebasePower, 2);

  Serial.println(" mW");

  Serial.print("Firebase ACS Current: ");
  Serial.print(firebaseAcsCurrent, 3);

  Serial.print(" A | Firebase ACS Voltage: ");
  Serial.print(firebaseAcsVoltage, 3);

  Serial.println(" V");
}

// ======================================================
// HANDLE RELAY
// ======================================================

void handleRelay(
  const char *relayPath,
  const char *statusPath,
  int pin,
  bool &stateOut) {
  if (
    Firebase.RTDB.getBool(
      &fbdo,
      relayPath)) {
    bool newState =
      fbdo.boolData();

    if (newState != stateOut) {
      Serial.print(relayPath);
      Serial.print(" -> ");
      Serial.println(
        newState ? "ON" : "OFF");
    }

    stateOut = newState;

    digitalWrite(
      pin,
      newState ? LOW : HIGH);

    Firebase.RTDB.setBool(
      &fbdo,
      statusPath,
      newState);
  } else {
    Serial.print("Relay Error (");
    Serial.print(relayPath);
    Serial.print(") : ");
    Serial.println(
      fbdo.errorReason());
  }
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);

  delay(500);

  // ==================================================
  // ADC
  // ==================================================

  analogReadResolution(12);

  analogSetPinAttenuation(ACS_PIN, ADC_11db);

  pinMode(ACS_PIN, INPUT);
  calibrateACSZero();

  // ==================================================
  // I2C LCD
  // ==================================================

  I2C_LCD.begin(
    LCD_SDA_PIN,
    LCD_SCL_PIN,
    100000);

  // ==================================================
  // I2C INA219
  // ==================================================

  I2C_INA.begin(
    INA_SDA_PIN,
    INA_SCL_PIN,
    100000);

  // ==================================================
  // LCD
  // ==================================================

  lcdInit();

  lcdOpening();

  // ==================================================
  // RELAY
  // ==================================================

  pinMode(
    RELAY1_PIN,
    OUTPUT);

  pinMode(
    RELAY2_PIN,
    OUTPUT);

  // ==================================================
  // ACS
  // ==================================================

  pinMode(
    ACS_PIN,
    INPUT);

  // ==================================================
  // RELAY OFF
  // ==================================================

  digitalWrite(
    RELAY1_PIN,
    HIGH);

  digitalWrite(
    RELAY2_PIN,
    HIGH);

  // ==================================================
  // CHECK INA219
  // ==================================================

  lcdClear();

  lcdLine(
    0,
    "[ Inisialisasi ]");

  lcdLine(
    1,
    "Cek INA219...");

  ina219Ready =
    ina219.begin(
      &I2C_INA);

  if (ina219Ready) {
    Serial.println("INA219 OK");

    lcdLine(
      2,
      "INA219: OK");
  } else {
    Serial.println("INA219 ERROR");

    lcdLine(
      2,
      "INA219: ERROR");
  }

  lcdLine(
    3,
    "LCD: OK");

  delay(1500);

  // ==================================================
  // READ SENSOR ASLI
  // ==================================================

  readACS();
  Serial.println("===== ACS712 DEBUG =====");

  Serial.print("ADC Raw       : ");
  Serial.println(acsRaw);

  Serial.print("Output Voltage: ");
  Serial.print(acsVoltage, 3);
  Serial.println(" V");

  Serial.print("Zero Voltage  : ");
  Serial.print(acsZeroVoltage, 3);
  Serial.println(" V");

  Serial.print("Current       : ");
  Serial.print(acsCurrent, 3);
  Serial.println(" A");

  Serial.println("========================");

  Serial.println("ACS OK");

  Serial.print("ACS RAW: ");
  Serial.println(acsRaw);

  Serial.print("ACS Voltage: ");
  Serial.print(acsVoltage, 3);

  Serial.println(" V");

  Serial.print("ACS Current: ");
  Serial.print(acsCurrent, 3);

  Serial.println(" A");

  // ==================================================
  // WIFI MANAGER
  // ==================================================

  if (!connectWiFi()) {
    return;
  }

  wifiWasConnected = true;

  // ==================================================
  // FIREBASE
  // ==================================================

  while (!connectFirebase()) {
    delay(300);
  }

  firebaseWasReady = true;

  // ==================================================
  // SYSTEM READY
  // ==================================================

  lcdSystemReady();

  // ==================================================
  // READ SENSOR ASLI
  // ==================================================

  readSensor();
  readACS();

  // ==================================================
  // FORCE FIRST FIREBASE UPDATE
  // ==================================================

  lastSensorUploadMillis =
    millis() - SENSOR_UPLOAD_INTERVAL_MS;

  lastFirebaseReadMillis =
    millis() - FIREBASE_READ_INTERVAL_MS;

  // ==================================================
  // UPLOAD DATA AWAL
  // ==================================================

  uploadSensor();

  // ==================================================
  // READ FIREBASE
  // ==================================================

  readFirebaseSensor();
}

// ======================================================
// LOOP
// ======================================================

void loop() {
  // ==================================================
  // STATUS
  // ==================================================

  printConnectionIndicator();

  // ==================================================
  // WIFI TERPUTUS
  // ==================================================

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      lcdWifiLost();

      wifiWasConnected = false;
    }

    // ----------------------------------------------
    // Coba reconnect menggunakan kredensial tersimpan
    // ----------------------------------------------

    WiFi.reconnect();

    delay(1000);

    if (WiFi.status() == WL_CONNECTED) {
      wifiWasConnected = true;

      Serial.println(
        "WiFi reconnect berhasil");

      lcdSystemReady();
    }

    return;
  }

  // ==================================================
  // FIREBASE
  // ==================================================

  if (!Firebase.ready()) {
    if (firebaseWasReady) {
      lcdFirebaseLost();

      firebaseWasReady = false;
    }

    return;
  }

  if (!firebaseWasReady) {
    firebaseWasReady = true;

    lcdSystemReady();
  }

  // ==================================================
  // RELAY
  // ==================================================

  if (
    millis() - lastRelayCheckMillis >= RELAY_CHECK_INTERVAL_MS) {
    lastRelayCheckMillis = millis();

    handleRelay(
      "/devices/main/relays/relay1",
      "/devices/main/status/relay1",
      RELAY1_PIN,
      relay1State);

    handleRelay(
      "/devices/main/relays/relay2",
      "/devices/main/status/relay2",
      RELAY2_PIN,
      relay2State);
  }

  // ==================================================
  // SENSOR ASLI
  // ==================================================

  readSensor();
  readACS();

  // ==================================================
  // SENSOR → FIREBASE
  // ==================================================

  uploadSensor();

  // ==================================================
  // FIREBASE → ESP32
  // ==================================================

  readFirebaseSensor();

  // ==================================================
  // FIREBASE → LCD
  // ==================================================

  if (
    millis() - lastLcdUpdateMillis >= LCD_UPDATE_INTERVAL_MS) {
    lastLcdUpdateMillis = millis();

    displayMainScreen();
  }
}