#!/Users/light/.espressif/python_env/idf5.4_py3.13_env/bin/python3
"""
Mac ESP32-S3 Flasher using libusb for WCH CH55x USB-Serial chips
Bypasses the macOS CDC driver by using libusb directly with proper packet sizes.

The WCH CH55x chip has a 32-byte max OUT packet size which causes issues with
the macOS CDC driver when esptool sends larger packets.

Usage:
    ./mac-flasher.py --dir ~/Desktop/45
    ./mac-flasher.py --test

Author: Generated for Watcher Voice Agent project
"""

import argparse
import struct
import sys
import time
import os
from typing import Optional, Tuple, List

try:
    import usb.core
    import usb.util
except ImportError:
    print("Error: pyusb not installed. Run: pip3 install pyusb")
    sys.exit(1)

# WCH CH55x USB identifiers (used in SenseCap Watcher)
WCH_VID = 0x1A86
WCH_PID = 0x55D2

# SLIP protocol constants
SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

# ESP bootloader commands
ESP_FLASH_BEGIN = 0x02
ESP_FLASH_DATA = 0x03
ESP_FLASH_END = 0x04
ESP_MEM_BEGIN = 0x05
ESP_MEM_END = 0x06
ESP_MEM_DATA = 0x07
ESP_SYNC = 0x08
ESP_WRITE_REG = 0x09
ESP_READ_REG = 0x0A

# Constants
# CRITICAL: The WCH CH55x USB-Serial chip corrupts large packets.
# Standard 1024-byte blocks create 1050-byte commands that get corrupted.
# Use much smaller blocks to keep each command under ~100 bytes.
FLASH_WRITE_SIZE = 0x40  # 64-byte blocks (command will be ~90 bytes total)
DEFAULT_TIMEOUT = 3000  # ms
SYNC_TIMEOUT = 100  # ms
MAX_USB_PACKET = 16  # Use 16 bytes (half of WCH's 32-byte limit for safety)
INTER_CHUNK_DELAY_MS = 5  # Delay between USB chunks to prevent WCH buffer corruption
DEBUG_USB = False  # Set to True to see USB packet details

# Colors
class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    BLUE = '\033[0;34m'
    NC = '\033[0m'

def cprint(color: str, msg: str):
    print(f"{color}{msg}{Colors.NC}")


class SLIPProtocol:
    """SLIP framing for ESP bootloader"""

    @staticmethod
    def encode(data: bytes) -> bytes:
        encoded = bytearray([SLIP_END])
        for byte in data:
            if byte == SLIP_END:
                encoded.extend([SLIP_ESC, SLIP_ESC_END])
            elif byte == SLIP_ESC:
                encoded.extend([SLIP_ESC, SLIP_ESC_ESC])
            else:
                encoded.append(byte)
        encoded.append(SLIP_END)
        return bytes(encoded)

    @staticmethod
    def decode(data: bytes) -> bytes:
        decoded = bytearray()
        escape = False
        for byte in data:
            if escape:
                if byte == SLIP_ESC_END:
                    decoded.append(SLIP_END)
                elif byte == SLIP_ESC_ESC:
                    decoded.append(SLIP_ESC)
                else:
                    decoded.append(byte)
                escape = False
            elif byte == SLIP_ESC:
                escape = True
            elif byte == SLIP_END:
                if decoded:  # End of frame
                    break
            else:
                decoded.append(byte)
        return bytes(decoded)


