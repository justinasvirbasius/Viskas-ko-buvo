# MM-LLM Decree: production-grade multimodal LLM scaffold

This repository is a production-oriented scaffold for a **multimodal causal language model service**. It includes:

- A PyTorch transformer core with RMSNorm, RoPE, causal SDPA attention, GQA-ready heads, modality prefix fusion, and greedy generation.
- Text, image-feature, and audio-feature input paths.
- “Modal decrees”: explicit contracts for each modality, its limits, feature shape, and safety checks.
- FastAPI OpenAI-compatible-ish `/v1/chat/completions` serving.
- Docker deployment, health checks, tests, and smoke training.
- Optional adapters for Hugging Face multimodal models and vLLM serving.

> Important: this repo is a **ready-to-extend production scaffold**, not a pretrained frontier model. The included PyTorch model is intentionally small and untrained so tests and local boot are fast. For production quality outputs, load trained checkpoints or route serving through the vLLM/Hugging Face adapters.

## Architecture diagram

```mermaid
flowchart TD
  Client[Client: chat/image/audio request] --> API[FastAPI API boundary]
  API --> S1[Schema validation]
  S1 --> S2[Safety + limit gates]
  S2 --> P[Processor layer]
  P --> T[Text tokenizer]
  P --> V[Vision encoder or image feature adapter]
  P --> A[Audio encoder or audio feature adapter]
  T --> F[Fusion: modal prefix + text sequence]
  V --> F
  A --> F
  F --> LLM[Decoder-only Transformer]
  LLM --> LG[Logits guards]
  LG --> D[Decoder]
  D --> OUT[Response + telemetry]
```

## Internal transformer flow

```mermaid
flowchart LR
  X[Token + modal embeddings] --> B1[Block 1]
  B1 --> B2[Block ...]
  B2 --> BN[Block N]
  BN --> R[RMSNorm]
  R --> H[LM head]
  H --> Z[Next-token logits]

  subgraph Block
    N1[RMSNorm] --> QKV[Q/K/V projections]
    QKV --> RoPE[RoPE rotation]
    RoPE --> SDPA[Causal scaled dot-product attention]
    SDPA --> RES1[Residual]
    RES1 --> N2[RMSNorm]
    N2 --> MLP[SwiGLU MLP]
    MLP --> RES2[Residual]
  end
```

## Modal decrees

```mermaid
flowchart TB
  D[Modal Decree Registry] --> TXT[Text decree]
  D --> IMG[Image decree]
  D --> AUD[Audio decree]
  TXT --> G1[Max chars, token budget, prompt hygiene]
  IMG --> G2[Max images, feature dim, image-token budget]
  AUD --> G3[Max audio clips, feature dim, duration budget]
  G1 --> Fuse[Accepted input envelope]
  G2 --> Fuse
  G3 --> Fuse
```

## Guarantees ledger

No LLM can guarantee truth, safety, or optimal behavior absolutely. This scaffold instead provides enforceable gates:

1. **Shape guarantee**: tensor shapes are validated by tests before model execution.
2. **Input guarantee**: Pydantic schemas and `safety.py` reject invalid/oversized requests.
3. **Modality guarantee**: `modality_decrees.py` defines allowed modality counts, dimensions, and feature budgets.
4. **Numerical guarantee**: model forward checks NaN/Inf contamination at the API boundary.
5. **Deployment guarantee**: Docker image exposes a health endpoint and can be replicated behind a load balancer.
6. **Extensibility guarantee**: custom PyTorch core and real model adapters are separated so you can swap in vLLM or Hugging Face models without rewriting the API.

## Quick start

```bash
python -m venv .venv
source .venv/bin/activate
pip install -e '.[dev]'
pytest -q
uvicorn mmllm.serve:app --host 0.0.0.0 --port 8000
```

Test request:

```bash
curl http://localhost:8000/v1/chat/completions \
  -H 'content-type: application/json' \
  -d '{
    "model":"mmllm-tiny",
    "messages":[{"role":"user","content":"Describe the modal decree system."}],
    "max_tokens":32
  }'
```

## Docker

```bash
docker build -t mmllm-decree .
docker run --rm -p 8000:8000 mmllm-decree
```

## Recommended production route

For real throughput and trained weights:

- Use this repo’s API, guardrails, tests, and modality contracts.
- Swap `InferenceEngine` with `adapters/hf_multimodal.py` for Hugging Face multimodal models.
- Use vLLM for OpenAI-compatible high-throughput serving when the target model is supported.
- Pin model revisions and package versions; do not deploy floating model references.

## Repository map

```text
src/mmllm/
  config.py              typed configuration
  tokenizer.py           byte-level smoke tokenizer
  layers.py              transformer building blocks
  model.py               multimodal causal LM
  modality_decrees.py    per-modality contracts
  safety.py              request guardrails and numerical checks
  schemas.py             OpenAI-ish API schemas
  inference.py           inference engine abstraction
  serve.py               FastAPI app
  adapters/
    hf_multimodal.py     optional Hugging Face adapter
configs/
  model.yaml             production-ish config template
docs/
  architecture.mmd       standalone Mermaid diagram
scripts/
  train_smoke.py         tiny next-token smoke trainer
tests/
  test_*.py              contracts, shape checks, API smoke
```
