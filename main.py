import base64
import json
import os
import uuid
from datetime import datetime, timezone

import requests
from flask import Flask, jsonify, request
from openai import OpenAI

app = Flask(__name__)

# =========================
# CONFIG 
# =========================

OPENAI_API_KEY = os.environ.get("OPENAI_API_KEY")
if not OPENAI_API_KEY:
    raise RuntimeError("OPENAI_API_KEY is required")

ROUTER_MODEL = os.environ.get("ROUTER_MODEL", "gpt-4o-mini")
VISION_MODEL = os.environ.get("VISION_MODEL", "gpt-4o-mini")
TRANSCRIBE_MODEL = os.environ.get("TRANSCRIBE_MODEL", "whisper-1")
TRANSCRIBE_LANGUAGE = os.environ.get("TRANSCRIBE_LANGUAGE", "es")

# TTS
ENABLE_TTS = os.environ.get("ENABLE_TTS", "true").lower() in ("1", "true", "yes", "y")
TTS_MODEL = os.environ.get("TTS_MODEL", "gpt-4o-mini-tts")
TTS_VOICE = os.environ.get("TTS_VOICE", "shimmer")
TTS_FORMAT = os.environ.get("TTS_FORMAT", "mp3")

# Tailscale local API
TAIL_URL = os.environ.get("TAIL_URL")
TAIL_TOKEN = os.environ.get("TAIL_TOKEN")
TAIL_MODEL = os.environ.get("TAIL_MODEL", "clawdbot:livi")
TAIL_VERIFY_TLS = os.environ.get("TAIL_VERIFY_TLS", "false").lower() in ("1", "true", "yes", "y")
DEBUG_RETURN_TAIL_RAW = os.environ.get("DEBUG_RETURN_TAIL_RAW", "false").lower() in ("1", "true", "yes", "y")

if not TAIL_URL:
    raise RuntimeError("TAIL_URL is required")

if not TAIL_TOKEN:
    raise RuntimeError("TAIL_TOKEN is required")

# Limits
MAX_AUDIO_BYTES = int(os.environ.get("MAX_AUDIO_BYTES", str(10 * 1024 * 1024)))
MAX_IMAGE_BYTES = 8 * 1024 * 1024
app.config["MAX_CONTENT_LENGTH"] = max(MAX_IMAGE_BYTES * 2, MAX_AUDIO_BYTES * 2)

client = OpenAI(api_key=OPENAI_API_KEY)

# =========================
# STATE
# =========================

STATE_DIR = os.environ.get("LIVI_STATE_DIR", "/tmp/livi_state")
os.makedirs(STATE_DIR, exist_ok=True)

IMAGE_DIR = os.environ.get("LIVI_IMAGE_DIR", os.path.join(os.getcwd(), "livi_uploads"))
os.makedirs(IMAGE_DIR, exist_ok=True)


def now_iso():
    return datetime.now(timezone.utc).isoformat()


def default_state():
    return {
        "need_image": False,
        "request_id": "",
        "prompt": "",
        "image_request": "",
        "last_image_path": "",
        "last_image_mime": "",
        "last_image_size": 0,
        "last_response": "",
        "last_audio_base64": "",
        "last_audio_id": "",
        "last_audio_format": "",
        "last_audio_mime": "",
        "last_audio_error": "",
        "updated_at": None,
    }


def state_path(device_id: str) -> str:
    safe = "".join(c for c in device_id if c.isalnum() or c in ("-", "_"))
    if not safe:
        safe = "default"
    return os.path.join(STATE_DIR, f"{safe}.json")


def load_state(device_id: str):
    p = state_path(device_id)
    try:
        with open(p, "r", encoding="utf-8") as f:
            data = json.load(f)
        s = default_state()
        s.update(data or {})
        return s
    except Exception:
        return default_state()


def save_state(device_id: str, state: dict):
    p = state_path(device_id)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, ensure_ascii=False)
    os.replace(tmp, p)


# =========================
# Helpers
# =========================

def get_payload(req):
    payload = {}
    if req.is_json:
        payload.update(req.get_json(silent=True) or {})
    if req.form:
        payload.update(req.form.to_dict(flat=True))
    if req.args:
        payload.update(req.args.to_dict(flat=True))
    return payload


def get_device_id(req):
    p = get_payload(req)
    return p.get("device_id") or req.headers.get("x-device-id") or "default"


def safe_json_parse(text):
    try:
        return json.loads(text)
    except Exception:
        return None


def get_output_text(resp):
    ot = getattr(resp, "output_text", None)
    if isinstance(ot, str) and ot.strip():
        return ot.strip()

    texts = []
    try:
        for item in getattr(resp, "output", []) or []:
            for c in getattr(item, "content", []) or []:
                t = getattr(c, "text", None)
                if isinstance(t, str) and t.strip():
                    texts.append(t.strip())
    except Exception:
        pass
    return "\n".join(texts).strip()


# =========================
# Router
# =========================

def route_transcript(transcript: str):
    instructions = (
        "You are Livi, an AI assistant built into smart glasses. "
        "Decide if the user request needs an image to answer accurately. "
        "Return ONLY a JSON object with keys: needs_image, image_request, assistant_response. "
        "IMPORTANT: Output must be valid json."
    )

    resp = client.responses.create(
        model=ROUTER_MODEL,
        input=[
            {"role": "system", "content": instructions},
            {"role": "user", "content": transcript},
        ],
        text={"format": {"type": "json_object"}},
        temperature=0,
    )

    raw = get_output_text(resp)
    parsed = safe_json_parse(raw)
    if not parsed:
        raise ValueError("Invalid router response")

    return {
        "needs_image": bool(parsed.get("needs_image")),
        "image_request": (parsed.get("image_request") or "").strip(),
        "assistant_response": (parsed.get("assistant_response") or "").strip(),
    }


# =========================
# Tailscale Call
# =========================

def call_tail_responses(prompt_text: str):
    headers = {
        "Authorization": f"Bearer {TAIL_TOKEN}",
        "Content-Type": "application/json",
    }

    payload = {
        "model": TAIL_MODEL,
        "input": [
            {
                "type": "message",
                "role": "system",
                "content": "Eres LIVI. Responde concisa.",
            },
            {"type": "message", "role": "user", "content": prompt_text},
        ],
    }

    r = requests.post(
        TAIL_URL,
        headers=headers,
        json=payload,
        timeout=25,
        verify=TAIL_VERIFY_TLS,
    )

    return r.text.strip()


# =========================
# Routes
# =========================

@app.get("/livi/health")
def health():
    return jsonify({"ok": True, "time": now_iso()})


@app.post("/livi/transcript")
def transcript():
    p = get_payload(request)
    transcript_text = (p.get("transcript") or "").strip()
    if not transcript_text:
        return jsonify({"error": "Missing transcript"}), 400

    try:
        decision = route_transcript(transcript_text)
        return jsonify(decision)
    except Exception as e:
        return jsonify({"error": str(e)}), 500


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "3000"))
    app.run(host="0.0.0.0", port=port)
