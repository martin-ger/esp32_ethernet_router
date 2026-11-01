# WT32-ETH01 / ESP32-ETH01 setup

This guide covers the prebuilt firmware for the WT32-ETH01 (also sold as
ESP32-ETH01) with the LAN8720 Ethernet PHY. The board connects to an existing
Wi-Fi network as a station and provides a wired Ethernet LAN with DHCP and NAT
enabled by default.

## What you need

- WT32-ETH01 / ESP32-ETH01 with a LAN8720 PHY
- A 3.3 V USB-to-UART adapter for flashing and serial logs
- An Ethernet cable and a Wi-Fi network for the upstream connection
- Python 3 with `esptool` installed, or an ESP-IDF environment

Do not connect a 5 V UART signal to the ESP32. Use the board's documented UART
pins and connect common ground.

## Flash the prebuilt image

The four images are in `firmware/` and use these ESP32 flash offsets:

| Image | Offset |
|---|---:|
| `bootloader.bin` | `0x1000` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `esp32_eth_router.bin` | `0x20000` |

On macOS or Linux, the repository helper detects common serial-port names:

```bash
python3 -m pip install esptool
./flash.sh /dev/ttyUSB0
```

Examples of macOS ports include `/dev/cu.usbserial-*` and
`/dev/cu.SLAB_USBtoUART`. A Linux USB-UART adapter is commonly
`/dev/ttyUSB0`; use `/dev/ttyACM0` when the adapter exposes an ACM device.

The helper preserves NVS by default. To erase the complete flash, including
saved Wi-Fi credentials and router settings, run:

```bash
ERASE=1 ./flash.sh /dev/ttyUSB0
```

Windows users can run the equivalent `esptool` command from the repository's
[Installation section](../README.md#installation).

## Automatic first-boot AP setup

When no upstream Wi-Fi credentials are stored (the factory-fresh state), the
firmware automatically brings up the Ethernet onboarding AP. It uses
`192.168.4.1/24`, starts DHCP, and runs the web configuration page; captive
portal DNS requests are redirected to that page as well.

1. Connect a computer or switch to the Ethernet port and leave the client set
   to DHCP.
2. Open `http://192.168.4.1` (or any HTTP URL if the client follows captive
   portal redirects).
3. In **Configuration**, enter the upstream Wi-Fi SSID and password and save.
4. The router reboots, stores the credentials in NVS, and starts the Wi-Fi STA
   uplink automatically.
5. Confirm the uplink on the **Status** page and set a web password in **Web
   UI Settings** before connecting the router to an untrusted network.

This onboarding network is the wired Ethernet downlink; the firmware does not
need a separate Wi-Fi softAP for first-run configuration.

The default Ethernet address is `192.168.4.1/24`. DHCP and NAT are enabled by
default, so wired clients can reach the Internet without a static route.

If a client does not obtain a lease, temporarily configure a static address
such as `192.168.4.10/24`, gateway `192.168.4.1`, and DNS
`192.168.4.1`, then open the web interface and inspect **Mappings**.

## LAN8720 pin configuration

The default WT32-ETH01 configuration uses:

| Signal | GPIO |
|---|---:|
| MDC | 23 |
| MDIO | 18 |
| PHY power | 16 |
| RMII clock | 0 |
| PHY address | 1 |

The status LED is configurable and defaults to GPIO 2 in the shipped
configuration.

## Serial console and recovery

Open the serial console at 115200 bps. Configure the uplink from the console
with:

```text
set_sta <ssid> <password>
restart
```

If the saved configuration prevents access, erase NVS with
`ERASE=1 ./flash.sh <PORT>` and configure the router again. A full erase is
destructive to saved settings; it is not required for a normal firmware
update.
