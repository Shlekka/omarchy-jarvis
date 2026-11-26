#!/usr/bin/env python3
"""Persistent offline British TTS worker for J.A.R.V.I.S.

Uses Kokoro-82M (via kokoro-onnx) by default with a calm, neutral British RP
voice blend (bm_george + bm_daniel), falling back automatically to Piper
(en_GB-alan-medium) if Kokoro is unavailable or JARVIS_TTS_ENGINE=piper is set.

Accepts JSON lines on stdin, synthesizes WAV files with optional subtle sci-fi
UI chimes, and returns the WAV path plus a 30 fps normalized RMS amplitude
envelope for hologram waveform synchronization.
"""

from __future__ import annotations

import json
import math
import os
import re
import sys
import tempfile
import wave
from pathlib import Path
from typing import Callable

import numpy as np

DATA_DIR = Path.home() / ".local" / "share" / "jarvis"
CONFIG_DIR = Path.home() / ".config" / "jarvis"
VOICES_DIR = DATA_DIR / "voices"
KOKORO_MODEL_PATH = VOICES_DIR / "kokoro-v1.0.onnx"
KOKORO_VOICES_PATH = VOICES_DIR / "voices-v1.0.bin"
PIPER_VOICE_PATH = VOICES_DIR / "en_GB-alan-medium.onnx"
CUSTOM_SOUNDS_DIR = DATA_DIR / "sounds"
OUT_DIR = Path(tempfile.gettempdir()) / f"jarvis-audio-{os.getuid()}"
OUT_DIR.mkdir(parents=True, exist_ok=True)

DEFAULT_KOKORO_VOICE = "bm_george:0.65,bm_daniel:0.35"
DEFAULT_KOKORO_SPEED = 1.0


def load_voice_prefs() -> tuple[str, str, float]:
    """Load optional voice overrides from ~/.config/jarvis/voice.json and env."""
    engine = "auto"
    voice_spec = DEFAULT_KOKORO_VOICE
    speed = DEFAULT_KOKORO_SPEED

    cfg_path = CONFIG_DIR / "voice.json"
    if cfg_path.is_file():
        try:
            cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
            if isinstance(cfg, dict):
                engine = str(cfg.get("engine", engine)).strip().lower() or engine
                voice_spec = str(cfg.get("voice", voice_spec)).strip() or voice_spec
                if "speed" in cfg:
                    speed = float(cfg["speed"])
        except Exception:
            pass

    engine = os.environ.get("JARVIS_TTS_ENGINE", engine).strip().lower() or engine
    voice_spec = os.environ.get("JARVIS_VOICE", voice_spec).strip() or voice_spec
    if "JARVIS_VOICE_SPEED" in os.environ:
        try:
            speed = float(os.environ["JARVIS_VOICE_SPEED"])
        except ValueError:
            pass

    speed = max(0.5, min(1.8, speed))
    return engine, voice_spec, speed


def clean_for_speech(text: str) -> str:
    """Strip markdown and code formatting so spoken output sounds natural."""
    # Replace fenced code blocks with a polite spoken summary.
    text = re.sub(r"```[\s\S]*?```", " I have prepared the code block on screen. ", text)
    text = re.sub(r"`([^`]+)`", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\([^\)]+\)", r"\1", text)
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"\bJ\.A\.R\.V\.I\.S\.?", "Jarvis", text)
    text = re.sub(r"[#*_~>|]", " ", text)
    text = re.sub(r"\s+", " ", text).strip()
    # Keep spoken turns crisp and avoid reading giant walls of text aloud.
    if len(text) > 520:
        cutoff = text.rfind(".", 0, 520)
        if cutoff > 180:
            text = text[: cutoff + 1]
        else:
            text = text[:520].rstrip() + "."
    return text


