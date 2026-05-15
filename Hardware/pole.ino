#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <Wire.h>
#include <SparkFun_ENS160.h>
#include <Adafruit_AHTX0.h>
#include <LiquidCrystal_I2C.h>

/* ---------- LCD ---------- */
LiquidCrystal_I2C lcd(0x27, 16, 4);

/* ---------- SECRETS (DO NOT COMMIT REAL VALUES TO GITHUB) ---------- */
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define API_KEY    "YOUR_API_KEY"
#define PROJECT_ID "YOUR_PROJECT_ID"

/* ---------- Sensors ---------- */
SparkFun_ENS160 ens160;
Adafruit_AHTX0 aht;

/* ---------- MQ135 SETTINGS ---------- */
#define MQ135_PIN 4
// Hardware & Calibration Parameters
const float R_load = 10.0;       // Physical 10kΩ resistor on the module
const float R0 = 33.0;           // Your calibrated baseline resistance in clean air
// MQ135 Datasheet Curve Parameters for General Gas / Smoke / Ammonia
const float a = 102.2;           // Scaling number from the datasheet curve
const float b = 2.473;           // Slope number from the datasheet curve

/* ---------- OPTICAL DUST SENSOR SETTINGS (GP2Y1010AU0F) ---------- */
// You may need to change these pins depending on your ESP32 wiring
#define DUST_LED_PIN 32  
#define DUST_OUT_PIN 33  

/* ---------- RESET BUTTON SETTINGS ---------- */
#define RESET_BTN_PIN 14  // Connect Button between GPIO 14 and GND

/* ---------- Timing ---------- */
unsigned long lastRead = 0;
const unsigned long READ_INTERVAL = 60000; // 1 minute
String idToken = "";

/* ---------- MQ135: General Pollution % ---------- */
int readGasPollutionPercentage() {
  int rawValue = analogRead(MQ135_PIN);
  float V_pin = rawValue * (3.3 / 4095.0);
  float V_true = V_pin * 2.0;

  if (V_true >= 5.0 || V_true <= 0) {
    return 0; 
  }

  float Rs = R_load * ((5.0 - V_true) / V_true);
  float ratio = Rs / R0;
  float ppm = a * pow(ratio, -b);
  float percentage = (ppm / 10.0) * 100.0;

  if (percentage < 0) percentage = 0;
  if (percentage > 100) percentage = 100;

  return (int)percentage; 
}

/* ---------- Real Dust Density (GP2Y1010AU0F) ---------- */
float readDustDensity() {
  // The sensor requires a specific pulsing of its internal LED to read the dust particles
  digitalWrite(DUST_LED_PIN, LOW); // Power on the LED
  delayMicroseconds(280);
  
  int voMeasured = analogRead(DUST_OUT_PIN); // Read the dust value
  
  delayMicroseconds(40);
  digitalWrite(DUST_LED_PIN, HIGH); // Turn the LED off
  delayMicroseconds(9680);

  // Convert analog reading to voltage (ESP32 is 12-bit ADC, 3.3V logic)
  float calcVoltage = voMeasured * (3.3 / 4095.0);
  
  // Linear equation from GP2Y1010AU0F datasheet to calculate mg/m3
  float dustDensity = 0.17 * calcVoltage - 0.1;

  if (dustDensity < 0) {
    dustDensity = 0.00; // Prevent negative readings in very clean air
  }
  
  return dustDensity; 
}

/* ---------- Time Utilities ---------- */
String isoTimestampUTC() {
  time_t now;
  time(&now);
  struct tm utc;
  gmtime_r(&now, &utc);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buf);
}

String localTimestamp() {
  struct tm t;
  getLocalTime(&t);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String minuteDocId() {
  struct tm t;
  getLocalTime(&t);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M", &t);
  return String(buf);
}

/* ---------- Firebase Auth ---------- */
void getToken() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client,
    "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=" API_KEY);
  http.addHeader("Content-Type", "application/json");

  if (http.POST("{}") > 0) {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, http.getString());
    idToken = doc["idToken"].as<String>();
  }
  http.end();
}

