#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <Wire.h>
#include <SparkFun_ENS160.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/* ---------- OLED Display ---------- */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C 

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

/* ---------- WiFi ---------- */
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

/* ---------- Firebase ---------- */
#define API_KEY    "YOUR_API_KEY"
#define PROJECT_ID "YOUR_PROJECT_ID"

/* ---------- Battery Monitoring ---------- */
#define BATTERY_PIN 6  // GPIO6 (ADC1_CH7) for battery voltage divider
#define ADC_RESOLUTION 4095.0  // 12-bit ADC
#define ADC_REF_VOLTAGE 3.3    // Reference voltage in volts
#define VOLTAGE_DIVIDER_RATIO 6.639  // Two equal resistors = ratio of 2

// Battery voltage thresholds (adjust based on your battery type)
// For LiPo: 4.2V = 100%, 3.7V = ~50%, 3.0V = 0%
#define BATTERY_MAX_VOLTAGE 4.2
#define BATTERY_MIN_VOLTAGE 3.2

/* ---------- Touch Sensor for Deep Sleep ---------- */
#define TOUCH_PIN 13  // GPIO 13 for TTP223 touch sensor

/* ---------- Sensors ---------- */
SparkFun_ENS160 ens160;
Adafruit_AHTX0 aht;

/* ---------- Timing ---------- */
unsigned long lastRead = 0;
const unsigned long READ_INTERVAL = 60000;

/* ---------- Touch Control ---------- */
unsigned long lastTouchTime = 0;
const unsigned long TOUCH_DEBOUNCE = 500;  // 500ms debounce
int lastTouchState = LOW;

/* ---------- RTC Memory (survives deep sleep) ---------- */
RTC_DATA_ATTR int bootCount = 0;  // Counts how many times we've woken up

String idToken = "";

/* ---------- Battery Functions ---------- */

