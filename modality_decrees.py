from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ModalDecree:
    name: str
    enabled: bool
    max_items: int
    max_tokens: int
    feature_dim: int | None
    mime_prefixes: tuple[str, ...]
    required_guards: tuple[str, ...]


DEFAULT_MODAL_DECREES: dict[str, ModalDecree] = {
    "text": ModalDecree(
        name="text",
        enabled=True,
        max_items=1,
        max_tokens=4096,
        feature_dim=None,
        mime_prefixes=("text/",),
        required_guards=("max_chars", "prompt_normalization", "schema_validation"),
    ),
    "image": ModalDecree(
        name="image",
        enabled=True,
        max_items=4,
        max_tokens=64,
        feature_dim=768,
        mime_prefixes=("image/png", "image/jpeg", "image/webp"),
        required_guards=("mime_allowlist", "pixel_budget", "feature_shape", "malformed_media_reject"),
    ),
    "audio": ModalDecree(
        name="audio",
        enabled=True,
        max_items=2,
        max_tokens=128,
        feature_dim=128,
        mime_prefixes=("audio/wav", "audio/mpeg", "audio/flac"),
        required_guards=("mime_allowlist", "duration_budget", "feature_shape", "malformed_media_reject"),
    ),
}


def get_decree(name: str) -> ModalDecree:
    try:
        return DEFAULT_MODAL_DECREES[name]
    except KeyError as exc:
        raise ValueError(f"Unsupported modality: {name}") from exc


def decree_report() -> dict[str, dict[str, object]]:
    return {name: decree.__dict__ for name, decree in DEFAULT_MODAL_DECREES.items()}
