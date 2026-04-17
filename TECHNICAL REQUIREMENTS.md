# Project Requirement: ESP12-F IoT Gateway "Omni-Node"

**Deskripsi Singkat:** Sistem berbasis ESP8266 (ESP12-F) yang berfungsi sebagai gateway sensor/aktuator dengan manajemen berbasis web mandiri (On-device Web Server), mendukung integrasi MQTT, dan memiliki fitur keamanan serta pemulihan sistem.

------

## 1. Network & Connectivity Requirements

- **Dual Mode WiFi:** Mendukung mode **STA (Station)** sebagai mode utama dan **AP (Access Point)** sebagai mode konfigurasi/fail-safe.
- **Advanced IP Config:** * Mendukung DHCP dan Static IP.
  - Konfigurasi DNS Manual (Primary & Secondary) untuk mendukung akses internet dari jaringan intranet yang restriktif.
- **mDNS (Multicast DNS):** Akses local dashboard via hostname (contoh: `http://omninode.local`) tanpa perlu menghafal IP.
- **NTP Client:** Sinkronisasi waktu otomatis untuk keperluan *logging* dan *timestamping* data sensor sebelum dikirim ke broker.

## 2. MQTT & Data Handling

- **MQTT Client:**
  - Koneksi ke Broker (IP, Port, User, Pass).
  - **Last Will and Testament (LWT):** Mengirim pesan "Offline" otomatis ke topik tertentu jika perangkat kehilangan koneksi.
  - **Topic Mapping:** Prefix topik dapat dikustomisasi via web.
- **IO Polling Engine:**
  - Interval polling yang dapat diatur (dalam milidetik/detik).
  - **Pin Mapping:** Fitur untuk mengaktifkan/matikan pin GPIO tertentu dan memberi nama alias (label) yang akan menjadi sub-topik MQTT.
  - **Pin Settings**: Fitur settings pin untuk konfigurasi pengiriman ke MQTT, yang berisi GPIO, nama variabel, tipe variabel (float32, int16, dst), dan multiplier.
- **Live Stream:** Menggunakan **WebSockets** untuk mengirim data pembacaan pin secara real-time ke web dashboard tanpa melakukan *refresh* halaman.

## 3. System Management & Security

- **OTA (Over-The-Air) Update:** Update firmware secara remote melalui interface web (upload file `.bin`).
- **Web Authentication:** Proteksi halaman konfigurasi menggunakan username dan password.
- **Failsafe Hardware Button:** * *Short press:* Reset perangkat.
  - *Long press (10 detik):* Factory reset (menghapus konfigurasi WiFi dan MQTT ke default/mode AP).
- **Persistent Storage:** Semua konfigurasi disimpan di **LittleFS** dalam format JSON agar tahan lama (non-volatile).

## 4. UI/UX Design Specification: "Minimalist Neo-Brutalist"

Gaya desain yang menggabungkan efisiensi resource ESP8266 dengan estetika modern yang berani:

- **Layout:** Layout grid yang kaku dengan border tebal (2px - 3px) berwarna hitam solid.
- **Color Palette:** Latar belakang putih/abu-abu sangat muda, dengan aksen warna primer yang kontras (misal: Kuning cerah atau Cyan) untuk tombol "Save".
- **Typography:** Menggunakan sistem font *Monospace* untuk kesan teknis/terminal.
- **Shadows:** Menggunakan *hard shadows* (bayangan hitam pekat tanpa blur) pada tombol dan kotak input.
- **Responsiveness:** Ringan (tanpa library berat seperti Bootstrap), cukup menggunakan CSS kustom atau framework mikro seperti *Milligram.css*.

------

## 5. Technical Stack (Reference)

- **Framework:** Arduino IDE atau PlatformIO.
- **Core Libs:** `ESP8266WiFi`, `ESP8266mDNS`, `ArduinoJson`, `PubSubClient` (MQTT), `ESPAsyncWebServer` (Non-blocking server).
- **Filesystem:** `LittleFS`.

------

## 6. Data Schema (config.json)

JSON

```json
{
  "device_id": "OMNI-01",
  "network": {
    "ssid": "", "pass": "",
    "static_ip": false,
    "ip": "", "subnet": "", "gw": "",
    "dns1": "8.8.8.8", "dns2": "1.1.1.1"
  },
  "mqtt": {
    "broker": "", "port": 1883,
    "user": "", "pass": "",
    "lwt_topic": "status", "prefix": "nodes/01"
  },
  "io_setup": [
    {"pin": 5, "label": "temp_sensor", "enabled": true, "type": "input_analog"},
    {"pin": 4, "label": "relay_main", "enabled": false, "type": "output_digital"}
  ],
  "system": {
    "web_pass": "admin123",
    "poll_interval": 5000
  }
}
```