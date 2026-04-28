#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
接收小智固件 WebSocket 调试数据：文本帧为 JSON 日志 {"type":"log","text":"..."}，
二进制帧为 PCM：魔数 XZPC + sample_rate(u32 LE) + channels(u16) + reserved(u16) + sample_count(u32) + s16le 数据。

依赖: pip install websockets

用法:
  python scripts/ws_debug_server.py --host 0.0.0.0 --port 8765 --out ./ws_debug_out
"""

from __future__ import annotations

import argparse
import asyncio
import json
import struct
import sys
from datetime import datetime
from pathlib import Path
from typing import Optional

import websockets
from websockets import ServerConnection
from websockets.exceptions import ConnectionClosed

PCM_MAGIC = b"XZPC"
PCM_HEADER_FMT = "<4sIHHI"  # magic, sample_rate, channels, reserved, sample_count
PCM_HEADER_SIZE = struct.calcsize(PCM_HEADER_FMT)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="WebSocket 接收设备日志与 PCM 并写入文件")
    p.add_argument("--host", default="0.0.0.0", help="监听地址")
    p.add_argument("--port", type=int, default=8765, help="监听端口")
    p.add_argument("--out", default="ws_debug_out", help="输出目录")
    return p.parse_args()


class SessionWriter:
    def __init__(self, out_dir: Path) -> None:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.session_dir = out_dir / ts
        self.session_dir.mkdir(parents=True, exist_ok=True)
        self.log_path = self.session_dir / "device.log"
        self.pcm_path = self.session_dir / "capture.pcm"
        self.meta_path = self.session_dir / "meta.json"
        self.log_fp = self.log_path.open("a", encoding="utf-8", buffering=1)
        self.pcm_fp = self.pcm_path.open("ab", buffering=0)
        self.sample_rate: Optional[int] = None
        self.channels: Optional[int] = None
        print(f"会话目录: {self.session_dir.resolve()}", flush=True)

    def close(self) -> None:
        self.log_fp.close()
        self.pcm_fp.close()
        meta = {}
        if self.sample_rate is not None:
            meta["sample_rate"] = self.sample_rate
        if self.channels is not None:
            meta["channels"] = self.channels
        meta["format"] = "s16le"
        meta["pcm_file"] = self.pcm_path.name
        meta["log_file"] = self.log_path.name
        self.meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")

    def write_log_text(self, text: str) -> None:
        self.log_fp.write(text)
        if not text.endswith("\n"):
            self.log_fp.write("\n")

    def write_pcm(self, sample_rate: int, channels: int, pcm: bytes) -> None:
        if self.sample_rate is None:
            self.sample_rate = sample_rate
        if self.channels is None:
            self.channels = channels
        self.pcm_fp.write(pcm)


async def handler(ws: ServerConnection, writer: SessionWriter) -> None:
    peer = ws.remote_address
    print(f"已连接: {peer}", flush=True)
    try:
        async for message in ws:
            if isinstance(message, str):
                try:
                    obj = json.loads(message)
                except json.JSONDecodeError:
                    writer.write_log_text(message)
                    continue
                if obj.get("type") == "log" and "text" in obj:
                    writer.write_log_text(str(obj["text"]))
                else:
                    writer.write_log_text(message)
            else:
                if len(message) < PCM_HEADER_SIZE:
                    continue
                magic, sr, ch, _reserved, n_samples = struct.unpack(
                    PCM_HEADER_FMT, message[:PCM_HEADER_SIZE]
                )
                if magic != PCM_MAGIC:
                    print(f"未知二进制帧 (magic={magic!r})", flush=True)
                    continue
                payload = message[PCM_HEADER_SIZE:]
                expect = n_samples * 2
                if len(payload) < expect:
                    print(
                        f"PCM 长度不足: got {len(payload)} expect {expect}",
                        flush=True,
                    )
                    continue
                writer.write_pcm(sr, ch, payload[:expect])
    except ConnectionClosed:
        # 对端未回 Close 帧、TCP 直接断等会触发 ConnectionClosedError，仍属会话结束
        pass
    finally:
        print(f"断开: {peer}", flush=True)


async def main_async(args: argparse.Namespace) -> None:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    async def conn(ws: ServerConnection) -> None:
        w = SessionWriter(out)
        try:
            await handler(ws, w)
        finally:
            w.close()

    async with websockets.serve(conn, args.host, args.port):
        print(
            f"监听 ws://{args.host}:{args.port}，保存到 {out.resolve()}",
            flush=True,
        )
        await asyncio.Future()


def main() -> None:
    args = parse_args()
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        print("退出", flush=True)
        sys.exit(0)


if __name__ == "__main__":
    main()