// Read battery voltage from ADC
float readBatteryVoltage() {
  // Take multiple readings for accuracy
  int adcSum = 0;
  const int numReadings = 10;
  
  for (int i = 0; i < numReadings; i++) {
    adcSum += analogRead(BATTERY_PIN);
    delay(10);
  }
  
  int adcValue = adcSum / numReadings;
  
  // Convert ADC value to voltage at the divider (half of battery voltage)
  float dividerVoltage = (adcValue / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
  
  // Calculate actual battery voltage (multiply by divider ratio)
  float batteryVoltage = dividerVoltage * VOLTAGE_DIVIDER_RATIO;
  
  return batteryVoltage;
}

// Calculate battery percentage
int getBatteryPercentage(float voltage) {
  if (voltage >= BATTERY_MAX_VOLTAGE) return 100;
  if (voltage <= BATTERY_MIN_VOLTAGE) return 0;
  
  // Linear interpolation between min and max
  float percentage = ((voltage - BATTERY_MIN_VOLTAGE) / 
                     (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE)) * 100.0;
  
  return constrain((int)percentage, 0, 100);
}

// Get battery icon character based on percentage
String getBatteryIcon(int percentage) {
  if (percentage >= 80) return "====";
  else if (percentage >= 60) return "=== ";
  else if (percentage >= 40) return "==  ";
  else if (percentage >= 20) return "=   ";
  else return "!   "; // Low battery warning
}

/* ---------- Time ---------- */
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

/* ---------- Run Score Calculation ---------- */

// Convert eCO2 to score (0-100)
float getECO2Score(int eco2) {
  // Lower CO2 is better
  // < 400 ppm = 100, 400-1000 = linear decrease, > 5000 = 0
  if (eco2 < 400) return 100.0;
  if (eco2 > 5000) return 0.0;
  return max(0.0, 100.0 - ((eco2 - 400) / 46.0));
}

// Convert TVOC to score (0-100)
float getTVOCScore(int tvoc) {
  // Lower TVOC is better
  // < 65 ppb = 100, 65-220 = good, 220-660 = moderate, > 2200 = 0
  if (tvoc < 65) return 100.0;
  if (tvoc > 2200) return 0.0;
  return max(0.0, 100.0 - ((tvoc - 65) / 21.35));
}

// Convert Temperature to score (0-100)
float getTempScore(float temp) {
  // Optimal running temp: 10-20°C
  // 10-20°C = 100, decrease outside this range
  if (temp >= 10 && temp <= 20) return 100.0;
  if (temp < 0 || temp > 40) return 0.0;
  
  if (temp < 10) {
    // 0°C = 50, 10°C = 100
    return 50.0 + (temp * 5.0);
  } else {
    // 20°C = 100, 40°C = 0
    return max(0.0, 100.0 - ((temp - 20) * 5.0));
  }
}

// Convert Humidity to score (0-100)
float getHumScore(float hum) {
  // Optimal humidity: 40-60%
  // 40-60% = 100, decrease outside this range
  if (hum >= 40 && hum <= 60) return 100.0;
  if (hum < 10 || hum > 90) return 0.0;
  
  if (hum < 40) {
    // 10% = 0, 40% = 100
    return (hum - 10) * 3.33;
  } else {
    // 60% = 100, 90% = 0
    return max(0.0, 100.0 - ((hum - 60) * 3.33));
  }
}

// Calculate Runner AQI (0-100)
float calculateRunnerAQI(int eco2, int tvoc, float temp, float hum) {
  float eco2_score = getECO2Score(eco2);
  float tvoc_score = getTVOCScore(tvoc);

  // 2. DOMINANT POLLUTANT: Health is dictated strictly by the WORST metric
  float health_aqi = min(eco2_score, tvoc_score);

  float temp_score = getTempScore(temp);
  float hum_score = getHumScore(hum);

  // Comfort is a blend of heat and humidity
  float comfort_score = (temp_score * 0.6) + (hum_score * 0.4); 
  
  // 4. Apply Comfort as a Scaling Penalty
  // Poor comfort degrades a runner's performance even in perfect air,
  // but perfect comfort cannot magically fix toxic air.
  // This multiplier ranges from 0.5 (terrible comfort) to 1.0 (perfect comfort).
  float comfort_multiplier = 0.5 + (comfort_score / 200.0);
  
  float runner_aqi = health_aqi * comfort_multiplier;

  return runner_aqi;
}

// Get status based on Runner AQI
String getStatus(float score) {
  if (score >= 85) return "SAFE";
  else if (score >= 70) return "LOW RISK";
  else if (score >= 50) return "CAUTION";
  else if (score >= 30) return "UNSAFE";
  else return "DANGEROUS";
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
  float runner_aqi, float batteryVoltage, int batteryPercent
) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url =
    "https://firestore.googleapis.com/v1/projects/" PROJECT_ID
    "/databases/(default)/documents/sensor_data/esp32_0/readings/"
    + minuteDocId()
    + "?access_token=" + idToken;

  StaticJsonDocument<512> doc;

  doc["fields"]["temperature"]["doubleValue"] = t;
  doc["fields"]["humidity"]["doubleValue"] = h;
  doc["fields"]["aqi"]["integerValue"] = aqi;
  doc["fields"]["eco2"]["integerValue"] = eco2;
  doc["fields"]["tvoc"]["integerValue"] = tvoc;
  doc["fields"]["runner_aqi"]["doubleValue"] = runner_aqi;
  doc["fields"]["battery_voltage"]["doubleValue"] = batteryVoltage;
  doc["fields"]["battery_percent"]["integerValue"] = batteryPercent;
  doc["fields"]["timestamp"]["stringValue"] = isoTimestamp();

  String json;
  serializeJson(doc, json);

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.sendRequest("PATCH", json);
  http.end();
}

/* ---------- Draw Battery Icon (top-right) ---------- */
void drawBatteryIcon(int percent) {
  // Battery body: 22x10 px, top-right corner (x=104, y=1)
  const int bx = 104, by = 1, bw = 22, bh = 10;

  // Outer rectangle
  display.drawRect(bx, by, bw, bh, SSD1306_WHITE);

  // Battery tip (nub) on the right side
  display.fillRect(bx + bw, by + 3, 2, 4, SSD1306_WHITE);

  // Fill level (leave 1px padding inside)
  int fillW = map(constrain(percent, 0, 100), 0, 100, 0, bw - 4);
  if (fillW > 0) {
    display.fillRect(bx + 2, by + 2, fillW, bh - 4, SSD1306_WHITE);
  }
}

