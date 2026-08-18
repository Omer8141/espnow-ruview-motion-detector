#pragma once

#include <stdint.h>

// Safe placeholder. training/export_c.py replaces this file with trained data.
// With kRuvModelReady=false the receiver reports UNKNOWN instead of pretending
// that zero-filled weights are a trained model.
static constexpr bool kRuvModelReady = false;
static constexpr uint32_t kRuvModelVersion = 1;
static constexpr float kRuvConfidenceThreshold = 0.65f;
static constexpr char kRuvModelSha256[] = "UNTRAINED";

static constexpr float kRuvInputMean[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static constexpr float kRuvInputStd[8] = {1, 1, 1, 1, 1, 1, 1, 1};

static const int8_t kRuvW1[64 * 8] = {0};
static constexpr float kRuvW1Scale[64] = {0};
static constexpr float kRuvB1[64] = {0};

static const int8_t kRuvW2[128 * 64] = {0};
static constexpr float kRuvW2Scale[128] = {0};
static constexpr float kRuvB2[128] = {0};

static const int8_t kRuvHeadW[3 * 128] = {0};
static constexpr float kRuvHeadScale[3] = {0};
static constexpr float kRuvHeadBias[3] = {0};

static constexpr const char* kRuvClassLabels[3] = {
    "empty", "still", "moving"
};

