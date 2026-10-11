#include <WiFi.h>
#include <Wire.h>
#include <FirebaseESP32.h>
#include "MAX30105.h"
#include <DHT.h>

// --- 1. Wi-Fi and Firebase Credentials ---
#define WIFI_SSID "HertzOne"
#define WIFI_PASSWORD "20040331"
#define DATABASE_URL "https://patient-monitor-1bf49-default-rtdb.firebaseio.com/"
#define DATABASE_SECRET "ZGLd7BByBPPy4r4lurVfJuy8okmoxLWEEQ8qG2OO"

FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

MAX30105 particleSensor;

// --- DHT11 Configuration (GPIO 4) ---
#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

const int BTN_WATER = 13;
const int BTN_HELP = 12;
const int BTN_HUNGRY = 14;

// --- Vital Calculation Logic ---
const byte RATE_SIZE = 4;
float rates[RATE_SIZE];
byte rateSpot = 0;
byte validRates = 0;

unsigned long lastBeatTime = 0;
float BPM = 0;
float AvgBPM = 0;

long previousIR = 0;
long previousPreviousIR = 0;

const int SAMPLE_SIZE = 100;
long redBuffer[SAMPLE_SIZE];
long irBuffer[SAMPLE_SIZE];
int sampleIndex = 0;
float SpO2 = 0;

float currentTemp = 0.0;
long irValue_global = 0;
bool isLivingBody = false;

// --- PPG Array Buffering (Real-time Wave Data) ---
const int PPG_BUFFER_SIZE = 25;
int ppgBuffer[PPG_BUFFER_SIZE];
int ppgIndex = 0;

unsigned long lastWaterLogTime = 0;
unsigned long lastHelpLogTime = 0;
unsigned long lastHungryLogTime = 0;
const unsigned long COOLDOWN_8S = 8000; 

// Firebase Sync Interval (Every 1 second)
unsigned long lastFirebaseSync = 0;
const unsigned long FIREBASE_INTERVAL = 1000; 

String alertMsg = "Touch Sensor Properly";
bool isDanger = false;

// DHT11 Temperature Reading Function
float readDHT11Temperature() {
  float t = dht.readTemperature();
  if (isnan(t)) {
    return 0.0; // Reading Error ඇති වුවහොත් 0.0 ලබා දෙයි
  }
  return t;
}

