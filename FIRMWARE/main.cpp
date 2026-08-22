// NeuroVox v2.0 - EMG gesture communication device
// Target board: 220mm x 50mm 2-layer PCB, ESP32-S3
// Development/validation build (potentiometer + MPU6050 stand-ins for
// the analog front end and IMU while the assembled board is in transit)

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Wire.h>
#include <math.h>

#define SIM_BUILD true // set false once flashing the assembled board

// Pin map (validation build)
#define EMG_CH1 1
#define EMG_CH2 2
#define EMG_CH3 3
#define EMG_CH4 4
#define LED_PIN 15
#define I2C_SDA 8
#define I2C_SCL 9
#define MPU_ADDR 0x68

// Detection parameters
#define ALPHA 0.05f
#define SILENCE_MS 400
#define MIN_PULSE_MS 30 // rejects sub-30ms noise spikes
#define BASE_THRESHOLD 1800
#define LOW_CONF_CUTOFF 70
#define ACCEL_LIMIT_G 1.5f
#define ROLLING_WINDOW_DAYS 7
#define BUFFER_SIZE 128

struct EMGSample {
  uint16_t ch[4];
  uint32_t time_ms;
  bool motionFlag;
};

struct GestureMessage {
  char text[64];
};

EMGSample ringBuffer[BUFFER_SIZE];
volatile uint8_t bufHead = 0;
volatile uint8_t bufTail = 0;

float dailyPeak[4] = {0, 0, 0, 0};
float rollingAvg[4] = {2000, 2000, 2000, 2000};
float adaptiveThreshold[4] = {BASE_THRESHOLD, BASE_THRESHOLD, BASE_THRESHOLD,
                              BASE_THRESHOLD};

volatile bool motionDetected = false;

#define SERVICE_UUID "180d1000-1234-1000-8000-00805f9b34fb"
#define CHAR_UUID "2a371000-1234-1000-8000-00805f9b34fb"

BLEServer *pServer = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;

QueueHandle_t gestureQueue;

TaskHandle_t samplingTaskHandle = nullptr;
TaskHandle_t classifierTaskHandle = nullptr;
TaskHandle_t bleTaskHandle = nullptr;
TaskHandle_t imuTaskHandle = nullptr;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    deviceConnected = true;
    Serial.println("phone connected");
  }
  void onDisconnect(BLEServer *s) override {
    deviceConnected = false;
    Serial.println("phone disconnected, restarting advertising");
    BLEDevice::startAdvertising();
  }
};

