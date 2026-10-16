"""Compile the real audio implementation with host I2S/filesystem fakes."""
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
TEMP_ROOT = os.environ.get("RESPICARE_TEST_TMPDIR", tempfile.gettempdir())

ARDUINO = r"""
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
using std::min;
inline void delay(int) {}
"""
FS = r"""
#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
class File {
 public:
  std::vector<uint8_t> data;
  size_t pos = 0;
  bool broken = false;
  bool seek(size_t p) { pos = p; return !broken; }
  size_t write(const uint8_t *p, size_t n) {
    if (broken) return 0;
    if (pos + n > data.size()) data.resize(pos + n);
    memcpy(data.data() + pos, p, n); pos += n; return n;
  }
  void flush() {}
  size_t size() { return data.size(); }
};
"""
I2S = r"""
#pragma once
#include <cassert>
#include <cstdint>
#include <climits>
typedef int i2s_mode_t;
enum {I2S_MODE_MASTER=1, I2S_MODE_RX=2, I2S_BITS_PER_SAMPLE_32BIT=32,
 I2S_CHANNEL_FMT_ONLY_LEFT=1, I2S_COMM_FORMAT_STAND_I2S=1,
 ESP_INTR_FLAG_LEVEL1=1, I2S_PIN_NO_CHANGE=-1, I2S_NUM_0=0, ESP_OK=0};
struct i2s_config_t {
 int mode, sample_rate, bits_per_sample, channel_format, communication_format;
 int intr_alloc_flags, dma_buf_count, dma_buf_len;
 bool use_apll, tx_desc_auto_clear; int fixed_mclk;
};
struct i2s_pin_config_t {int bck_io_num, ws_io_num, data_out_num, data_in_num;};
inline int i2s_driver_install(int port, i2s_config_t *c, int, void *) {
 assert(port==0 && c->sample_rate==16000 && c->bits_per_sample==32);
 assert(c->channel_format==1 && c->dma_buf_count==8 && c->dma_buf_len==256);
 return 0;
}
inline int i2s_set_pin(int, i2s_pin_config_t *p) {
 assert(p->bck_io_num==18 && p->ws_io_num==19 && p->data_in_num==34);
 return 0;
}
inline int i2s_zero_dma_buffer(int) {return 0;}
#define pdMS_TO_TICKS(v) (v)
inline bool failRead = false;
inline uint32_t sampleIndex = 0;
inline const int32_t samples[] = {INT32_MIN, -65536, 0, 65536, INT32_MAX};
inline int i2s_read(int, void *raw, size_t bytes, size_t *read, int timeout) {
 assert(timeout==250);
 if (failRead) {*read=0; return -1;}
 for (size_t i=0; i<bytes/4; ++i)
   static_cast<int32_t *>(raw)[i]=samples[sampleIndex++ % 5];
 *read=bytes; return 0;
}
"""
TEST = r"""
#include "AudioRecorder.cpp"
#include <cassert>
#include <cstdio>
int main() {
 AudioRecorder audio;
 assert(audio.begin());
 int16_t pcm[kBufferSize];
 assert(audio.read(pcm)==256);
 const int16_t expected[]={-32768,-1,0,1,32767};
 for (size_t i=0;i<256;++i) assert(pcm[i]==expected[i%5]);
 File file;
 sampleIndex=0;
 assert(audio.record(file,1));
 assert(file.size()==32044);
 fwrite(file.data.data(),1,file.data.size(),stdout);
 File tooLarge;
 assert(!audio.record(tooLarge,1639));
 File broken; broken.broken=true;
 assert(!audio.record(broken,1));
 failRead=true;
 File interrupted;
 assert(!audio.record(interrupted,1));
}
"""

with tempfile.TemporaryDirectory(prefix="respicare-audio-", dir=TEMP_ROOT) as tmp:
    path = Path(tmp)
    (path / "driver").mkdir()
    for name, content in {"Arduino.h": ARDUINO, "FS.h": FS,
                          "driver/i2s.h": I2S, "test.cpp": TEST}.items():
        (path / name).write_text(content)
    binary = path / "audio-test"
    subprocess.run(["c++", "-std=c++17", "-I", str(path), "-I", str(ROOT / "src"),
                    str(path / "test.cpp"), "-o", str(binary)], check=True)
    wav = subprocess.check_output([str(binary)])
    assert wav[:4] == b"RIFF" and wav[8:12] == b"WAVE"
    assert struct.unpack_from("<H", wav, 20)[0] == 1
    assert struct.unpack_from("<I", wav, 4)[0] == len(wav) - 8
    with wave.open(io.BytesIO(wav), "rb") as reader:
        assert (reader.getnchannels(), reader.getsampwidth(), reader.getframerate(),
                reader.getnframes()) == (1, 2, 16000, 16000)
        values = struct.unpack("<16000h", reader.readframes(16000))
        assert values != tuple([-32768, -1, 0, 1, 32767][i % 5] for i in range(16000))
        assert max(map(abs, values[100:])) < 15000  # High-frequency fixture is attenuated.
        assert values[255] != values[256]  # Filter state continues across reads.
print("PASS: original I2S settings, filtered PCM WAV, size/write/read failures")
