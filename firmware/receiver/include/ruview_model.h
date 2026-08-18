#pragma once

#include <stddef.h>
#include <stdint.h>

struct ModelPrediction {
  uint8_t classIndex;
  float confidence;
  float probabilities[3];
  uint32_t latencyUs;
  bool ready;
};

class RuViewModel {
 public:
  bool ready() const;
  ModelPrediction predict(const float features[8]);

 private:
  static float gelu(float value);
  static float quantizeVector(const float* input, int8_t* output, size_t length);
  static void fullyConnected(const int8_t* weights,
                             const float* weightScales,
                             const float* biases,
                             size_t outputDim,
                             size_t inputDim,
                             const int8_t* input,
                             float inputScale,
                             float* output);
};
