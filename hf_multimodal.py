from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import torch


@dataclass
class HFMultimodalAdapter:
    """Optional Hugging Face adapter.

    Install with:
        pip install -e '.[hf]'

    The adapter uses AutoProcessor because multimodal models need processors that
    combine tokenizers, image processors, and/or audio feature extractors.
    """

    model_id: str
    device_map: str = "auto"
    torch_dtype: str = "auto"

    def __post_init__(self) -> None:
        from transformers import AutoModelForVision2Seq, AutoProcessor

        self.processor = AutoProcessor.from_pretrained(self.model_id, trust_remote_code=False)
        dtype: Any = self.torch_dtype
        if self.torch_dtype == "bfloat16":
            dtype = torch.bfloat16
        elif self.torch_dtype == "float16":
            dtype = torch.float16
        self.model = AutoModelForVision2Seq.from_pretrained(
            self.model_id,
            device_map=self.device_map,
            torch_dtype=dtype,
            trust_remote_code=False,
        )

    @torch.no_grad()
    def generate(self, conversation: list[dict[str, Any]], max_new_tokens: int = 256) -> str:
        inputs = self.processor.apply_chat_template(
            conversation,
            add_generation_prompt=True,
            tokenize=True,
            return_dict=True,
            return_tensors="pt",
        )
        inputs = {k: v.to(self.model.device) if hasattr(v, "to") else v for k, v in inputs.items()}
        output_ids = self.model.generate(**inputs, max_new_tokens=max_new_tokens)
        return self.processor.decode(output_ids[0], skip_special_tokens=True)