float readAccelMagnitude() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) {
    return 1.0f;
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

void imuTask(void *param) {
  while (true) {
    float magnitude = readAccelMagnitude();
    motionDetected = (magnitude > ACCEL_LIMIT_G);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void samplingTask(void *param) {
  const TickType_t period = SIM_BUILD ? pdMS_TO_TICKS(20) : pdMS_TO_TICKS(1);
  TickType_t start = xTaskGetTickCount();

  while (true) {
    EMGSample s;
    s.ch[0] = analogRead(EMG_CH1);
    s.ch[1] = analogRead(EMG_CH2);
    s.ch[2] = analogRead(EMG_CH3);
    s.ch[3] = analogRead(EMG_CH4);
    s.time_ms = millis();
    s.motionFlag = motionDetected;

    uint8_t nextHead = (bufHead + 1) % BUFFER_SIZE;
    if (nextHead != bufTail) {
      ringBuffer[bufHead] = s;
      bufHead = nextHead;
    }

    vTaskDelayUntil(&start, period);
  }
}

void classifierTask(void *param) {
  float envelope[4] = {0, 0, 0, 0};
  float peakThisGesture[4] = {0, 0, 0, 0};
  bool inGesture[4] = {false, false, false, false};
  uint32_t gestureStart[4] = {0, 0, 0, 0};
  int pulseCount[4] = {0, 0, 0, 0};
  uint32_t lastPulse[4] = {0, 0, 0, 0};
  uint32_t lastDailyUpdate = millis();

  while (true) {
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
        inGesture[ch] = true;
        gestureStart[ch] = now;
        peakThisGesture[ch] = envelope[ch];
      }

      if (inGesture[ch] && envelope[ch] < (adaptiveThreshold[ch] * 0.6f)) {
        inGesture[ch] = false;

        // ignore contractions shorter than MIN_PULSE_MS, treat as noise
        if (now - gestureStart[ch] >= MIN_PULSE_MS) {
          pulseCount[ch]++;
          lastPulse[ch] = now;

          if (peakThisGesture[ch] > dailyPeak[ch]) {
            dailyPeak[ch] = peakThisGesture[ch];
          }
        }
      }

      if (!inGesture[ch] && pulseCount[ch] > 0 &&
          (millis() - lastPulse[ch]) > SILENCE_MS) {

        float margin = (peakThisGesture[ch] - adaptiveThreshold[ch]) /
                       adaptiveThreshold[ch] * 100.0f;
        float confidence = constrain(60.0f + margin, 0.0f, 100.0f);
        bool lowConf = confidence < LOW_CONF_CUTOFF;

        GestureMessage msg;
        const char *gestureName;
        switch (pulseCount[ch]) {
        case 1:
          gestureName = "YES";
          break;
        case 2:
          gestureName = "NO";
          break;
        case 3:
          gestureName = "HELP";
          break;
        default:
          gestureName = "WAIT";
          break;
        }

        snprintf(msg.text, sizeof(msg.text), "CH%d:%s:%.0f%%%s", ch + 1,
                 gestureName, confidence, lowConf ? ":LOWCONF" : "");

        pulseCount[ch] = 0;
        xQueueSend(gestureQueue, &msg, 0);
      }
    }

    if (millis() - lastDailyUpdate > 60000) {
      lastDailyUpdate = millis();
      for (int ch = 0; ch < 4; ch++) {
        rollingAvg[ch] =
            (rollingAvg[ch] * (ROLLING_WINDOW_DAYS - 1) + dailyPeak[ch]) /
            ROLLING_WINDOW_DAYS;

        if (dailyPeak[ch] > rollingAvg[ch] * 0.8f) {
          adaptiveThreshold[ch] = rollingAvg[ch] * 0.9f;
        }
        dailyPeak[ch] = 0;
      }
    }
  }
}

void bleBroadcastTask(void *param) {
  GestureMessage incomingMsg;
  while (true) {
    if (xQueueReceive(gestureQueue, &incomingMsg, portMAX_DELAY) == pdTRUE) {
      Serial.print("gesture: ");
      Serial.println(incomingMsg.text);

      if (!SIM_BUILD && deviceConnected && pCharacteristic != nullptr) {
        pCharacteristic->setValue((uint8_t *)incomingMsg.text,
                                  strlen(incomingMsg.text));
        pCharacteristic->notify();
      }

      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(100));
      digitalWrite(LED_PIN, LOW);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("NeuroVox v2.0 booting");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(I2C_SDA, I2C_SCL);

  gestureQueue = xQueueCreate(10, sizeof(GestureMessage));
  if (gestureQueue == nullptr) {
    Serial.println("queue creation failed");
    while (true) {
      delay(1000);
    }
  }

  if (!SIM_BUILD) {
    BLEDevice::init("NeuroVox-v2");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);
    pCharacteristic = pService->createCharacteristic(
        CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    pCharacteristic->addDescriptor(new BLE2902());
    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    Serial.println("BLE radio started");
  } else {
    Serial.println("BLE bypassed for this build");
  }

  xTaskCreatePinnedToCore(samplingTask, "ADC_Sample", 3072, nullptr, 5,
                          &samplingTaskHandle, 0);
  xTaskCreatePinnedToCore(classifierTask, "Classifier", 4096, nullptr, 3,
                          &classifierTaskHandle, 1);
  xTaskCreatePinnedToCore(bleBroadcastTask, "BLE_Tx", 4096, nullptr, 1,
                          &bleTaskHandle, 1);
  xTaskCreatePinnedToCore(imuTask, "IMU_Monitor", 2048, nullptr, 1,
                          &imuTaskHandle, 1);

  Serial.println("all tasks running");
}

void loop() { vTaskDelay(portMAX_DELAY); }