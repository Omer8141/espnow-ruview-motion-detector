#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

#include "detector_state.h"
#include "model_data.h"
#include "peer_config.h"
#include "ruview_features.h"
#include "ruview_model.h"

namespace {

constexpr uint32_t kPacketMagic = 0x52564353;
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kMaximumCsiBytes = 384;
constexpr uint32_t kOfflineAfterMs = 1000;

struct __attribute__((packed)) ProbePacket {
  uint32_t magic;
  uint8_t version;
  uint8_t reserved[3];
  uint32_t sequence;
  uint32_t txMicros;
};

struct CsiQueueFrame {
  uint64_t timestampUs;
  uint32_t sequence;
  int8_t rssi;
  uint8_t channel;
  uint16_t length;
  bool firstWordInvalid;
  uint8_t sourceMac[6];
  int8_t iq[kMaximumCsiBytes];
};

QueueHandle_t gCsiQueue = nullptr;
volatile uint32_t gLatestSequence = 0;
volatile uint32_t gLastPacketMs = 0;
volatile uint32_t gQueueDrops = 0;
volatile uint32_t gMalformedFrames = 0;
volatile uint32_t gSequenceGaps = 0;
// Temporary CSI diagnostics: how often the callback fires and why frames are
// dropped. Remove once the CSI path is confirmed working.
volatile uint32_t gCsiCallbacks = 0;
volatile uint32_t gCsiMacRejects = 0;
uint8_t gLastCsiMac[6] = {};
uint32_t gLastAcceptedSequence = 0;
bool gHaveSequence = false;

RuViewFeatureExtractor gFeatures;
RuViewModel gModel;
DetectorStateMachine gStateMachine;
Preferences gPreferences;

char gLabel[16] = "none";
char gSession[48] = "none";
bool gLogging = false;

bool isZeroMac(const uint8_t mac[6]) {
  uint8_t combined = 0;
  for (size_t i = 0; i < 6; ++i) combined |= mac[i];
  return combined == 0;
}

bool expectedMac(const uint8_t mac[6]) {
  return !isZeroMac(TRANSMITTER_MAC) && memcmp(mac, TRANSMITTER_MAC, 6) == 0;
}

void printStationMac() {
  uint8_t mac[6] = {};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial.printf("Receiver STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onEspNowReceive(const esp_now_recv_info_t* receiveInfo,
                     const uint8_t* data, int length) {
  if (receiveInfo == nullptr || !expectedMac(receiveInfo->src_addr)) return;
#else
void onEspNowReceive(const uint8_t* sourceMac, const uint8_t* data, int length) {
  if (sourceMac == nullptr || !expectedMac(sourceMac)) return;
#endif
  if (data == nullptr || length != static_cast<int>(sizeof(ProbePacket))) return;
  ProbePacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != kPacketMagic || packet.version != kProtocolVersion) return;

  if (gHaveSequence && packet.sequence > gLastAcceptedSequence + 1) {
    gSequenceGaps += packet.sequence - gLastAcceptedSequence - 1;
  }
  gLastAcceptedSequence = packet.sequence;
  gHaveSequence = true;
  gLatestSequence = packet.sequence;
  gLastPacketMs = millis();
}

void onCsi(void*, wifi_csi_info_t* info) {
  if (info == nullptr || info->buf == nullptr) return;
  ++gCsiCallbacks;
  if (!expectedMac(info->mac)) {
    ++gCsiMacRejects;
    memcpy(gLastCsiMac, info->mac, sizeof(gLastCsiMac));
    return;
  }
  if (info->len < 4 || info->len > kMaximumCsiBytes || (info->len & 1U) != 0) {
    ++gMalformedFrames;
    return;
  }

  CsiQueueFrame frame = {};
  frame.timestampUs = esp_timer_get_time();
  frame.sequence = gLatestSequence;
  frame.rssi = info->rx_ctrl.rssi;
  frame.channel = info->rx_ctrl.channel;
  frame.length = info->len;
  frame.firstWordInvalid = info->first_word_invalid;
  memcpy(frame.sourceMac, info->mac, sizeof(frame.sourceMac));
  memcpy(frame.iq, info->buf, frame.length);
  if (xQueueSend(gCsiQueue, &frame, 0) != pdTRUE) ++gQueueDrops;
}

bool safeToken(const char* token) {
  if (token == nullptr || *token == '\0') return false;
  for (const char* cursor = token; *cursor; ++cursor) {
    const char c = *cursor;
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
  }
  return true;
}

void handleCommand(const char* command) {
  char label[16] = {};
  char session[48] = {};
  if (sscanf(command, "LABEL %15s %47s", label, session) == 2) {
    const bool knownLabel = strcmp(label, "empty") == 0 ||
                            strcmp(label, "still") == 0 ||
                            strcmp(label, "moving") == 0;
    if (!knownLabel || !safeToken(session)) {
      Serial.println("ERROR: LABEL requires empty|still|moving and a safe session id.");
      return;
    }
    strlcpy(gLabel, label, sizeof(gLabel));
    strlcpy(gSession, session, sizeof(gSession));
    gLogging = true;
    Serial.printf("LOGGING label=%s session=%s\n", gLabel, gSession);
    return;
  }
  if (strcmp(command, "STOP") == 0) {
    gLogging = false;
    strlcpy(gLabel, "none", sizeof(gLabel));
    strlcpy(gSession, "none", sizeof(gSession));
    Serial.println("LOGGING stopped");
    return;
  }
  if (strcmp(command, "CALIBRATE") == 0) {
    gFeatures.startCalibration();
    gPreferences.remove("ambient");
    Serial.println("CALIBRATION started; keep the monitored area empty for 30 seconds.");
    return;
  }
  if (strcmp(command, "STATUS") == 0) {
    Serial.printf("STATUS state=%s online=%s calibration=%u%% model=%s "
                  "queue_drops=%lu malformed=%lu sequence_gaps=%lu "
                  "csi_cb=%lu csi_mac_rejects=%lu last_csi_mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  detectorStateName(gStateMachine.state()),
                  (millis() - gLastPacketMs < kOfflineAfterMs) ? "yes" : "no",
                  gFeatures.calibrationPercent(), gModel.ready() ? "ready" : "untrained",
                  static_cast<unsigned long>(gQueueDrops),
                  static_cast<unsigned long>(gMalformedFrames),
                  static_cast<unsigned long>(gSequenceGaps),
                  static_cast<unsigned long>(gCsiCallbacks),
                  static_cast<unsigned long>(gCsiMacRejects),
                  gLastCsiMac[0], gLastCsiMac[1], gLastCsiMac[2],
                  gLastCsiMac[3], gLastCsiMac[4], gLastCsiMac[5]);
    return;
  }
  Serial.println("Commands: LABEL <empty|still|moving> <session>, STOP, CALIBRATE, STATUS");
}

void serviceSerial() {
  static char buffer[96] = {};
  static size_t used = 0;
  while (Serial.available()) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\r') continue;
    if (value == '\n') {
      buffer[used] = '\0';
      if (used > 0) handleCommand(buffer);
      used = 0;
      continue;
    }
    if (used + 1 < sizeof(buffer)) buffer[used++] = value;
  }
}

void printRawFrame(const CsiQueueFrame& frame) {
  if (!gLogging) return;
  Serial.printf("{\"type\":\"raw_csi\",\"timestamp_us\":%llu,\"sequence\":%lu,"
                "\"source_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                "\"channel\":%u,\"rssi\":%d,\"csi_length\":%u,"
                "\"first_word_invalid\":%s,\"iq\":[",
                static_cast<unsigned long long>(frame.timestampUs),
                static_cast<unsigned long>(frame.sequence),
                frame.sourceMac[0], frame.sourceMac[1], frame.sourceMac[2],
                frame.sourceMac[3], frame.sourceMac[4], frame.sourceMac[5],
                frame.channel, frame.rssi, frame.length,
                frame.firstWordInvalid ? "true" : "false");
  for (size_t i = 0; i < frame.length; ++i) {
    if (i) Serial.write(',');
    Serial.print(frame.iq[i]);
  }
  Serial.printf("],\"label\":\"%s\",\"session_id\":\"%s\"}\n",
                gLabel, gSession);
}

void printFeature(const RuViewFeatureVector& feature) {
  Serial.printf("{\"type\":\"feature\",\"timestamp_us\":%llu,"
                "\"sequence\":%lu,\"rssi\":%d,\"subcarriers\":%u,"
                "\"features\":[",
                static_cast<unsigned long long>(feature.timestampUs),
                static_cast<unsigned long>(feature.sourceSequence),
                feature.rssi, feature.subcarriers);
  for (size_t i = 0; i < 8; ++i) {
    if (i) Serial.write(',');
    Serial.print(feature.values[i], 8);
  }
  Serial.printf("],\"label\":\"%s\",\"session_id\":\"%s\"}\n",
                gLabel, gSession);
}

void printState(DetectorState state, const ModelPrediction& prediction) {
  Serial.printf("{\"type\":\"state\",\"state\":\"%s\","
                "\"confidence\":%.6f,\"probabilities\":[%.6f,%.6f,%.6f],"
                "\"inference_us\":%lu,\"model_sha256\":\"%s\"}\n",
                detectorStateName(state), prediction.confidence,
                prediction.probabilities[0], prediction.probabilities[1],
                prediction.probabilities[2],
                static_cast<unsigned long>(prediction.latencyUs),
                kRuvModelSha256);
}

void configureRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
  ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
  ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_recv_cb(onEspNowReceive));

  if (!isZeroMac(TRANSMITTER_MAC)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, TRANSMITTER_MAC, sizeof(peer.peer_addr));
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    const esp_err_t result = esp_now_add_peer(&peer);
    if (result != ESP_OK && result != ESP_ERR_ESPNOW_EXIST) ESP_ERROR_CHECK(result);
  }

  wifi_csi_config_t config = {};
  config.lltf_en = true;
  config.htltf_en = true;
  config.stbc_htltf2_en = true;
  config.ltf_merge_en = true;
  config.channel_filter_en = false;
  config.manu_scale = false;
  ESP_ERROR_CHECK(esp_wifi_set_csi_config(&config));
  ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(onCsi, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

}  // namespace

