#pragma once

#include <stddef.h>
#include <stdint.h>

struct RuViewFeatureVector {
  float values[8];
  uint32_t sourceSequence;
  uint64_t timestampUs;
  int8_t rssi;
  uint16_t subcarriers;
};

class RuViewFeatureExtractor {
 public:
  static constexpr size_t kMaxSubcarriers = 128;
  static constexpr size_t kTopK = 8;
  static constexpr size_t kHistory = 256;
  static constexpr uint32_t kCalibrationFrames = 600;  // 30 seconds at 20 Hz

  RuViewFeatureExtractor();

  void startCalibration();
  void setAmbientThreshold(float threshold);
  bool calibrating() const { return calibrating_; }
  uint8_t calibrationPercent() const;
  float ambientThreshold() const { return ambientThreshold_; }

  bool processFrame(const int8_t* iqBytes, size_t byteLength,
                    bool firstWordInvalid, int8_t rssi,
                    uint32_t sourceSequence, uint64_t timestampUs);
  bool takeFeature(RuViewFeatureVector* output);

 private:
  struct WelfordState {
    uint32_t count;
    float mean;
    float m2;
  };

  struct BandFilter {
    float previousInput;
    float highpass;
    float lowpass;
    bool initialized;
  };

  static float clamp01(float value);
  static float unwrap(float previous, float current);
  static void updateWelford(WelfordState* state, float value);
  static float variance(const WelfordState& state);
  static float processBand(BandFilter* state, float input,
                           float lowCutHz, float highCutHz);
  static float estimateBpm(const float* history, size_t head, size_t count,
                           float minimumBpm, float maximumBpm);

  void resetSignalState();
  void updateTopK(size_t subcarrierCount);
  float recentVariance(size_t window) const;
  uint8_t estimatePersonCount() const;
  void finishCalibration();
  void buildFeature(uint32_t sourceSequence, uint64_t timestampUs,
                    int8_t rssi, uint16_t subcarrierCount);

  WelfordState subcarrierStats_[kMaxSubcarriers];
  float previousPhase_[kMaxSubcarriers];
  bool phaseInitialized_[kMaxSubcarriers];
  uint8_t topK_[kTopK];
  size_t topKCount_;

  float phaseHistory_[kHistory];
  float breathingHistory_[kHistory];
  float heartHistory_[kHistory];
  size_t historyHead_;
  size_t historyCount_;
  BandFilter breathingFilter_;
  BandFilter heartFilter_;

  uint32_t frameCount_;
  float motionEnergy_;
  float previousVelocity_;
  uint8_t fallConsecutive_;
  uint32_t fallHoldUntilMs_;

  bool calibrating_;
  uint32_t calibrationCount_;
  double calibrationSum_;
  double calibrationSumSquares_;
  float ambientThreshold_;

  bool featurePending_;
  RuViewFeatureVector pendingFeature_;
};

