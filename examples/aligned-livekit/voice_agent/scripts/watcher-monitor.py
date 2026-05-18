#!/usr/bin/env python3
"""
SenseCap Watcher Serial Monitor

Automatically connects to the Watcher device and displays serial output.
Handles port detection, correct baud rate (921600), and reconnection.

Usage:
    ./watcher-monitor.py          # Auto-detect and connect
    ./watcher-monitor.py --list   # List available ports
    ./watcher-monitor.py --port /dev/cu.usbmodemXXX  # Use specific port
"""

import serial
import serial.tools.list_ports
import sys
import time
import argparse
import signal

# Watcher uses 921600 baud for the main serial output
BAUD_RATE = 921600
# USB Vendor IDs for Watcher
ESPRESSIF_VID = 0x303A  # ESP32-S3 native USB
WCH_VID = 0x1A86        # WCH CH55x USB-to-serial chip (actual Watcher)

class WatcherMonitor:
    def __init__(self, baud_rate=BAUD_RATE):
        self.ser = None
        self.running = True
        self.baud_rate = baud_rate
        signal.signal(signal.SIGINT, self._signal_handler)

    def _signal_handler(self, sig, frame):
        print("\n\n[Monitor] Ctrl+C detected, exiting...")
        self.running = False
        if self.ser:
            self.ser.close()
        sys.exit(0)

    def find_watcher_ports(self):
        """Find all potential Watcher USB serial ports."""
        ports = []
        for port in serial.tools.list_ports.comports():
            # Look for Watcher USB ports (WCH chip VID, Espressif VID, or "usbmodem" on macOS)
            if ('usbmodem' in port.device.lower() or
                port.vid == WCH_VID or
                port.vid == ESPRESSIF_VID):
                ports.append(port)
        return ports

    def detect_correct_port(self, ports):
        """
        Detect which port has readable output at the correct baud rate.
        The Watcher has two USB interfaces - we want the one with text output.
        """
        print("[Monitor] Scanning ports for Watcher output...")

        for port in ports:
            try:
                ser = serial.Serial(port.device, self.baud_rate, timeout=0.5)
                time.sleep(0.1)
                data = ser.read(512)
                ser.close()

                if data:
                    # Check if it looks like readable text
                    try:
                        text = data.decode('utf-8', errors='replace')
                        printable = sum(1 for c in text if c.isprintable() or c in '\n\r\t')
                        if printable > len(text) * 0.3:
                            print(f"[Monitor] Found Watcher on {port.device}")
                            return port.device
                    except:
                        pass
            except Exception as e:
                pass

        # If no port had output, return the first one (user might need to trigger output)
        if ports:
            print(f"[Monitor] No active output detected, trying {ports[0].device}")
            return ports[0].device

        return None

    def connect(self, port=None):
        """Connect to the Watcher."""
        if port is None:
            ports = self.find_watcher_ports()
            if not ports:
                print("[Monitor] ERROR: No Watcher device found!")
                print("[Monitor] Make sure the device is plugged in via USB.")
                return False

            port = self.detect_correct_port(ports)
            if not port:
                print("[Monitor] ERROR: Could not detect Watcher port.")
                return False

        try:
            self.ser = serial.Serial(port, self.baud_rate, timeout=0.1)
            print(f"[Monitor] Connected to {port} @ {self.baud_rate} baud")
            print("[Monitor] Press Ctrl+C to exit")
            print("-" * 60)
            return True
        except Exception as e:
            print(f"[Monitor] ERROR: Could not open {port}: {e}")
            return False

    def run(self):
        """Main loop - read and display serial data."""
        if not self.ser:
            return

        while self.running:
            try:
                data = self.ser.read(1024)
                if data:
                    text = data.decode('utf-8', errors='replace')
                    print(text, end='', flush=True)
            except serial.SerialException as e:
                print(f"\n[Monitor] Connection lost: {e}")
                print("[Monitor] Attempting to reconnect...")
                self.ser.close()
                time.sleep(2)
                if not self.reconnect():
                    break
            except Exception as e:
                print(f"\n[Monitor] Error: {e}")
                time.sleep(0.1)

    def reconnect(self):
        """Try to reconnect after connection loss."""
        for attempt in range(5):
            ports = self.find_watcher_ports()
            if ports:
                port = self.detect_correct_port(ports)
                if port:
                    try:
                        self.ser = serial.Serial(port, self.baud_rate, timeout=0.1)
                        print(f"[Monitor] Reconnected to {port}")
                        print("-" * 60)
                        return True
                    except:
                        pass
            print(f"[Monitor] Reconnect attempt {attempt + 1}/5...")
            time.sleep(2)

        print("[Monitor] Failed to reconnect. Please check the device.")
        return False

    def list_ports(self):
        """List all available USB serial ports."""
        print("\nAvailable USB Serial Ports:")
        print("-" * 60)

        all_ports = list(serial.tools.list_ports.comports())
        watcher_ports = self.find_watcher_ports()

        if not all_ports:
            print("  No USB serial ports found.")
            return

        for port in all_ports:
            is_watcher = port in watcher_ports
            marker = " <-- Likely Watcher" if is_watcher else ""
            print(f"  {port.device}")
            print(f"      Description: {port.description}")
            if port.vid and port.pid:
                print(f"      VID:PID: {port.vid:04X}:{port.pid:04X}{marker}")
            print()


def main():
    parser = argparse.ArgumentParser(
        description="SenseCap Watcher Serial Monitor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  ./watcher-monitor.py              # Auto-detect and connect
  ./watcher-monitor.py --list       # List available ports
  ./watcher-monitor.py -p /dev/cu.usbmodem56D50186501  # Specific port
        """
    )
    parser.add_argument('--list', '-l', action='store_true',
                        help='List available serial ports')
    parser.add_argument('--port', '-p', type=str,
                        help='Specify serial port to use')
    parser.add_argument('--baud', '-b', type=int, default=BAUD_RATE,
                        help=f'Baud rate (default: {BAUD_RATE})')

    args = parser.parse_args()

    if args.list:
        monitor = WatcherMonitor()
        monitor.list_ports()
        return

    monitor = WatcherMonitor(baud_rate=args.baud)

    print()
    print("=" * 60)
    print("  SenseCap Watcher Serial Monitor")
    print("=" * 60)
    print()

    if monitor.connect(args.port):
        monitor.run()


if __name__ == "__main__":
    main()
