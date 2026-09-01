#!/usr/bin/env python3

"""Dependency-free Chrome DevTools Protocol client for decoder provenance.

The power comparison has to prove which decoder actually ran, because "the
video played" is true for both arms. The Media domain reports
``kVideoDecoderName`` and ``kIsPlatformVideoDecoder`` for every player, which
is the same evidence the browser acceptance reports already require.

The target tablet has no websockets package and no pip network path, so this
speaks RFC 6455 directly. It also deliberately does no polling between
``measure_start`` and ``measure_end``: every DevTools round trip costs power,
and a sampler that perturbs the thing it measures is worse than no sampler.
"""

import argparse
import base64
import json
import os
import re
import socket
import struct
import sys
import time
import urllib.request

WS_URL = re.compile(r"ws://([^:/]+):(\d+)(/.*)")


class ProtocolError(RuntimeError):
    pass


class Connection:
    """One websocket to one DevTools target."""

    def __init__(self, url, timeout=30.0):
        match = WS_URL.match(url)
        if not match:
            raise ProtocolError(f"not a devtools websocket url: {url}")
        host, port, path = match.group(1), int(match.group(2)), match.group(3)
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ProtocolError("devtools closed during handshake")
            buf += chunk
        status = buf.split(b"\r\n", 1)[0]
        if b"101" not in status:
            raise ProtocolError(f"handshake refused: {status!r}")
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
        self.next_id = 0
        self.events = []

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _need(self, count):
        while len(self.buf) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ProtocolError("devtools closed")
            self.buf += chunk

    def _frame(self):
        while True:
            self._need(2)
            first, second = self.buf[0], self.buf[1]
            length = second & 0x7F
            offset = 2
            if length == 126:
                self._need(4)
                length = struct.unpack(">H", self.buf[2:4])[0]
                offset = 4
            elif length == 127:
                self._need(10)
                length = struct.unpack(">Q", self.buf[2:10])[0]
                offset = 10
            self._need(offset + length)
            payload = self.buf[offset : offset + length]
            self.buf = self.buf[offset + length :]
            opcode = first & 0x0F
            if opcode == 1:
                return payload.decode()
            if opcode == 8:
                raise ProtocolError("devtools sent close")

    def _send(self, payload):
        data = payload.encode()
        header = b"\x81"
        length = len(data)
        mask = os.urandom(4)
        if length < 126:
            header += bytes([0x80 | length])
        elif length < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", length)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", length)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(header + mask + masked)

    def call(self, method, params=None, timeout=30.0):
        self.next_id += 1
        call_id = self.next_id
        self._send(json.dumps({"id": call_id, "method": method, "params": params or {}}))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError(f"{method} timed out")
            self.sock.settimeout(remaining)
            message = json.loads(self._frame())
            if message.get("id") != call_id:
                if "method" in message:
                    self.events.append(message)
                continue
            if "error" in message:
                raise ProtocolError(f"{method} failed: {message['error']}")
            return message.get("result", {})

    def drain(self, seconds):
        """Collect events for a fixed window without sending anything."""
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            self.sock.settimeout(remaining)
            try:
                message = json.loads(self._frame())
            except (socket.timeout, TimeoutError):
                return
            if "method" in message:
                self.events.append(message)

    def evaluate(self, expression, timeout=30.0):
        result = self.call(
            "Runtime.evaluate",
            {"expression": expression, "awaitPromise": True, "returnByValue": True},
            timeout=timeout,
        )
        if "exceptionDetails" in result:
            raise ProtocolError(f"js failed: {json.dumps(result['exceptionDetails'])[:400]}")
        return result.get("result", {}).get("value")


def http_json(port, path, timeout=15.0):
    url = f"http://127.0.0.1:{port}{path}"
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read().decode())


def wait_for_browser(port, seconds):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            return http_json(port, "/json/version")
        except Exception as error:  # noqa: BLE001 - report the last failure
            last = error
            time.sleep(0.5)
    raise ProtocolError(f"devtools did not answer on {port}: {last}")


def first_page(port):
    for target in http_json(port, "/json/list"):
        if target.get("type") == "page" and target.get("webSocketDebuggerUrl"):
            return target
    raise ProtocolError("no page target with a debugger url")


def media_properties(events):
    """Fold Media domain property events into one dict per player."""
    players = {}
    for event in events:
        if event.get("method") != "Media.playerPropertiesChanged":
            continue
        params = event.get("params", {})
        player = players.setdefault(params.get("playerId", "unknown"), {})
        for prop in params.get("properties", []):
            name, value = prop.get("name"), prop.get("value")
            if name:
                player[name] = value
    return players