def synth_chime(kind: str, sample_rate: int) -> np.ndarray | None:
    """Generate a subtle harmonic holographic UI chime (or load custom WAV)."""
    if not kind or kind == "none":
        return None

    custom_path = CUSTOM_SOUNDS_DIR / f"{kind}.wav"
    if custom_path.is_file():
        try:
            with wave.open(str(custom_path), "rb") as wf:
                frames = wf.readframes(wf.getnframes())
                arr = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
                if wf.getnchannels() > 1:
                    arr = arr.reshape(-1, wf.getnchannels()).mean(axis=1)
                if wf.getframerate() != sample_rate and len(arr) > 0:
                    x_old = np.linspace(0, 1, len(arr))
                    new_len = max(1, int(len(arr) * sample_rate / wf.getframerate()))
                    x_new = np.linspace(0, 1, new_len)
                    arr = np.interp(x_new, x_old, arr).astype(np.float32)
                return arr
        except Exception:
            pass

    def tone(freq: float, dur: float, amp: float = 0.14, delay: float = 0.0) -> tuple[float, np.ndarray]:
        n = int(sample_rate * dur)
        t = np.linspace(0.0, dur, n, endpoint=False, dtype=np.float32)
        env = np.minimum(1.0, t / 0.012) * np.exp(-t * (5.5 / max(dur, 0.05)))
        sig = (
            np.sin(2.0 * math.pi * freq * t)
            + 0.25 * np.sin(2.0 * math.pi * (freq * 2.0) * t)
        ) * env * amp
        return delay, sig.astype(np.float32)

    specs: dict[str, list[tuple[float, float, float, float]]] = {
        "startup": [(523.25, 0.24, 0.12, 0.0), (659.25, 0.26, 0.12, 0.08), (1046.5, 0.36, 0.10, 0.16)],
        "listen": [(698.46, 0.14, 0.11, 0.0), (880.0, 0.18, 0.10, 0.06)],
        "work": [(587.33, 0.10, 0.08, 0.0), (783.99, 0.12, 0.07, 0.05)],
        "done": [(659.25, 0.18, 0.11, 0.0), (987.77, 0.28, 0.11, 0.08)],
        "alert": [(440.0, 0.16, 0.14, 0.0), (349.23, 0.22, 0.14, 0.10)],
    }
    notes = specs.get(kind)
    if not notes:
        return None

    total_sec = max(d + dur for _, dur, _, d in notes) + 0.04
    buf = np.zeros(int(sample_rate * total_sec), dtype=np.float32)
    for freq, dur, amp, delay in notes:
        d, sig = tone(freq, dur, amp, delay)
        start = int(sample_rate * d)
        end = min(len(buf), start + len(sig))
        buf[start:end] += sig[: end - start]
    return np.clip(buf, -0.95, 0.95)


def compute_envelope(samples: np.ndarray, sample_rate: int, fps: int = 30) -> list[float]:
    if len(samples) == 0:
        return []
    hop = max(1, sample_rate // fps)
    env: list[float] = []
    for i in range(0, len(samples), hop):
        chunk = samples[i : i + hop]
        rms = float(np.sqrt(np.mean(chunk * chunk)))
        # Boost gently for lively holographic ring response.
        level = min(1.0, math.pow(rms * 3.8, 0.72))
        env.append(round(level, 3))
    return env


def write_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    pcm = np.clip(samples * 32767.0, -32768.0, 32767.0).astype(np.int16)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm.tobytes())


def resolve_kokoro_voice(kokoro, voice_spec: str):
    """Resolve a single Kokoro voice name or weighted blend ('bm_george:0.65,bm_daniel:0.35')."""
    available = set(kokoro.get_voices())
    parts = [p.strip() for p in voice_spec.split(",") if p.strip()]
    weighted: list[tuple[str, float]] = []
    for part in parts:
        if ":" in part:
            name, raw_w = part.split(":", 1)
            name = name.strip()
            try:
                w = float(raw_w.strip())
            except ValueError:
                w = 1.0
        else:
            name, w = part, 1.0
        if name in available and w > 0:
            weighted.append((name, w))

    if not weighted:
        fallback = "bm_george" if "bm_george" in available else next(iter(sorted(available)))
        return fallback, fallback

    if len(weighted) == 1:
        name = weighted[0][0]
        return name, name

    total_w = sum(w for _, w in weighted)
    blended = None
    label_parts: list[str] = []
    for name, w in weighted:
        ratio = w / total_w
        vec = kokoro.get_voice_style(name) * ratio
        blended = vec if blended is None else (blended + vec)
        label_parts.append(f"{name}:{ratio:.2f}")
    return blended, "+".join(label_parts)


