#!/usr/bin/env python3
"""Hands-free PipeWire microphone VAD + Superwhisper transcription worker.

Captures 16 kHz mono PCM from the default PipeWire audio source (`pw-record`),
streams live normalized audio levels for the J.A.R.V.I.S. hologram while speech
is detected, segments utterances automatically on natural pauses, transcribes
them via `superwhisper transcribe`, and emits JSON events on stdout.
"""

from __future__ import annotations

import collections
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 16000
FRAME_SAMPLES = 512  # 32 ms per frame (~31 fps)
FRAME_BYTES = FRAME_SAMPLES * 2
PRE_ROLL_FRAMES = 14  # ~450 ms before trigger so initial syllables aren't clipped
START_TRIGGER_FRAMES = 3
SILENCE_END_FRAMES = 30  # ~0.95 s of silence ends utterance
MIN_SPEECH_FRAMES = 8    # ~256 ms minimum utterance
MAX_SPEECH_FRAMES = 560  # ~18 s safety cap

OUT_DIR = Path(tempfile.gettempdir()) / f"jarvis-mic-{os.getuid()}"
OUT_DIR.mkdir(parents=True, exist_ok=True)


def emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def write_wav(path: Path, raw_pcm: bytes) -> None:
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(raw_pcm)


def transcribe_with_superwhisper(wav_path: Path) -> str:
    sw = shutil.which("superwhisper")
    if not sw:
        emit({"event": "error", "error": "superwhisper CLI not found"})
        return ""

    try:
        submit = subprocess.run(
            [sw, "transcribe", str(wav_path)],
            capture_output=True,
            text=True,
            timeout=5,
        )
    except Exception as exc:
        emit({"event": "error", "error": f"superwhisper submit failed: {exc}"})
        return ""

    if submit.returncode != 0:
        emit({"event": "error", "error": submit.stderr.strip() or "superwhisper transcribe failed"})
        return ""

    try:
        submit_data = json.loads(submit.stdout)
        sid = submit_data.get("session_id") if isinstance(submit_data, dict) else None
    except json.JSONDecodeError:
        sid = None

    deadline = time.monotonic() + 12.0
    while time.monotonic() < deadline:
        time.sleep(0.08)
        try:
            res = subprocess.run([sw, "read"], capture_output=True, text=True, timeout=3)
            if res.returncode != 0:
                continue
            data = json.loads(res.stdout)
        except Exception:
            continue

        # Superwhisper returns JSON `null` while the active session is still processing.
        if not isinstance(data, dict):
            continue

        if sid is not None and data.get("session_id") != sid:
            continue

        if data.get("error"):
            emit({"event": "error", "error": str(data.get("error"))})
            return ""

        text = str(data.get("text") or "").strip()
        return text

    emit({"event": "error", "error": "Transcription timed out"})
    return ""


def start_recorder() -> subprocess.Popen | None:
    pw = shutil.which("pw-record")
    if pw:
        return subprocess.Popen(
            [pw, "--raw", "--format=s16", f"--rate={SAMPLE_RATE}", "--channels=1", "-"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    pa = shutil.which("parecord")
    if pa:
        return subprocess.Popen(
            [pa, "--format=s16le", f"--rate={SAMPLE_RATE}", "--channels=1", "--raw"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    return None


def main() -> int:
    paused = False
    resume_at = 0.0
    stop_flag = False
    state_lock = threading.Lock()

    def stdin_loop() -> None:
        nonlocal paused, resume_at, stop_flag
        for raw in sys.stdin:
            raw = raw.strip()
            if not raw:
                continue
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            cmd = msg.get("cmd")
            with state_lock:
                if cmd == "pause":
                    paused = True
                elif cmd == "resume":
                    paused = False
                    resume_at = time.monotonic() + 0.30
                elif cmd == "quit":
                    stop_flag = True
                    return

    threading.Thread(target=stdin_loop, daemon=True).start()

    rec = start_recorder()
    if rec is None or rec.stdout is None:
        emit({"event": "error", "error": "No PipeWire/PulseAudio recorder found (pw-record / parecord)"})
        return 1

    emit({"event": "ready"})

    pre_roll: collections.deque[bytes] = collections.deque(maxlen=PRE_ROLL_FRAMES)
    speech_frames: list[bytes] = []
    in_speech = False
    above_count = 0
    silence_count = 0
    noise_floor = 0.010
    utt_id = 0

    try:
        while True:
            with state_lock:
                if stop_flag:
                    break
                is_paused = paused or (time.monotonic() < resume_at)

            chunk = rec.stdout.read(FRAME_BYTES)
            if not chunk or len(chunk) < FRAME_BYTES:
                break

            if is_paused:
                if in_speech:
                    in_speech = False
                    speech_frames.clear()
                    above_count = 0
                    silence_count = 0
                    emit({"event": "listening", "active": False})
                pre_roll.clear()
                continue

            samples = np.frombuffer(chunk, dtype=np.int16).astype(np.float32) / 32768.0
            rms = float(np.sqrt(np.mean(samples * samples)))
            threshold = max(0.018, min(0.09, noise_floor * 2.8))

            if not in_speech:
                pre_roll.append(chunk)
                if rms > threshold:
                    above_count += 1
                    if above_count >= START_TRIGGER_FRAMES:
                        in_speech = True
                        silence_count = 0
                        speech_frames = list(pre_roll)
                        emit({"event": "listening", "active": True})
                else:
                    above_count = 0
                    # Adapt noise floor slowly during silence.
                    noise_floor = noise_floor * 0.96 + min(rms, 0.04) * 0.04
            else:
                speech_frames.append(chunk)
                level = min(1.0, math.pow(rms * 5.2, 0.72))
                emit({"event": "level", "level": round(level, 3)})

                if rms > threshold * 0.78:
                    silence_count = 0
                else:
                    silence_count += 1

                if silence_count >= SILENCE_END_FRAMES or len(speech_frames) >= MAX_SPEECH_FRAMES:
                    in_speech = False
                    above_count = 0
                    emit({"event": "listening", "active": False})

                    total_speech = len(speech_frames) - silence_count
                    if total_speech >= MIN_SPEECH_FRAMES:
                        utt_id += 1
                        wav_path = OUT_DIR / f"mic_{utt_id}.wav"
                        pcm_data = b"".join(speech_frames)
                        speech_frames = []
                        pre_roll.clear()
                        try:
                            write_wav(wav_path, pcm_data)
                            emit({"event": "transcribing"})
                            text = transcribe_with_superwhisper(wav_path)
                            if text:
                                emit({"event": "transcript", "text": text})
                            else:
                                emit({"event": "empty"})
                        finally:
                            try:
                                wav_path.unlink(missing_ok=True)
                            except OSError:
                                pass
                    else:
                        speech_frames = []
                        pre_roll.clear()
    finally:
        if rec.poll() is None:
            rec.terminate()
            try:
                rec.wait(timeout=1)
            except subprocess.TimeoutExpired:
                rec.kill()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
