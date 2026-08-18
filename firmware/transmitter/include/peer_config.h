#pragma once

#include <stdint.h>

// Replace this with the Wi-Fi station MAC printed by the receiver.
// Example: {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC}
static constexpr uint8_t RECEIVER_MAC[6] = {0x94, 0xA9, 0x90, 0xD1, 0xDB, 0x40};

static constexpr uint8_t ESPNOW_CHANNEL = 6;
static constexpr uint32_t TX_INTERVAL_US = 50000;  // 20 Hz

