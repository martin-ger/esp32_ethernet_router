#!/bin/bash
# Build ESP32 Ethernet Router firmware and collect binaries into firmware/
set -e

BUILD_DIR="build_eth_sta"
OUT_DIR="firmware"
SDKCONFIG="sdkconfig.defaults;sdkconfig.defaults.wt32_eth_sta_uplink"

echo "=== ESP32 Ethernet Router firmware build ==="

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
idf.py set-target esp32
idf.py \
    -B "$BUILD_DIR" \
    -D SDKCONFIG_DEFAULTS="$SDKCONFIG" \
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
echo "  esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash \\"
echo "    0x1000  $OUT_DIR/bootloader.bin \\"
echo "    0x8000  $OUT_DIR/partition-table.bin \\"
echo "    0xf000  $OUT_DIR/ota_data_initial.bin \\"
echo "    0x20000 $OUT_DIR/esp32_eth_router.bin"
