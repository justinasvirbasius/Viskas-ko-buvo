from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml


@dataclass(frozen=True)
class ModelConfig:
    vocab_size: int = 259
    d_model: int = 256
    n_layers: int = 4
    n_heads: int = 8
    n_kv_heads: int = 4
    ffn_mult: int = 4
    max_seq_len: int = 1024
    dropout: float = 0.0
    rope_theta: float = 10_000.0
    image_feature_dim: int = 768
    audio_feature_dim: int = 128
    max_image_tokens: int = 64
    max_audio_tokens: int = 128
    modalities: tuple[str, ...] = ("text", "image", "audio")

    def __post_init__(self) -> None:
        if self.d_model % self.n_heads != 0:
            raise ValueError("d_model must be divisible by n_heads")
        if self.n_heads % self.n_kv_heads != 0:
            raise ValueError("n_heads must be divisible by n_kv_heads for grouped-query attention")
        if self.max_seq_len <= 0:
            raise ValueError("max_seq_len must be positive")
        if not (0.0 <= self.dropout < 1.0):
            raise ValueError("dropout must be in [0, 1)")


@dataclass(frozen=True)
class ServingConfig:
    model_name: str = "mmllm-tiny"
    max_prompt_chars: int = 16_000
    max_new_tokens: int = 256
    temperature: float = 0.0
    require_api_key: bool = False
    allowed_origins: tuple[str, ...] = field(default_factory=lambda: ("*",))


@dataclass(frozen=True)
class AppConfig:
    model: ModelConfig = field(default_factory=ModelConfig)
    serving: ServingConfig = field(default_factory=ServingConfig)


def _as_tuple(value: Any) -> Any:
    if isinstance(value, list):
        return tuple(value)
    return value


def load_config(path: str | Path | None) -> AppConfig:
    if path is None:
        return AppConfig()
    data = yaml.safe_load(Path(path).read_text()) or {}
    model_raw = {k: _as_tuple(v) for k, v in (data.get("model") or {}).items()}
    serving_raw = {k: _as_tuple(v) for k, v in (data.get("serving") or {}).items()}
    return AppConfig(model=ModelConfig(**model_raw), serving=ServingConfig(**serving_raw))
