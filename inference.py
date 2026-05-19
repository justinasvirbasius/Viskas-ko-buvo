from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

import torch

from .config import AppConfig
from .model import MultiModalCausalLM
from .safety import SafetyLimits, assert_feature_shape, validate_request_limits
from .tokenizer import ByteTokenizer


@dataclass
class GenerationResult:
    text: str
    prompt_tokens: int
    completion_tokens: int


class InferenceEngine:
    """Owns model, tokenizer, request limits, and generation policy."""

    def __init__(self, cfg: AppConfig) -> None:
        self.cfg = cfg
        self.device = torch.device(os.getenv("MMLLM_DEVICE", "cpu"))
        self.tokenizer = ByteTokenizer()
        self.model = MultiModalCausalLM(cfg.model).to(self.device)
        ckpt = os.getenv("MMLLM_CHECKPOINT")
        if ckpt:
            state = torch.load(Path(ckpt), map_location=self.device)
            self.model.load_state_dict(state["model"] if "model" in state else state)
        if os.getenv("MMLLM_COMPILE", "0") == "1":
            self.model = torch.compile(self.model)  # type: ignore[assignment]
        self.limits = SafetyLimits(
            max_prompt_chars=cfg.serving.max_prompt_chars,
            max_new_tokens=cfg.serving.max_new_tokens,
        )

    def generate(
        self,
        prompt: str,
        max_tokens: int,
        image_features: torch.Tensor | None = None,
        audio_features: torch.Tensor | None = None,
    ) -> GenerationResult:
        prompt = validate_request_limits(
            prompt=prompt,
            max_tokens=max_tokens,
            image_count=1 if image_features is not None else 0,
            audio_count=1 if audio_features is not None else 0,
            limits=self.limits,
        )
        assert_feature_shape(
            "image_features",
            image_features,
            self.cfg.model.image_feature_dim,
            self.cfg.model.max_image_tokens,
        )
        assert_feature_shape(
            "audio_features",
            audio_features,
            self.cfg.model.audio_feature_dim,
            self.cfg.model.max_audio_tokens,
        )
        ids = self.tokenizer.encode(prompt, add_bos=True, add_eos=False)
        if len(ids) > self.cfg.model.max_seq_len:
            ids = ids[-self.cfg.model.max_seq_len :]
        input_ids = torch.tensor([ids], dtype=torch.long, device=self.device)
        if image_features is not None:
            image_features = image_features.to(self.device)
        if audio_features is not None:
            audio_features = audio_features.to(self.device)
        out = self.model.generate(
            input_ids,
            max_new_tokens=max_tokens,
            eos_token_id=self.tokenizer.eos_id,
            image_features=image_features,
            audio_features=audio_features,
        )
        new_ids = out[0, input_ids.shape[1] :].tolist()
        return GenerationResult(
            text=self.tokenizer.decode(new_ids),
            prompt_tokens=len(ids),
            completion_tokens=len(new_ids),
        )
