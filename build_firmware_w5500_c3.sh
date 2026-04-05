#!/bin/bash
# Build ESP32-C3 + W5500 SPI Ethernet firmware and collect binaries into firmware_w5500_c3/
set -e

BUILD_DIR="build_w5500_c3"
OUT_DIR="firmware_w5500_c3"
SDKCONFIG="sdkconfig.w5500_c3"
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.w5500_c3"

echo "=== W5500 + ESP32-C3 firmware build ==="

# Verify IDF environment
if [ -z "$IDF_PATH" ]; then
    echo "ERROR: IDF_PATH is not set. Source your ESP-IDF export script first:"
    echo "  . \$IDF_PATH/export.sh"
    exit 1
fi

echo "IDF_PATH: $IDF_PATH"
echo "Build dir: $BUILD_DIR"
echo "Output dir: $OUT_DIR"
echo ""

# Clean previous build
echo "--- Cleaning previous build ---"
rm -rf "$BUILD_DIR"

# Build
echo "--- Building ---"
idf.py \
    set-target esp32c3 \
    -B "$BUILD_DIR" \
    -D SDKCONFIG="$SDKCONFIG" \
    -D SDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS" \
    build

echo ""
echo "--- Collecting binaries ---"
mkdir -p "$OUT_DIR"

cp "$BUILD_DIR/bootloader/bootloader.bin"           "$OUT_DIR/bootloader.bin"
cp "$BUILD_DIR/partition_table/partition-table.bin" "$OUT_DIR/partition-table.bin"
cp "$BUILD_DIR/ota_data_initial.bin"                "$OUT_DIR/ota_data_initial.bin"
cp "$BUILD_DIR/esp32_eth_router.bin"                "$OUT_DIR/esp32_eth_router.bin"

echo ""
echo "=== Done ==="
echo ""
echo "Firmware files in $OUT_DIR/:"
ls -lh "$OUT_DIR/"
echo ""
echo "Flash command:"
echo "  esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 460800 write_flash \\"
echo "    0x0000  $OUT_DIR/bootloader.bin \\"
echo "    0x8000  $OUT_DIR/partition-table.bin \\"
echo "    0xf000  $OUT_DIR/ota_data_initial.bin \\"
echo "    0x20000 $OUT_DIR/esp32_eth_router.bin"