class WCHFlasher:
    """ESP32-S3 flasher for WCH CH55x USB-Serial"""

    def __init__(self):
        self.dev = None
        self.ep_out = None
        self.ep_in = None
        self.intf_num = 1  # CDC Data interface

    def find_device(self) -> bool:
        cprint(Colors.YELLOW, f"Looking for WCH CH55x (VID={hex(WCH_VID)}, PID={hex(WCH_PID)})...")
        self.dev = usb.core.find(idVendor=WCH_VID, idProduct=WCH_PID)
        if self.dev is None:
            cprint(Colors.RED, "Device not found!")
            return False
        cprint(Colors.GREEN, f"Found: {self.dev.product or 'WCH CH55x'}")
        return True

    def connect(self, port_num: int = 0) -> bool:
        """Connect to device. port_num: 0=first serial, 1=second serial"""
        if not self.find_device():
            return False

        try:
            # Detach kernel driver from both CDC interfaces
            for intf in [0, 1, 2, 3]:
                try:
                    if self.dev.is_kernel_driver_active(intf):
                        cprint(Colors.YELLOW, f"Detaching kernel driver from interface {intf}...")
                        self.dev.detach_kernel_driver(intf)
                except:
                    pass

            # Set configuration
            try:
                self.dev.set_configuration()
            except usb.core.USBError:
                pass  # May already be configured

            # Select which serial port (0 or 1)
            if port_num == 0:
                self.intf_num = 1  # First serial data interface
                ep_out_addr = 0x02
                ep_in_addr = 0x82
            else:
                self.intf_num = 3  # Second serial data interface
                ep_out_addr = 0x03
                ep_in_addr = 0x83

            # Claim the data interface
            try:
                usb.util.claim_interface(self.dev, self.intf_num)
            except:
                pass

            # Get endpoints
            cfg = self.dev.get_active_configuration()
            intf = cfg[(self.intf_num, 0)]

            self.ep_out = usb.util.find_descriptor(
                intf,
                custom_match=lambda e: e.bEndpointAddress == ep_out_addr
            )
            self.ep_in = usb.util.find_descriptor(
                intf,
                custom_match=lambda e: e.bEndpointAddress == ep_in_addr
            )

            if self.ep_out is None or self.ep_in is None:
                cprint(Colors.RED, "Could not find endpoints!")
                return False

            cprint(Colors.GREEN, f"Connected via interface {self.intf_num}")
            cprint(Colors.YELLOW, f"  OUT: {hex(self.ep_out.bEndpointAddress)}, max {self.ep_out.wMaxPacketSize} bytes")
            cprint(Colors.YELLOW, f"  IN:  {hex(self.ep_in.bEndpointAddress)}, max {self.ep_in.wMaxPacketSize} bytes")

            return True

        except usb.core.USBError as e:
            cprint(Colors.RED, f"USB Error: {e}")
            return False

    def disconnect(self):
        if self.dev:
            try:
                usb.util.release_interface(self.dev, self.intf_num)
                usb.util.dispose_resources(self.dev)
                # Reattach kernel driver
                for intf in [0, 1, 2, 3]:
                    try:
                        self.dev.attach_kernel_driver(intf)
                    except:
                        pass
            except:
                pass

    def write(self, data: bytes, timeout: int = DEFAULT_TIMEOUT):
        """Write data in small chunks with flow control to respect WCH buffer limits.

        The WCH CH55x chip has a 32-byte USB OUT buffer. Sending data too fast
        causes the chip's internal FIFO to corrupt data. We use:
        1. Small chunks (16 bytes) - well under the 32-byte limit
        2. Explicit delay between chunks to let the chip forward data to UART
        3. Verification that each USB transfer completed successfully
        """
        total_len = len(data)
        offset = 0
        chunk_num = 0

        if DEBUG_USB:
            cprint(Colors.YELLOW, f"USB WRITE: {total_len} bytes in {(total_len + MAX_USB_PACKET - 1) // MAX_USB_PACKET} chunks")

        while offset < total_len:
            chunk = data[offset:offset + MAX_USB_PACKET]
            chunk_len = len(chunk)

            try:
                # Write with explicit timeout and wait for completion
                bytes_written = self.ep_out.write(chunk, timeout)

                if bytes_written != chunk_len:
                    raise RuntimeError(f"Incomplete write: {bytes_written}/{chunk_len} bytes at offset {offset}")

                if DEBUG_USB and chunk_num < 3:
                    cprint(Colors.YELLOW, f"  Chunk {chunk_num}: {chunk_len} bytes -> {chunk[:8].hex()}...")

            except usb.core.USBError as e:
                raise RuntimeError(f"USB write failed at offset {offset}/{total_len}: {e}")

            offset += chunk_len
            chunk_num += 1

            # CRITICAL: Delay between chunks to let WCH chip forward data to UART
            # The WCH CH55x has limited internal buffering - if we send too fast,
            # the USB-to-UART conversion corrupts data.
            # At 115200 baud, 16 bytes takes ~1.4ms to transmit over UART.
            # We use 5ms to be safe and allow the chip's FIFO to drain.
            time.sleep(INTER_CHUNK_DELAY_MS / 1000.0)

    def read(self, size: int = 64, timeout: int = DEFAULT_TIMEOUT) -> bytes:
        """Read data from device"""
        try:
            data = self.ep_in.read(size, timeout)
            return bytes(data)
        except usb.core.USBTimeoutError:
            return b''

    def flush(self):
        """Flush any pending data"""
        for _ in range(10):
            try:
                data = self.read(64, 10)
                if not data:
                    break
            except:
                break

    @staticmethod
    def checksum(data: bytes, state: int = 0xEF) -> int:
        for byte in data:
            state ^= byte
        return state

    def command(self, op: int, data: bytes = b'', chk: int = 0,
                timeout: float = DEFAULT_TIMEOUT) -> Tuple[int, bytes]:
        """Send command and receive response.

        The ESP32 ROM bootloader may be printing garbage text (like "invalid header")
        while also responding to commands. We need to find the valid SLIP frame
        in a stream that may contain both text and responses.
        """
        # Build packet: direction(1) + command(1) + size(2) + checksum(4) + data
        pkt = struct.pack('<BBHI', 0x00, op, len(data), chk) + data
        slip_pkt = SLIPProtocol.encode(pkt)

        # Send
        self.write(slip_pkt, int(timeout))

        # Read response - may contain garbage text mixed with SLIP frames
        start = time.time()
        response_data = bytearray()

        # Read until we have enough data or timeout
        while (time.time() - start) < (timeout / 1000):
            try:
                chunk = self.read(64, 50)
                if chunk:
                    response_data.extend(chunk)

                    # Try to find a valid SLIP frame for our command
                    frame = self._find_valid_response(response_data, op)
                    if frame is not None:
                        return frame
            except:
                pass
            time.sleep(0.005)

        # Final attempt to parse what we have
        if response_data:
            frame = self._find_valid_response(response_data, op)
            if frame is not None:
                return frame

        raise TimeoutError(f"Timeout waiting for response to {hex(op)}, got: {response_data[:50].hex() if response_data else 'nothing'}")

    def _find_valid_response(self, data: bytearray, expected_op: int) -> Optional[Tuple[int, bytes]]:
        """Find and parse a valid SLIP response frame in potentially garbage-filled data.

        Returns (value, body) tuple if found, None if not found yet.
        """
        # Look for all SLIP_END markers
        end_positions = [i for i, b in enumerate(data) if b == SLIP_END]

        if len(end_positions) < 2:
            return None  # Not enough frame markers yet

        # Try each pair of SLIP_END markers as potential frame boundaries
        for i in range(len(end_positions) - 1):
            frame_start = end_positions[i]
            frame_end = end_positions[i + 1]

            if frame_end - frame_start < 10:  # Too short to be a valid response
                continue

            frame = bytes(data[frame_start:frame_end + 1])
            decoded = SLIPProtocol.decode(frame)

            if len(decoded) < 8:
                continue  # Too short

            # Parse header: direction(1) + command(1) + size(2) + value(4)
            try:
                direction, cmd, size, val = struct.unpack('<BBHI', decoded[:8])
            except:
                continue

            # Validate: direction should be 1 (response), command should match
            if direction != 1:
                continue
            if cmd != expected_op:
                continue

            body = decoded[8:]

            # Check for valid body length (should match 'size' field, or close)
            if len(body) < 2:
                continue

            # Status bytes at end
            status = body[-2:]
            if status[0] != 0:
                error_code = status[1] if len(status) > 1 else 0
                raise RuntimeError(f"Command {hex(expected_op)} failed: error {hex(error_code)}")

            # Success!
            if DEBUG_USB:
                cprint(Colors.GREEN, f"  Valid response found: dir={direction}, cmd={hex(cmd)}, size={size}, val={val}")
            return val, body[:-2] if len(body) > 2 else b''

        return None  # No valid frame found yet

    def set_baud_rate(self, baud: int = 115200):
        """Set baud rate via CDC ACM SET_LINE_CODING request"""
        # SET_LINE_CODING: bmRequestType=0x21, bRequest=0x20
        # Data: dwDTERate(4) + bCharFormat(1) + bParityType(1) + bDataBits(1)
        # Standard: 115200, 1 stop bit, no parity, 8 data bits
        line_coding = struct.pack('<IBBB', baud, 0, 0, 8)

        control_intfs = [self.intf_num - 1, 0, 2]

        for ctrl_intf in control_intfs:
            try:
                self.dev.ctrl_transfer(
                    0x21,  # bmRequestType: Class, Interface, Host-to-Device
                    0x20,  # bRequest: SET_LINE_CODING
                    0,     # wValue: 0
                    ctrl_intf,  # wIndex: CDC control interface
                    line_coding,  # Data: line coding struct
                    1000   # timeout
                )
                cprint(Colors.GREEN, f"Baud rate set to {baud}")
                return True
            except Exception as e:
                continue

        cprint(Colors.YELLOW, f"Baud rate set skipped (WCH chip may not support)")
        return False

    def set_control_lines(self, dtr: bool, rts: bool):
        """Set DTR and RTS control lines via CDC ACM control transfer"""
        # SET_CONTROL_LINE_STATE: bmRequestType=0x21, bRequest=0x22
        # wValue: bit 0 = DTR, bit 1 = RTS
        value = (1 if dtr else 0) | (2 if rts else 0)

        # CDC control interface is typically intf_num - 1 for standard CDC ACM
        # But WCH chips may use different layout, try multiple
        control_intfs = [self.intf_num - 1, 0, 2]

        for ctrl_intf in control_intfs:
            try:
                self.dev.ctrl_transfer(
                    0x21,  # bmRequestType: Class, Interface, Host-to-Device
                    0x22,  # bRequest: SET_CONTROL_LINE_STATE
                    value,  # wValue: DTR and RTS bits
                    ctrl_intf,  # wIndex: CDC control interface
                    None,  # No data
                    1000   # timeout
                )
                return  # Success
            except Exception as e:
                continue  # Try next interface

        cprint(Colors.YELLOW, f"Control line set skipped (WCH chip may not support)")

    def reset_to_bootloader(self):
        """Try to reset ESP32 into bootloader mode using DTR/RTS"""
        cprint(Colors.YELLOW, "Attempting bootloader reset sequence...")

        # Classic ESP32 bootloader entry sequence:
        # 1. RTS=1, DTR=0 -> RESET=0, GPIO0=1 (normal boot)
        # 2. RTS=0, DTR=1 -> RESET=1, GPIO0=0 (hold GPIO0 low)
        # 3. RTS=1, DTR=1 -> RESET=0, GPIO0=0 (release reset while holding GPIO0)
        # 4. RTS=0, DTR=0 -> Release all

        cprint(Colors.YELLOW, "  Step 1: Reset chip...")
        self.set_control_lines(dtr=False, rts=True)  # RTS->EN, DTR->GPIO0
        time.sleep(0.1)

        cprint(Colors.YELLOW, "  Step 2: Enter bootloader (hold GPIO0)...")
        self.set_control_lines(dtr=True, rts=False)  # GPIO0 low
        time.sleep(0.1)

        cprint(Colors.YELLOW, "  Step 3: Release reset...")
        self.set_control_lines(dtr=True, rts=True)  # Release reset while GPIO0 low
        time.sleep(0.05)

        cprint(Colors.YELLOW, "  Step 4: Release GPIO0...")
        self.set_control_lines(dtr=False, rts=False)
        time.sleep(0.3)  # Wait for bootloader to start

        cprint(Colors.GREEN, "Bootloader reset sequence complete")

    def sync(self) -> bool:
        """Sync with bootloader"""
        cprint(Colors.YELLOW, "Syncing with bootloader...")

        # Very aggressive flush - the ROM prints boot messages that we need to discard
        cprint(Colors.YELLOW, "Clearing USB buffers (waiting for boot messages to stop)...")
        time.sleep(0.5)  # Wait for boot messages to finish

        garbage_data = bytearray()
        for _ in range(50):  # More flush iterations
            try:
                data = self.read(64, 20)
                if data:
                    garbage_data.extend(data)
            except:
                pass

        if garbage_data:
            # Show as text if printable, else hex
            try:
                text = garbage_data.decode('ascii', errors='replace')
                cprint(Colors.YELLOW, f"  Flushed: {text[:100]}...")
            except:
                cprint(Colors.YELLOW, f"  Flushed: {garbage_data[:50].hex()}")

        time.sleep(0.3)  # Additional settle time

        sync_data = b'\x07\x07\x12\x20' + (b'\x55' * 32)

        for attempt in range(15):  # More attempts
            try:
                # Flush right before each attempt
                for _ in range(5):
                    try:
                        self.read(64, 5)
                    except:
                        pass

                val, _ = self.command(ESP_SYNC, sync_data, timeout=SYNC_TIMEOUT * 2)  # Longer timeout

                # Drain ALL extra sync responses - ROM sends 8 total
                cprint(Colors.YELLOW, "Draining sync responses...")
                for i in range(15):
                    try:
                        # Read and discard
                        self.read(64, 50)
                    except:
                        pass
                    time.sleep(0.01)

                # One more flush
                self.flush()
                time.sleep(0.1)  # Let things settle
                self.flush()

                cprint(Colors.GREEN, "Synced with bootloader!")
                return True

            except Exception as e:
                if DEBUG_USB:
                    cprint(Colors.YELLOW, f"  Attempt {attempt + 1}: {e}")
                if attempt < 14:
                    time.sleep(0.1)  # Longer delay between attempts
                else:
                    cprint(Colors.RED, f"Sync failed: {e}")

        return False

    def flash_begin(self, size: int, offset: int) -> int:
        """Begin flash operation"""
        num_blocks = (size + FLASH_WRITE_SIZE - 1) // FLASH_WRITE_SIZE
        erase_size = size

        cprint(Colors.YELLOW, f"Flash begin: {size} bytes at {hex(offset)} ({num_blocks} blocks)")

        # Longer timeout for erase
        timeout = max(DEFAULT_TIMEOUT, (size / (1024 * 1024)) * 30000)

        params = struct.pack('<IIII', erase_size, num_blocks, FLASH_WRITE_SIZE, offset)
        self.command(ESP_FLASH_BEGIN, params, timeout=timeout)

        return num_blocks

    def flash_block(self, data: bytes, seq: int):
        """Write a block to flash"""
        # Pad to block size
        if len(data) < FLASH_WRITE_SIZE:
            data = data + (b'\xFF' * (FLASH_WRITE_SIZE - len(data)))

        params = struct.pack('<IIII', len(data), seq, 0, 0) + data
        self.command(ESP_FLASH_DATA, params, chk=self.checksum(data), timeout=DEFAULT_TIMEOUT)

    def flash_finish(self, reboot: bool = False):
        """Finish flash operation"""
        pkt = struct.pack('<I', int(not reboot))
        self.command(ESP_FLASH_END, pkt)

    def flash_file(self, filepath: str, offset: int) -> Tuple[bool, float]:
        """Flash a file to the specified offset. Returns (success, elapsed_seconds)."""
        filename = os.path.basename(filepath)

        if not os.path.exists(filepath):
            cprint(Colors.RED, f"File not found: {filepath}")
            return False, 0.0

        with open(filepath, 'rb') as f:
            data = f.read()

        size = len(data)
        cprint(Colors.BLUE, f"\nFlashing {filename} ({size:,} bytes) to {hex(offset)}")

        flash_start = time.time()
        try:
            num_blocks = self.flash_begin(size, offset)

            for seq in range(num_blocks):
                block_start = seq * FLASH_WRITE_SIZE
                block_end = min(block_start + FLASH_WRITE_SIZE, size)
                block = data[block_start:block_end]

                self.flash_block(block, seq)

                progress = ((seq + 1) * 100) // num_blocks
                elapsed = time.time() - flash_start
                rate = (seq + 1) * FLASH_WRITE_SIZE / elapsed if elapsed > 0 else 0
                print(f"\r  Progress: {progress}% ({seq + 1}/{num_blocks}) - {rate/1024:.1f} KB/s", end='', flush=True)

            elapsed = time.time() - flash_start
            rate = size / elapsed if elapsed > 0 else 0
            print()
            cprint(Colors.GREEN, f"  {filename} flashed in {elapsed:.1f}s ({rate/1024:.1f} KB/s)")
            return True, elapsed

        except Exception as e:
            cprint(Colors.RED, f"\n  Failed: {e}")
            return False, time.time() - flash_start


