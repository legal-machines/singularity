# Third-party notices

Singularity is licensed under the Legal Machines Singularity License (`LICENSE.md`). It builds on the components
below, each under its own license, which Singularity's license does not change. The build fetches each one at the version
named here, or carries it in this repository where the "Where" column says so.

## The native core

| Component | Version | License | Where |
| --- | --- | --- | --- |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | b10516 (commit b95502b), with Legal Machines' patches | MIT | built from source (`dependencies/llama.cpp`) |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | 1.23.2 | MIT | fetched |
| [webview](https://github.com/webview/webview) | 0.12.0 | MIT | fetched |
| [Native File Dialog Extended](https://github.com/btzy/nativefiledialog-extended) | 1.3.0 | Zlib | fetched |
| [JSON for Modern C++](https://github.com/nlohmann/json) | 3.11.3 | MIT | fetched |
| [libzip](https://github.com/nih-at/libzip) | commit 6f8a0cdd | BSD-3-Clause | fetched |
| [pugixml](https://github.com/zeux/pugixml) | commit c8033ce9 | MIT | fetched |
| [utf8proc](https://github.com/JuliaStrings/utf8proc) | commit e5e79922 | MIT, with the Unicode data license | fetched |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.7 | Apache-2.0 | fetched |
| [Monocypher](https://monocypher.org) | 4.0.2 | BSD-2-Clause or CC0-1.0 | fetched |
| [PDFium](https://pdfium.googlesource.com/pdfium/), as built by [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) | chromium/8086 | BSD-3-Clause (PDFium); MIT (the binaries' packaging) | fetched |
| [Microsoft Edge WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2) (Windows only) | 1.0.4258.31 | Microsoft's license for the WebView2 SDK | fetched |
| gliner2-rs, the inference engine of the Papinian NER runtime | vendored, with the changes its patch note lists | Apache-2.0, Copyright 2026 Dario Finardi, Semplifica s.r.l.; its Rust crates under their own licenses (`Cargo.lock`) | `code/backend/LocalModel/Models/PapinianNER/NativeRuntime/vendor/gliner2_inference` |
| English common words, from *Webster's Second International Dictionary* (1934) | carried | public domain | `code/backend/PapinianGuard/CommonWords` |

## The window

| Component | Version | License |
| --- | --- | --- |
| [React](https://react.dev) and React DOM | 19 | MIT |
| [qr](https://github.com/paulmillr/qr) | 0.7.2 | MIT or Apache-2.0 |
| [Roboto](https://fonts.google.com/specimen/Roboto), through @fontsource-variable/roboto | 5 | SIL Open Font License 1.1 |
| [Inter](https://rsms.me/inter/), [Sora](https://fonts.google.com/specimen/Sora) and [Syne](https://fonts.google.com/specimen/Syne) | as carried | SIL Open Font License 1.1; each license lies beside its font in `code/frontend/onboarding/fonts` |

The tools that build and test the window (Vite, Vitest, jsdom and the React plugin for Vite, all MIT) are not part of
what Singularity distributes.

## Models

No model is part of this repository. The protection model behind Papinian Guard, which Singularity downloads from
Legal Machines, is fastino's gliner2-privacy-filter-PII-multi (revision c153999da5f4c509df4322b0c6a1baf3d2c284d7,
Apache-2.0) with the mDeBERTa-v3-base model on which it is built (Microsoft, MIT), and it keeps those licenses. Legal
Machines exported the checkpoint to ONNX as eight 16-bit floating-point graphs with its tokenizer (GLiNER2 1.3.2, PyTorch
2.13.0, ONNX 1.20.1); its weights were not trained further. A model a firm uses from its own files is licensed by its
publisher.
