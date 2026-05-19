from __future__ import annotations

import torch
from torch import nn
from torch.nn import functional as F

from .config import ModelConfig
from .layers import RMSNorm, TransformerBlock


class FeatureProjector(nn.Module):
    """Projects precomputed modality features into the LM token space."""

    def __init__(self, in_dim: int, d_model: int) -> None:
        super().__init__()
        self.net = nn.Sequential(
            nn.LayerNorm(in_dim),
            nn.Linear(in_dim, d_model),
            nn.GELU(),
            nn.Linear(d_model, d_model),
        )

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        if features.ndim != 3:
            raise ValueError("features must have shape [batch, tokens, feature_dim]")
        return self.net(features)


class MultiModalCausalLM(nn.Module):
    """Decoder-only multimodal LM.

    Modal features are fused as prefix tokens before text tokens:

        [image-prefix tokens] [audio-prefix tokens] [text tokens]

    For real production systems, pair this with trained encoders and a trained checkpoint.
    """

    def __init__(self, cfg: ModelConfig) -> None:
        super().__init__()
        self.cfg = cfg
        self.token_emb = nn.Embedding(cfg.vocab_size, cfg.d_model)
        self.image_projector = FeatureProjector(cfg.image_feature_dim, cfg.d_model)
        self.audio_projector = FeatureProjector(cfg.audio_feature_dim, cfg.d_model)
        self.modality_type = nn.ParameterDict(
            {
                "image": nn.Parameter(torch.zeros(1, 1, cfg.d_model)),
                "audio": nn.Parameter(torch.zeros(1, 1, cfg.d_model)),
                "text": nn.Parameter(torch.zeros(1, 1, cfg.d_model)),
            }
        )
        self.blocks = nn.ModuleList(
            [
                TransformerBlock(
                    cfg.d_model,
                    cfg.n_heads,
                    cfg.n_kv_heads,
                    cfg.ffn_mult,
                    cfg.max_seq_len,
                    cfg.dropout,
                )
                for _ in range(cfg.n_layers)
            ]
        )
        self.norm = RMSNorm(cfg.d_model)
        self.lm_head = nn.Linear(cfg.d_model, cfg.vocab_size, bias=False)
        self.lm_head.weight = self.token_emb.weight
        self.apply(self._init_weights)

    def _init_weights(self, module: nn.Module) -> None:
        if isinstance(module, nn.Linear):
            nn.init.normal_(module.weight, mean=0.0, std=0.02)
            if module.bias is not None:
                nn.init.zeros_(module.bias)
        elif isinstance(module, nn.Embedding):
            nn.init.normal_(module.weight, mean=0.0, std=0.02)

    def fuse_embeddings(
        self,
        input_ids: torch.Tensor,
        image_features: torch.Tensor | None = None,
        audio_features: torch.Tensor | None = None,
    ) -> tuple[torch.Tensor, int]:
        pieces: list[torch.Tensor] = []
        prefix_len = 0
        batch = input_ids.shape[0]
        device = input_ids.device

        if image_features is not None:
            if image_features.shape[0] != batch:
                raise ValueError("image_features batch must match input_ids batch")
            if image_features.shape[1] > self.cfg.max_image_tokens:
                raise ValueError("too many image feature tokens")
            image_features = image_features.to(device=device)
            img = self.image_projector(image_features) + self.modality_type["image"].to(device)
            pieces.append(img)
            prefix_len += img.shape[1]

        if audio_features is not None:
            if audio_features.shape[0] != batch:
                raise ValueError("audio_features batch must match input_ids batch")
            if audio_features.shape[1] > self.cfg.max_audio_tokens:
                raise ValueError("too many audio feature tokens")
            audio_features = audio_features.to(device=device)
            aud = self.audio_projector(audio_features) + self.modality_type["audio"].to(device)
            pieces.append(aud)
            prefix_len += aud.shape[1]

        text = self.token_emb(input_ids) + self.modality_type["text"].to(device)
        pieces.append(text)
        x = torch.cat(pieces, dim=1)
        if x.shape[1] > self.cfg.max_seq_len:
            raise ValueError(f"sequence length {x.shape[1]} exceeds max_seq_len {self.cfg.max_seq_len}")
        return x, prefix_len

    def forward(
        self,
        input_ids: torch.Tensor,
        labels: torch.Tensor | None = None,
        image_features: torch.Tensor | None = None,
        audio_features: torch.Tensor | None = None,
    ) -> dict[str, torch.Tensor]:
        x, prefix_len = self.fuse_embeddings(input_ids, image_features, audio_features)
        for block in self.blocks:
            x = block(x)
        logits = self.lm_head(self.norm(x))
        out = {"logits": logits}

        if labels is not None:
            if prefix_len:
                ignore = torch.full((labels.shape[0], prefix_len), -100, dtype=labels.dtype, device=labels.device)
                labels = torch.cat([ignore, labels], dim=1)
            shift_logits = logits[:, :-1, :].contiguous()
            shift_labels = labels[:, 1:].contiguous()
            out["loss"] = F.cross_entropy(
                shift_logits.view(-1, shift_logits.size(-1)),
                shift_labels.view(-1),
                ignore_index=-100,
            )
        return out

    @torch.no_grad()
    def generate(
        self,
        input_ids: torch.Tensor,
        max_new_tokens: int,
        eos_token_id: int = 2,
        image_features: torch.Tensor | None = None,
        audio_features: torch.Tensor | None = None,
    ) -> torch.Tensor:
        self.eval()
        generated = input_ids
        for _ in range(max_new_tokens):
            context = generated[:, -self.cfg.max_seq_len :]
            out = self(context, image_features=image_features, audio_features=audio_features)
            next_token = out["logits"][:, -1, :].argmax(dim=-1, keepdim=True)
            generated = torch.cat([generated, next_token], dim=1)
            if torch.all(next_token == eos_token_id):
                break
            # Modal prefixes are consumed on first pass; afterward text context carries state.
            image_features = None
            audio_features = None
        return generated