def main():
    parser = argparse.ArgumentParser(
        description='Mac ESP32-S3 Flasher for WCH CH55x USB-Serial',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  ./mac-flasher.py --dir ~/Desktop/45
  ./mac-flasher.py --test
  ./mac-flasher.py bootloader.bin@0x0 voice_agent.bin@0x110000
"""
    )

    parser.add_argument('--dir', '-d', help='Directory with firmware files')
    parser.add_argument('--test', '-t', action='store_true', help='Test connection')
    parser.add_argument('--port', '-p', type=int, default=1, choices=[0, 1],
                        help='Serial port number (0 or 1, default: 1)')
    parser.add_argument('--baud', '-b', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--chunk', '-c', type=int, default=16,
                        help='USB chunk size in bytes (default: 16, max: 32)')
    parser.add_argument('--delay', type=int, default=5,
                        help='Delay between USB chunks in ms (default: 5)')
    parser.add_argument('--debug', action='store_true', help='Enable USB debug output')
    parser.add_argument('--no-reboot', action='store_true', help="Don't reboot after flash")
    parser.add_argument('--reset', '-r', action='store_true', help='Reset into bootloader mode')
    parser.add_argument('files', nargs='*', help='Files: file.bin@offset')

    args = parser.parse_args()

    # Apply global settings from command line
    global MAX_USB_PACKET, INTER_CHUNK_DELAY_MS, DEBUG_USB
    MAX_USB_PACKET = min(args.chunk, 32)  # Cap at 32 (WCH limit)
    INTER_CHUNK_DELAY_MS = args.delay
    DEBUG_USB = args.debug

    # Determine files to flash
    flash_files = []

    if args.dir:
        dir_path = os.path.expanduser(args.dir)
        standard_files = [
            ('bootloader.bin', 0x0),
            ('partition-table.bin', 0x8000),
            ('voice_agent.bin', 0x110000),
            ('srmodels.bin', 0xd10000),
        ]
        for filename, offset in standard_files:
            filepath = os.path.join(dir_path, filename)
            if os.path.exists(filepath):
                flash_files.append((filepath, offset))

    elif args.files:
        for file_spec in args.files:
            if '@' in file_spec:
                filepath, offset_str = file_spec.rsplit('@', 1)
                offset = int(offset_str, 0)
                flash_files.append((filepath, offset))

    # Header
    cprint(Colors.GREEN, "=" * 50)
    cprint(Colors.GREEN, "  Mac ESP32-S3 Flasher (WCH CH55x)")
    cprint(Colors.GREEN, "  Bypasses macOS CDC driver issues")
    cprint(Colors.GREEN, "=" * 50)
    cprint(Colors.YELLOW, f"  USB chunk: {MAX_USB_PACKET} bytes, delay: {INTER_CHUNK_DELAY_MS}ms")
    if DEBUG_USB:
        cprint(Colors.YELLOW, "  Debug mode: ON")
    print()

    flasher = WCHFlasher()

    try:
        if not flasher.connect(args.port):
            cprint(Colors.YELLOW, """
=== Enter Bootloader Mode ===
1. Hold BOOT button on Watcher
2. Press RESET while holding BOOT
3. Release BOOT after 1 second
""")
            sys.exit(1)

        # Set baud rate - CRITICAL for proper communication!
        flasher.set_baud_rate(args.baud)

        # Try auto-reset into bootloader if --reset is specified
        if args.reset:
            flasher.reset_to_bootloader()

        if not flasher.sync():
            cprint(Colors.RED, "Failed to sync!")
            cprint(Colors.YELLOW, "Make sure device is in bootloader mode")
            sys.exit(1)

        if args.test:
            cprint(Colors.GREEN, "\nConnection test successful!")
            flasher.disconnect()
            sys.exit(0)

        if not flash_files:
            cprint(Colors.YELLOW, "No files to flash. Use --dir or specify files.")
            flasher.disconnect()
            sys.exit(0)

        # Flash files
        cprint(Colors.BLUE, f"\nFlashing {len(flash_files)} file(s)...")

        total_start = time.time()
        success = True
        total_bytes = 0
        for filepath, offset in flash_files:
            ok, elapsed = flasher.flash_file(filepath, offset)
            if not ok:
                success = False
                break
            total_bytes += os.path.getsize(filepath)

        total_elapsed = time.time() - total_start
        if success:
            flasher.flash_finish(reboot=not args.no_reboot)
            total_rate = total_bytes / total_elapsed if total_elapsed > 0 else 0
            cprint(Colors.GREEN, "\n" + "=" * 50)
            cprint(Colors.GREEN, f"  Flashing complete!")
            cprint(Colors.GREEN, f"  Total: {total_bytes:,} bytes in {total_elapsed:.1f}s")
            cprint(Colors.GREEN, f"  Average rate: {total_rate/1024:.1f} KB/s")
            cprint(Colors.GREEN, "=" * 50)
        else:
            cprint(Colors.RED, "\nFlashing failed!")
            sys.exit(1)

    except KeyboardInterrupt:
        cprint(Colors.YELLOW, "\nInterrupted")
    except Exception as e:
        cprint(Colors.RED, f"\nError: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
    finally:
        flasher.disconnect()


if __name__ == '__main__':
    main()
