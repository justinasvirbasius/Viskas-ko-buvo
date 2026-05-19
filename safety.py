from __future__ import annotations

from dataclasses import dataclass

import torch

from .modality_decrees import DEFAULT_MODAL_DECREES


@dataclass(frozen=True)
class SafetyLimits:
    max_prompt_chars: int = 16_000
    max_new_tokens: int = 256
    max_images: int = DEFAULT_MODAL_DECREES["image"].max_items
    max_audio_clips: int = DEFAULT_MODAL_DECREES["audio"].max_items


class SafetyError(ValueError):
    pass


def normalize_prompt(text: str) -> str:
    # Keep semantic content, remove NULL bytes and normalize CRLF.
    return text.replace("\x00", "").replace("\r\n", "\n").strip()


def validate_request_limits(
    prompt: str,
    max_tokens: int,
    image_count: int,
    audio_count: int,
    limits: SafetyLimits,
) -> str:
    prompt = normalize_prompt(prompt)
    if not prompt:
        raise SafetyError("prompt must not be empty")
    if len(prompt) > limits.max_prompt_chars:
        raise SafetyError(f"prompt too large: {len(prompt)} > {limits.max_prompt_chars}")
    if max_tokens < 1 or max_tokens > limits.max_new_tokens:
        raise SafetyError(f"max_tokens must be in [1, {limits.max_new_tokens}]")
    if image_count > limits.max_images:
        raise SafetyError(f"too many images: {image_count} > {limits.max_images}")
    if audio_count > limits.max_audio_clips:
        raise SafetyError(f"too many audio clips: {audio_count} > {limits.max_audio_clips}")
    return prompt


def assert_finite_tensor(name: str, tensor: torch.Tensor) -> None:
    if not torch.isfinite(tensor).all():
        raise SafetyError(f"non-finite tensor detected: {name}")


def assert_feature_shape(name: str, features: torch.Tensor | None, expected_dim: int, max_tokens: int) -> None:
    if features is None:
        return
    if features.ndim != 3:
        raise SafetyError(f"{name} must be [batch, tokens, dim]")
    if features.shape[-1] != expected_dim:
        raise SafetyError(f"{name} feature dim {features.shape[-1]} != {expected_dim}")
    if features.shape[1] > max_tokens:
        raise SafetyError(f"{name} token count {features.shape[1]} exceeds {max_tokens}")
    assert_finite_tensor(name, features)
