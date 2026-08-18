#pragma once

#include <stdint.h>

enum class DetectorState : uint8_t {
  CALIBRATING,
  EMPTY,
  STILL_PERSON,
  MOVING_PERSON,
  UNKNOWN,
  SENSOR_OFFLINE,
};

const char* detectorStateName(DetectorState state);

class DetectorStateMachine {
 public:
  DetectorState update(bool online, bool calibrating, bool modelReady,
                       uint8_t predictedClass, float confidence,
                       float confidenceThreshold);
  DetectorState state() const { return state_; }

 private:
  DetectorState state_ = DetectorState::CALIBRATING;
  DetectorState candidate_ = DetectorState::UNKNOWN;
  uint8_t candidateCount_ = 0;
  uint8_t lowConfidenceCount_ = 0;
};

