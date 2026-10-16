#include "AudioRecorder.h"
#include "driver/i2s.h"

bool AudioRecorder::begin() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = kSampleRate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = kBufferSize,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = 18,
    .ws_io_num = 19,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = 34
  };
  return i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) == ESP_OK &&
         i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK &&
         i2s_zero_dma_buffer(I2S_NUM_0) == ESP_OK;
}

int AudioRecorder::read(int16_t *pcm) {
  int32_t raw[kBufferSize];
  size_t bytesRead = 0;
  if (i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead,
               pdMS_TO_TICKS(250)) != ESP_OK || bytesRead == 0) return -1;
  int count = bytesRead / sizeof(int32_t);
  for (int i = 0; i < count; ++i) pcm[i] = (int16_t)(raw[i] >> kBitShift);
  return count;
}

bool WavWriter::header(File &file, uint32_t pcmBytes) {
  uint8_t h[44] = {};
  auto u16 = [&](int at, uint16_t v) {
    h[at] = v; h[at + 1] = v >> 8;
  };
  auto u32 = [&](int at, uint32_t v) {
    for (int i = 0; i < 4; ++i) h[at + i] = v >> (8 * i);
  };
  memcpy(h, "RIFF", 4); u32(4, pcmBytes + 36);
  memcpy(h + 8, "WAVEfmt ", 8); u32(16, 16);
  u16(20, 1); u16(22, 1); u32(24, kSampleRate);
  u32(28, kSampleRate * 2); u16(32, 2); u16(34, 16);
  memcpy(h + 36, "data", 4); u32(40, pcmBytes);
  return file.seek(0) && file.write(h, sizeof(h)) == sizeof(h);
}

bool AudioRecorder::record(File &file, uint32_t seconds) {
  uint32_t target = seconds * kSampleRate * 2;
  if (target + 44 > kMaxWavBytes || !WavWriter::header(file, 0)) return false;
  // Discard stale DMA samples accumulated while idle; conversion is unchanged.
  i2s_zero_dma_buffer(I2S_NUM_0);
  uint32_t written = 0;
  int16_t pcm[kBufferSize];
  while (written < target) {
    int count = read(pcm);
    if (count <= 0) return false;
    size_t bytes = min(uint32_t(count * 2), target - written);
    if (file.write((uint8_t *)pcm, bytes) != bytes) return false;
    written += bytes;
    delay(1);
  }
  bool ok = WavWriter::header(file, written);
  file.flush();
  return ok && file.size() == target + 44;
}
