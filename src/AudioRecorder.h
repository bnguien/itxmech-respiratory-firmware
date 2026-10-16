#pragma once
#include <Arduino.h>
#include <FS.h>

constexpr uint32_t kSampleRate = 16000;
constexpr size_t kBufferSize = 256;
constexpr int kBitShift = 16;
constexpr uint32_t kMaxWavBytes = 50UL * 1024 * 1024;

class WavWriter {
 public:
  static bool header(File &file, uint32_t pcmBytes);
};

class AudioRecorder {
 public:
  bool begin();
  int read(int16_t *pcm);
  bool record(File &file, uint32_t seconds);
};