def pick_video_player(players):
    """Choose the player that reports a video decoder.

    A page can register more than one player; an audio-only or aborted one
    carries no decoder name and must not be reported as the decode evidence.
    """
    for player_id, props in players.items():
        if props.get("kVideoDecoderName"):
            return player_id, props
    if players:
        player_id = next(iter(players))
        return player_id, players[player_id]
    return None, {}


VIDEO_STATS_JS = """
(() => {
  const v = document.querySelector('video');
  if (!v) return null;
  const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : null;
  return {
    ready_state: v.readyState,
    paused: v.paused,
    ended: v.ended,
    current_time: v.currentTime,
    width: v.videoWidth,
    height: v.videoHeight,
    decoded_frames: q ? q.totalVideoFrames : null,
    dropped_frames: q ? q.droppedVideoFrames : null,
    error: v.error ? v.error.code : null
  };
})()
"""


def wait_for_playback(connection, timeout, min_frames):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = connection.evaluate(VIDEO_STATS_JS)
        if last and last.get("error"):
            raise ProtocolError(f"video element reported error code {last['error']}")
        if (
            last
            and not last.get("paused")
            and (last.get("decoded_frames") or 0) >= min_frames
        ):
            return last
        time.sleep(0.5)
    raise ProtocolError(f"playback did not reach {min_frames} frames: {last}")


def command_run(args):
    version = wait_for_browser(args.port, args.connect_timeout)
    target = first_page(args.port)
    connection = Connection(target["webSocketDebuggerUrl"])
    report = {
        "schema_version": 1,
        "browser": version.get("Browser"),
        "user_agent": version.get("User-Agent"),
        "url": args.url,
    }
    try:
        connection.call("Page.enable")
        connection.call("Runtime.enable")
        # Enable Media before navigating so playersCreated and the first
        # property batch cannot land before anyone is listening.
        connection.call("Media.enable")
        connection.call("Page.navigate", {"url": args.url})
        started = wait_for_playback(connection, args.playback_timeout, args.min_start_frames)
        report["start_stats"] = started
        # Let the decoder settle before the window opens: the first seconds of
        # a cold pipeline include GPU process bring-up and page layout.
        connection.drain(args.warmup_seconds)
        report["warmup_seconds"] = args.warmup_seconds
        report["measure_start_epoch"] = time.time()
        connection.drain(args.measure_seconds)
        report["measure_end_epoch"] = time.time()
        report["measure_seconds"] = args.measure_seconds
        report["end_stats"] = connection.evaluate(VIDEO_STATS_JS)
        players = media_properties(connection.events)
        player_id, props = pick_video_player(players)
        report["player_id"] = player_id
        report["player_count"] = len(players)
        report["decoder_name"] = props.get("kVideoDecoderName")
        platform = props.get("kIsPlatformVideoDecoder")
        if isinstance(platform, str):
            platform = platform.lower() == "true"
        report["is_platform_video_decoder"] = platform
        report["media_properties"] = props
    finally:
        connection.close()

    start = report.get("start_stats") or {}
    end = report.get("end_stats") or {}
    for key in ("decoded_frames", "dropped_frames"):
        if isinstance(start.get(key), int) and isinstance(end.get(key), int):
            report[f"window_{key}"] = end[key] - start[key]

    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    else:
        print(text)
    if report.get("window_decoded_frames") is not None and report["window_decoded_frames"] <= 0:
        print("FAIL no frames decoded inside the measurement window", file=sys.stderr)
        return 1
    return 0


def command_probe(args):
    print(json.dumps(wait_for_browser(args.port, args.connect_timeout), indent=2))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=9333)
    parser.add_argument("--connect-timeout", type=float, default=45.0)
    sub = parser.add_subparsers(dest="command", required=True)

    probe = sub.add_parser("probe", help="wait for the DevTools endpoint")
    probe.set_defaults(func=command_probe)

    run = sub.add_parser("run", help="navigate, hold a measurement window, report the decoder")
    run.add_argument("--url", required=True)
    run.add_argument("--warmup-seconds", type=float, default=20.0)
    run.add_argument("--measure-seconds", type=float, default=300.0)
    run.add_argument("--playback-timeout", type=float, default=60.0)
    run.add_argument("--min-start-frames", type=int, default=10)
    run.add_argument("--out")
    run.set_defaults(func=command_run)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ProtocolError as error:
        print(f"FAIL cdp: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
