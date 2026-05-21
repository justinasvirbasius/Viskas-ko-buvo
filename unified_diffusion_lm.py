"""
Unified Diffusion Language Model
=================================
A single transformer trained with block-wise masked diffusion that supports
three decoding modes from the SAME weights:

  1. Autoregressive (AR)        -- block_size=1, deterministic left-to-right
  2. Parallel diffusion          -- denoise a whole block of [MASK] tokens in T steps
  3. Self-speculative decoding   -- diffusion drafts K tokens, AR pass verifies them

Inspired by: SEDD (Lou+'24), MDLM (Sahoo+'24), Block Diffusion (Arriola+'25),
and the self-speculative literature (Medusa, EAGLE, He+'23).

Run:   python unified_diffusion_lm.py
"""

from __future__ import annotations
import math
from dataclasses import dataclass
from typing import Optional, Tuple, List

import torch
import torch.nn as nn
import torch.nn.functional as F


# ---------------------------------------------------------------------------
# 1. Config
# ---------------------------------------------------------------------------
@dataclass
class ModelConfig:
    vocab_size: int = 256          # byte-level for the toy demo
    d_model: int   = 256
    n_layers: int  = 4
    n_heads: int   = 4
    d_ff: int      = 1024
    max_seq_len: int = 512
    dropout: float = 0.0
    mask_token_id: int = 255       # reserve last id as [MASK]
    pad_token_id: int  = 0


# ---------------------------------------------------------------------------
# 2. Transformer backbone with a *hybrid* attention mask
# ---------------------------------------------------------------------------
# Key trick that unifies the three modes:
#   - tokens within the *current* block see each other bidirectionally
#     (so we can denoise them in parallel)
#   - tokens see all *previous* blocks causally
#   - this single mask reduces to pure causal when block_size=1  -> AR
#     and to pure bidirectional when block_size=L -> full diffusion
# ---------------------------------------------------------------------------
def build_block_causal_mask(seq_len: int, block_size: int, device) -> torch.Tensor:
    """Returns a [seq_len, seq_len] additive mask (0 keep, -inf drop)."""
    idx = torch.arange(seq_len, device=device)
    block_id = idx // block_size
    # token i attends to j iff block(j) < block(i)  OR  block(j) == block(i)
    allowed = block_id.unsqueeze(0) <= block_id.unsqueeze(1)   # [L, L]
    mask = torch.zeros(seq_len, seq_len, device=device)
    mask.masked_fill_(~allowed, float('-inf'))
    return mask


class MultiHeadAttention(nn.Module):
    def __init__(self, cfg: ModelConfig):
        super().__init__()
        self.h = cfg.n_heads
        self.dk = cfg.d_model // cfg.n_heads
        self.qkv = nn.Linear(cfg.d_model, 3 * cfg.d_model, bias=False)
        self.proj = nn.Linear(cfg.d_model, cfg.d_model, bias=False)
        self.drop = nn.Dropout(cfg.dropout)

    def forward(self, x, attn_mask):
        B, L, D = x.shape
        q, k, v = self.qkv(x).chunk(3, dim=-1)
        q = q.view(B, L, self.h, self.dk).transpose(1, 2)   # B,h,L,dk
        k = k.view(B, L, self.h, self.dk).transpose(1, 2)
        v = v.view(B, L, self.h, self.dk).transpose(1, 2)

        att = (q @ k.transpose(-2, -1)) / math.sqrt(self.dk)   # B,h,L,L
        att = att + attn_mask                                  # broadcast
        att = F.softmax(att, dim=-1)
        att = self.drop(att)
        out = att @ v                                          # B,h,L,dk
        out = out.transpose(1, 2).contiguous().view(B, L, D)
        return self.proj(out)


class Block(nn.Module):
    def __init__(self, cfg: ModelConfig):
        super().__init__()
        self.ln1 = nn.LayerNorm(cfg.d_model)
        self.attn = MultiHeadAttention(cfg)
        self.ln2 = nn.LayerNorm(cfg.d_model)
        self.mlp = nn.Sequential(
            nn.Linear(cfg.d_model, cfg.d_ff),
            nn.GELU(),
            nn.Linear(cfg.d_ff, cfg.d_model),
            nn.Dropout(cfg.dropout),
        )

    def forward(self, x, mask):
        x = x + self.attn(self.ln1(x), mask)
        x = x + self.mlp(self.ln2(x))
        return x


