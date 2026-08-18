#include "detector_state.h"

const char* detectorStateName(DetectorState state) {
  switch (state) {
    case DetectorState::CALIBRATING: return "calibrating";
    case DetectorState::EMPTY: return "empty";
    case DetectorState::STILL_PERSON: return "still_person";
    case DetectorState::MOVING_PERSON: return "moving_person";
    case DetectorState::UNKNOWN: return "unknown";
    case DetectorState::SENSOR_OFFLINE: return "sensor_offline";
  }
  return "unknown";
}

DetectorState DetectorStateMachine::update(bool online, bool calibrating,
                                           bool modelReady,
                                           uint8_t predictedClass,
                                           float confidence,
                                           float confidenceThreshold) {
  if (!online) {
    state_ = DetectorState::SENSOR_OFFLINE;
    candidateCount_ = 0;
    return state_;
  }
  if (calibrating) {
    state_ = DetectorState::CALIBRATING;
    candidateCount_ = 0;
    return state_;
  }
  if (!modelReady) {
    state_ = DetectorState::UNKNOWN;
    candidateCount_ = 0;
    lowConfidenceCount_ = 0;
    return state_;
  }
  if (confidence < confidenceThreshold) {
    candidateCount_ = 0;
    if (++lowConfidenceCount_ >= 12) state_ = DetectorState::UNKNOWN;
    return state_;
  }
  lowConfidenceCount_ = 0;

  DetectorState proposed = DetectorState::UNKNOWN;
  if (predictedClass == 0) proposed = DetectorState::EMPTY;
  if (predictedClass == 1) proposed = DetectorState::STILL_PERSON;
  if (predictedClass == 2) proposed = DetectorState::MOVING_PERSON;

  if (proposed == state_) {
    candidate_ = proposed;
    candidateCount_ = 0;
    return state_;
  }
  if (proposed != candidate_) {
    candidate_ = proposed;
    candidateCount_ = 1;
  } else {
    ++candidateCount_;
  }

  const uint8_t required = proposed == DetectorState::MOVING_PERSON ? 2 : 4;
  if (candidateCount_ >= required) {
    state_ = proposed;
    candidateCount_ = 0;
  }
  return state_;
}
