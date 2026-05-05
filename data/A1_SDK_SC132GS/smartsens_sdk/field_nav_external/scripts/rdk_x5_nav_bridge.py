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


# A1 -> RDK 导航帧长度，必须与 main.cpp 的 packet[16] 保持一致。
NAV_FRAME_LEN = 16
# RDK -> 下位机控制帧长度，当前同样固定为 16 字节。
CMD_FRAME_LEN = 16
# A1 导航帧帧头：用于在串口字节流中重新同步帧边界。
NAV_HEADER = b"\xA5\x5A"
# 下位机控制帧帧头：便于下位机区分 RDK 控制命令。
CMD_HEADER = b"\xB5\x5B"

# Linux termios 波特率枚举映射；只允许表内值，避免传入平台不支持的波特率。
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
    """A1 导航帧的结构化结果。

    字段来自 16 字节协议帧：seq 用于观察丢帧，valid/status/confidence 用于安全判定，
    deviation_px/angle_deg/bottom_x_px 用于计算 RDK 输出给下位机的线速度和角速度。
    """

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
    """计算协议校验和。

    输入 frame 至少 15 字节；输出为前 15 字节累加后的低 8 位。
    使用注意：A1 导航帧和 RDK 控制帧共用该校验规则。
    """

    return sum(frame[:15]) & 0xFF


def clamp(value: float, low: float, high: float) -> float:
    """把 value 限制在 [low, high]，用于速度和协议 int16 字段防溢出。"""

    return max(low, min(high, value))


def open_uart(path: str, baudrate: int) -> int:
    """打开并配置 RDK 侧 UART。

    输入 path 为 /dev/ttyS* 等设备，baudrate 为 BAUD_MAP 支持值。
    输出为非阻塞 fd；异常表示设备不存在、权限不足或波特率不支持。
    """

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
    """从串口接收缓冲区中解析完整 A1 导航帧。

    输入 buffer 会被原地消费；输出为本次解析出的 NavFrame 列表。
    实现原理：查找 A5 5A 帧头，长度够 16 字节后校验 checksum，坏帧直接丢弃并继续同步。
    """

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
    """生成 RDK 发给下位机的 16 字节控制帧。

    输入 seq/enable/valid_nav/速度/偏差/mode；输出 bytes。
    使用注意：线速度单位为 mm/s，角速度单位为 mrad/s，偏差按 px*10 写入 int16。
    """

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
    """把最近一帧 A1 导航数据转换为下位机速度命令。

    输入 nav 可为空，args 提供阈值和比例参数；输出 enable/valid/linear/angular/deviation/mode。
    安全策略：无帧、超时、低置信度或 A1 status 非 0 时全部输出停车。
    """

    if nav is None:
        return False, False, 0, 0, 0.0, 2

    age = time.monotonic() - nav.timestamp
    valid_nav = nav.valid and nav.status == 0 and nav.confidence_pct >= args.min_confidence and age <= args.timeout
    if not valid_nav:
        return False, False, 0, 0, nav.deviation_px, 2

    # 简单 P 控制：横向偏差和航向角共同决定角速度，下位机只执行 RDK 的最终控制帧。
    angular = args.kp_dev * nav.deviation_px + args.kp_ang * nav.angle_deg
    angular = int(clamp(round(angular), -args.max_angular, args.max_angular))
    linear = args.linear
    if abs(nav.deviation_px) > args.slow_dev or abs(nav.angle_deg) > args.slow_angle:
        linear = int(linear * 0.5)
    return True, True, int(linear), angular, nav.deviation_px, 1


def main() -> int:
    """RDK 桥接主循环。

    负责打开 UART、持续解析 A1 导航帧、按固定频率输出控制帧，并在退出时发送停车帧。
    """

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

    rx_buffer = bytearray()  # 串口接收缓冲，parse_nav_frames 会原地消费完整帧。
    last_nav: NavFrame | None = None  # 最近一帧有效格式的 A1 导航数据。
    next_send = time.monotonic()  # 下一次发送下位机控制帧的时间点。
    interval = 1.0 / max(args.rate, 1.0)  # RDK 输出控制帧周期，最低按 1Hz 防止除零。
    cmd_seq = 0  # RDK 控制帧序号，16 位循环。
    last_report = time.monotonic()  # 人类可读诊断日志的上次打印时间。

    try:
        while not stop:
            now = time.monotonic()
            timeout = max(0.0, min(0.02, next_send - now))
            # select 控制阻塞时间：既及时读 A1 串口，也保证按 rate 周期发送控制帧。
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
                # 每个周期都发送控制帧；无有效导航时 compute_control 会返回停车命令。
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
        # 无论异常还是 Ctrl+C 退出，都尽量先发停车帧，降低下位机继续运动风险。
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
