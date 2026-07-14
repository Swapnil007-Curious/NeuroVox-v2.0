// ============================================================
// NeuroVox v2.0 — Complete Firmware (Fixed & Optimized)
// Board: ESP32-S3-DevKitC-1 (Wokwi simulation)
// 4-channel EMG simulation via potentiometers
// MPU6050 simulates IMU motion rejection
// Adaptive threshold + confidence scoring + BLE GATT notify
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#define RUNNING_IN_WOKWI_SIMULATOR true  // Change to false when flashing real hardware


// ---------------- PIN DEFINITIONS ----------------
#define EMG_CH1     1
#define EMG_CH2     2
#define EMG_CH3     3
#define EMG_CH4     4
#define LED_PIN     15
#define I2C_SDA     8
#define I2C_SCL     9
#define MPU_ADDR    0x68

// ---------------- DETECTION PARAMETERS ----------------
#define ALPHA               0.05f
#define SILENCE_MS          400
#define BASE_THRESHOLD      1800
#define LOW_CONF_CUTOFF     70      // percent
#define ACCEL_LIMIT_G       1.5f
#define ROLLING_WINDOW_DAYS 7
#define BUFFER_SIZE         128

// ---------------- DATA STRUCTURES ----------------
struct EMGSample {
  uint16_t ch[4];
  uint32_t time_ms;
  bool     motionFlag;
};

struct GestureMessage {
  char text[64];
};

EMGSample ringBuffer[BUFFER_SIZE];
volatile uint8_t bufHead = 0;
volatile uint8_t bufTail = 0;

// Adaptive threshold state per channel
float dailyPeak[4]        = {0, 0, 0, 0};
float rollingAvg[4]       = {2000, 2000, 2000, 2000}; // seeded baseline
float adaptiveThreshold[4]= {BASE_THRESHOLD, BASE_THRESHOLD, BASE_THRESHOLD, BASE_THRESHOLD};

// Motion state (shared between IMU task and classifier)
volatile bool motionDetected = false;

// ---------------- BLE ----------------
#define SERVICE_UUID   "180d1000-1234-1000-8000-00805f9b34fb"
#define CHAR_UUID      "2a371000-1234-1000-8000-00805f9b34fb"

BLEServer*         pServer         = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool               deviceConnected = false;

QueueHandle_t gestureQueue;

TaskHandle_t samplingTaskHandle   = nullptr;
TaskHandle_t classifierTaskHandle = nullptr;
TaskHandle_t bleTaskHandle        = nullptr;
TaskHandle_t imuTaskHandle        = nullptr;

// ============================================================
// BLE CALLBACKS
// ============================================================
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    deviceConnected = true;
    Serial.println("[BLE] Phone connected");
  }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    Serial.println("[BLE] Phone disconnected, restarting advertising");
    BLEDevice::startAdvertising();
  }
};

// ============================================================
// MPU6050 HELPER — read accelerometer magnitude in G
// ============================================================
float readAccelMagnitude() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); // ACCEL_XOUT_H register
  if (Wire.endTransmission(false) != 0) {
    return 1.0f; // Return normal gravity baseline if device communication fails
  }
  Wire.requestFrom(MPU_ADDR, 6, true);

  if (Wire.available() >= 6) {
    int16_t ax = (Wire.read() << 8) | Wire.read();
    int16_t ay = (Wire.read() << 8) | Wire.read();
    int16_t az = (Wire.read() << 8) | Wire.read();

    float axg = ax / 16384.0f;
    float ayg = ay / 16384.0f;
    float azg = az / 16384.0f;

    return sqrtf(axg * axg + ayg * ayg + azg * azg);
  }
  return 1.0f;
}