def load_tts_engine() -> tuple[Callable[[str], np.ndarray], int, str]:
    engine_pref, voice_spec, speed = load_voice_prefs()

    if engine_pref in ("auto", "kokoro") and KOKORO_MODEL_PATH.is_file() and KOKORO_VOICES_PATH.is_file():
        try:
            from kokoro_onnx import Kokoro

            kokoro = Kokoro(str(KOKORO_MODEL_PATH), str(KOKORO_VOICES_PATH))
            voice_vec, voice_label = resolve_kokoro_voice(kokoro, voice_spec)

            def synth_kokoro(text: str) -> np.ndarray:
                samples, _ = kokoro.create(text, voice=voice_vec, speed=speed, lang="en-gb")
                return np.asarray(samples, dtype=np.float32)

            return synth_kokoro, 24000, f"kokoro:{voice_label}"
        except Exception:
            if engine_pref == "kokoro":
                raise

    if not PIPER_VOICE_PATH.is_file():
        raise FileNotFoundError(f"No TTS model found in {VOICES_DIR}")

    from piper import PiperVoice, SynthesisConfig

    voice = PiperVoice.load(str(PIPER_VOICE_PATH))
    sample_rate = voice.config.sample_rate
    syn_cfg = SynthesisConfig(length_scale=0.96, noise_scale=0.52, noise_w_scale=0.72)

    def synth_piper(text: str) -> np.ndarray:
        chunks: list[np.ndarray] = []
        for chunk in voice.synthesize(text, syn_config=syn_cfg):
            arr = np.asarray(chunk.audio_float_array, dtype=np.float32)
            if len(arr) > 0:
                chunks.append(arr)
                chunks.append(np.zeros(int(sample_rate * 0.04), dtype=np.float32))
        return np.concatenate(chunks) if chunks else np.zeros(0, dtype=np.float32)

    return synth_piper, sample_rate, f"piper:{PIPER_VOICE_PATH.stem}"


def main() -> int:
    try:
        synthesize_text, sample_rate, voice_name = load_tts_engine()
    except Exception as exc:
        print(json.dumps({"event": "error", "error": str(exc)}), flush=True)
        return 1

    print(json.dumps({"event": "ready", "sampleRate": sample_rate, "voice": voice_name}), flush=True)

    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            req = json.loads(raw)
        except json.JSONDecodeError:
            continue

        req_id = req.get("id", 0)
        text = clean_for_speech(str(req.get("text", "")))
        chime_kind = str(req.get("chime", "none"))

        parts: list[np.ndarray] = []
        chime_audio = synth_chime(chime_kind, sample_rate)
        if chime_audio is not None:
            parts.append(chime_audio)
            if text:
                parts.append(np.zeros(int(sample_rate * 0.06), dtype=np.float32))

        if text:
            try:
                spoken = synthesize_text(text)
                if len(spoken) > 0:
                    parts.append(spoken)
            except Exception as exc:
                print(json.dumps({"event": "error", "id": req_id, "error": str(exc)}), flush=True)
                continue

        if not parts:
            print(json.dumps({"event": "empty", "id": req_id}), flush=True)
            continue

        audio = np.concatenate(parts)
        wav_path = OUT_DIR / f"utt_{req_id}.wav"
        write_wav(wav_path, audio, sample_rate)
        envelope = compute_envelope(audio, sample_rate, fps=30)
        duration_ms = int(len(audio) * 1000 / sample_rate)

        print(
            json.dumps(
                {
                    "event": "synthesized",
                    "id": req_id,
                    "wav": str(wav_path),
                    "durationMs": duration_ms,
                    "envelope": envelope,
                }
            ),
            flush=True,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
