# Legal Machines vendor patch

## Summary

- **What it is.** The `rust_component/` crate of `SemplificaAI/gliner2-rs` at
  commit `62376385092826094a1810d81050440639fec7ac`. It is Apache-2.0, with its
  upstream `LICENSE` and `NOTICE`.
- **What Singularity changes:**
  - the execution-provider order;
  - schema processing, tokenisation and decoding, aligned with pinned GLiNER2
    1.3.2;
  - the tokenizer, parsed while the sessions build;
  - each encoder call, bounded to 512 positions;
  - its prints, which leave a failed write unsaid: std's print macros panic
    when standard output is a pipe nobody reads any more, and that panic
    stopped the engine as it loaded its model (`src/lib.rs`).

This directory is the `rust_component/` crate from
`SemplificaAI/gliner2-rs` commit
`62376385092826094a1810d81050440639fec7ac`, retaining its upstream
Apache-2.0 `LICENSE` and `NOTICE` files.

Singularity changes only the V2 execution-provider selection:

- macOS/iOS: CoreML, then CPU;
- `FORCE_CPU`: CPU only;
- other targets: the upstream provider order is unchanged.

The official ONNX Runtime macOS artifact includes CoreML in the main dylib but
does not ship OpenVINO, CUDA, ROCm, or XNNPACK provider libraries. Avoiding
those unavailable probes removes eight misleading loader errors during the
eager eight-session construction while preserving deterministic CPU fallback.

Singularity also aligns the native schema processor with pinned GLiNER2 1.3.2:
the concatenated schema/text subwords go directly to the encoder. Upstream
`gliner2-rs` inserted additional `[CLS]` and `[SEP]` ids that the Python
`SchemaTransformer` does not insert, causing prediction drift under an
otherwise identical requested-label schema.
Text words are likewise lowercased before subword tokenization, matching the
pinned Python transform while the original text and byte offsets remain local
for exact restoration.
Entity decoding reads count slot zero only and uses an inclusive confidence
threshold, matching GLiNER2 1.3.2; predicted-count slots remain available to
structured and relation tasks.
Schema field indices gather the `[E]`/`[R]`/`[L]` marker embeddings, rather
than the following label word, matching Python's `schema_special_positions`.

V2 engine construction parses `tokenizer.json` on a scoped thread while the
eight sessions are built, rather than after them (about 0.15 s of every
launch). Session options, provider order and inference are unchanged.

A later change (once called Phase 24.1D) bounds each V2 encoder call to 512 positions, including schema
tokens. Overlapping word windows cover the complete input and preserve every
span within the model width; offsets are remapped into the original text and
identical spans are deduplicated by confidence. The C ABI exposes the bounded
runtime identity and measured chunk progress. The focused Rust suite includes
real-model cases for long Cyrillic input and entities beyond the first window.

The opt-in `SINGULARITY_GLINER2_MEMDIAG` checkpoints read the resident size from
each OS's own accounting: Mach `task_info` on macOS/iOS, the working set on
Windows and procfs `VmRSS` on Linux.
