#!/bin/bash
#
# Build script for Watcher LiveKit Voice Agent firmware
# This ensures proper configuration and clean builds
#

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get script directory and project root
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Watcher Voice Agent Build Script${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Change to project directory
cd "$PROJECT_DIR"
echo -e "${YELLOW}Project directory:${NC} $PROJECT_DIR"

# Check if sdkconfig.watcher exists
if [ ! -f "sdkconfig.watcher" ]; then
    echo -e "${RED}ERROR: sdkconfig.watcher not found!${NC}"
    exit 1
fi

# Parse command line arguments
CLEAN_BUILD=false
FLASH=false
MONITOR=false
PORT="/dev/cu.usbmodem56D50186503"
BUILD_NUMBER=""
COPY_TO_DESKTOP=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --flash)
            FLASH=true
            shift
            ;;
        --monitor)
            MONITOR=true
            shift
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --build-number)
            BUILD_NUMBER="$2"
            COPY_TO_DESKTOP=true
            shift 2
            ;;
        *)
            echo -e "${YELLOW}Unknown option: $1${NC}"
            echo "Usage: $0 [--clean] [--flash] [--monitor] [--port PORT] [--build-number NUM]"
            exit 1
            ;;
    esac
done

# Copy watcher config to sdkconfig if it's different or doesn't exist
if [ ! -f "sdkconfig" ] || ! cmp -s "sdkconfig.watcher" "sdkconfig"; then
    echo -e "${YELLOW}Updating sdkconfig from sdkconfig.watcher...${NC}"
    cp sdkconfig.watcher sdkconfig
fi

# Clean build if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo -e "${YELLOW}Performing full clean build...${NC}"
    idf.py fullclean
fi

# Build
echo -e "${GREEN}Building firmware...${NC}"
echo -e "${YELLOW}Configuration:${NC}"
echo "  - Target: ESP32-S3"
echo "  - Audio: Mono (1 channel), 48kHz, 16-bit"
echo "  - Buffers: Optimized (8KB raw, 16KB render)"
echo "  - LVGL Draw Buffer: 80 lines"
echo ""

if idf.py build; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Build successful! ✓${NC}"
    echo -e "${GREEN}========================================${NC}"

    # Show binary info
    echo ""
    echo -e "${YELLOW}Binary information:${NC}"
    ls -lh build/voice_agent.bin

    # Copy build artifacts to Desktop if build number specified
    if [ "$COPY_TO_DESKTOP" = true ]; then
        DESKTOP_DIR="$HOME/Desktop/$BUILD_NUMBER"
        echo ""
        echo -e "${GREEN}Copying build artifacts to Desktop/$BUILD_NUMBER...${NC}"

        # Create desktop directory
        mkdir -p "$DESKTOP_DIR"

        # Copy only the 4 essential .bin files flat to root (for ESP Flash Tool)
        cp build/bootloader/bootloader.bin "$DESKTOP_DIR/" 2>/dev/null || true
        cp build/partition_table/partition-table.bin "$DESKTOP_DIR/" 2>/dev/null || true
        cp build/voice_agent.bin "$DESKTOP_DIR/" 2>/dev/null || true
        cp build/srmodels/srmodels.bin "$DESKTOP_DIR/" 2>/dev/null || true

        # Count files copied
        FILE_COUNT=$(ls -1 "$DESKTOP_DIR"/*.bin 2>/dev/null | wc -l | tr -d ' ')
        echo -e "${GREEN}$FILE_COUNT bin files copied to $DESKTOP_DIR${NC}"

        # List the files
        echo -e "${YELLOW}Files ready for flashing:${NC}"
        echo "  0x0        -> bootloader.bin"
        echo "  0x8000     -> partition-table.bin"
        echo "  0x110000   -> voice_agent.bin"
        echo "  0xd10000   -> srmodels.bin"
    fi

    # Flash if requested
    if [ "$FLASH" = true ]; then
        echo ""
        echo -e "${GREEN}Flashing to device on $PORT...${NC}"
        idf.py -p "$PORT" flash

        # Monitor if requested
        if [ "$MONITOR" = true ]; then
            echo ""
            echo -e "${GREEN}Starting serial monitor...${NC}"
            echo -e "${YELLOW}Press Ctrl+] to exit${NC}"
            idf.py -p "$PORT" monitor
        fi
    else
        echo ""
        echo -e "${YELLOW}To flash: ./scripts/build-v.sh --flash${NC}"
        echo -e "${YELLOW}To flash and monitor: ./scripts/build-v.sh --flash --monitor${NC}"
        echo -e "${YELLOW}Mac users with USB issues: ./scripts/flash-mac.sh${NC}"
    fi
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Build failed! ✗${NC}"
    echo -e "${RED}========================================${NC}"
    exit 1
fi
