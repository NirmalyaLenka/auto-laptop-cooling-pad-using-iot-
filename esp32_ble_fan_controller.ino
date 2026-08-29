

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID       "6b1e6a10-6e1a-4a0c-9e21-6f1a9c8a10f0"
#define TEMP_CHAR_UUID      "6b1e6a11-6e1a-4a0c-9e21-6f1a9c8a10f0"
#define STATUS_CHAR_UUID     "6b1e6a12-6e1a-4a0c-9e21-6f1a9c8a10f0"

#define DEVICE_NAME "CoolPad-ESP32"


const int FAN_PWM_PIN   = 25;      
const int PWM_FREQ_HZ   = 25000;  
const int PWM_RESOLUTION = 8;      
const int PWM_CHANNEL   = 0;

// duty floor instead of already sitting mid-ramp.
const float TEMP_MIN      = 48.0;  // at/below this: minimum duty (covers idle)
const float TEMP_MAX      = 78.0;  // at/above this: 100% duty (typical load ceiling)
const int   DUTY_MIN_PCT  = 20;    // floor so fan doesn't stall
const int   DUTY_MAX_PCT  = 100;
const float HYSTERESIS_C  = 2.0;   // ignore temp changes smaller than this

// ---- Safety fallback ----
const unsigned long DATA_TIMEOUT_MS = 15000; // no update in this window -> fallback
const int FALLBACK_DUTY_PCT = 50;            // safe mid speed if link is lost

BLEServer *pServer = nullptr;
BLECharacteristic *pTempChar = nullptr;
BLECharacteristic *pStatusChar = nullptr;

volatile bool deviceConnected = false;
volatile float lastTempC = 0.0f;
volatile unsigned long lastTempMillis = 0;

float appliedTempC = -100.0f; // forces first update to apply
int currentDutyPct = DUTY_MIN_PCT;

// ---------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    deviceConnected = true;
  }
  void onDisconnect(BLEServer *server) override {
    deviceConnected = false;
    BLEDevice::startAdvertising(); // resume advertising after disconnect
  }
};

class TempCharCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *chr) override {
    std::string value = chr->getValue();
    if (value.length() >= 2) {
      int16_t raw = (uint8_t)value[0] | ((int16_t)(uint8_t)value[1] << 8);
      lastTempC = raw / 100.0f;
      lastTempMillis = millis();
    }
  }
};

// ---------------------------------------------------------------------
int tempToDutyPct(float tempC) {
  if (tempC <= TEMP_MIN) return DUTY_MIN_PCT;
  if (tempC >= TEMP_MAX) return DUTY_MAX_PCT;
  float ratio = (tempC - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
  return DUTY_MIN_PCT + (int)(ratio * (DUTY_MAX_PCT - DUTY_MIN_PCT));
}

void applyDutyPct(int pct) {
  pct = constrain(pct, 0, 100);
  currentDutyPct = pct;
  int duty255 = (int)((pct / 100.0f) * 255.0f);
  ledcWrite(PWM_CHANNEL, duty255);
}

void publishStatus(uint8_t tempRounded, uint8_t dutyPct, uint8_t fresh) {
  if (!deviceConnected || pStatusChar == nullptr) return;
  uint8_t payload[3] = {tempRounded, dutyPct, fresh};
  pStatusChar->setValue(payload, 3);
  pStatusChar->notify();
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  ledcSetup(PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION);
  ledcAttachPin(FAN_PWM_PIN, PWM_CHANNEL);
  applyDutyPct(DUTY_MIN_PCT);

  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTempChar = pService->createCharacteristic(
      TEMP_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pTempChar->setCallbacks(new TempCharCallbacks());

  pStatusChar = pService->createCharacteristic(
      STATUS_CHAR_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  pStatusChar->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("BLE cooling pad controller ready, advertising as " DEVICE_NAME);
}

// ---------------------------------------------------------------------
void loop() {
  unsigned long now = millis();
  bool dataFresh = (now - lastTempMillis) < DATA_TIMEOUT_MS;

  if (lastTempMillis == 0) {
    // never received a reading yet, hold at minimum duty
    applyDutyPct(DUTY_MIN_PCT);
  } else if (!dataFresh) {
    // link lost or laptop stopped sending, fall back to safe fixed speed
    applyDutyPct(FALLBACK_DUTY_PCT);
  } else {
    float t = lastTempC;
    if (fabs(t - appliedTempC) >= HYSTERESIS_C) {
      appliedTempC = t;
      applyDutyPct(tempToDutyPct(t));
    }
  }

  static unsigned long lastStatusMillis = 0;
  if (now - lastStatusMillis >= 1000) {
    lastStatusMillis = now;
    uint8_t tempRounded = (uint8_t)constrain((int)round(lastTempC), 0, 255);
    publishStatus(tempRounded, (uint8_t)currentDutyPct, dataFresh ? 1 : 0);
  }

  delay(50);
}
