#include "ruview_features.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kSampleRateHz = 20.0f;
constexpr uint32_t kFeatureEveryFrames = 5;  // 4 Hz
constexpr size_t kMotionWindow = 20;         // 1 second
constexpr float kFallAccelerationThreshold = 1.5f;
}

RuViewFeatureExtractor::RuViewFeatureExtractor() {
  resetSignalState();
  startCalibration();
}

void RuViewFeatureExtractor::resetSignalState() {
  memset(subcarrierStats_, 0, sizeof(subcarrierStats_));
  memset(previousPhase_, 0, sizeof(previousPhase_));
  memset(phaseInitialized_, 0, sizeof(phaseInitialized_));
  memset(topK_, 0, sizeof(topK_));
  memset(phaseHistory_, 0, sizeof(phaseHistory_));
  memset(breathingHistory_, 0, sizeof(breathingHistory_));
  memset(heartHistory_, 0, sizeof(heartHistory_));
  memset(&breathingFilter_, 0, sizeof(breathingFilter_));
  memset(&heartFilter_, 0, sizeof(heartFilter_));
  memset(&pendingFeature_, 0, sizeof(pendingFeature_));
  topKCount_ = 0;
  historyHead_ = 0;
  historyCount_ = 0;
  frameCount_ = 0;
  motionEnergy_ = 0.0f;
  previousVelocity_ = 0.0f;
  fallConsecutive_ = 0;
  fallHoldUntilMs_ = 0;
  featurePending_ = false;
}

void RuViewFeatureExtractor::startCalibration() {
  resetSignalState();
  calibrating_ = true;
  calibrationCount_ = 0;
  calibrationSum_ = 0.0;
  calibrationSumSquares_ = 0.0;
  ambientThreshold_ = 0.0f;
}

void RuViewFeatureExtractor::setAmbientThreshold(float threshold) {
  ambientThreshold_ = fmaxf(threshold, 1.0e-5f);
  calibrating_ = false;
}

uint8_t RuViewFeatureExtractor::calibrationPercent() const {
  if (!calibrating_) return 100;
  const uint32_t percent = calibrationCount_ * 100U / kCalibrationFrames;
  return static_cast<uint8_t>(percent > 100U ? 100U : percent);
}

float RuViewFeatureExtractor::clamp01(float value) {
  return fminf(1.0f, fmaxf(0.0f, value));
}

float RuViewFeatureExtractor::unwrap(float previous, float current) {
  float difference = current - previous;
  if (difference > kPi) difference -= 2.0f * kPi;
  if (difference < -kPi) difference += 2.0f * kPi;
  return previous + difference;
}

void RuViewFeatureExtractor::updateWelford(WelfordState* state, float value) {
  ++state->count;
  const float delta = value - state->mean;
  state->mean += delta / static_cast<float>(state->count);
  const float delta2 = value - state->mean;
  state->m2 += delta * delta2;
}

float RuViewFeatureExtractor::variance(const WelfordState& state) {
  return state.count > 1 ? state.m2 / static_cast<float>(state.count - 1) : 0.0f;
}

float RuViewFeatureExtractor::processBand(BandFilter* state, float input,
                                          float lowCutHz, float highCutHz) {
  const float dt = 1.0f / kSampleRateHz;
  if (!state->initialized) {
    state->previousInput = input;
    state->initialized = true;
  }
  const float highRc = 1.0f / (2.0f * kPi * lowCutHz);
  const float highAlpha = highRc / (highRc + dt);
  state->highpass = highAlpha *
                    (state->highpass + input - state->previousInput);
  state->previousInput = input;

  const float lowRc = 1.0f / (2.0f * kPi * highCutHz);
  const float lowAlpha = dt / (lowRc + dt);
  state->lowpass += lowAlpha * (state->highpass - state->lowpass);
  return state->lowpass;
}

