"""Optional Modal + vLLM deployment sketch.

Install Modal locally, authenticate, then adapt MODEL_NAME to a supported model.
This file is intentionally isolated from the core package so local tests do not
require Modal or vLLM.
"""

from __future__ import annotations

MODEL_NAME = "NousResearch/Meta-Llama-3-8B-Instruct"

try:
    import modal
except Exception:  # pragma: no cover - optional dependency
    modal = None

if modal is not None:  # pragma: no cover - cloud deployment path
    image = (
        modal.Image.debian_slim(python_version="3.12")
        .pip_install("vllm", "huggingface_hub")
        .env({"HF_HUB_ENABLE_HF_TRANSFER": "1"})
    )
    app = modal.App("mmllm-vllm-openai")

    @app.function(image=image, gpu="H100", timeout=60 * 30, scaledown_window=60 * 5)
    @modal.web_server(port=8000, startup_timeout=60 * 20)
    def serve() -> None:
        import subprocess

        subprocess.Popen(
            [
                "vllm",
                "serve",
                MODEL_NAME,
                "--host",
                "0.0.0.0",
                "--port",
                "8000",
                "--dtype",
                "auto",
            ]
        )
