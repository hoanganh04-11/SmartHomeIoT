#define BLYNK_TEMPLATE_ID "TMPL6oTINShm2"
#define BLYNK_TEMPLATE_NAME "VUONTHONGMINH"
#define BLYNK_AUTH_TOKEN "7Axq2iSiCuXtzvSS1egNLpUvKlTltXtq"
#define BLYNK_PRINT Serial

#include "blynk_bridge.h"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

#include "lora_receiver.h"

// WiFi/Blynk config
static const char *WIFI_SSID = "trungta"; // thay bằng tk mk wifi máy mày phát ra
static const char *WIFI_PASSWORD = "123456789";

// Virtual pins theo dashboard
#define VPIN_TEMPERATURE V0
#define VPIN_SOIL_HUMIDITY V1
#define VPIN_GAS V2
// Anh dashboard dang trung nhan V4 cho 2 den, map tach rieng thanh V3/V4 de dieu khien 2 relay.
#define VPIN_RELAY_1 V3
#define VPIN_RELAY_2 V4
#define VPIN_LIGHT V5
#define VPIN_AUTO_MODE V6
#define VPIN_TEMP_THRESHOLD V7
#define VPIN_GAS_THRESHOLD V8
#define VPIN_ALERT V9

static const unsigned long SENDDATA_INTERVAL_MS = 10000;
static const unsigned long NODE_RESPONSE_TIMEOUT_MS = 2000;

static SensorData_t txShadow = {0.0f, 0.0f, 0, 0, 35, 60, 0, 0, false};
static unsigned long lastSendDataMs = 0;
static unsigned long lastWiFiRetry = 0;
static unsigned long lastBlynkRetry = 0;
static bool waitingNode = false;
static bool hasBlynkCmd = false;

static uint16_t sanitizeThreshold(int value)
{
  return value < 0 ? 0 : (uint16_t)value;
}

static void publishCurrentState()
{
  if (!Blynk.connected())
    return;

  Blynk.virtualWrite(VPIN_TEMPERATURE, txShadow.temperature);
  Blynk.virtualWrite(VPIN_SOIL_HUMIDITY, txShadow.humidity);
  Blynk.virtualWrite(VPIN_GAS, txShadow.gasValue);
  Blynk.virtualWrite(VPIN_LIGHT, txShadow.lightValue);
  Blynk.virtualWrite(VPIN_TEMP_THRESHOLD, txShadow.tempThreshold);
  Blynk.virtualWrite(VPIN_GAS_THRESHOLD, txShadow.gasThreshold);
  Blynk.virtualWrite(VPIN_RELAY_1, (txShadow.relayStatus & 0x01) ? 1 : 0);
  Blynk.virtualWrite(VPIN_RELAY_2, (txShadow.relayStatus & 0x02) ? 1 : 0);
  Blynk.virtualWrite(VPIN_AUTO_MODE, txShadow.autoMode ? 1 : 0);
  Blynk.virtualWrite(VPIN_ALERT, txShadow.alertStatus ? 255 : 0);
}

static bool sendShadowToNode()
{
  bool ok = sendLoraSensorData(txShadow);
  Serial.println(ok ? "Gui frame LoRa OK" : "Gui frame LoRa FAIL");
  return ok;
}

static void publishChangedState(const SensorData_t &oldData, const SensorData_t &newData)
{
  if (!Blynk.connected())
    return;

  if (oldData.temperature != newData.temperature)
  {
    Serial.printf("[BLYNK] VPIN_TEMPERATURE <- %.2f\n", newData.temperature);
    Blynk.virtualWrite(VPIN_TEMPERATURE, newData.temperature);
  }

  if (oldData.humidity != newData.humidity)
  {
    Serial.printf("[BLYNK] VPIN_SOIL_HUMIDITY <- %.2f\n", newData.humidity);
    Blynk.virtualWrite(VPIN_SOIL_HUMIDITY, newData.humidity);
  }

  if (oldData.gasValue != newData.gasValue)
  {
    Serial.printf("[BLYNK] VPIN_GAS <- %d\n", newData.gasValue);
    Blynk.virtualWrite(VPIN_GAS, newData.gasValue);
  }

  if (oldData.lightValue != newData.lightValue)
  {
    Serial.printf("[BLYNK] VPIN_LIGHT <- %d\n", newData.lightValue);
    Blynk.virtualWrite(VPIN_LIGHT, newData.lightValue);
  }

  if (oldData.tempThreshold != newData.tempThreshold)
  {
    Serial.printf("[BLYNK] VPIN_TEMP_THRESHOLD <- %d\n", newData.tempThreshold);
    Blynk.virtualWrite(VPIN_TEMP_THRESHOLD, newData.tempThreshold);
  }

  if (oldData.gasThreshold != newData.gasThreshold)
  {
    Serial.printf("[BLYNK] VPIN_GAS_THRESHOLD <- %d\n", newData.gasThreshold);
    Blynk.virtualWrite(VPIN_GAS_THRESHOLD, newData.gasThreshold);
  }

  if ((oldData.relayStatus & 0x01) != (newData.relayStatus & 0x01))
  {
    Serial.printf("[BLYNK] VPIN_RELAY_1 <- %d\n", (newData.relayStatus & 0x01) ? 1 : 0);
    Blynk.virtualWrite(VPIN_RELAY_1, (newData.relayStatus & 0x01) ? 1 : 0);
  }

  if ((oldData.relayStatus & 0x02) != (newData.relayStatus & 0x02))
  {
    Serial.printf("[BLYNK] VPIN_RELAY_2 <- %d\n", (newData.relayStatus & 0x02) ? 1 : 0);
    Blynk.virtualWrite(VPIN_RELAY_2, (newData.relayStatus & 0x02) ? 1 : 0);
  }

  if (oldData.alertStatus != newData.alertStatus)
  {
    Serial.printf("[BLYNK] VPIN_ALERT <- %d\n", newData.alertStatus);
    Blynk.virtualWrite(VPIN_ALERT, newData.alertStatus);
  }

  if (oldData.autoMode != newData.autoMode)
  {
    Serial.printf("[BLYNK] VPIN_AUTO_MODE <- %d\n", newData.autoMode ? 1 : 0);
    Blynk.virtualWrite(VPIN_AUTO_MODE, newData.autoMode ? 1 : 0);
  }
}

