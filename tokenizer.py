from __future__ import annotations


class ByteTokenizer:
    """Deterministic byte-level tokenizer for smoke tests and local scaffolding.

    Production note: replace this with a trained tokenizer from your chosen model family.
    The byte tokenizer keeps the repo self-contained and makes tests deterministic.
    """

    pad_id = 0
    bos_id = 1
    eos_id = 2
    byte_offset = 3
    vocab_size = 259

    def encode(self, text: str, add_bos: bool = True, add_eos: bool = False) -> list[int]:
        ids = [b + self.byte_offset for b in text.encode("utf-8", errors="replace")]
        if add_bos:
            ids.insert(0, self.bos_id)
        if add_eos:
            ids.append(self.eos_id)
        return ids

    def decode(self, ids: list[int]) -> str:
        data = bytearray()
        for token_id in ids:
            if token_id in {self.pad_id, self.bos_id, self.eos_id}:
                continue
            if self.byte_offset <= token_id < self.vocab_size:
                data.append(token_id - self.byte_offset)
        return data.decode("utf-8", errors="replace")