void setup() {
  Serial.begin(921600);
  delay(500);
  gCsiQueue = xQueueCreate(16, sizeof(CsiQueueFrame));
  if (gCsiQueue == nullptr) {
    Serial.println("FATAL: cannot allocate CSI queue.");
    while (true) delay(1000);
  }

  configureRadio();
  printStationMac();
  if (isZeroMac(TRANSMITTER_MAC)) {
    Serial.println("ERROR: edit include/peer_config.h and set TRANSMITTER_MAC.");
  }

  gPreferences.begin("ruview", false);
  const float storedAmbient = gPreferences.getFloat("ambient", -1.0f);
  if (storedAmbient > 0.0f && isfinite(storedAmbient)) {
    gFeatures.setAmbientThreshold(storedAmbient);
    Serial.printf("Loaded ambient threshold %.8f from NVS.\n", storedAmbient);
  } else {
    Serial.println("Keep the monitored area empty: 30-second calibration started.");
  }
  Serial.printf("Channel=%u model=%s commands: LABEL, STOP, CALIBRATE, STATUS\n",
                ESPNOW_CHANNEL, gModel.ready() ? "ready" : "untrained");
}

void loop() {
  serviceSerial();

  CsiQueueFrame frame = {};
  while (xQueueReceive(gCsiQueue, &frame, 0) == pdTRUE) {
    printRawFrame(frame);
    const bool wasCalibrating = gFeatures.calibrating();
    gFeatures.processFrame(frame.iq, frame.length, frame.firstWordInvalid,
                           frame.rssi, frame.sequence, frame.timestampUs);
    if (wasCalibrating && !gFeatures.calibrating()) {
      gPreferences.putFloat("ambient", gFeatures.ambientThreshold());
      Serial.printf("CALIBRATION complete ambient_threshold=%.8f\n",
                    gFeatures.ambientThreshold());
    }

    RuViewFeatureVector feature = {};
    if (gFeatures.takeFeature(&feature)) {
      printFeature(feature);
      const ModelPrediction prediction = gModel.predict(feature.values);
      const bool online = millis() - gLastPacketMs < kOfflineAfterMs;
      const DetectorState previous = gStateMachine.state();
      const DetectorState current = gStateMachine.update(
          online, gFeatures.calibrating(), prediction.ready,
          prediction.classIndex, prediction.confidence,
          kRuvConfidenceThreshold);
      if (current != previous) printState(current, prediction);
    }
  }

  static uint32_t lastHealthMs = 0;
  if (millis() - lastHealthMs >= 1000) {
    lastHealthMs = millis();
    const bool online = millis() - gLastPacketMs < kOfflineAfterMs;
    if (!online && gStateMachine.state() != DetectorState::SENSOR_OFFLINE) {
      const ModelPrediction emptyPrediction = {};
      const DetectorState current = gStateMachine.update(
          false, gFeatures.calibrating(), gModel.ready(), 0, 0.0f,
          kRuvConfidenceThreshold);
      printState(current, emptyPrediction);
    }
  }
  delay(1);
}