// --- Firebase Sync Function ---
void sendToFirebase() {
  FirebaseJson json;
  FirebaseJsonArray ppgArray;
  
  if (!isLivingBody) {
    json.set("bpm", 0);
    json.set("spo2", 0);
    json.set("isLivingBody", false);
    for (int i = 0; i < PPG_BUFFER_SIZE; i++) {
      ppgArray.add(0);
    }
  } else {
    json.set("bpm", (int)AvgBPM);
    json.set("spo2", (int)SpO2);
    json.set("isLivingBody", true);
    for (int i = 0; i < PPG_BUFFER_SIZE; i++) {
      ppgArray.add(ppgBuffer[i]);
    }
  }

  json.set("ppgSamples", ppgArray);
  json.set("temp", currentTemp);
  json.set("patientId", "PT-9042");
  json.set("patientName", "Eleanor Vance");
  json.set("isDanger", isDanger);      
  json.set("status", alertMsg);
  json.set("lastUpdated", millis());

  if (Firebase.setJSON(firebaseData, "/patient", json)) {
    // Firebase Update Successful
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BTN_WATER, INPUT_PULLUP);
  pinMode(BTN_HELP, INPUT_PULLUP);
  pinMode(BTN_HUNGRY, INPUT_PULLUP);

  dht.begin(); // Start DHT Sensor

  Wire.begin(21, 22);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 Sensor Not Found!");
    while (1);
  } else {
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x24);
    particleSensor.setPulseAmplitudeIR(0x24);
  }

  // --- Wi-Fi Connection ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  // --- Firebase Config ---
  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  // 1. Read MAX30102
  long IR = particleSensor.getIR();
  long RED = particleSensor.getRed();
  irValue_global = IR;

  // 2. Dynamic Living Body Check (AC Pulse Fluctuation)
  long irDelta = abs(IR - previousIR);
  if (IR >= 10000 && irDelta > 30) {
    isLivingBody = true;
  } else if (IR < 10000) {
    isLivingBody = false;
  }

  // 3. Collect Samples into Array Buffer & Serial Plotter Stream
  if (isLivingBody) {
    ppgBuffer[ppgIndex] = IR;
    Serial.print("PPG:");
    Serial.println(IR);
  } else {
    ppgBuffer[ppgIndex] = 0;
    Serial.print("PPG:");
    Serial.println(0);
  }
  ppgIndex = (ppgIndex + 1) % PPG_BUFFER_SIZE;

  // 4. Reset & Calculation Logic
  if (!isLivingBody) {
    BPM = 0; AvgBPM = 0; SpO2 = 0;
    rateSpot = 0; validRates = 0; lastBeatTime = 0;
    previousIR = 0; previousPreviousIR = 0; sampleIndex = 0;
  } else {
    // BPM Peak Detection Logic
    long diff1 = IR - previousIR;
    long diff2 = previousIR - previousPreviousIR;

    if (diff2 > 0 && diff1 <= 0) {
      unsigned long now = millis();
      if (lastBeatTime != 0) {
        unsigned long delta = now - lastBeatTime;
        if (delta >= 400 && delta <= 1500) {
          float newBPM = 60000.0 / delta;
          if (newBPM >= 40 && newBPM <= 150) {
            bool acceptBeat = true;
            if (validRates >= 2) {
              float previousAverage = 0;
              for (byte i = 0; i < validRates; i++) previousAverage += rates[i];
              previousAverage /= validRates;
              if (abs(newBPM - previousAverage) > 30) acceptBeat = false;
            }
            if (acceptBeat) {
              BPM = newBPM;
              rates[rateSpot] = BPM;
              rateSpot = (rateSpot + 1) % RATE_SIZE;
              if (validRates < RATE_SIZE) validRates++;
              float total = 0;
              for (byte i = 0; i < validRates; i++) total += rates[i];
              AvgBPM = total / validRates;
            }
          }
        }
      }
      lastBeatTime = now;
    }

    previousPreviousIR = previousIR;
    previousIR = IR;

    // SpO2 Buffer Calculation
    redBuffer[sampleIndex] = RED;
    irBuffer[sampleIndex] = IR;
    sampleIndex++;

    if (sampleIndex >= SAMPLE_SIZE) {
      float redDC = 0, irDC = 0;
      for (int i = 0; i < SAMPLE_SIZE; i++) {
        redDC += redBuffer[i];
        irDC += irBuffer[i];
      }
      redDC /= SAMPLE_SIZE; irDC /= SAMPLE_SIZE;

      float redAC = 0, irAC = 0;
      for (int i = 0; i < SAMPLE_SIZE; i++) {
        redAC += abs(redBuffer[i] - redDC);
        irAC += abs(irBuffer[i] - irDC);
      }
      redAC /= SAMPLE_SIZE; irAC /= SAMPLE_SIZE;

      if (redDC > 0 && irDC > 0 && irAC > 0) {
        float R = (redAC / redDC) / (irAC / irDC);
        float calculatedSpO2 = 110.0 - (25.0 * R);
        if (calculatedSpO2 >= 70 && calculatedSpO2 <= 100) {
          SpO2 = calculatedSpO2;
        }
      }
      sampleIndex = 0;
    }
  }

  // Read Temperature via DHT11
  currentTemp = readDHT11Temperature();

  // --- BUTTON REQUESTS ---
  if (digitalRead(BTN_WATER) == LOW && (millis() - lastWaterLogTime >= COOLDOWN_8S)) lastWaterLogTime = millis();
  if (digitalRead(BTN_HELP) == LOW && (millis() - lastHelpLogTime >= COOLDOWN_8S)) lastHelpLogTime = millis();
  if (digitalRead(BTN_HUNGRY) == LOW && (millis() - lastHungryLogTime >= COOLDOWN_8S)) lastHungryLogTime = millis();

  bool isWaterActive = (lastWaterLogTime > 0 && (millis() - lastWaterLogTime < COOLDOWN_8S));
  bool isHelpActive = (lastHelpLogTime > 0 && (millis() - lastHelpLogTime < COOLDOWN_8S));
  bool isHungryActive = (lastHungryLogTime > 0 && (millis() - lastHungryLogTime < COOLDOWN_8S));

  isDanger = false;

  // --- STATUS LOGIC ---
  if (isWaterActive) { alertMsg = "Water Required!"; isDanger = true; }
  else if (isHelpActive) { alertMsg = "Emergency Help Needed!"; isDanger = true; }
  else if (isHungryActive) { alertMsg = "Food Required!"; isDanger = true; }
  else if (!isLivingBody) { alertMsg = "Touch Sensor Properly (No Pulse)"; }
  else if (validRates < 1) { alertMsg = "Measuring Vitals..."; }
  else if (AvgBPM < 50 || AvgBPM > 120 || SpO2 < 90 || currentTemp > 38.0) {
    alertMsg = "Hazard: Clinical Threshold Breach";
    isDanger = true;
  } else { alertMsg = "Normal Condition"; }

  // --- Firebase Sync (Every 1 Second) ---
  if (millis() - lastFirebaseSync >= FIREBASE_INTERVAL) {
    lastFirebaseSync = millis();
    sendToFirebase();
  }

  delay(10); // 100Hz sampling speed
}