class UnifiedDiffusionLM(nn.Module):
    """One transformer, three decoding modes."""

    def __init__(self, cfg: ModelConfig):
        super().__init__()
        self.cfg = cfg
        self.tok_emb = nn.Embedding(cfg.vocab_size, cfg.d_model)
        self.pos_emb = nn.Embedding(cfg.max_seq_len, cfg.d_model)
        # time embedding (only used at diffusion training/inference)
        self.time_emb = nn.Sequential(
            nn.Linear(1, cfg.d_model),
            nn.GELU(),
            nn.Linear(cfg.d_model, cfg.d_model),
        )
        self.blocks = nn.ModuleList([Block(cfg) for _ in range(cfg.n_layers)])
        self.ln_f = nn.LayerNorm(cfg.d_model)
        self.head = nn.Linear(cfg.d_model, cfg.vocab_size, bias=False)
        # weight tying
        self.head.weight = self.tok_emb.weight

    def forward(
        self,
        idx: torch.Tensor,                  # [B, L]
        block_size: int = 1,                # 1 => AR; L => full diffusion
        t: Optional[torch.Tensor] = None,   # [B] in (0,1], noise level
    ) -> torch.Tensor:
        B, L = idx.shape
        device = idx.device
        pos = torch.arange(L, device=device)
        x = self.tok_emb(idx) + self.pos_emb(pos)[None]

        if t is not None:
            te = self.time_emb(t.view(B, 1, 1))    # [B,1,D]
            x = x + te

        mask = build_block_causal_mask(L, block_size, device)
        mask = mask[None, None]                    # broadcast over B,heads

        for blk in self.blocks:
            x = blk(x, mask)
        x = self.ln_f(x)
        return self.head(x)                        # [B, L, V]


# ---------------------------------------------------------------------------
# 3. Training loss: a single objective that subsumes AR + masked diffusion
# ---------------------------------------------------------------------------
#  - With probability p_ar   : block_size = 1, mask=all-future (i.e. AR CE)
#  - Otherwise               : pick random block_size, mask within-block tokens
#                              with rate t ~ U(0,1] (MDLM-style absorbing diff.)
# Both branches reduce to predicting the original token from its context, so
# the network is dual-trained for free.
# ---------------------------------------------------------------------------
def unified_loss(
    model: UnifiedDiffusionLM,
    batch: torch.Tensor,            # [B, L]
    p_ar: float = 0.5,
) -> torch.Tensor:
    cfg = model.cfg
    B, L = batch.shape
    device = batch.device

    if torch.rand(()) < p_ar:
        # --- AR mode -----------------------------------------------------
        logits = model(batch[:, :-1], block_size=1, t=None)
        targets = batch[:, 1:]
        return F.cross_entropy(
            logits.reshape(-1, cfg.vocab_size),
            targets.reshape(-1),
            ignore_index=cfg.pad_token_id,
        )

    # --- Masked-diffusion mode (within-block bidirectional) -------------
    # choose block size that divides L cleanly for simplicity
    candidates = [b for b in (4, 8, 16, 32) if L % b == 0 and b <= L]
    block_size = candidates[torch.randint(len(candidates), (1,)).item()]

    # per-sample noise level in (eps, 1]; MDLM absorbing schedule
    t = torch.rand(B, device=device) * 0.99 + 0.01
    mask_prob = t                                          # linear schedule
    rand = torch.rand(B, L, device=device)
    mask = rand < mask_prob[:, None]                       # which positions get [MASK]

    # IMPORTANT: only positions inside the *current* block are corrupted in
    # the strict block-diffusion formulation. For simplicity we mask anywhere
    # since the block-causal attention already controls information flow.
    corrupted = torch.where(mask, torch.full_like(batch, cfg.mask_token_id), batch)

    logits = model(corrupted, block_size=block_size, t=t)
    # loss only on masked positions, reweighted by 1/t (standard MDLM ELBO)
    loss_tok = F.cross_entropy(
        logits.reshape(-1, cfg.vocab_size),
        batch.reshape(-1),
        reduction='none',
    ).view(B, L)
    weight = 1.0 / mask_prob[:, None]
    loss = (loss_tok * mask.float() * weight).sum() / (mask.float().sum() + 1e-8)
    return loss


# ---------------------------------------------------------------------------
# 4. Three decoders that share weights
# ---------------------------------------------------------------------------
@torch.no_grad()
def decode_ar(
    model: UnifiedDiffusionLM,
    prompt: torch.Tensor,           # [B, P]
    n_new: int,
    temperature: float = 1.0,
) -> torch.Tensor:
    """Plain autoregressive sampling. block_size=1."""
    model.eval()
    x = prompt.clone()
    for _ in range(n_new):
        logits = model(x, block_size=1, t=None)[:, -1] / temperature
        nxt = torch.multinomial(F.softmax(logits, dim=-1), 1)
        x = torch.cat([x, nxt], dim=1)
    return x