/* ---------- Display Runner Dashboard ---------- */
void drawRunnerDashboard(float runScore, float temp, float hum, int eco2, int tvoc, 
                        float batteryVoltage, int batteryPercent) {
  display.clearDisplay();

  // ========== TOP RIGHT: Battery icon + percentage ==========
  // Percentage text (right-aligned before icon)
  display.setTextSize(1);
  char batStr[5];
  snprintf(batStr, sizeof(batStr), "%d%%", batteryPercent);
  int textX = 100 - (strlen(batStr) * 6);  // 6px per char at textSize 1
  display.setCursor(textX, 2);
  display.print(batStr);

  // Draw the battery icon
  drawBatteryIcon(batteryPercent);

  // Separator line
  display.drawFastHLine(0, 13, SCREEN_WIDTH, SSD1306_WHITE);
  
  // ========== MAIN SECTION: Run Score ==========
  display.setTextSize(1);
  display.setCursor(2, 16);
  display.print("Run Score");

  // Large score number
  display.setTextSize(3);
  int scoreInt = (int)runScore;
  if (scoreInt < 10) {
    display.setCursor(50, 26);
  } else if (scoreInt < 100) {
    display.setCursor(40, 26);
  } else {
    display.setCursor(30, 26);
  }
  display.print(scoreInt);
    
  // ========== BOTTOM: Status ==========
  display.setTextSize(1);
  display.setCursor(2, 50);
  display.print("Status: ");
  
  String status = getStatus(runScore);
  display.setTextSize(1);
  display.setCursor(50, 50);
  display.print(status);
  
  // Progress bar
  int barWidth = map(runScore, 0, 100, 10, 126);
  display.fillRect(2, 62, barWidth, 2, SSD1306_WHITE);
  
  display.display();
}

/* ---------- Deep Sleep Functions ---------- */

// Print the wakeup reason
void printWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0: 
      Serial.println("Wakeup caused by external signal using RTC_IO (Touch Sensor)");
      break;
    case ESP_SLEEP_WAKEUP_EXT1: 
      Serial.println("Wakeup caused by external signal using RTC_CNTL");
      break;
    case ESP_SLEEP_WAKEUP_TIMER: 
      Serial.println("Wakeup caused by timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: 
      Serial.println("Wakeup caused by touchpad");
      break;
    case ESP_SLEEP_WAKEUP_ULP: 
      Serial.println("Wakeup caused by ULP program");
      break;
    default: 
      Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
      break;
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  Serial.println("=============================");
  Serial.println("ENTERING DEEP SLEEP MODE");
  Serial.println("Touch sensor to wake up...");
  Serial.println("=============================");
  
  // Read battery one more time before sleep
  float batteryV = readBatteryVoltage();
  int batteryP = getBatteryPercentage(batteryV);
  
  // Display sleep message
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(10, 5);
  display.println("   SLEEP MODE");
  display.println("");
  display.println("  Touch sensor");
  display.println("   to wake up");
  display.println("");
  display.printf("  Boots: %d", bootCount);
  display.println("");
  display.printf("  Batt: %d%% %.2fV", batteryP, batteryV);
  display.display();
  
  delay(2000);  // Show message for 2 seconds
  
  // Turn off display
  display.clearDisplay();
  display.display();
  
  // Configure wake-up source
  // Wake up when GPIO 13 goes HIGH (touch sensor is touched)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_13, 1);
  
  // Optional: Add timer wakeup as backup (e.g., wake every 10 minutes)
  // esp_sleep_enable_timer_wakeup(10 * 60 * 1000000ULL);  // 10 minutes
  
  Serial.flush();  // Wait for serial to finish
  
  // Enter deep sleep
  esp_deep_sleep_start();
  
  // Code never reaches here - ESP32 resets on wake
}

/* ---------- Touch Sensor Handling ---------- */

// Check if touch sensor is pressed (with debouncing)
bool isTouchPressed() {
  int currentState = digitalRead(TOUCH_PIN);
  unsigned long currentTime = millis();
  
  // Check if state changed and debounce time passed
  if (currentState == HIGH && lastTouchState == LOW) {
    if (currentTime - lastTouchTime > TOUCH_DEBOUNCE) {
      lastTouchTime = currentTime;
      lastTouchState = currentState;
      return true;
    }
  }
  
  lastTouchState = currentState;
  return false;
}

