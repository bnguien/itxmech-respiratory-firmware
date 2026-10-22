# RespiCare Respiratory Firmware — Hướng dẫn cài đặt trên Windows

Firmware cho thiết bị ống nghe điện tử RespiCare sử dụng **ESP32**, micro I2S, Wi-Fi, MQTT/TLS và LittleFS để ghi WAV và gửi bản ghi lên backend qua HTTPS.

> Tài liệu này dành cho **Windows 10/11 + PowerShell**, đặc biệt là người vừa clone repository. Các bước nạp firmware giả định thiết bị sử dụng ESP32 DevKit tương thích cấu hình `esp32dev` và phân vùng flash đã được xác nhận. **Không xóa flash hoặc filesystem của thiết bị đang lưu dữ liệu.**

## 1. Yêu cầu chuẩn bị

- Git for Windows: https://git-scm.com/downloads/win
- Python 3: https://www.python.org/downloads/windows/ (kiểm tra `py --version`)
- ESP32 và **cáp USB truyền dữ liệu**; cài driver USB-UART phù hợp (CP210x/CH340) nếu Windows chưa nhận cổng COM.
- Kết nối Internet để PlatformIO tải công cụ và thư viện.
- Thông tin cấu hình MQTT, Web API và chứng chỉ CA **được cấp riêng** bởi người quản lý hệ thống.
- Nếu thử ghi âm: kiểm tra chân I2S của phần cứng đang sử dụng (theo cấu hình repo: **BCLK/SCK GPIO18, WS GPIO19, SD GPIO34**, nguồn **3.3 V**).

Đối chiếu phần cứng thật trước khi nạp: target trong `platformio.ini` là `esp32dev`, PlatformIO `espressif32@6.10.0`, Arduino, filesystem LittleFS và layout `default.csv`. Không tự ý áp dụng cho bo mạch có flash/partition khác.

## 2. Clone và cài PlatformIO

Mở **PowerShell**:

```powershell
git clone https://github.com/bnguien/itxmech-respiratory-firmware.git
cd itxmech-respiratory-firmware

py -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install platformio
.\.venv\Scripts\pio.exe --version
```

Các lệnh trong README dùng `\.venv\Scripts\pio.exe` để **không phải sửa Execution Policy** hoặc kích hoạt virtual environment. Nếu đã cài `pio` toàn cục, có thể dùng `pio` tương đương.

## 3. Tạo file cấu hình riêng `.env`

```powershell
Copy-Item .env.example .env
notepad .env
```

Điền giá trị được cấp cho thiết bị, ví dụ **chỉ là cấu trúc**, không phải credentials thật:

```dotenv
# Để trống 2 trường này nếu muốn cấu hình Wi-Fi bằng Captive Portal
WIFI_SSID=
WIFI_PASSWORD=

DEVICE_ID=REPLACE_ME
MQTT_HOST=REPLACE_ME
MQTT_PORT=8883
MQTT_USERNAME=REPLACE_ME
MQTT_PASSWORD=REPLACE_ME
WEB_BASE_URL=https://REPLACE_ME
FIRMWARE_DEVICE_API_KEY=REPLACE_ME
UPLOAD_HOST=REPLACE_ME
RECORD_SECONDS=20
AUDIO_SERIAL_DEBUG=0
```

**Lưu ý:**

- Thay toàn bộ giá trị `REPLACE_ME` bằng giá trị hợp lệ do backend/broker cấp. Script build từ chối biến bắt buộc bị thiếu hoặc placeholder.
- `WEB_BASE_URL` phải bắt đầu bằng `https://`; `UPLOAD_HOST` chỉ là **hostname**, không chứa `https://`, đường dẫn hoặc token trong signed URL.
- `WIFI_SSID` và `WIFI_PASSWORD` có thể để trống để ESP32 phát Wi-Fi Setup khi chưa có credentials lưu trong NVS.
- Không chia sẻ `.env`, file firmware đã build hoặc khóa API. Không commit chúng lên Git.

Đặt file chứng chỉ MQTT tại thư mục gốc của repo:

```text
itxmech-respiratory-firmware/
├── .env
├── respicare-mqtt-ca.crt
├── platformio.ini
├── scripts/
└── src/
```

`respicare-mqtt-ca.crt` phải nhận từ người quản trị broker qua kênh tin cậy và kiểm tra fingerprint. Không tự tạo CA giả để qua bước build, không dùng `setInsecure()`.

## 4. Nạp `.env` vào PowerShell trước khi build

**Quan trọng:** `scripts/build_config.py` đọc biến môi trường của tiến trình, **không tự load `.env`**. Mỗi lần mở cửa sổ PowerShell mới, phải chạy lại đoạn sau:

```powershell
Get-Content .env | ForEach-Object {
    $line = $_.Trim()
    if ($line -match '^\s*(?:export\s+)?([A-Z][A-Z0-9_]*)=(.*)$') {
        $name = $matches[1]
        $value = $matches[2].Trim().Trim([char[]]@([char]39, [char]34))
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}

$env:AUDIO_SERIAL_DEBUG = '0'

# Xóa chế độ maintenance cũ nếu còn (không báo lỗi nếu biến không tồn tại)
if (Test-Path Env:MAINTENANCE_VISIT_ID) { Remove-Item Env:MAINTENANCE_VISIT_ID }
if (Test-Path Env:MAINTENANCE_CONFIRM) { Remove-Item Env:MAINTENANCE_CONFIRM }
```

Nếu muốn ép firmware mở Wi-Fi Setup ở **lần đầu** và không lấy mạng từ build-time:

```powershell
$env:WIFI_SSID = ''
$env:WIFI_PASSWORD = ''
```

Việc để trống hai biến trên **không xóa credentials đã lưu trong NVS**. Thiết bị đã từng cấu hình thành công sẽ ưu tiên Wi-Fi đã lưu.

## 5. Build firmware

```powershell
.\.venv\Scripts\pio.exe run -e esp32dev
```

PlatformIO sẽ tự tải dependencies đã ghim trong `platformio.ini`, trong đó có thư viện MQTT `256dpi/MQTT@2.5.2` và ArduinoJson `7.4.2`. Khi build thành công sẽ có `SUCCESS`.

Nếu vừa chuyển từ build debug/maintenance sang firmware chính, hoặc vừa đổi cấu hình thiết bị, chạy clean rồi build lại:

```powershell
.\.venv\Scripts\pio.exe run -e esp32dev -t clean
.\.venv\Scripts\pio.exe run -e esp32dev
```

## 6. Tìm cổng COM và nạp ESP32

Cắm ESP32 bằng cáp USB, rồi chạy:

```powershell
.\.venv\Scripts\pio.exe device list
```

Xác định cổng tương ứng (ví dụ **COM11**). Đóng Serial Monitor trước khi upload:

```powershell
.\.venv\Scripts\pio.exe run -e esp32dev -t upload --upload-port COM11
```

Thay `COM11` bằng cổng thật. Sau khi nạp thành công, mở log:

```powershell
.\.venv\Scripts\pio.exe device monitor --port COM11 --baud 921600
```

Thoát monitor bằng **Ctrl+C** trước khi upload lần tiếp theo.

> **Không chạy** `erase`, `erase_flash`, `uploadfs` hoặc format LittleFS khi cập nhật firmware cho thiết bị đã có file WAV, checkpoint, lịch sử hoặc Wi-Fi lưu trong NVS. `uploadfs` ghi đè phân vùng filesystem. Chỉ provisioning filesystem trống trên phần cứng mới sau khi đã xác nhận layout và dữ liệu có thể bị mất.

## 7. Thiết lập Wi-Fi bằng Captive Portal

Nếu ESP32 chưa có Wi-Fi lưu trong NVS và hai biến build-time để trống, ESP32 sẽ mở một mạng có tên dạng:

```text
RespiCare-Setup-<DEVICE_ID>
```

Mật khẩu mạng Setup do firmware tạo, hiển thị trong Serial Monitor dưới nhãn `[SETUP]`. **Không phải mật khẩu Wi-Fi nhà.**

1. Trên điện thoại hoặc laptop, kết nối vào `RespiCare-Setup-...` bằng mật khẩu lấy từ Serial.
2. Chấp nhận kết nối dù thiết bị báo **Không có Internet**.
3. Mở `http://192.168.4.1/` (địa chỉ HTTP, không phải HTTPS). Trên macOS có thể xuất hiện cửa sổ Captive Portal tự động.
4. Nhập **SSID chính xác của Wi-Fi 2.4 GHz muốn ESP32 sử dụng** và mật khẩu của mạng đó, **không nhập tên `RespiCare-Setup-...`**.
5. Nhấn **Kết nối** một lần; chờ tối đa khoảng 30 giây. Thiết bị có thể chuyển kênh khi kết nối, làm cửa sổ Setup ngắt tạm thời.
6. Khi báo **Kết nối thành công. Đã lưu cấu hình**, ESP32 lưu credentials trong NVS và đóng mạng Setup sau khoảng 10 giây.
7. Kết nối laptop/điện thoại trở lại mạng Internet để mở Web RespiCare.

Để kiểm tra ghi nhớ Wi-Fi: giữ router/hotspot đang bật, nhấn **RESET** ESP32, quan sát thiết bị tự kết nối lại mà không cần nhập mật khẩu.

Nếu Wi-Fi đã lưu không còn khả dụng, firmware mở lại Setup sau khoảng **60 giây mất liên kết Wi-Fi**. Việc MQTT hoặc Internet không hoạt động trong khi vẫn kết nối Wi-Fi không tự mở Setup. Hiện không có nút mở Setup chủ động khi Wi-Fi cũ vẫn khả dụng.

## 8. Xác nhận kết nối MQTT/backend

Trong Serial Monitor, kiểm tra các log tương đương:

```text
[WIFI] connected
[NTP] synchronized UTC ...
[MQTT] connecting TLS
[MQTT] TLS connected; command subscribed QoS1
[MQTT] DEVICE_ONLINE retained QoS1
```

ESP32 cần có kết nối Internet, DNS và NTP hoạt động để đồng bộ giờ trước TLS. Nếu MQTT không kết nối được, kiểm tra broker hostname/port, credentials, ACL, MQTT CA và NTP; **không tắt xác minh TLS**.

Mở Web RespiCare bằng máy có Internet để kiểm tra trạng thái Online. Khi thử luồng ghi âm thật, một lệnh `ASSIGN_VISIT` hợp lệ sẽ bắt đầu ghi WAV và upload ngay; chỉ thực hiện khi backend/test visit đã chuẩn bị sẵn.

Luồng chính: Web → Backend/Gateway → MQTT → ESP32 ghi WAV → API lấy signed upload URL → Storage PUT → API complete → backend/AI. File WAV hiện được ghi thành **PCM 16-bit, mono, 16 kHz**, thời lượng mặc định 20 giây.

## 9. Lỗi thường gặp

| Hiện tượng | Nguyên nhân / cách kiểm tra |
|---|---|
| `pio` không được nhận diện | Dùng `\.venv\Scripts\pio.exe` hoặc cài lại PlatformIO trong `.venv`. |
| `Missing or placeholder environment variable` | Điền `.env` và chạy lại đoạn nạp biến ở mục 4 **trong cùng terminal build**. |
| Không thấy COM | Kiểm tra cáp dữ liệu, driver CP210x/CH340, Device Manager. |
| `Could not open COM...` | Đóng Serial Monitor hoặc ứng dụng khác đang giữ cổng; kiểm tra đúng COM. |
| Không vào được `192.168.4.1` | Thiết bị dùng để mở trình duyệt phải kết nối trực tiếp mạng `RespiCare-Setup-...`, không phải Wi-Fi nhà. |
| `Connection already in progress` | Yêu cầu trước đang được xử lý; không bấm gửi lại nhiều lần. Mở `/` để xem trạng thái. |
| `NO_AP_FOUND` / `reason=201` | ESP32 không thấy AP phù hợp; kiểm tra tên SSID (không nhập nhầm Setup AP), 2.4 GHz, tín hiệu và cấu hình router. Thử hotspot 2.4 GHz để phân biệt. |
| `AUTH_FAIL` / `reason=202` | Kiểm tra mật khẩu và phương thức bảo mật Wi-Fi. |
| `Wi-Fi connection timeout` | Xem các dòng `[WIFI DEBUG]` và thử AP/hotspot khác. |
| `/littlefs/recording.wav does not exist` | Không nhất thiết là lỗi Wi-Fi. Kiểm tra trạng thái ghi âm/storage riêng; không format dữ liệu khi chưa xác định nguyên nhân. |
| Có Wi-Fi nhưng MQTT Offline | Kiểm tra NTP, DNS, TLS, MQTT broker/ACL và cấu hình backend. |

## 10. Kiểm tra source và lưu ý bảo mật

Có thể chạy host tests bằng Python (nếu môi trường Windows có đủ công cụ biên dịch được test yêu cầu):

```powershell
.\.venv\Scripts\python.exe tests/test_audio.py
.\.venv\Scripts\python.exe tests/test_config.py
.\.venv\Scripts\python.exe tests/test_flow.py
```

Các test trên **không thay thế kiểm thử phần cứng**. Kiểm thử chất lượng âm thanh, LittleFS, SSL/TLS, MQTT và việc phân tích AI phải được thực hiện trên thiết bị và backend thực tế.

Các thành phần chính trong repo:

- `src/AudioRecorder.*`: I2S và tạo WAV; bộ lọc có thể ảnh hưởng kết quả mô hình AI.
- `src/Network.*`: Wi-Fi Setup, NTP và MQTT/TLS.
- `src/RecordingStorage.*`: LittleFS và checkpoint.
- `src/RecordingApiClient.*`: upload HTTPS.
- `src/DeviceFlow.*`: điều phối luồng ghi âm/upload.
- `scripts/build_config.py`: tạo `include/BuildConfig.h` từ **biến môi trường hiện tại**.

Không commit `.env`, `include/BuildConfig.h`, `.pio/` hoặc chứng chỉ/khóa bí mật. Nếu ESP32 lưu thông tin nhạy cảm, firmware build và flash của thiết bị cũng phải được bảo vệ. Không public mạng Captive Portal qua port forwarding.

### Cập nhật source ở những lần sau

```powershell
git pull
# Nạp lại .env theo mục 4 nếu vừa mở PowerShell mới
.\.venv\Scripts\pio.exe run -e esp32dev
.\.venv\Scripts\pio.exe run -e esp32dev -t upload --upload-port COM11
```

Trước khi chạy `git pull`, đảm bảo các thay đổi local đã được commit hoặc lưu lại, tránh ghi đè công việc đang làm. Không dùng lệnh xóa NVS/flash khi chỉ cần cập nhật chương trình.