float RuViewFeatureExtractor::estimateBpm(const float* history, size_t head,
                                          size_t count, float minimumBpm,
                                          float maximumBpm) {
  if (count < 40) return 0.0f;
  const size_t start = (head + kHistory - count) % kHistory;
  float previous = history[start];
  uint16_t crossings = 0;
  for (size_t index = 1; index < count; ++index) {
    const float current = history[(start + index) % kHistory];
    if (previous <= 0.0f && current > 0.0f) ++crossings;
    previous = current;
  }
  const float durationSeconds = static_cast<float>(count - 1) / kSampleRateHz;
  const float bpm = durationSeconds > 0.0f
                        ? static_cast<float>(crossings) * 60.0f / durationSeconds
                        : 0.0f;
  return bpm >= minimumBpm && bpm <= maximumBpm ? bpm : 0.0f;
}

void RuViewFeatureExtractor::updateTopK(size_t subcarrierCount) {
  topKCount_ = min(kTopK, subcarrierCount);
  bool used[kMaxSubcarriers] = {};
  for (size_t rank = 0; rank < topKCount_; ++rank) {
    float bestVariance = -1.0f;
    size_t bestIndex = 0;
    for (size_t index = 0; index < subcarrierCount; ++index) {
      const float candidate = variance(subcarrierStats_[index]);
      if (!used[index] && candidate > bestVariance) {
        bestVariance = candidate;
        bestIndex = index;
      }
    }
    used[bestIndex] = true;
    topK_[rank] = static_cast<uint8_t>(bestIndex);
  }
}

float RuViewFeatureExtractor::recentVariance(size_t window) const {
  const size_t count = min(window, historyCount_);
  if (count < 2) return 0.0f;
  double sum = 0.0;
  double sumSquares = 0.0;
  for (size_t offset = 0; offset < count; ++offset) {
    const size_t index = (historyHead_ + kHistory - 1 - offset) % kHistory;
    const float value = phaseHistory_[index];
    sum += value;
    sumSquares += static_cast<double>(value) * value;
  }
  const double mean = sum / static_cast<double>(count);
  return static_cast<float>(fmax(0.0, sumSquares / count - mean * mean));
}

uint8_t RuViewFeatureExtractor::estimatePersonCount() const {
  if (topKCount_ == 0 || motionEnergy_ < ambientThreshold_) return 0;
  float strongest = 0.0f;
  float groupEnergy[4] = {};
  for (size_t i = 0; i < topKCount_; ++i) {
    const size_t group = min(static_cast<size_t>(3), i * 4 / topKCount_);
    groupEnergy[group] = fmaxf(groupEnergy[group],
                               variance(subcarrierStats_[topK_[i]]));
    strongest = fmaxf(strongest, groupEnergy[group]);
  }
  const float threshold = fmaxf(ambientThreshold_ * 3.0f, strongest * 0.20f);
  uint8_t persons = 0;
  for (float energy : groupEnergy) {
    if (energy >= threshold) ++persons;
  }
  return persons == 0 ? 1 : persons;
}

void RuViewFeatureExtractor::finishCalibration() {
  const double count = static_cast<double>(calibrationCount_);
  const double mean = calibrationSum_ / count;
  const double varianceValue = fmax(0.0, calibrationSumSquares_ / count - mean * mean);
  ambientThreshold_ = static_cast<float>(fmax(1.0e-5, mean + 3.0 * sqrt(varianceValue)));
  calibrating_ = false;
}

void RuViewFeatureExtractor::buildFeature(uint32_t sourceSequence,
                                          uint64_t timestampUs, int8_t rssi,
                                          uint16_t subcarrierCount) {
  const float breathingBpm = estimateBpm(breathingHistory_, historyHead_,
                                         historyCount_, 6.0f, 30.0f);
  const float heartBpm = estimateBpm(heartHistory_, historyHead_, historyCount_,
                                     40.0f, 120.0f);
  float meanVariance = 0.0f;
  for (size_t i = 0; i < topKCount_; ++i) {
    meanVariance += variance(subcarrierStats_[topK_[i]]);
  }
  if (topKCount_ > 0) meanVariance /= static_cast<float>(topKCount_);

  const float normalizedMotion = clamp01(motionEnergy_ / 10.0f);
  pendingFeature_.values[0] = normalizedMotion;  // RuView presence score
  pendingFeature_.values[1] = normalizedMotion;
  pendingFeature_.values[2] = clamp01(breathingBpm / 30.0f);
  pendingFeature_.values[3] = clamp01(heartBpm / 120.0f);
  pendingFeature_.values[4] = clamp01(meanVariance);
  pendingFeature_.values[5] = clamp01(estimatePersonCount() / 4.0f);
  pendingFeature_.values[6] = millis() < fallHoldUntilMs_ ? 1.0f : 0.0f;
  pendingFeature_.values[7] = clamp01((static_cast<float>(rssi) + 100.0f) / 100.0f);
  pendingFeature_.sourceSequence = sourceSequence;
  pendingFeature_.timestampUs = timestampUs;
  pendingFeature_.rssi = rssi;
  pendingFeature_.subcarriers = subcarrierCount;
  featurePending_ = true;
}