// ============================================================
// TASK 1 — IMU MONITOR (Core 1, lowest priority)
// Reads MPU6050 every 10ms, flags motion artifacts
// ============================================================
void imuTask(void* param) {
  while (true) {
    float magnitude = readAccelMagnitude();
    motionDetected = (magnitude > ACCEL_LIMIT_G);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================
// TASK 2 — ADC SAMPLING (Core 0, highest priority)
// Optimized to run at 200Hz in simulation to save CPU speed
// ============================================================
void samplingTask(void* param) {
  // Set to 'true' for smooth 100% Wokwi simulation speed. 
  // Change to 'false' later when uploading to real physical hardware.
  #define WOKWI_SIM true 

  const TickType_t period = WOKWI_SIM ? pdMS_TO_TICKS(5) : pdMS_TO_TICKS(1);
  TickType_t start = xTaskGetTickCount();

  while (true) {
    EMGSample s;
    s.ch[0]      = analogRead(EMG_CH1);
    s.ch[1]      = analogRead(EMG_CH2);
    s.ch[2]      = analogRead(EMG_CH3);
    s.ch[3]      = analogRead(EMG_CH4);
    s.time_ms    = millis();
    s.motionFlag = motionDetected;

    uint8_t nextHead = (bufHead + 1) % BUFFER_SIZE;
    if (nextHead != bufTail) {
      ringBuffer[bufHead] = s;
      bufHead = nextHead;
    }

    vTaskDelayUntil(&start, period);
  }
}


// ============================================================
// TASK 3 — GESTURE CLASSIFIER (Core 1, medium priority)
// Optimized buffer delay to prevent CPU choking
// ============================================================
void classifierTask(void* param) {
  float    envelope[4]       = {0, 0, 0, 0};
  float    peakThisGesture[4]= {0, 0, 0, 0};
  bool     inGesture[4]      = {false, false, false, false};
  int      pulseCount[4]     = {0, 0, 0, 0};
  uint32_t lastPulse[4]      = {0, 0, 0, 0};
  uint32_t lastDailyUpdate   = millis();

  while (true) {
    // If the data buffer is empty, rest for 10ms instead of 2ms
    if (bufTail == bufHead) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    EMGSample s = ringBuffer[bufTail];
    bufTail = (bufTail + 1) % BUFFER_SIZE;

    if (s.motionFlag) {
      continue;
    }

    uint32_t now = s.time_ms;

    for (int ch = 0; ch < 4; ch++) {
      float rectified = fabsf((float)s.ch[ch] - 2048.0f);
      envelope[ch] = ALPHA * rectified + (1.0f - ALPHA) * envelope[ch];

      if (envelope[ch] > peakThisGesture[ch]) {
        peakThisGesture[ch] = envelope[ch];
      }

      if (!inGesture[ch] && envelope[ch] > adaptiveThreshold[ch]) {
        inGesture[ch]        = true;
        peakThisGesture[ch]  = envelope[ch];
      }

      if (inGesture[ch] && envelope[ch] < (adaptiveThreshold[ch] * 0.6f)) {
        inGesture[ch] = false;
        pulseCount[ch]++;
        lastPulse[ch] = now;

        if (peakThisGesture[ch] > dailyPeak[ch]) {
          dailyPeak[ch] = peakThisGesture[ch];
        }
      }

      if (!inGesture[ch] && pulseCount[ch] > 0 && (millis() - lastPulse[ch]) > SILENCE_MS) {
        float margin = (peakThisGesture[ch] - adaptiveThreshold[ch]) / adaptiveThreshold[ch] * 100.0f;
        float confidence = constrain(60.0f + margin, 0.0f, 100.0f);
        bool lowConf = confidence < LOW_CONF_CUTOFF;

        GestureMessage msg;
        const char* gestureName;
        switch (pulseCount[ch]) {
          case 1:  gestureName = "YES";  break;
          case 2:  gestureName = "NO";   break;
          case 3:  gestureName = "HELP"; break;
          default: gestureName = "WAIT"; break;
        }

        snprintf(msg.text, sizeof(msg.text), "CH%d:%s:%.0f%%%s",
                 ch + 1, gestureName, confidence,
                 lowConf ? ":LOWCONF" : "");

        pulseCount[ch] = 0;
        xQueueSend(gestureQueue, &msg, 0);
      }
    }

    if (millis() - lastDailyUpdate > 60000) {
      lastDailyUpdate = millis();
      for (int ch = 0; ch < 4; ch++) {
        rollingAvg[ch] = (rollingAvg[ch] * (ROLLING_WINDOW_DAYS - 1) + dailyPeak[ch]) / ROLLING_WINDOW_DAYS;

        if (dailyPeak[ch] > rollingAvg[ch] * 0.8f) {
          adaptiveThreshold[ch] = rollingAvg[ch] * 0.9f; 
        }
        dailyPeak[ch] = 0; 
      }
    }
  }
}


// ============================================================
// TASK 4 — BLE DISPATCHER (Core 1, low priority)
// Processes data from the queue and sends notifications
// ============================================================
void bleBroadcastTask(void* param) {
  GestureMessage incomingMsg;
  while (true) {
    if (xQueueReceive(gestureQueue, &incomingMsg, portMAX_DELAY) == pdTRUE) {
      Serial.print("[CLASSIFIER OUTPUT] -> ");
      Serial.println(incomingMsg.text);

      // Only talk to the real radio if we aren't simulating
      if (!RUNNING_IN_WOKWI_SIMULATOR && deviceConnected && pCharacteristic != nullptr) {
        pCharacteristic->setValue((uint8_t*)incomingMsg.text, strlen(incomingMsg.text));
        pCharacteristic->notify();
      }
      
      // Keep your visual notification LED working in the simulator!
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(100));
      digitalWrite(LED_PIN, LOW);
    }
  }
}


// ============================================================
// SETUP & HARDWARE ENGINE INTIALIZATION
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Initializing NeuroVox v2.0 System Architecture...");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Initialize explicit I2C Pin maps required for simulated ESP32-S3 boards
  Wire.begin(I2C_SDA, I2C_SCL);

  // Initialize safe static messaging layout array
  gestureQueue = xQueueCreate(10, sizeof(GestureMessage));

  /  // Initialize BLE Stack Architecture (Bypassed for smooth simulation speed)
  if (!RUNNING_IN_WOKWI_SIMULATOR) {
    BLEDevice::init("NeuroVox-v2");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
                        CHAR_UUID,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
    pCharacteristic->addDescriptor(new BLE2902());

    pService->start();
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    Serial.println("[BLE] Hardware Radio Started.");
  } else {
    Serial.println("[WOKWI ECO-MODE] BLE Radio bypassed to achieve 100% simulation speed.");
  }


  // Spin up parallel operations across the asymmetric core processor links
  xTaskCreatePinnedToCore(samplingTask,   "ADC_Sample",  3072, nullptr, 5, &samplingTaskHandle,   0);
  xTaskCreatePinnedToCore(classifierTask, "Classifier",  4096, nullptr, 3, &classifierTaskHandle, 1);
