from __future__ import annotations

import os
import time
import uuid
from functools import lru_cache

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware

from .config import load_config
from .inference import InferenceEngine
from .modality_decrees import decree_report
from .safety import SafetyError
from .schemas import ChatCompletionChoice, ChatCompletionRequest, ChatCompletionResponse, ChatMessage


@lru_cache(maxsize=1)
def get_engine() -> InferenceEngine:
    return InferenceEngine(load_config(os.getenv("MMLLM_CONFIG")))


cfg = load_config(os.getenv("MMLLM_CONFIG"))
app = FastAPI(title="MM-LLM Decree", version="0.1.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=list(cfg.serving.allowed_origins),
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


def _messages_to_prompt(messages: list[ChatMessage]) -> tuple[str, int, int]:
    image_count = 0
    audio_count = 0
    lines: list[str] = []
    for msg in messages:
        if isinstance(msg.content, str):
            lines.append(f"<{msg.role}> {msg.content}")
            continue
        parts: list[str] = []
        for part in msg.content:
            if part.type == "text" and part.text:
                parts.append(part.text)
            elif part.type == "image_url":
                image_count += 1
                parts.append("[image]")
            elif part.type == "audio_url":
                audio_count += 1
                parts.append("[audio]")
        lines.append(f"<{msg.role}> " + " ".join(parts))
    lines.append("<assistant>")
    return "\n".join(lines), image_count, audio_count


@app.get("/health")
def health() -> dict[str, object]:
    return {"ok": True, "model": cfg.serving.model_name, "time": int(time.time())}


@app.get("/v1/modal_decrees")
def modal_decrees() -> dict[str, object]:
    return {"modal_decrees": decree_report()}


@app.post("/v1/chat/completions", response_model=ChatCompletionResponse)
def chat_completions(req: ChatCompletionRequest) -> ChatCompletionResponse:
    if req.stream:
        raise HTTPException(status_code=400, detail="streaming is not implemented in this scaffold")
    prompt, image_count, audio_count = _messages_to_prompt(req.messages)
    try:
        # This tiny local model accepts precomputed image/audio features. URLs are counted and
        # represented in prompt text; wire real encoders via adapters/hf_multimodal.py.
        engine = get_engine()
        result = engine.generate(prompt=prompt, max_tokens=req.max_tokens)
    except SafetyError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    usage = {
        "prompt_tokens": result.prompt_tokens,
        "completion_tokens": result.completion_tokens,
        "total_tokens": result.prompt_tokens + result.completion_tokens,
        "image_count": image_count,
        "audio_count": audio_count,
    }
    return ChatCompletionResponse(
        id=f"chatcmpl-{uuid.uuid4().hex[:24]}",
        model=req.model,
        choices=[
            ChatCompletionChoice(
                index=0,
                message=ChatMessage(role="assistant", content=result.text or "[empty generation]"),
                finish_reason="stop",
            )
        ],
        usage=usage,
    )