@torch.no_grad()
def decode_diffusion(
    model: UnifiedDiffusionLM,
    prompt: torch.Tensor,           # [B, P]
    n_new: int,
    n_steps: int = 8,
    temperature: float = 1.0,
) -> torch.Tensor:
    """Parallel masked-diffusion decoding (MDLM-style remasking).

    We append `n_new` [MASK] tokens then iteratively unmask the most-confident
    ones in `n_steps` denoising rounds. Block_size = n_new so all new tokens
    see each other bidirectionally while still attending causally to the prompt.
    """
    model.eval()
    cfg = model.cfg
    B, P = prompt.shape
    mask_id = cfg.mask_token_id

    x = torch.cat([prompt, torch.full((B, n_new), mask_id, device=prompt.device)], dim=1)
    block_size = n_new                                  # one big diffusion block at the end

    for step in range(n_steps):
        # noise level decreases from 1 -> 0
        t = torch.full((B,), 1.0 - step / n_steps, device=x.device)
        logits = model(x, block_size=block_size, t=t)[:, P:] / temperature
        probs = F.softmax(logits, dim=-1)

        # sample candidates
        sampled = torch.multinomial(probs.view(-1, cfg.vocab_size), 1).view(B, n_new)
        conf = probs.gather(-1, sampled.unsqueeze(-1)).squeeze(-1)   # [B, n_new]

        # how many to keep this step (linear schedule)
        keep_n = int(round((step + 1) / n_steps * n_new))
        is_mask = (x[:, P:] == mask_id)
        # only consider currently-masked positions
        conf = conf.masked_fill(~is_mask, -float('inf'))
        # top-k confident positions become final
        topk = torch.topk(conf, k=min(keep_n, n_new), dim=-1).indices  # [B, keep_n]
        new_block = x[:, P:].clone()
        # write sampled values at chosen positions
        for b in range(B):
            new_block[b, topk[b]] = sampled[b, topk[b]]
        # leftover positions remain [MASK] for next step
        # (anything previously committed stays committed)
        committed = ~is_mask
        new_block = torch.where(committed, x[:, P:], new_block)
        x = torch.cat([prompt, new_block], dim=1)

        if (x[:, P:] == mask_id).sum() == 0:
            break

    # final cleanup: if any masks survive, greedy-fill them with one AR pass
    still_masked = (x == mask_id)
    if still_masked.any():
        logits = model(x, block_size=block_size, t=torch.zeros(B, device=x.device))
        fill = logits.argmax(-1)
        x = torch.where(still_masked, fill, x)
    return x


@torch.no_grad()
def decode_self_speculative(
    model: UnifiedDiffusionLM,
    prompt: torch.Tensor,           # [B, P], B must be 1 for clarity
    n_new: int,
    draft_block: int = 8,
    n_diffusion_steps: int = 4,
    temperature: float = 1.0,
    verbose: bool = False,
) -> Tuple[torch.Tensor, dict]:
    """Self-speculative decoding using ONE model.

    Loop:
      1. DRAFT: run `n_diffusion_steps` of parallel masked diffusion to fill
         a block of `draft_block` future tokens.
      2. VERIFY: run a single AR forward pass over [prompt + draft] and read
         off the model's own next-token distribution at every position.
         Accept the longest prefix where the AR greedy/sampled token matches
         the draft (standard speculative-decoding acceptance rule).
      3. Always emit at least 1 token (the AR sample at the first rejection).
    """
    assert prompt.size(0) == 1, "demo implementation assumes batch=1"
    model.eval()
    cfg = model.cfg
    device = prompt.device
    x = prompt.clone()
    stats = {"accepted": 0, "proposed": 0, "rounds": 0}

    remaining = n_new
    while remaining > 0:
        k = min(draft_block, remaining)
        # ---- 1. Draft with diffusion ----------------------------------
        drafted = decode_diffusion(
            model, x, n_new=k,
            n_steps=n_diffusion_steps,
            temperature=temperature,
        )                                        # [1, P_cur + k]
        draft_tokens = drafted[0, -k:]           # [k]

        # ---- 2. Verify with one AR forward ----------------------------
        # AR mode looks at causal context; we want the model's prediction
        # for positions [P_cur .. P_cur+k-1] given the draft so far.
        ar_in = drafted                          # contains prompt + draft
        ar_logits = model(ar_in, block_size=1, t=None)
        # prediction for position p comes from logits at p-1
        verify_logits = ar_logits[0, -k-1:-1] / temperature   # [k, V]
        verify_probs = F.softmax(verify_logits, dim=-1)

        # acceptance: take the longest prefix of greedy matches
        ar_greedy = verify_probs.argmax(-1)      # [k]
        matches = (ar_greedy == draft_tokens)
        # first False position
        if matches.all():
            n_accept = k
        else:
            n_accept = int(torch.where(~matches)[0][0].item())

        # ---- 3. Commit accepted tokens + 1 corrective AR sample -------
        accepted = draft_tokens[:n_accept]
        x = torch.cat([x, accepted.unsqueeze(0)], dim=1)
        emitted = n_accept

        if n_accept < k and remaining - n_accept > 0:
            # sample correction from AR distribution at the rejection point
            corr_logits = verify_logits[n_accept]
            corr_tok = torch.multinomial(F.softmax(corr_logits, dim=-1), 1)
            x = torch.cat([x, corr_tok.unsqueeze(0)], dim=1)
            emitted += 1

        stats["accepted"] += n_accept
        stats["proposed"] += k
        stats["rounds"] += 1
        remaining -= emitted

        if verbose:
            print(f"  round {stats['rounds']}: drafted {k}, accepted {n_accept}, "
                  f"emitted {emitted}")

    stats["acceptance_rate"] = stats["accepted"] / max(stats["proposed"], 1)
    stats["tokens_per_round"] = (n_new) / max(stats["rounds"], 1)
    return x, stats


