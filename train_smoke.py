from __future__ import annotations

import torch

from mmllm.config import ModelConfig
from mmllm.model import MultiModalCausalLM
from mmllm.tokenizer import ByteTokenizer


def main() -> None:
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    tok = ByteTokenizer()
    cfg = ModelConfig(d_model=128, n_layers=2, n_heads=4, n_kv_heads=2, max_seq_len=256)
    model = MultiModalCausalLM(cfg).to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=3e-4, weight_decay=0.1)
    corpus = [
        "modal decree: text image audio fuse into a causal language model.",
        "guarantee gates validate shapes limits and numerical finiteness.",
        "production systems need tests telemetry deployment and rollback.",
    ]
    encoded = [tok.encode(x, add_bos=True, add_eos=True) for x in corpus]
    for step in range(20):
        batch = []
        for ids in encoded:
            ids = ids[: cfg.max_seq_len]
            batch.append(ids + [tok.pad_id] * (cfg.max_seq_len - len(ids)))
        x = torch.tensor(batch, dtype=torch.long, device=device)
        labels = x.clone()
        labels[labels == tok.pad_id] = -100
        out = model(x, labels=labels)
        loss = out["loss"]
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        if step % 5 == 0:
            print({"step": step, "loss": round(float(loss), 4)})
    torch.save({"model": model.state_dict(), "config": cfg.__dict__}, "mmllm_smoke.pt")


if __name__ == "__main__":
    main()