bool RuViewFeatureExtractor::processFrame(const int8_t* iqBytes,
                                          size_t byteLength,
                                          bool firstWordInvalid, int8_t rssi,
                                          uint32_t sourceSequence,
                                          uint64_t timestampUs) {
  if (iqBytes == nullptr || byteLength < 4 || (byteLength & 1U) != 0) return false;
  const size_t offset = firstWordInvalid && byteLength > 4 ? 4 : 0;
  const size_t subcarrierCount = min(kMaxSubcarriers, (byteLength - offset) / 2);
  if (subcarrierCount < 8) return false;

  float phases[kMaxSubcarriers];
  for (size_t subcarrier = 0; subcarrier < subcarrierCount; ++subcarrier) {
    // RuView compatibility: byte 0 is treated as I and byte 1 as Q.
    const float iValue = iqBytes[offset + subcarrier * 2];
    const float qValue = iqBytes[offset + subcarrier * 2 + 1];
    const float raw = atan2f(qValue, iValue);
    phases[subcarrier] = phaseInitialized_[subcarrier]
                             ? unwrap(previousPhase_[subcarrier], raw)
                             : raw;
    previousPhase_[subcarrier] = phases[subcarrier];
    phaseInitialized_[subcarrier] = true;
    updateWelford(&subcarrierStats_[subcarrier], phases[subcarrier]);
  }

  ++frameCount_;
  if (topKCount_ == 0 || frameCount_ % 20U == 1U) updateTopK(subcarrierCount);
  if (topKCount_ == 0) return false;

  const float primaryPhase = phases[topK_[0]];
  const size_t writeIndex = historyHead_;
  phaseHistory_[writeIndex] = primaryPhase;
  breathingHistory_[writeIndex] = processBand(&breathingFilter_, primaryPhase,
                                              0.10f, 0.50f);
  heartHistory_[writeIndex] = processBand(&heartFilter_, primaryPhase,
                                          0.80f, 2.00f);
  historyHead_ = (historyHead_ + 1) % kHistory;
  historyCount_ = min(kHistory, historyCount_ + 1);

  motionEnergy_ = recentVariance(kMotionWindow);
  if (historyCount_ >= 2) {
    const size_t previousIndex = (historyHead_ + kHistory - 2) % kHistory;
    const float velocity = primaryPhase - phaseHistory_[previousIndex];
    const float acceleration = fabsf(velocity - previousVelocity_);
    previousVelocity_ = velocity;
    if (acceleration > kFallAccelerationThreshold) {
      if (++fallConsecutive_ >= 3) {
        fallHoldUntilMs_ = millis() + 5000;
        fallConsecutive_ = 0;
      }
    } else {
      fallConsecutive_ = 0;
    }
  }

  if (calibrating_) {
    ++calibrationCount_;
    calibrationSum_ += motionEnergy_;
    calibrationSumSquares_ += static_cast<double>(motionEnergy_) * motionEnergy_;
    if (calibrationCount_ >= kCalibrationFrames) finishCalibration();
  }

  if (historyCount_ >= 40 && frameCount_ % kFeatureEveryFrames == 0) {
    buildFeature(sourceSequence, timestampUs, rssi,
                 static_cast<uint16_t>(subcarrierCount));
  }
  return true;
}

bool RuViewFeatureExtractor::takeFeature(RuViewFeatureVector* output) {
  if (!featurePending_ || output == nullptr) return false;
  *output = pendingFeature_;
  featurePending_ = false;
  return true;
}