# ---------------------------------------------------------------------------
# 5. Smoke test: train on a tiny synthetic task, then decode three ways
# ---------------------------------------------------------------------------
def _make_toy_batch(B, L, vocab_size, device):
    """Synthetic 'copy + shift' task: bytes follow a simple periodic pattern
    so the model can actually learn something in a few hundred steps."""
    # pattern: each sequence is an arithmetic progression mod V
    starts = torch.randint(1, vocab_size - 1, (B, 1), device=device)
    steps  = torch.randint(1, 5,            (B, 1), device=device)
    pos    = torch.arange(L, device=device)[None]
    seq = (starts + steps * pos) % (vocab_size - 1)        # avoid mask id
    seq[seq == 255] = 1
    return seq


def main():
    torch.manual_seed(0)
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    cfg = ModelConfig(vocab_size=256, d_model=128, n_layers=3,
                      n_heads=4, d_ff=512, max_seq_len=128)
    model = UnifiedDiffusionLM(cfg).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=3e-4)

    print(f"Model params: {sum(p.numel() for p in model.parameters())/1e6:.2f}M")
    print(f"Device: {device}")
    print("\n--- training on toy arithmetic-progression task ---")

    L = 64
    for step in range(400):
        batch = _make_toy_batch(32, L, cfg.vocab_size, device)
        loss = unified_loss(model, batch, p_ar=0.5)
        opt.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        if step % 50 == 0:
            print(f"  step {step:4d}  loss {loss.item():.3f}")

    print("\n--- decoding the same prompt three ways ---")
    prompt = _make_toy_batch(1, 8, cfg.vocab_size, device)
    print(f"prompt tokens: {prompt[0].tolist()}")

    import time
    n_new = 24

    t0 = time.time()
    out_ar = decode_ar(model, prompt, n_new=n_new)
    t_ar = time.time() - t0
    print(f"\n[1] AR              ({t_ar*1000:.1f} ms)")
    print(f"    continuation: {out_ar[0, -n_new:].tolist()}")

    t0 = time.time()
    out_diff = decode_diffusion(model, prompt, n_new=n_new, n_steps=6)
    t_diff = time.time() - t0
    print(f"\n[2] Parallel diffusion ({t_diff*1000:.1f} ms, 6 steps)")
    print(f"    continuation: {out_diff[0, -n_new:].tolist()}")

    t0 = time.time()
    out_spec, stats = decode_self_speculative(
        model, prompt, n_new=n_new,
        draft_block=8, n_diffusion_steps=3, verbose=True,
    )
    t_spec = time.time() - t0
    print(f"\n[3] Self-speculative ({t_spec*1000:.1f} ms)")
    print(f"    continuation: {out_spec[0, -n_new:].tolist()}")
    print(f"    acceptance rate: {stats['acceptance_rate']:.2%}")
    print(f"    avg tokens/round: {stats['tokens_per_round']:.2f}")

    print("\nDone. Same weights, three decoders.")


if __name__ == "__main__":
    main()