/* ---------- Setup ---------- */
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Configure ADC for battery monitoring
  analogReadResolution(12);  // 12-bit resolution (0-4095)
  analogSetAttenuation(ADC_11db);  // Full range: 0-3.3V
  pinMode(BATTERY_PIN, INPUT);
  Serial.println("Battery monitoring configured on GPIO6");
  
  // Increment boot count
  bootCount++;
  
  Serial.println("=================================");
  Serial.println("=== RUNNER AQI SYSTEM (v2.1) ===");
  Serial.println("=== With Battery Monitoring  ===");
  Serial.println("=================================");
  Serial.printf("Boot count: %d\n", bootCount);
  
  // Print wakeup reason
  printWakeupReason();
  
  // Read initial battery level
  float initialBatteryV = readBatteryVoltage();
  int initialBatteryP = getBatteryPercentage(initialBatteryV);
  Serial.printf("Battery: %.2fV (%d%%)\n", initialBatteryV, initialBatteryP);
  
  // Configure touch sensor pin
  pinMode(TOUCH_PIN, INPUT);
  Serial.printf("Touch sensor configured on GPIO %d\n", TOUCH_PIN);
  
  Wire.begin(8, 9); 
  Serial.println("I2C Initialized");

  // Init OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); 
  }
  Serial.println("OLED Initialized");
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Project name - large, centered
  display.setTextSize(2);
  display.setCursor(4, 4);
  display.print("Smart");
  display.setCursor(4, 22);
  display.print("RunSense");

  // Divider
  display.drawFastHLine(0, 43, SCREEN_WIDTH, SSD1306_WHITE);

  // Sub-info row
  display.setTextSize(1);
  display.setCursor(0, 47);
  display.printf("v2.1  Boot#%d", bootCount);
  display.setCursor(0, 57);
  display.printf("Batt: %d%%  %.2fV", initialBatteryP, initialBatteryV);

  display.display();
  Serial.println("OLED Display Updated");

  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    if(attempts > 40) { // 20 seconds timeout
      Serial.println("\nWiFi connection failed!");
      display.clearDisplay();
      display.setCursor(0, 10);
      display.println("WiFi Failed!");
      display.println("");
      display.println("Touch to retry");
      display.println("");
      display.println("Check:");
      display.println("- SSID/Password");
      display.display();

      // Wait for touch sensor to retry WiFi instead of hard-locking
      Serial.println("Touch sensor to retry WiFi...");
      while (true) {
        if (isTouchPressed()) {
          Serial.println("Touch detected — retrying WiFi...");
          display.clearDisplay();
          display.setCursor(0, 10);
          display.println("Retrying WiFi...");
          display.display();
          WiFi.disconnect(true);
          delay(500);
          WiFi.begin(WIFI_SSID, WIFI_PASS);
          attempts = 0;  // Reset attempt counter to re-enter outer while loop
          break;         // Break inner loop, outer while(WiFi!=connected) resumes
        }
        delay(50); // Small yield to avoid watchdog trigger
      }
    }
  }
  
  Serial.println("\nWiFi Connected!");
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("WiFi Connected!");
  display.println("");
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.display();

  configTime(5.5 * 3600, 0, "pool.ntp.org");
  
  delay(1000);
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Getting Auth Token...");
  display.display();
  
  getToken();

  // Init Sensors
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Initializing Sensors...");
  display.display();
  
  if (!aht.begin()) {
    Serial.println("AHT Sensor failed");
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("ERROR:");
    display.println("AHT Sensor Failed!");
    display.display();
    delay(3000);
  } else {
    Serial.println("AHT Sensor OK");
  }
  
  if (!ens160.begin()) {
    Serial.println("ENS160 Sensor failed");
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("ERROR:");
    display.println("ENS160 Sensor Failed!");
    display.display();
    delay(3000);
  } else {
    Serial.println("ENS160 Sensor OK");
  }
  
  ens160.setOperatingMode(SFE_ENS160_STANDARD);

  display.clearDisplay();
  display.setCursor(0, 15);
  display.println("   System Ready!");
  display.println("");
  display.println(" Touch to Sleep");
  display.display();
  
  Serial.println("\n>>> Touch sensor to enter deep sleep <<<\n");
  
  delay(2000);
  lastRead = millis() - READ_INTERVAL;
}

