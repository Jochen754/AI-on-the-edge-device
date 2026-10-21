# ESP32-S3-ETH: Ethernet (LAN / PoE) instead of WLAN

AI-on-the-edge also runs on the **ESP32-S3-ETH** board (Waveshare design):

| Part | ESP32-S3-ETH |
|---|---|
| MCU | ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM |
| Network | W5500 Ethernet (SPI), optional PoE module |
| Camera | OV2640 / OV5640 (DVP connector) |
| Storage | microSD (SDMMC 1-line) |

The device is connected by **LAN cable only**. WLAN is not used at all. All other functions are the same as on the ESP32-CAM:
web interface, digit/analog recognition, MQTT (incl. Home Assistant discovery), InfluxDB, webhook, REST API, OTA updates and mDNS.

## Firmware

Build environment `esp32s3-eth` (`code/platformio.ini`), board definition `BOARD_ESP32_S3_ETH` (`code/include/defines.h`),
board settings in `code/sdkconfig.esp32s3-eth.defaults`.

```
cd code
pio run -e esp32s3-eth
```

The first installation is done via USB (USB-C port of the board, native USB of the ESP32-S3), e.g. with
[ESPHome Web](https://web.esphome.io) or esptool, using a merged image at offset `0x0`:

```
esptool.py --chip esp32s3 merge_bin -o factory.bin --flash_mode dio --flash_size 16MB \
    0x0 bootloader.bin 0x8000 partitions.bin 0xd000 ota_data_initial.bin 0x10000 firmware.bin
esptool.py --chip esp32s3 write_flash 0x0 factory.bin
```

Later updates: OTA via the web interface (over LAN), like on the ESP32-CAM.

## Network configuration (`wlan.ini`)

`wlan.ini` is **optional**. Without it the device uses **DHCP** and the hostname `watermeter`.
`ssid`, `password` and `RSSIThreshold` are not needed and are ignored. Still used:

| Parameter | Meaning |
|---|---|
| `hostname` | Name of the device in the network (DHCP, mDNS, MQTT main topic default) |
| `ip`, `gateway`, `netmask` | Fixed IP instead of DHCP (all three needed) |
| `dns` | DNS server (optional, default: gateway) |
| `http_username`, `http_password` | Password protection of web interface and REST API |

## Setup mode

If `/config/config.ini` is missing (e.g. empty SD card), the setup mode starts like on the ESP32-CAM.
Instead of an own access point, the setup page is served **via LAN** (DHCP or fixed IP from `wlan.ini`):
upload `remote-setup.zip`, reboot – done. There is no WLAN step.

## Differences to the ESP32-CAM

| Topic | ESP32-S3-ETH |
|---|---|
| WLAN signal | not available: the overview shows `Network: LAN (Ethernet)`, the info page shows `Network: LAN (Ethernet)` instead of the SSID |
| REST API `/rssi`, `/rssi.html` | not available |
| REST API `/info?type=NetworkType` | `Ethernet` (WLAN boards: `WLAN`) |
| MQTT `wifiRSSI` | not published, no Home Assistant entity |
| Prometheus metric `*_rssi_dbm` | not available |
| Parameter `RSSIThreshold` | hidden in the configuration page, no WLAN roaming |
| Flash LED | none on the board: connect an LED (via transistor/MOSFET) to **GPIO16**, PWM dimmable via `LEDIntensity` |
| Status LED | none on the board: optional LED on **GPIO17** (active low) |
| GPIO extension | the IO1/IO3/IO4/IO12/IO13 offered in the web UI are used by camera, SD card and Ethernet on this board – do **not** enable them. The on-board WS2812 LED (GPIO21) can be configured manually in `config.ini` (`IO21 … external-flash-ws281x …`) |

## Pin assignment (`BOARD_ESP32_S3_ETH`)

| Function | GPIO |
|---|---|
| W5500 MISO / MOSI / SCLK / CS / INT / RST | 12 / 11 / 13 / 14 / 10 / 9 |
| SD card CLK / CMD / D0 / D3 (SDMMC 1-line) | 7 / 6 / 5 / 4 |
| Camera XCLK / SDA / SCL | 3 / 48 / 47 |
| Camera D0 … D7 | 41, 45, 46, 42, 40, 38, 15, 18 |
| Camera VSYNC / HREF / PCLK | 1 / 2 / 39 |
| Camera enable (power down, high = off) | 8 |
| Flash LED (external) | 16 |
| Status LED (external, optional) | 17 |

## Troubleshooting

- **`Detected camera not supported`**: no camera answered on SCCB/I²C. Check the ribbon cable (contacts, locking clip).
- **`Ethernet init failed` / `W5500 driver could not be created`**: Ethernet controller not found – wrong board or defective hardware.
- **No IP**: check the `Link up` message in the log; without link: cable, switch port, PoE.
- Serial log: USB-C port, 115200 baud.
