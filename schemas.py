from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field


class ContentPart(BaseModel):
    type: Literal["text", "image_url", "audio_url"]
    text: str | None = None
    image_url: dict[str, str] | None = None
    audio_url: dict[str, str] | None = None


class ChatMessage(BaseModel):
    role: Literal["system", "user", "assistant", "tool"]
    content: str | list[ContentPart]


class ChatCompletionRequest(BaseModel):
    model: str = "mmllm-tiny"
    messages: list[ChatMessage] = Field(min_length=1)
    max_tokens: int = Field(default=128, ge=1)
    temperature: float = Field(default=0.0, ge=0.0, le=2.0)
    stream: bool = False


class ChatCompletionChoice(BaseModel):
    index: int
    message: ChatMessage
    finish_reason: str = "stop"


class ChatCompletionResponse(BaseModel):
    id: str
    object: str = "chat.completion"
    model: str
    choices: list[ChatCompletionChoice]
    usage: dict[str, int]