/* ---------- Firestore Write ---------- */
void sendToFirestore(
  float t, float h,
  int aqi, int eco2, int tvoc,
  float dust, int gasPercent
) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url =
    "https://firestore.googleapis.com/v1/projects/" PROJECT_ID
    "/databases/(default)/documents/sensor_data/esp32/readings/"
    + minuteDocId()
    + "?access_token=" + idToken;

  StaticJsonDocument<768> doc;

  doc["fields"]["temperature"]["doubleValue"] = t;
  doc["fields"]["humidity"]["doubleValue"] = h;
  doc["fields"]["aqi"]["integerValue"] = aqi;
  doc["fields"]["eco2"]["integerValue"] = eco2;
  doc["fields"]["tvoc"]["integerValue"] = tvoc;
  doc["fields"]["dust"]["doubleValue"] = dust;
  doc["fields"]["gas_pollution_percent"]["integerValue"] = gasPercent;
  doc["fields"]["timestamp"]["timestampValue"] = isoTimestampUTC();

  String json;
  serializeJson(doc, json);

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest("PATCH", json);
  Serial.print("Firestore HTTP: ");
  Serial.println(code);
  http.end();
}

/* ---------- Setup ---------- */
void setup() {
  Serial.begin(115200);

  // Setup Reset Button Pin
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  
  // Setup Dust Sensor Pins
  pinMode(DUST_LED_PIN, OUTPUT);
  pinMode(DUST_OUT_PIN, INPUT);
  digitalWrite(DUST_LED_PIN, HIGH); // Sensor LED defaults to OFF

  Wire.begin(21, 20);

  /* ---- LCD ---- */
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Init...");
  lcd.setCursor(0, 1);
  lcd.print("WiFi Connecting");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  lcd.command(0x90);
  lcd.print("WiFi Connected");

  configTime(19800, 0, "pool.ntp.org");
  getToken();

  aht.begin();
  ens160.begin();
  ens160.setOperatingMode(SFE_ENS160_STANDARD);

  delay(2000);
  lcd.clear();

  lastRead = millis() - READ_INTERVAL;
}

/* ---------- Loop ---------- */
void loop() {
  // ---- RESET BUTTON CHECK ----
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    Serial.println("Reset Button Pressed!");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("System Reset...");
    delay(1000); 
    ESP.restart(); 
  }
  // --------------------------------

  if (idToken == "") return;

  if (millis() - lastRead >= READ_INTERVAL) {
    lastRead = millis();

    /* ---- 1. READ SENSORS ---- */
    
    // Get real dust reading
    float dust = readDustDensity();

    // Temp & Hum Reading
    sensors_event_t hum, temp;
    aht.getEvent(&hum, &temp);

    // ENS160 Reading
    int aqi  = ens160.getAQI();
    int eco2 = ens160.getECO2(); 
    int tvoc = ens160.getTVOC();

    // MQ135 Reading
    int gasPercent = readGasPollutionPercentage();

    /* ---- 2. UPDATE LCD ---- */
    lcd.clear(); 

    // Row 0
    lcd.setCursor(0, 0);
    lcd.print("T:"); lcd.print(temp.temperature, 1);
    lcd.print("C H:"); lcd.print(hum.relative_humidity, 0); lcd.print("%");

    // Row 1
    lcd.setCursor(0, 1);
    lcd.print("AQI:"); lcd.print(aqi);
    lcd.print(" Dst:"); lcd.print(dust, 3); 

    // Row 2
    lcd.command(0x90);
    lcd.print("CO2: "); lcd.print(eco2); lcd.print(" ppm");

    // Row 3
    lcd.command(0xD0);
    lcd.print("Gas: "); lcd.print(gasPercent); lcd.print("%");
    lcd.print(" VOC:"); lcd.print(tvoc);

    /* ---- 3. SERIAL MONITOR DEBUG ---- */
    Serial.println("-----------------------");
    Serial.println(localTimestamp());

    /* ---- 4. SEND TO FIRESTORE ---- */
    sendToFirestore(
      temp.temperature,
      hum.relative_humidity,
      aqi,
      eco2,
      tvoc,
      dust,
      gasPercent
    );
  }
}