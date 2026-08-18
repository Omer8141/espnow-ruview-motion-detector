#include <Arduino.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "peer_config.h"

namespace {

constexpr uint32_t kPacketMagic = 0x52564353;  // "RVCS"
constexpr uint8_t kProtocolVersion = 1;

struct __attribute__((packed)) ProbePacket {
  uint32_t magic;
  uint8_t version;
  uint8_t reserved[3];
  uint32_t sequence;
  uint32_t txMicros;
};

volatile bool gSendInFlight = false;
volatile uint32_t gSendOk = 0;
volatile uint32_t gSendFailed = 0;
uint32_t gSequence = 0;
uint32_t gNextSendUs = 0;

bool isZeroMac(const uint8_t mac[6]) {
  uint8_t combined = 0;
  for (size_t i = 0; i < 6; ++i) combined |= mac[i];
  return combined == 0;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onSend(const esp_now_send_info_t*, esp_now_send_status_t status) {
#else
void onSend(const uint8_t*, esp_now_send_status_t status) {
#endif
  if (status == ESP_NOW_SEND_SUCCESS) {
    ++gSendOk;
  } else {
    ++gSendFailed;
  }
  gSendInFlight = false;
}

void printMac() {
  uint8_t mac[6] = {};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial.printf("Transmitter STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
  ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
  printMac();

  if (isZeroMac(RECEIVER_MAC)) {
    Serial.println("ERROR: edit include/peer_config.h and set RECEIVER_MAC.");
    return;
  }

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(onSend));

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_MAC, sizeof(peer.peer_addr));
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  ESP_ERROR_CHECK(esp_now_add_peer(&peer));

  // CSI is measured from the L-LTF/HT-LTF training fields, which only exist in
  // OFDM frames. ESP-NOW defaults to 1 Mbps 802.11b DSSS, which carries no such
  // fields, so the receiver's CSI callback never fires for our probes. Force an
  // OFDM rate, as Espressif's esp-csi examples do.
  const esp_err_t rateResult =
      esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_MCS0_LGI);
  if (rateResult != ESP_OK) {
    Serial.printf("ERROR: could not force OFDM ESP-NOW rate (%d); "
                  "the receiver will not see CSI.\n",
                  static_cast<int>(rateResult));
  }

  gNextSendUs = micros();
  Serial.printf("ESP-NOW probe active on channel %u at 20 Hz (MCS0_LGI).\n",
                ESPNOW_CHANNEL);
}

void loop() {
  if (isZeroMac(RECEIVER_MAC)) {
    delay(1000);
    return;
  }

  const uint32_t now = micros();
  if (!gSendInFlight && static_cast<int32_t>(now - gNextSendUs) >= 0) {
    ProbePacket packet = {};
    packet.magic = kPacketMagic;
    packet.version = kProtocolVersion;
    packet.sequence = gSequence++;
    packet.txMicros = now;

    gSendInFlight = true;
    const esp_err_t result = esp_now_send(RECEIVER_MAC,
                                          reinterpret_cast<const uint8_t*>(&packet),
                                          sizeof(packet));
    if (result != ESP_OK) {
      gSendInFlight = false;
      ++gSendFailed;
    }
    gNextSendUs += TX_INTERVAL_US;
    if (static_cast<int32_t>(now - gNextSendUs) > static_cast<int32_t>(TX_INTERVAL_US)) {
      gNextSendUs = now + TX_INTERVAL_US;
    }
  }

  static uint32_t lastReportMs = 0;
  if (millis() - lastReportMs >= 5000) {
    lastReportMs = millis();
    Serial.printf("tx_ok=%lu tx_failed=%lu sequence=%lu\n",
                  static_cast<unsigned long>(gSendOk),
                  static_cast<unsigned long>(gSendFailed),
                  static_cast<unsigned long>(gSequence));
  }
  delay(1);
}
