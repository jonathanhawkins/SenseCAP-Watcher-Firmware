#!/bin/bash
#
# Mac Flash Script - Uses libusb to bypass AppleUSBCDC driver issues
# This is a wrapper around mac-flasher.py for convenience
#

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FLASHER="$SCRIPT_DIR/mac-flasher.py"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Mac ESP32-S3 Flasher (libusb)${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Parse arguments
BUILD_DIR="$PROJECT_DIR/build"
FLASH_DIR=""
TEST_ONLY=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --dir|-d)
            FLASH_DIR="$2"
            shift 2
            ;;
        --test|-t)
            TEST_ONLY=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --dir, -d DIR    Flash from directory (default: build/)"
            echo "  --test, -t       Test connection only"
            echo "  --help, -h       Show this help"
            echo ""
            echo "Examples:"
            echo "  $0                    # Flash from build/"
            echo "  $0 --dir ~/Desktop/45  # Flash from Desktop/45"
            echo "  $0 --test              # Test connection"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Determine source directory
if [ -n "$FLASH_DIR" ]; then
    SRC_DIR="$FLASH_DIR"
elif [ -d "$BUILD_DIR" ]; then
    SRC_DIR="$BUILD_DIR"
else
    echo -e "${RED}Build directory not found: $BUILD_DIR${NC}"
    echo -e "${YELLOW}Run build-v.sh first or specify --dir${NC}"
    exit 1
fi

# Check for required files
if [ "$TEST_ONLY" = false ]; then
    # Check for flat structure (Desktop copy) or build structure
    if [ -f "$SRC_DIR/voice_agent.bin" ]; then
        # Flat structure
        BOOTLOADER="$SRC_DIR/bootloader.bin"
        PARTITION="$SRC_DIR/partition-table.bin"
        APP="$SRC_DIR/voice_agent.bin"
        SRMODELS="$SRC_DIR/srmodels.bin"
    elif [ -f "$SRC_DIR/build/voice_agent.bin" ]; then
        # Nested build structure
        BOOTLOADER="$SRC_DIR/build/bootloader/bootloader.bin"
        PARTITION="$SRC_DIR/build/partition_table/partition-table.bin"
        APP="$SRC_DIR/build/voice_agent.bin"
        SRMODELS="$SRC_DIR/build/srmodels/srmodels.bin"
    else
        # Standard build structure
        BOOTLOADER="$SRC_DIR/bootloader/bootloader.bin"
        PARTITION="$SRC_DIR/partition_table/partition-table.bin"
        APP="$SRC_DIR/voice_agent.bin"
        SRMODELS="$SRC_DIR/srmodels/srmodels.bin"
    fi

    echo -e "${YELLOW}Source directory: $SRC_DIR${NC}"
    echo ""
    echo -e "${YELLOW}Files to flash:${NC}"

    FLASH_ARGS=""

    if [ -f "$BOOTLOADER" ]; then
        echo "  0x0       -> $(basename $BOOTLOADER)"
        FLASH_ARGS="$FLASH_ARGS $BOOTLOADER@0x0"
    else
        echo -e "${RED}  bootloader.bin not found!${NC}"
    fi

    if [ -f "$PARTITION" ]; then
        echo "  0x8000    -> $(basename $PARTITION)"
        FLASH_ARGS="$FLASH_ARGS $PARTITION@0x8000"
    else
        echo -e "${RED}  partition-table.bin not found!${NC}"
    fi

    if [ -f "$APP" ]; then
        echo "  0x110000  -> $(basename $APP)"
        FLASH_ARGS="$FLASH_ARGS $APP@0x110000"
    else
        echo -e "${RED}  voice_agent.bin not found!${NC}"
        exit 1
    fi

    if [ -f "$SRMODELS" ]; then
        echo "  0xd10000  -> $(basename $SRMODELS)"
        FLASH_ARGS="$FLASH_ARGS $SRMODELS@0xd10000"
    else
        echo -e "${YELLOW}  srmodels.bin not found (optional)${NC}"
    fi

    echo ""
fi

# Instructions for bootloader mode
echo -e "${YELLOW}=== BOOTLOADER MODE ===${NC}"
echo "1. Hold BOOT button on Watcher"
echo "2. Press RESET while holding BOOT"
echo "3. Release BOOT after 1 second"
echo ""
read -p "Press Enter when device is in bootloader mode..."

# Run flasher
if [ "$TEST_ONLY" = true ]; then
    "$FLASHER" --test
else
    "$FLASHER" $FLASH_ARGS
fi
