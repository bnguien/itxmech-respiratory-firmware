import serial
import wave
import time

PORT = "COM11"         

BAUD_RATE = 921600

SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2       # 16-bit PCM

DURATION = 20          # số giây muốn thu

OUTPUT_FILE = "lung_sound_raw.wav"

ser = serial.Serial(
    PORT,
    BAUD_RATE,
    timeout=2
)

time.sleep(2)

ser.reset_input_buffer()

bytes_per_second = (
    SAMPLE_RATE
    * CHANNELS
    * SAMPLE_WIDTH
)

target_bytes = (
    bytes_per_second
    * DURATION
)

print("Recording...")
print("Sample rate:", SAMPLE_RATE)
print("Duration:", DURATION)

audio = bytearray()

while len(audio) < target_bytes:
    remaining = target_bytes - len(audio)

    data = ser.read(
        min(4096, remaining)
    )

    if data:
        audio.extend(data)

ser.close()

with wave.open(OUTPUT_FILE, "wb") as wav:
    wav.setnchannels(CHANNELS)
    wav.setsampwidth(SAMPLE_WIDTH)
    wav.setframerate(SAMPLE_RATE)
    wav.writeframes(audio)

print("Done:", OUTPUT_FILE)