static void connectWiFiTask()
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  unsigned long now = millis();
  if (now - lastWiFiRetry < 5000)
    return;
  lastWiFiRetry = now;

  Serial.print("WiFi reconnecting to ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void connectBlynkTask()
{
  if (WiFi.status() != WL_CONNECTED || Blynk.connected())
    return;

  unsigned long now = millis();
  if (now - lastBlynkRetry < 3000)
    return;
  lastBlynkRetry = now;

  Serial.println("Blynk reconnecting...");
  Blynk.connect(1000);
}

static void setRelayBit(uint8_t bitMask, bool isOn, const char *reason)
{
  if (txShadow.autoMode == 1)
  {
    Blynk.virtualWrite(VPIN_RELAY_1, (txShadow.relayStatus & 0x01) ? 1 : 0);
    Blynk.virtualWrite(VPIN_RELAY_2, (txShadow.relayStatus & 0x02) ? 1 : 0);
    return;
  }
  uint8_t relayBefore = txShadow.relayStatus;
  if (isOn)
  {
    txShadow.relayStatus |= bitMask;
  }
  else
  {
    txShadow.relayStatus &= (uint8_t)(~bitMask);
  }

  if (txShadow.relayStatus == relayBefore)
  {
    Serial.print(reason);
    Serial.println(" -> relay khong doi");
    return;
  }

  Serial.print(reason);
  Serial.println(" -> doi relay, cho gui trong loop");
  hasBlynkCmd = true;
}

static void setTempThreshold(uint16_t value)
{
  if (txShadow.tempThreshold == value)
  {
    return;
  }

  txShadow.tempThreshold = value;
  Serial.println("Cap nhat nguong nhiet do -> cho gui trong loop");
  hasBlynkCmd = true;
}

static void setGasThreshold(uint16_t value)
{
  if (txShadow.gasThreshold == value)
  {
    return;
  }

  txShadow.gasThreshold = value;
  Serial.println("Cap nhat nguong khi ga -> cho gui trong loop");
  hasBlynkCmd = true;
}

static void setAutoMode(bool enabled)
{
  if (txShadow.autoMode == enabled)
  {
    return;
  }

  txShadow.autoMode = enabled;
  Serial.println("Cap nhat che do tu dong -> cho gui trong loop");
  hasBlynkCmd = true;
}

static void requestNodeSendData()
{
  bool ok = sendLoraTextCommand("senddata");
  lastSendDataMs = millis();
  if (ok)
  {
    waitingNode = true;
  }
  Serial.println(ok ? "Da gui lenh senddata xuong node" : "Gui lenh senddata FAIL");
}

static void checkWaitingNodeTimeout()
{
  if (waitingNode && (millis() - lastSendDataMs >= NODE_RESPONSE_TIMEOUT_MS))
  {
    waitingNode = false;
    Serial.println("Qua 2s khong co du lieu node -> waitingNode = false");
  }
}

static void handlePendingBlynkSend()
{
  if (!waitingNode && hasBlynkCmd)
  {
    if (sendShadowToNode())
    {
      Serial.println("Da gui lenh Blynk xuong node");
    }
    hasBlynkCmd = false;
  }
}

BLYNK_CONNECTED()
{
  //publishCurrentState();
  Blynk.syncVirtual(VPIN_RELAY_1, VPIN_RELAY_2, VPIN_AUTO_MODE, VPIN_TEMP_THRESHOLD, VPIN_GAS_THRESHOLD);
}

BLYNK_WRITE(VPIN_RELAY_1)
{
  setRelayBit(0x01, param.asInt() != 0, "Dieu khien den 1");
}

BLYNK_WRITE(VPIN_RELAY_2)
{
  setRelayBit(0x02, param.asInt() != 0, "Dieu khien den 2");
}

BLYNK_WRITE(VPIN_AUTO_MODE)
{
  setAutoMode(param.asInt() != 0);
}

BLYNK_WRITE(VPIN_TEMP_THRESHOLD)
{
  setTempThreshold(sanitizeThreshold(param.asInt()));
}

BLYNK_WRITE(VPIN_GAS_THRESHOLD)
{
  setGasThreshold(sanitizeThreshold(param.asInt()));
}

void initBlynkBridge()
{
  WiFi.mode(WIFI_STA);
  Blynk.config(BLYNK_AUTH_TOKEN);

  connectWiFiTask();
  connectBlynkTask();
}

void blynkBridgeRequestNodeDataByInterval()
{
  unsigned long now = millis();
  if (now - lastSendDataMs < SENDDATA_INTERVAL_MS)
    return;

  requestNodeSendData();
}

void blynkBridgeLoop()
{
  connectWiFiTask();
  connectBlynkTask();

  if (Blynk.connected())
  {
    Blynk.run();
  }
  checkWaitingNodeTimeout();
  handlePendingBlynkSend();
  blynkBridgeRequestNodeDataByInterval();
}

void blynkBridgePublishChangedFromLora(const SensorData_t &oldData, const SensorData_t &newData)
{
  waitingNode = false;
  txShadow = newData;
  publishChangedState(oldData, newData);
}