/* ---------- Loop ---------- */
void loop() {
  if (idToken == "") return;

  // ========== TOUCH SENSOR CHECK ==========
  // Check for touch to enter deep sleep
  if (isTouchPressed()) {
    Serial.println("\n>>> TOUCH DETECTED - Entering Deep Sleep <<<\n");
    delay(100);  // Small delay for debouncing
    enterDeepSleep();  // Never returns
  }
  
  // ========== SENSOR READING ==========
  if (millis() - lastRead >= READ_INTERVAL) {
    lastRead = millis();

    /* ---- Battery Reading ---- */
    float batteryVoltage = readBatteryVoltage();
    int batteryPercent = getBatteryPercentage(batteryVoltage);

    /* ---- Temp & Hum (AHT) ---- */
    sensors_event_t hum, temp;
    aht.getEvent(&hum, &temp);

    /* ---- ENS160 ---- */
    ens160.setTempCompensation(temp.temperature);
    ens160.setRHCompensation(hum.relative_humidity);

    int aqi  = ens160.getAQI();
    int eco2 = ens160.getECO2();
    int tvoc = ens160.getTVOC();

    /* ---- Calculate Runner AQI ---- */
    float runnerScore = calculateRunnerAQI(eco2, tvoc, temp.temperature, hum.relative_humidity);
    
    /* ---- Display Update ---- */
    drawRunnerDashboard(runnerScore, temp.temperature, hum.relative_humidity, 
                       eco2, tvoc, batteryVoltage, batteryPercent);

    /* ---- Serial Debug ---- */
    Serial.println("=============================");
    Serial.println("RUNNER AQI REPORT");
    Serial.println(isoTimestamp());
    Serial.println("-----------------------------");
    Serial.printf("Battery: %.2fV (%d%%)\n", batteryVoltage, batteryPercent);
    Serial.println("-----------------------------");
    Serial.printf("eCO2: %d ppm (Score: %.1f)\n", eco2, getECO2Score(eco2));
    Serial.printf("TVOC: %d ppb (Score: %.1f)\n", tvoc, getTVOCScore(tvoc));
    Serial.printf("Temp: %.1f C (Score: %.1f)\n", temp.temperature, getTempScore(temp.temperature));
    Serial.printf("Humidity: %.1f %% (Score: %.1f)\n", hum.relative_humidity, getHumScore(hum.relative_humidity));
    Serial.println("-----------------------------");
    Serial.printf("RUNNER SCORE: %.1f\n", runnerScore);
    Serial.printf("STATUS: %s\n", getStatus(runnerScore).c_str());
    Serial.println("=============================\n");

    sendToFirestore(
      temp.temperature,
      hum.relative_humidity,
      aqi,
      eco2,
      tvoc,
      runnerScore,
      batteryVoltage,
      batteryPercent
    );
  }
}

/* ---------- NOTES ON BATTERY VOLTAGE DIVIDER ---------- */
/*
 * Hardware Setup:
 * - Battery (+) ---- 100kΩ ---- GPIO6 (ADC) ---- 100kΩ ---- GND
 * 
 * The voltage divider creates Vout = Vin × (R2 / (R1 + R2))
 * With equal resistors: Vout = Vin × 0.5
 * 
 * Maximum safe ADC input: 3.3V
 * Maximum battery voltage measurable: 3.3V × 2 = 6.6V
 * 
 * Calibration Tips:
 * 1. Measure your actual battery voltage with a multimeter
 * 2. Compare with the voltage reported by the code
 * 3. Adjust VOLTAGE_DIVIDER_RATIO if needed (actual ratio may vary due to resistor tolerance)
 * 4. Adjust BATTERY_MAX_VOLTAGE and BATTERY_MIN_VOLTAGE based on your battery type:
 *    - LiPo: 4.2V (full) to 3.0V (empty)
 *    - Li-ion: 4.2V (full) to 3.0V (empty)
 *    - 3.7V LiPo: Use values above
 * 
 * To calibrate the voltage divider ratio:
 * 1. Measure real battery voltage with multimeter: e.g., 4.05V
 * 2. Note the voltage displayed by the code (before calibration): e.g., 1.22V
 * 3. Calculate correction: VOLTAGE_DIVIDER_RATIO = (4.05 / 1.22) × 2.0 = 6.639
 *    (multiply by old ratio of 2.0 since the base divider uses equal resistors)
 * 4. Update the #define VOLTAGE_DIVIDER_RATIO value
 * 
 * *** CALIBRATED VALUES (your hardware) ***
 * Multimeter reading : 4.05V
 * Code displayed     : 1.22V  (with old ratio = 2.0)
 * Calibrated ratio   : 6.639  ✓ (currently applied)
 */
