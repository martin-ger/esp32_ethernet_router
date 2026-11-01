#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="${FIRMWARE_DIR:-$SCRIPT_DIR/firmware}"
BAUD="${BAUD:-460800}"

usage() {
    cat <<'USAGE'
Usage: ./flash.sh [PORT]

Flash the prebuilt WT32-ETH01 / ESP32-ETH01 firmware from firmware/.

Environment:
  ERASE=1       Erase the complete flash before writing (also removes NVS).
  BAUD=460800   Override the esptool baud rate.
  FIRMWARE_DIR  Use another directory containing the four firmware images.

Examples:
  ./flash.sh /dev/ttyUSB0
  ./flash.sh /dev/cu.usbserial-XXXX
  ERASE=1 ./flash.sh /dev/ttyUSB0
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi
if [[ "$#" -gt 1 ]]; then
    echo "error: only one serial port may be supplied" >&2
    usage >&2
    exit 2
fi

PORT="${1:-}"
if [[ -z "$PORT" ]]; then
    shopt -s nullglob
    serial_ports=(
        /dev/cu.usbserial-*
        /dev/cu.SLAB_USBtoUART
        /dev/cu.wchusbserial*
        /dev/cu.usbmodem*
        /dev/ttyUSB*
        /dev/ttyACM*
    )
    shopt -u nullglob
    if [[ "${#serial_ports[@]}" -gt 0 ]]; then
        PORT="${serial_ports[0]}"
    fi
fi

if [[ -z "$PORT" || ! -e "$PORT" ]]; then
    echo "error: no ESP32 serial port found" >&2
    echo "pass a port explicitly, for example: ./flash.sh /dev/ttyUSB0" >&2
    exit 2
fi

firmware_files=(
    bootloader.bin
    partition-table.bin
    ota_data_initial.bin
    esp32_eth_router.bin
)
for firmware_file in "${firmware_files[@]}"; do
    if [[ ! -f "$FIRMWARE_DIR/$firmware_file" ]]; then
        echo "error: missing firmware image: $FIRMWARE_DIR/$firmware_file" >&2
        exit 1
    fi
done

if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL=(esptool.py)
elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL=(esptool)
elif command -v python3 >/dev/null 2>&1 && python3 -m esptool version >/dev/null 2>&1; then
    ESPTOOL=(python3 -m esptool)
else
    echo "error: esptool is not installed" >&2
    echo "install it with: python3 -m pip install esptool" >&2
    exit 1
fi

if [[ "${ERASE:-0}" == "1" ]]; then
    echo "Erasing the complete flash on $PORT (NVS will be removed)..."
    "${ESPTOOL[@]}" --chip esp32 --port "$PORT" erase_flash
fi

echo "Writing WT32-ETH01 firmware to $PORT..."
"${ESPTOOL[@]}" \
    --chip esp32 \
    --port "$PORT" \
    --baud "$BAUD" \
    write_flash \
    --flash_mode dio \
    --flash_freq 40m \
    --flash_size detect \
    0x1000 "$FIRMWARE_DIR/bootloader.bin" \
    0x8000 "$FIRMWARE_DIR/partition-table.bin" \
    0xf000 "$FIRMWARE_DIR/ota_data_initial.bin" \
    0x20000 "$FIRMWARE_DIR/esp32_eth_router.bin"

echo "Flash complete. Connect Ethernet and open http://192.168.4.1."
