#!/usr/bin/env python3
"""RDK X5 UART bridge for A1 field navigation.

The same RDK 40Pin UART is used in full-duplex mode:
  A1 UART0TX  -> RDK UART RX
  RDK UART TX -> lower controller UART RX

This script has no third-party dependency. It uses Linux termios directly.
"""

from __future__ import annotations

import argparse
import os
import select
import signal
import struct
import sys
import termios
import time
from dataclasses import dataclass


NAV_FRAME_LEN = 16
CMD_FRAME_LEN = 16
NAV_HEADER = b"\xA5\x5A"
CMD_HEADER = b"\xB5\x5B"


BAUD_MAP = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
    230400: termios.B230400,
    460800: termios.B460800,
    921600: termios.B921600,
}


@dataclass
class NavFrame:
    seq: int
    valid: bool
    deviation_px: float
    angle_deg: float
    confidence_pct: int
    point_count: int
    bottom_x_px: int
    status: int
    timestamp: float


def checksum15(frame: bytes) -> int:
    return sum(frame[:15]) & 0xFF


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def open_uart(path: str, baudrate: int) -> int:
    if baudrate not in BAUD_MAP:
        raise ValueError(f"unsupported baudrate: {baudrate}")

    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    baud = BAUD_MAP[baudrate]

    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = baud
    attrs[5] = baud
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def parse_nav_frames(buffer: bytearray) -> list[NavFrame]:
    frames: list[NavFrame] = []
    while True:
        start = buffer.find(NAV_HEADER)
        if start < 0:
            del buffer[:-1]
            return frames
        if start > 0:
            del buffer[:start]
        if len(buffer) < NAV_FRAME_LEN:
            return frames

        raw = bytes(buffer[:NAV_FRAME_LEN])
        del buffer[:NAV_FRAME_LEN]
        if checksum15(raw) != raw[15]:
            continue

        seq = raw[4] | (raw[5] << 8)
        deviation_x10 = struct.unpack_from("<h", raw, 6)[0]
        angle_x100 = struct.unpack_from("<h", raw, 8)[0]
        bottom_x = raw[12] | (raw[13] << 8)
        frames.append(
            NavFrame(
                seq=seq,
                valid=(raw[3] & 0x01) != 0,
                deviation_px=deviation_x10 / 10.0,
                angle_deg=angle_x100 / 100.0,
                confidence_pct=raw[10],
                point_count=raw[11],
                bottom_x_px=bottom_x,
                status=raw[14],
                timestamp=time.monotonic(),
            )
        )


def make_command_frame(
    seq: int,
    enable: bool,
    valid_nav: bool,
    linear_v_mm_s: int,
    angular_w_mrad_s: int,
    deviation_px: float,
    mode: int,
) -> bytes:
    packet = bytearray(CMD_FRAME_LEN)
    packet[0:2] = CMD_HEADER
    packet[2] = 0x01
    packet[3] = (0x01 if enable else 0x00) | (0x02 if valid_nav else 0x00)
    struct.pack_into("<H", packet, 4, seq & 0xFFFF)
    struct.pack_into("<h", packet, 6, int(clamp(linear_v_mm_s, -32768, 32767)))
    struct.pack_into("<h", packet, 8, int(clamp(angular_w_mrad_s, -32768, 32767)))
    struct.pack_into("<h", packet, 10, int(clamp(round(deviation_px * 10.0), -32768, 32767)))
    packet[12] = mode & 0xFF
    packet[13] = 0
    packet[14] = 0
    packet[15] = checksum15(packet)
    return bytes(packet)


def compute_control(nav: NavFrame | None, args: argparse.Namespace) -> tuple[bool, bool, int, int, float, int]:
    if nav is None:
        return False, False, 0, 0, 0.0, 2

    age = time.monotonic() - nav.timestamp
    valid_nav = nav.valid and nav.status == 0 and nav.confidence_pct >= args.min_confidence and age <= args.timeout
    if not valid_nav:
        return False, False, 0, 0, nav.deviation_px, 2

    angular = args.kp_dev * nav.deviation_px + args.kp_ang * nav.angle_deg
    angular = int(clamp(round(angular), -args.max_angular, args.max_angular))
    linear = args.linear
    if abs(nav.deviation_px) > args.slow_dev or abs(nav.angle_deg) > args.slow_angle:
        linear = int(linear * 0.5)
    return True, True, int(linear), angular, nav.deviation_px, 1


def main() -> int:
    parser = argparse.ArgumentParser(description="RDK X5 bridge: A1 nav UART -> lower controller command UART")
    parser.add_argument("--port", required=True, help="RDK 40Pin UART device, for example /dev/ttyS1")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--rate", type=float, default=10.0)
    parser.add_argument("--timeout", type=float, default=0.3, help="A1 nav timeout in seconds")
    parser.add_argument("--linear", type=int, default=150, help="track mode linear speed, mm/s")
    parser.add_argument("--kp-dev", type=float, default=-2.0, help="mrad/s per pixel deviation")
    parser.add_argument("--kp-ang", type=float, default=-20.0, help="mrad/s per degree heading angle")
    parser.add_argument("--max-angular", type=int, default=800, help="angular speed clamp, mrad/s")
    parser.add_argument("--min-confidence", type=int, default=30, help="minimum A1 confidence percent")
    parser.add_argument("--slow-dev", type=float, default=180.0)
    parser.add_argument("--slow-angle", type=float, default=15.0)
    args = parser.parse_args()

    stop = False

    def handle_signal(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    fd = open_uart(args.port, args.baud)
    print(f"[rdk_nav] opened {args.port} baud={args.baud} rate={args.rate}Hz", flush=True)

    rx_buffer = bytearray()
    last_nav: NavFrame | None = None
    next_send = time.monotonic()
    interval = 1.0 / max(args.rate, 1.0)
    cmd_seq = 0
    last_report = time.monotonic()

    try:
        while not stop:
            now = time.monotonic()
            timeout = max(0.0, min(0.02, next_send - now))
            readable, _, _ = select.select([fd], [], [], timeout)
            if readable:
                try:
                    chunk = os.read(fd, 128)
                    if chunk:
                        rx_buffer.extend(chunk)
                        for frame in parse_nav_frames(rx_buffer):
                            last_nav = frame
                except BlockingIOError:
                    pass

            now = time.monotonic()
            if now >= next_send:
                enable, valid_nav, linear, angular, deviation, mode = compute_control(last_nav, args)
                packet = make_command_frame(cmd_seq, enable, valid_nav, linear, angular, deviation, mode)
                os.write(fd, packet)
                cmd_seq = (cmd_seq + 1) & 0xFFFF
                next_send = now + interval

            if now - last_report >= 1.0:
                if last_nav is None:
                    print("[rdk_nav] no A1 nav frame yet, sending stop", flush=True)
                else:
                    age_ms = (now - last_nav.timestamp) * 1000.0
                    print(
                        "[rdk_nav] nav seq={} valid={} dev={:.1f}px angle={:.2f}deg conf={} age={:.0f}ms".format(
                            last_nav.seq,
                            1 if last_nav.valid else 0,
                            last_nav.deviation_px,
                            last_nav.angle_deg,
                            last_nav.confidence_pct,
                            age_ms,
                        ),
                        flush=True,
                    )
                last_report = now
    finally:
        stop_packet = make_command_frame(cmd_seq, False, False, 0, 0, 0.0, 0)
        try:
            os.write(fd, stop_packet)
        except OSError:
            pass
        os.close(fd)
        print("[rdk_nav] closed, stop frame sent", flush=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
