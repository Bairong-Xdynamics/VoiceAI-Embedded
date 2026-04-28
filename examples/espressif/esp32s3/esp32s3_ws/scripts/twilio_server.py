from __future__ import annotations

import asyncio
import base64
import json
import logging
import time
import wave
from typing import Optional

import websockets

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("TwilioServer")

SERVER_HOST = "172.16.184.46"
SERVER_PORT = 8765
FRAME_MS = 20

# 回复音频：原始 PCM，16000 Hz、单声道、16-bit 小端（无头），见 test.pcm
REPLY_PCM_PATH = "test.pcm"
REPLY_SAMPLE_RATE = 16000
REPLY_SAMPWIDTH = 2  # 16-bit


def load_reply_pcm(path: str) -> tuple[bytes, int, int]:
    """从原始 PCM 文件读取全部字节。约定格式：16000 Hz、单声道、16-bit 小端。"""
    with open(path, "rb") as f:
        pcm = f.read()
    if len(pcm) == 0:
        raise ValueError("PCM 文件为空")
    if len(pcm) % REPLY_SAMPWIDTH != 0:
        raise ValueError(f"PCM 字节数须为 {REPLY_SAMPWIDTH} 的倍数（16-bit 对齐）")
    return pcm, REPLY_SAMPLE_RATE, REPLY_SAMPWIDTH


class WavPCMStreamer:
    """16k/mono/16-bit PCM 按帧取出；播放到末尾后从开头无缝循环。"""

    def __init__(self, pcm: bytes, sample_rate: int, sampwidth: int, frame_ms: int = FRAME_MS):
        self.pcm = pcm
        self.sample_rate = sample_rate
        self.sampwidth = sampwidth
        samples_per_frame = max(1, int(sample_rate * frame_ms / 1000))
        self.frame_bytes = samples_per_frame * sampwidth
        self._offset = 0

    def next_chunk(self) -> bytes:
        fb = self.frame_bytes
        pcm = self.pcm
        L = len(pcm)
        if fb <= 0 or L == 0:
            return b""
        # 常见：整段 WAV 远大于一帧，最多两次切片（跨环边界）
        if L >= fb:
            o = self._offset
            if o + fb <= L:
                self._offset = o + fb
                return pcm[o : o + fb]
            part1 = pcm[o:]
            need2 = fb - len(part1)
            part2 = pcm[:need2]
            self._offset = need2
            return part1 + part2
        out = bytearray()
        while len(out) < fb:
            need = fb - len(out)
            if self._offset >= L:
                self._offset = 0
            take = min(need, L - self._offset)
            out.extend(pcm[self._offset : self._offset + take])
            self._offset += take
        return bytes(out)


class PCMRecorder:
    def __init__(self, filename="recv_audio.wav", sample_rate=16000, sampwidth=2):
        self.filename = filename
        self.sample_rate = sample_rate
        self.sampwidth = sampwidth
        self.frames = []

    def push_base64(self, b64_str: str):
        pcm_bytes = base64.b64decode(b64_str)
        self.frames.append(pcm_bytes)

    def save(self):
        if not self.frames:
            return
        with wave.open(self.filename, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(self.sampwidth)
            wf.setframerate(self.sample_rate)
            for frame in self.frames:
                wf.writeframes(frame)
        logger.info(f"Saved received audio to {self.filename}")


async def handler(ws):
    logger.info("Client connected")
    recorder = PCMRecorder()
    streamer: Optional[WavPCMStreamer] = None
    stream_sid: Optional[str] = None
    pcm_out_task: Optional[asyncio.Task] = None
    frame_sec = FRAME_MS / 1000.0

    async def pcm_out_loop():
        """按 16000Hz 帧长对齐发送：用单调时钟截止时间减漂移，落后超过一帧则重同步。"""
        seq = 1
        next_tick = time.monotonic()
        try:
            while True:
                if streamer is None or stream_sid is None:
                    await asyncio.sleep(min(frame_sec, 0.05))
                    next_tick = time.monotonic()
                    continue
                delay = next_tick - time.monotonic()
                if delay > 0:
                    await asyncio.sleep(delay)
                elif delay < -frame_sec:
                    next_tick = time.monotonic()
                chunk = streamer.next_chunk()
                logger.debug("Sending chunk: %d bytes", len(chunk))
                out_b64 = base64.b64encode(chunk).decode("ascii")
                await ws.send(
                    json.dumps(
                        {
                            "event": "media",
                            "sequenceNumber": str(seq),
                            "media": {
                                "track": "outbound",
                                "chunk": str(seq),
                                "timestamp": str(seq * FRAME_MS),
                                "payload": out_b64,
                            },
                            "streamSid": stream_sid,
                        }
                    )
                )
                seq += 1
                next_tick += frame_sec
        except asyncio.CancelledError:
            raise

    try:
        pcm, rate, sw = load_reply_pcm(REPLY_PCM_PATH)
        streamer = WavPCMStreamer(pcm, rate, sw, FRAME_MS)
        logger.info(
            "Loaded reply PCM: %s (%d Hz, mono, %d-bit, %d bytes, loop)",
            REPLY_PCM_PATH,
            rate,
            sw * 8,
            len(pcm),
        )
    except FileNotFoundError:
        logger.error("未找到回复音频文件: %s", REPLY_PCM_PATH)
    except Exception as e:
        logger.exception("加载 PCM 失败: %s", e)

    try:
        async for msg in ws:
            data = json.loads(msg)
            event = data.get("event")

            if event == "connected":
                logger.info(f"➡️ connected event: {data}")
            elif event == "start":
                logger.info(f"➡️ start event: {data}")
                stream_sid = data.get("streamSid") or (data.get("start") or {}).get("streamSid")
                if streamer is not None:
                    streamer._offset = 0
                if pcm_out_task is not None and not pcm_out_task.done():
                    pcm_out_task.cancel()
                    try:
                        await pcm_out_task
                    except asyncio.CancelledError:
                        pass
                pcm_out_task = asyncio.create_task(pcm_out_loop())
            elif event == "media":
                media = data.get("media", {})
                payload = media.get("payload")
                if payload:
                    recorder.push_base64(payload)
                if stream_sid is None:
                    stream_sid = data.get("streamSid")
            elif event == "stop":
                logger.info(f"➡️ stop event: {data}")
                if pcm_out_task is not None and not pcm_out_task.done():
                    pcm_out_task.cancel()
                    try:
                        await pcm_out_task
                    except asyncio.CancelledError:
                        pass
                break
            else:
                logger.info(f"Unknown event: {data}")
    except websockets.ConnectionClosed:
        logger.info("Client disconnected")
    finally:
        if pcm_out_task is not None and not pcm_out_task.done():
            pcm_out_task.cancel()
            try:
                await pcm_out_task
            except asyncio.CancelledError:
                pass
        recorder.save()


async def main():
    async with websockets.serve(handler, SERVER_HOST, SERVER_PORT, ping_interval=None):
        logger.info(f"Server started at ws://{SERVER_HOST}:{SERVER_PORT}")
        stop_event = asyncio.Event()
        try:
            await stop_event.wait()
        except asyncio.CancelledError:
            pass


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Server stopped by user")
