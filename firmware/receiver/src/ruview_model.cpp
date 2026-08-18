#include "ruview_model.h"

#include <Arduino.h>
#include <math.h>

#include "model_data.h"

bool RuViewModel::ready() const {
  return kRuvModelReady;
}

float RuViewModel::gelu(float value) {
  const float cubic = value * value * value;
  return 0.5f * value *
         (1.0f + tanhf(0.7978845608f * (value + 0.044715f * cubic)));
}

float RuViewModel::quantizeVector(const float* input, int8_t* output,
                                  size_t length) {
  float maximum = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    maximum = fmaxf(maximum, fabsf(input[i]));
  }
  const float scale = maximum > 1.0e-12f ? maximum / 127.0f : 1.0f;
  for (size_t i = 0; i < length; ++i) {
    const long quantized = lroundf(input[i] / scale);
    output[i] = static_cast<int8_t>(constrain(quantized, -127L, 127L));
  }
  return scale;
}

void RuViewModel::fullyConnected(const int8_t* weights,
                                 const float* weightScales,
                                 const float* biases,
                                 size_t outputDim, size_t inputDim,
                                 const int8_t* input, float inputScale,
                                 float* output) {
  for (size_t row = 0; row < outputDim; ++row) {
    int32_t accumulator = 0;
    const int8_t* rowWeights = weights + row * inputDim;
    for (size_t column = 0; column < inputDim; ++column) {
      accumulator += static_cast<int32_t>(rowWeights[column]) *
                     static_cast<int32_t>(input[column]);
    }
    output[row] = static_cast<float>(accumulator) * inputScale *
                      weightScales[row] +
                  biases[row];
  }
}

ModelPrediction RuViewModel::predict(const float features[8]) {
  ModelPrediction prediction = {};
  prediction.ready = kRuvModelReady;
  prediction.classIndex = 0;
  prediction.confidence = 0.0f;
  if (!kRuvModelReady) return prediction;

  const uint32_t started = micros();
  float standardized[8];
  int8_t inputQ[8];
  float hidden1[64];
  int8_t hidden1Q[64];
  float embedding[128];
  int8_t embeddingQ[128];
  float logits[3];

  for (size_t i = 0; i < 8; ++i) {
    const float denominator = fabsf(kRuvInputStd[i]) > 1.0e-9f
                                  ? kRuvInputStd[i]
                                  : 1.0f;
    standardized[i] = (features[i] - kRuvInputMean[i]) / denominator;
  }

  const float inputScale = quantizeVector(standardized, inputQ, 8);
  fullyConnected(kRuvW1, kRuvW1Scale, kRuvB1, 64, 8, inputQ,
                 inputScale, hidden1);
  for (float& value : hidden1) value = gelu(value);

  const float hiddenScale = quantizeVector(hidden1, hidden1Q, 64);
  fullyConnected(kRuvW2, kRuvW2Scale, kRuvB2, 128, 64, hidden1Q,
                 hiddenScale, embedding);

  float normSquared = 0.0f;
  for (float value : embedding) normSquared += value * value;
  const float inverseNorm = 1.0f / sqrtf(fmaxf(normSquared, 1.0e-12f));
  for (float& value : embedding) value *= inverseNorm;

  const float embeddingScale = quantizeVector(embedding, embeddingQ, 128);
  fullyConnected(kRuvHeadW, kRuvHeadScale, kRuvHeadBias, 3, 128,
                 embeddingQ, embeddingScale, logits);

  const float maxLogit = fmaxf(logits[0], fmaxf(logits[1], logits[2]));
  float probabilitySum = 0.0f;
  for (size_t i = 0; i < 3; ++i) {
    prediction.probabilities[i] = expf(logits[i] - maxLogit);
    probabilitySum += prediction.probabilities[i];
  }
  for (size_t i = 0; i < 3; ++i) {
    prediction.probabilities[i] /= probabilitySum;
    if (prediction.probabilities[i] > prediction.confidence) {
      prediction.confidence = prediction.probabilities[i];
      prediction.classIndex = i;
    }
  }
  prediction.latencyUs = micros() - started;
  return prediction;
}

