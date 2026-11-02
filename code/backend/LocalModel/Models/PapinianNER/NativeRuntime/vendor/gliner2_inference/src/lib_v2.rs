// Copyright 2026 Dario Finardi, Semplifica s.r.l.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! `Gliner2EngineV2` — engine IOBinding-ready che usa i modelli ONNX v2
//! generati da `export_gliner2_onnx_fragments_v2.py`.
//!
//! # Differenze rispetto a `Gliner2Engine` (v1)
//!
//! | v1 (`Gliner2Engine`)         | v2 (`Gliner2EngineV2`)                  |
//! |------------------------------|-----------------------------------------|
//! | 5 sessioni ONNX              | 8 sessioni ONNX                         |
//! | Gather/ArgMax/Einsum in Rust | Gather/ArgMax/Einsum fusi nell'ONNX     |
//! | `extract_iobinding` → stub   | `extract_iobinding` → implementata      |
//! | `extract_standard` completa  | `extract_standard` identica al v1       |
//!
//! # File ONNX attesi
//!
//! La directory dei modelli deve contenere i file generati dallo script v2:
//! - `encoder_fp16_iobinding.onnx`
//! - `token_gather_fp16_iobinding.onnx`
//! - `span_rep_fp16_iobinding.onnx`
//! - `schema_gather_fp16_iobinding.onnx`
//! - `count_pred_argmax_fp16_iobinding.onnx`
//! - `count_lstm_fixed_fp16_iobinding.onnx`
//! - `scorer_fp16_iobinding.onnx`
//! - `classifier_fp16_iobinding.onnx`
//! - `tokenizer.json`
//!
//! Oppure le varianti `_fp32.onnx` / `_fp16.onnx` per Standard mode / CoreML.

use anyhow::Result;
use ndarray::{s, Array1, Array2, Array3, Array4, ArrayD};

/// Estrae un `ndarray::ArrayD<f32>` da un output ONNX, provando prima `f32` e
/// poi `f16` con conversione. Necessario perché i modelli v2 in formato
/// `fp16_iobinding` emettono output `Tensor<f16>` mentre senza questo fallback
/// `try_extract_array::<f32>()` fallirebbe con
/// `Cannot extract Tensor<f32> from Tensor<f16>` rompendo l'inferenza.
///
/// PATCH LOCALE — upstream crate non ha questo fallback su tutti i path.
macro_rules! extract_f32 {
    ($val:expr) => {{
        let __v = $val;
        if let Ok(t) = __v.try_extract_array::<f32>() {
            t.into_owned()
        } else if let Ok(t) = __v.try_extract_array::<half::f16>() {
            t.into_owned().mapv(|x| x.to_f32())
        } else {
            anyhow::bail!("tensor output: né f32 né f16");
        }
    }};
}
use ort::{
    execution_providers::{
        CPU, CUDA, CoreML,
        OpenVINO, ROCm,
        XNNPACK,
    },
    memory::{AllocationDevice, AllocatorType, MemoryInfo, MemoryType},
    session::{builder::GraphOptimizationLevel, Session},
    value::{DynValue, Tensor},
};
use tokenizers::Tokenizer;
use std::path::Path;
use std::sync::{Mutex, OnceLock, RwLock};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

// ─────────────────────────────────────────────────────────────────────────────
// TEMPORARY diagnostic instrumentation (phase24_1d memory-growth investigation)
// ─────────────────────────────────────────────────────────────────────────────
// Raw mach FFI, no new Cargo dependency: resident_size from the small, stable
// MACH_TASK_BASIC_INFO struct (not the larger, versioned TASK_VM_INFO the C++
// side uses for phys_footprint) - good enough to localize which ONNX session
// a jump happens around; the outer C++ diagnostic remains the authoritative
// phys_footprint (the kernel's own jetsam accounting) bracketing the whole
// infer() call. SINGULARITY_GLINER2_MEMDIAG=1 gates all of this off by default so
// it costs nothing outside an explicit diagnostic run.
//
// The resident size comes from each OS's own accounting: Mach's task_info on
// Apple targets, the working set on Windows, VmRSS from procfs elsewhere;
// 0 when the OS gives no answer.
#[cfg(any(target_os = "macos", target_os = "ios"))]
#[repr(C)]
struct DiagTimeValue { seconds: i32, microseconds: i32 }
#[cfg(any(target_os = "macos", target_os = "ios"))]
#[repr(C)]
struct DiagMachTaskBasicInfo {
    virtual_size: u64,
    resident_size: u64,
    resident_size_max: u64,
    user_time: DiagTimeValue,
    system_time: DiagTimeValue,
    policy: i32,
    suspend_count: i32,
}
#[cfg(any(target_os = "macos", target_os = "ios"))]
extern "C" {
    fn mach_task_self() -> u32;
    fn task_info(target_task: u32, flavor: u32, task_info_out: *mut u32, task_info_count: *mut u32) -> i32;
}
#[cfg(any(target_os = "macos", target_os = "ios"))]
const DIAG_MACH_TASK_BASIC_INFO: u32 = 20;
#[cfg(any(target_os = "macos", target_os = "ios"))]
fn diag_resident_bytes() -> u64 {
    unsafe {
        let mut info: DiagMachTaskBasicInfo = std::mem::zeroed();
        let mut count: u32 =
            (std::mem::size_of::<DiagMachTaskBasicInfo>() / std::mem::size_of::<u32>()) as u32;
        let kr = task_info(mach_task_self(), DIAG_MACH_TASK_BASIC_INFO,
            &mut info as *mut _ as *mut u32, &mut count);
        if kr == 0 { info.resident_size } else { 0 }
    }
}
#[cfg(target_os = "windows")]
#[repr(C)]
struct DiagProcessMemoryCounters {
    cb: u32,
    page_fault_count: u32,
    peak_working_set_size: usize,
    working_set_size: usize,
    quota_peak_paged_pool_usage: usize,
    quota_paged_pool_usage: usize,
    quota_peak_non_paged_pool_usage: usize,
    quota_non_paged_pool_usage: usize,
    pagefile_usage: usize,
    peak_pagefile_usage: usize,
}
#[cfg(target_os = "windows")]
#[link(name = "kernel32")]
extern "system" {
    fn GetCurrentProcess() -> *mut std::ffi::c_void;
    fn K32GetProcessMemoryInfo(process: *mut std::ffi::c_void, counters: *mut DiagProcessMemoryCounters, size: u32) -> i32;
}
#[cfg(target_os = "windows")]
fn diag_resident_bytes() -> u64 {
    unsafe {
        let size = std::mem::size_of::<DiagProcessMemoryCounters>() as u32;
        let mut counters: DiagProcessMemoryCounters = std::mem::zeroed();
        counters.cb = size;
        let ok = K32GetProcessMemoryInfo(GetCurrentProcess(), &mut counters, size);
        if ok != 0 { counters.working_set_size as u64 } else { 0 }
    }
}
#[cfg(not(any(target_os = "macos", target_os = "ios", target_os = "windows")))]
fn diag_resident_bytes() -> u64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|status| {
            status.lines().find_map(|line| {
                line.strip_prefix("VmRSS:")
                    .and_then(|value| value.trim().trim_end_matches("kB").trim().parse::<u64>().ok())
            })
        })
        .map_or(0, |kib| kib * 1024)
}
fn diag_enabled() -> bool {
    std::env::var_os("SINGULARITY_GLINER2_MEMDIAG").is_some()
}
fn diag_checkpoint(label: &str) {
    if !diag_enabled() { return; }
    static STARTED: OnceLock<Instant> = OnceLock::new();
    let elapsed_us = STARTED.get_or_init(Instant::now).elapsed().as_micros();
    let resident = diag_resident_bytes();
    eprintln!("RUSTMEMDIAG {:<40} elapsed_us={:>12} resident={:>12} ({:>8.2} MB)",
        label, elapsed_us, resident, resident as f64 / (1024.0 * 1024.0));
}
fn diag_shape(label: &str, shape: &[usize]) {
    if !diag_enabled() { return; }
    let bytes: usize = shape.iter().product::<usize>() * 4; // f32/i64 both 4-8B; logged as f32-equivalent lower bound
    eprintln!("RUSTMEMDIAG   {:<38} shape={:?} elements={} approx_bytes>={}",
        label, shape, shape.iter().product::<usize>(), bytes);
}

/// Converte un errore `ort` in `anyhow::Error`.
///
/// Da ort rc.13 `Error<R>` porta con sé un valore di *recovery* (il
/// SessionBuilder, la MemoryInfo, …) che non è Send+Sync: `?` verso
/// anyhow non compila più. Qui codice e messaggio vengono estratti e
/// l'errore diventa un anyhow piatto.
#[inline]
fn oa<R>(e: ort::Error<R>) -> anyhow::Error {
    anyhow::anyhow!("ort [{:?}]: {}", e.code(), e.message())
}


use crate::{
    error::GlinerError,
    processor::{ProcessedRecord, SchemaTask, SchemaTransformer},
    ExecutionMode, ExtractedClassification, ExtractedEntity, ExtractedRelation, Gliner2Config,
    InferenceParams,
};

// ─────────────────────────────────────────────────────────────────────────────
// Struct principale
// ─────────────────────────────────────────────────────────────────────────────

/// Engine IOBinding-ready per modelli ONNX v2.
pub struct Gliner2EngineV2 {
    encoder: Mutex<Session>,
    token_gather: Mutex<Session>,
    span_rep: Mutex<Session>,
    schema_gather: Mutex<Session>,
    count_pred_argmax: Mutex<Session>,
    count_lstm_fixed: Mutex<Session>,
    scorer: Mutex<Session>,
    classifier: Mutex<Session>,
    tokenizer:          Tokenizer,
    config:             Gliner2Config,
    pub execution_mode: RwLock<ExecutionMode>,
}

// The pinned production checkpoint's encoder is a `microsoft/mdeberta-v3-base`
// fine-tune (see Gliner2Adapter.cpp's Gliner2SemanticModelAdapter::identity());
// that published architecture's own `max_position_embeddings` is 512, matching
// this model family's own training-time sequence cap
// (LocalModel/Models/PapinianNER's Training/config/training-v0.1.json:
// "max_length": 512). No code anywhere in this crate previously enforced
// this - extract_with_progress() below is what now guarantees no single real
// encoder invocation (schema + requested labels/fields + separators + text)
// ever exceeds it.
pub const MAX_ENCODER_POSITIONS: usize = 512;

// Diagnostic counter only: the production execution path increments this
// immediately before each bounded chunk reaches the real eight-session ONNX
// pipeline. Reading it does not alter inference or retain per-request data.
static SEMANTIC_CHUNKS_EXECUTED_TOTAL: AtomicUsize = AtomicUsize::new(0);
static ENCODER_FORWARD_CALLS_TOTAL: AtomicUsize = AtomicUsize::new(0);
static ENCODER_BATCHES_TOTAL: AtomicUsize = AtomicUsize::new(0);
static DOWNSTREAM_FORWARD_CALLS_TOTAL: AtomicUsize = AtomicUsize::new(0);

pub fn semantic_chunks_executed_total() -> usize {
    SEMANTIC_CHUNKS_EXECUTED_TOTAL.load(Ordering::Relaxed)
}

pub fn encoder_forward_calls_total() -> usize {
    ENCODER_FORWARD_CALLS_TOTAL.load(Ordering::Relaxed)
}

pub fn encoder_batches_total() -> usize {
    ENCODER_BATCHES_TOTAL.load(Ordering::Relaxed)
}

pub fn downstream_forward_calls_total() -> usize {
    DOWNSTREAM_FORWARD_CALLS_TOTAL.load(Ordering::Relaxed)
}

pub type ExtractionResult =
    (Vec<ExtractedEntity>, Vec<ExtractedRelation>, Vec<ExtractedClassification>);

pub struct EncoderEquivalenceReport {
    pub old_shape: [usize; 3],
    pub new_shape: [usize; 3],
    pub old_dtype: &'static str,
    pub new_dtype: &'static str,
    pub max_absolute_difference: f64,
    pub mean_absolute_difference: f64,
}

#[derive(Clone)]
struct DiagnosticTensorSnapshot {
    name: String,
    shape: Vec<usize>,
    dtype: String,
    value_type: String,
    values: Vec<f64>,
    memory: String,
    contiguous: bool,
}

#[derive(Default)]
struct TailDiagnosticTrace {
    tensors: Vec<DiagnosticTensorSnapshot>,
    candidates: Vec<ExtractedEntity>,
    final_entities: Vec<ExtractedEntity>,
}

fn diagnostic_tensor_snapshot(name: impl Into<String>, value: &DynValue)
    -> Result<DiagnosticTensorSnapshot>
{
    let memory = value.memory_info();
    let memory = format!("device={} device_id={} allocator={:?} memory_type={:?} device_type={:?}",
        memory.allocation_device().as_str(), memory.device_id(), memory.allocator_type(),
        memory.memory_type(), memory.device_type());
    if let Ok(array) = value.try_extract_array::<f32>() {
        return Ok(DiagnosticTensorSnapshot {
            name: name.into(), shape: array.shape().to_vec(), dtype: "f32".to_owned(),
            value_type: format!("{:?}", value.dtype()),
            values: array.iter().map(|&item| item as f64).collect(), memory,
            contiguous: array.is_standard_layout(),
        });
    }
    if let Ok(array) = value.try_extract_array::<half::f16>() {
        return Ok(DiagnosticTensorSnapshot {
            name: name.into(), shape: array.shape().to_vec(), dtype: "f16".to_owned(),
            value_type: format!("{:?}", value.dtype()),
            values: array.iter().map(|&item| item.to_f32() as f64).collect(), memory,
            contiguous: array.is_standard_layout(),
        });
    }
    if let Ok(array) = value.try_extract_array::<i64>() {
        return Ok(DiagnosticTensorSnapshot {
            name: name.into(), shape: array.shape().to_vec(), dtype: "i64".to_owned(),
            value_type: format!("{:?}", value.dtype()),
            values: array.iter().map(|&item| item as f64).collect(), memory,
            contiguous: array.is_standard_layout(),
        });
    }
    anyhow::bail!("diagnostic tensor '{}' has unsupported ORT type {:?}", name.into(), value.dtype())
}

fn diagnostic_tensor_comparison(
    old: &DiagnosticTensorSnapshot, new: &DiagnosticTensorSnapshot,
) -> (String, bool) {
    if old.shape != new.shape || old.dtype != new.dtype || old.values.len() != new.values.len() {
        return (format!(
            "STAGE {} old_shape={:?} new_shape={:?} old_dtype={} new_dtype={} old_elements={} new_elements={} max_abs_diff=NA mean_abs_diff=NA first_difference=shape_or_dtype",
            old.name, old.shape, new.shape, old.dtype, new.dtype, old.values.len(), new.values.len()), true);
    }
    let mut maximum = 0.0f64;
    let mut total = 0.0f64;
    let mut first = None;
    for (index, (&left, &right)) in old.values.iter().zip(new.values.iter()).enumerate() {
        let difference = (left - right).abs();
        maximum = maximum.max(difference);
        total += difference;
        if difference != 0.0 && first.is_none() {
            first = Some((index, left, right));
        }
    }
    let first_text = first.map(|(index, left, right)|
        format!("index={} standalone={} batch={}", index, left, right))
        .unwrap_or_else(|| "none".to_owned());
    (format!(
        "STAGE {} shape={:?} dtype={} elements={} max_abs_diff={} mean_abs_diff={} first_difference={}",
        old.name, old.shape, old.dtype, old.values.len(), maximum,
        if old.values.is_empty() { 0.0 } else { total / old.values.len() as f64 }, first_text),
        first.is_some())
}

fn diagnostic_entity_comparison(
    name: &str, old: &[ExtractedEntity], new: &[ExtractedEntity],
) -> (String, bool) {
    let mut maximum = 0.0f64;
    let mut total = 0.0f64;
    let mut compared_scores = 0usize;
    let mut first = None;
    for (index, (left, right)) in old.iter().zip(new.iter()).enumerate() {
        let score_difference = (left.score as f64 - right.score as f64).abs();
        maximum = maximum.max(score_difference);
        total += score_difference;
        compared_scores += 1;
        if first.is_none() && (left.text != right.text || left.label != right.label
                || left.start_tok != right.start_tok || left.end_tok != right.end_tok
                || left.start_char != right.start_char || left.end_char != right.end_char
                || score_difference != 0.0) {
            first = Some(format!(
                "index={} standalone={{text={:?},label={},score={},tok=[{},{}],char=[{},{}]}} batch={{text={:?},label={},score={},tok=[{},{}],char=[{},{}]}}",
                index, left.text, left.label, left.score, left.start_tok, left.end_tok,
                left.start_char, left.end_char, right.text, right.label, right.score,
                right.start_tok, right.end_tok, right.start_char, right.end_char));
        }
    }
    if first.is_none() && old.len() != new.len() {
        first = Some(format!("count standalone={} batch={}", old.len(), new.len()));
    }
    let first_text = first.clone().unwrap_or_else(|| "none".to_owned());
    (format!(
        "STAGE {} shape=[{}] dtype=entity_candidate elements={} max_abs_diff={} mean_abs_diff={} first_difference={}",
        name, old.len(), old.len().max(new.len()), maximum,
        if compared_scores == 0 { 0.0 } else { total / compared_scores as f64 }, first_text),
        first.is_some())
}

fn capture_diagnostic_tensor(
    trace: &mut Option<&mut TailDiagnosticTrace>, name: impl Into<String>, value: &DynValue,
) -> Result<(), GlinerError> {
    if let Some(trace) = trace.as_deref_mut() {
        trace.tensors.push(diagnostic_tensor_snapshot(name, value)
            .map_err(GlinerError::Other)?);
    }
    Ok(())
}

struct PreparedBatchChunk {
    sample_index: usize,
    chunk_index: usize,
    char_start: usize,
    text: String,
    record: ProcessedRecord,
}

/// Exact encoder semantics required for samples to share one forward pass.
/// The engine and schema are call-scoped invariants today, but keeping them in
/// the key makes it impossible for a future multi-schema/runtime caller to
/// batch merely because two token sequences happen to have the same length.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct ExactLengthBatchKey {
    sequence_length: usize,
    schema_tasks: Vec<SchemaTask>,
    models_dir: String,
    max_width: usize,
    model_type: crate::ModelType,
}

/// Pure chunk-planning arithmetic, independently testable without a real
/// tokenizer/ONNX runtime: given each word's own encoder-token length (from
/// a real ProcessedRecord::word_to_token_maps in production, or a synthetic
/// slice in tests), the remaining per-chunk text-token budget after schema
/// overhead, and the model's own overlap-in-words contract, returns each
/// chunk's word range [start, end) (exclusive end). Errors (rather than
/// silently truncating) if a single word alone cannot fit - the only case
/// this function cannot resolve by chunking.
fn plan_word_chunks(
    word_token_lens: &[usize],
    text_token_budget: usize,
    overlap_words: usize,
    schema_overhead_tokens: usize,
) -> Result<Vec<(usize, usize)>> {
    let num_words = word_token_lens.len();
    let mut ranges: Vec<(usize, usize)> = Vec::new();
    let mut start_word = 0usize;
    while start_word < num_words {
        let mut end_word = start_word;
        let mut token_sum = 0usize;
        while end_word < num_words {
            let word_tokens = word_token_lens[end_word];
            if token_sum + word_tokens > text_token_budget {
                break;
            }
            token_sum += word_tokens;
            end_word += 1;
        }
        if end_word == start_word {
            anyhow::bail!(
                "A single word at word-index {} alone encodes to more tokens than remain in the {}-token encoder window after {}-token schema overhead - cannot chunk safely without splitting one word's own subwords.",
                start_word, MAX_ENCODER_POSITIONS, schema_overhead_tokens);
        }
        ranges.push((start_word, end_word));
        if end_word >= num_words {
            break;
        }
        let next_start = end_word.saturating_sub(overlap_words);
        // Always make forward progress even if overlap_words >= this
        // chunk's own width (a pathological max_width far larger than any
        // real chunk could hold) - never loop forever.
        start_word = if next_start > start_word { next_start } else { end_word };
    }
    Ok(ranges)
}

/// Deterministic overlap dedup: two chunks can both detect the same
/// real-text entity inside their shared overlap region. Collapses by
/// identity in the ORIGINAL text (start_char, end_char, label) and keeps the
/// single highest-confidence detection - never both, never an arbitrary
/// pick (a tie keeps whichever was inserted first, since neither is more
/// correct than the other).
fn dedup_entities_by_identity(entities: Vec<ExtractedEntity>) -> Vec<ExtractedEntity> {
    let mut best_by_identity: std::collections::HashMap<
        (usize, usize, String), (usize, ExtractedEntity)> =
        std::collections::HashMap::new();
    for (ordinal, entity) in entities.into_iter().enumerate() {
        let key = (entity.start_char, entity.end_char, entity.label.clone());
        let replace = match best_by_identity.get(&key) {
            Some((_, existing)) => entity.score > existing.score,
            None => true,
        };
        if replace {
            let first_ordinal = best_by_identity.get(&key)
                .map(|(first_ordinal, _)| *first_ordinal)
                .unwrap_or(ordinal);
            best_by_identity.insert(key, (first_ordinal, entity));
        }
    }
    let mut deduped: Vec<(usize, ExtractedEntity)> = best_by_identity.into_values().collect();
    deduped.sort_by_key(|(first_ordinal, _)| *first_ordinal);
    deduped.into_iter().map(|(_, entity)| entity).collect()
}

impl Gliner2EngineV2 {
    /// Scarica i modelli V2 da Hugging Face
    pub fn from_pretrained(
        repo_id: &str,
        subfolder: Option<&str>,
        model_type: crate::ModelType,
    ) -> Result<Self> {
        let api = hf_hub::api::sync::ApiBuilder::new()
            .with_user_agent(env!("CARGO_PKG_NAME"), env!("CARGO_PKG_VERSION"))
            .with_user_agent("rust", "unknown")
            .with_user_agent(std::env::consts::OS, "unknown")
            .build()
            .map_err(|e| anyhow::anyhow!("Failed to initialize HF API: {}", e))?;

        let repo = api.model(repo_id.to_string());

        let is_fp16 = subfolder.unwrap_or("").contains("16");
        
        // Optimize download size: download only IOBinding files for CUDA/ROCm platforms, 
        // and standard FP16 files for CoreML (Apple) platforms or if explicitly requested.
        let is_apple = std::env::consts::OS == "macos" || std::env::consts::OS == "ios";
        let no_iobinding = std::env::var("GLINER2_NO_IOBINDING").is_ok();
        let prefer_iobinding = is_fp16 && !is_apple && !no_iobinding;

        let suffix = if is_fp16 { 
            if prefer_iobinding { "_fp16_iobinding.onnx" } else { "_fp16.onnx" }
        } else { 
            "_fp32.onnx" 
        };

        let mut files_to_download = vec![
            "tokenizer.json".to_string(),
        ];
        
        let bases = [
            "encoder",
            "token_gather",
            "span_rep",
            "schema_gather",
            "count_pred_argmax",
            "count_lstm_fixed",
            "scorer",
            "classifier"
        ];
        
        for base in bases.iter() {
            files_to_download.push(format!("{}{}", base, suffix));
        }

        let mut models_dir = std::path::PathBuf::new();

        for file in files_to_download {
            let path_in_repo = match subfolder {
                Some(sub) => format!("{}/{}", sub, file),
                None => file.clone(),
            };

            println!("Downloading/verifying {}...", path_in_repo);
            let local_path = repo.get(&path_in_repo).map_err(|e| {
                anyhow::anyhow!("Failed to download {}: {}", path_in_repo, e)
            })?;

            if models_dir.as_os_str().is_empty() {
                if let Some(parent) = local_path.parent() {
                    models_dir = parent.to_path_buf();
                }
            }
        }

        let config = Gliner2Config {
            models_dir: models_dir.to_string_lossy().to_string(),
            max_width: 8,
            model_type,
        };

        Self::new(config)
    }

    // ── Costruzione ──────────────────────────────────────────────────────────

    /// Carica l'engine dalla directory dei modelli v2.
    ///
    /// Cerca automaticamente nella priorità: `_fp16_iobinding.onnx`,
    /// poi `_fp16.onnx`, poi `_fp32.onnx`.
    pub fn new(config: Gliner2Config) -> Result<Self> {
        let dir = Path::new(&config.models_dir);

        let load = |base: &str| -> Result<Session> {
            // Priorità: iobinding FP16 > FP16 > FP32
            let candidates = [
                dir.join(format!("{}_fp16_iobinding.onnx", base)),
                dir.join(format!("{}_fp16.onnx", base)),
                dir.join(format!("{}_fp32.onnx", base)),
            ];

            let path = candidates.iter()
                .find(|p| p.exists())
                .ok_or_else(|| anyhow::anyhow!(
                    "Nessun modello trovato per '{}' in {:?}", base, dir
                ))?;

            let force_cpu = std::env::var("FORCE_CPU").is_ok();

            // QNN/Hexagon NPU **NON usato** per GLiNER2: i modelli sono
            // FP32 e non possono essere quantizzati INT8 senza perdere
            // precisione (verifica utente 2026-04-27). HTP ottimizza
            // principalmente INT8, su FP32 il guadagno è negativo.
            // Bench Snapdragon X Elite: CPU vince ~10% vs QNN/HTP.
            // Vedi memory `reference_qnn_npu_benchmark_2026_04_27`.
            let mut builder = Session::builder()?
                .with_optimization_level(GraphOptimizationLevel::Level3).map_err(oa)?
                .with_memory_pattern(false).map_err(oa)?;

            if force_cpu {
                builder = builder.with_execution_providers([
                    CPU::default().build(),
                ]).map_err(oa)?;
            } else if cfg!(target_os = "macos") || cfg!(target_os = "ios") {
                // The official macOS ONNX Runtime bundle carries CoreML in
                // libonnxruntime itself. Do not probe OpenVINO/CUDA/ROCm or
                // XNNPACK: those providers are not shipped for this target
                // and every session would otherwise emit a loader error
                // before falling through.
                builder = builder.with_execution_providers([
                    CoreML::default().build(),
                    CPU::default().build(),
                ]).map_err(oa)?;
            } else {
                // Multi-backend cross-platform per future build:
                // - OpenVINO (Intel CPU+iGPU)
                // - CoreML (Apple Silicon ANE+GPU)
                // - CUDA (NVIDIA discrete)
                // - ROCm (AMD)
                // - XNNPACK (CPU SIMD ottimizzato)
                // - CPU (fallback finale)
                builder = builder.with_execution_providers([
                    OpenVINO::default().build(),
                    CoreML::default().build(),
                    CUDA::default().build(),
                    ROCm::default().build(),
                    XNNPACK::default().build(),
                    CPU::default().build(),
                ]).map_err(oa)?;
            }

            println!("  Caricamento {:?}", path.file_name().unwrap_or_default());
            builder.commit_from_file(path)
                .map_err(|e| anyhow::anyhow!("Errore caricamento {:?}: {}", path, e))
        };

        // Singularity: tokenizer.json (16 MB of JSON) is parsed on its own thread
        // while the eight sessions are built, instead of after them - about
        // 0.15 s off every launch. Each side reads only its own files; a
        // session failure still returns its error once the tokenizer thread
        // has been joined by the scope.
        let (encoder, token_gather, span_rep, schema_gather, count_pred_argmax,
             count_lstm_fixed, scorer, classifier, tokenizer) = std::thread::scope(|scope| -> Result<_> {
            let tokenizer_loading = scope.spawn(|| Tokenizer::from_file(dir.join("tokenizer.json")));

            let encoder           = load("encoder")?;
            let token_gather      = load("token_gather")?;
            let span_rep          = load("span_rep")?;
            let schema_gather     = load("schema_gather")?;
            let count_pred_argmax = load("count_pred_argmax")?;
            let count_lstm_fixed  = load("count_lstm_fixed")?;
            let scorer            = load("scorer")?;
            let classifier        = load("classifier")?;

            let tokenizer = tokenizer_loading.join()
                .map_err(|_| anyhow::anyhow!("Errore tokenizer: panic durante il caricamento"))?
                .map_err(|e| anyhow::anyhow!("Errore tokenizer: {}", e))?;

            Ok((encoder, token_gather, span_rep, schema_gather, count_pred_argmax,
                count_lstm_fixed, scorer, classifier, tokenizer))
        })?;

        Ok(Self {
            encoder: Mutex::new(encoder),
            token_gather: Mutex::new(token_gather),
            span_rep: Mutex::new(span_rep),
            schema_gather: Mutex::new(schema_gather),
            count_pred_argmax: Mutex::new(count_pred_argmax),
            count_lstm_fixed: Mutex::new(count_lstm_fixed),
            scorer: Mutex::new(scorer),
            classifier: Mutex::new(classifier),
            tokenizer,
            config,
            execution_mode: RwLock::new(ExecutionMode::IoBinding),
        })
    }

    // ── API pubblica ─────────────────────────────────────────────────────────

    pub fn compare_encoder_paths_b1(
        &self, text: &str, tasks: &[SchemaTask],
    ) -> Result<EncoderEquivalenceReport> {
        let transformer = SchemaTransformer::new(self.tokenizer.clone());
        let record = transformer.transform(text, tasks)?;
        let seq_len = record.input_ids.len();
        if seq_len > MAX_ENCODER_POSITIONS {
            anyhow::bail!("diagnostic sample exceeds the encoder window");
        }
        let input_ids = Array2::from_shape_vec((1, seq_len), record.input_ids.clone())?;
        let attention_mask = Array2::from_shape_vec((1, seq_len), record.attention_mask.clone())?;
        let mut encoder = self.encoder.lock().unwrap();

        let (old_values, old_dtype) = {
            let input_ids_t = Tensor::from_array(input_ids.clone())?;
            let attention_mask_t = Tensor::from_array(attention_mask.clone())?;
            let output_name = encoder.outputs().get(0)
                .map(|output| output.name().to_string())
                .ok_or_else(|| anyhow::anyhow!("encoder has no output"))?;
            let device_mem = MemoryInfo::new(
                AllocationDevice::CUDA, 0, AllocatorType::Device, MemoryType::Default)
                .map_err(oa)?;
            let mut binding = encoder.create_binding().map_err(oa)?;
            binding.bind_input("input_ids", &input_ids_t).map_err(oa)?;
            binding.bind_input("attention_mask", &attention_mask_t).map_err(oa)?;
            binding.bind_output_to_device(&output_name, &device_mem).map_err(oa)?;
            let values = encoder.run_binding(&binding).map_err(oa)?;
            let output = values.get(output_name.as_str())
                .ok_or_else(|| anyhow::anyhow!("IoBinding encoder output missing"))?;
            if let Ok(value) = output.try_extract_array::<f32>() {
                (value.into_owned(), "f32")
            } else if let Ok(value) = output.try_extract_array::<half::f16>() {
                (value.into_owned().mapv(|item| item.to_f32()), "f16")
            } else {
                anyhow::bail!("IoBinding encoder output has unsupported dtype");
            }
        };

        let (new_values, new_dtype) = {
            let outputs = encoder.run(ort::inputs![
                "input_ids" => Tensor::from_array(input_ids)?,
                "attention_mask" => Tensor::from_array(attention_mask)?
            ])?;
            if let Some(output) = outputs.get("last_hidden_state") {
                if let Ok(value) = output.try_extract_array::<f32>() {
                    (value.into_owned(), "f32")
                } else if let Ok(value) = output.try_extract_array::<half::f16>() {
                    (value.into_owned().mapv(|item| item.to_f32()), "f16")
                } else {
                    anyhow::bail!("batched encoder output has unsupported dtype");
                }
            } else {
                let output = outputs.values().next()
                    .ok_or_else(|| anyhow::anyhow!("batched encoder output missing"))?;
                if let Ok(value) = output.try_extract_array::<f32>() {
                    (value.into_owned(), "f32")
                } else if let Ok(value) = output.try_extract_array::<half::f16>() {
                    (value.into_owned().mapv(|item| item.to_f32()), "f16")
                } else {
                    anyhow::bail!("batched encoder output has unsupported dtype");
                }
            }
        };

        let old_shape = old_values.shape();
        let new_shape = new_values.shape();
        if old_shape.len() != 3 || new_shape.len() != 3
                || old_values.len() != new_values.len() {
            anyhow::bail!("encoder comparison shape mismatch: {:?} vs {:?}", old_shape, new_shape);
        }
        let mut maximum = 0.0f64;
        let mut total = 0.0f64;
        for (&old, &new) in old_values.iter().zip(new_values.iter()) {
            let difference = (old as f64 - new as f64).abs();
            maximum = maximum.max(difference);
            total += difference;
        }
        Ok(EncoderEquivalenceReport {
            old_shape: [old_shape[0], old_shape[1], old_shape[2]],
            new_shape: [new_shape[0], new_shape[1], new_shape[2]],
            old_dtype,
            new_dtype,
            max_absolute_difference: maximum,
            mean_absolute_difference: if old_values.is_empty() { 0.0 }
                else { total / old_values.len() as f64 },
        })
    }

    pub fn compare_tail_paths_b1(
        &self, text: &str, tasks: &[SchemaTask],
    ) -> Result<String> {
        let transformer = SchemaTransformer::new(self.tokenizer.clone());
        let record = transformer.transform(text, tasks)?;
        let seq_len = record.input_ids.len();
        if seq_len > MAX_ENCODER_POSITIONS {
            anyhow::bail!("diagnostic sample exceeds the encoder window");
        }

        let standalone_hs = self.run_encoder_iobinding(&record)
            .map_err(|error| anyhow::anyhow!(error))?;
        let mut standalone_trace = TailDiagnosticTrace::default();
        let (standalone_entities, _, _) = self.run_tail_iobinding(
            text, &record, None, &standalone_hs, Some(&mut standalone_trace))
            .map_err(|error| anyhow::anyhow!(error))?;

        let batch_hs_array = {
            let input_ids = Array2::from_shape_vec((1, seq_len), record.input_ids.clone())?;
            let attention_mask = Array2::from_shape_vec(
                (1, seq_len), record.attention_mask.clone())?;
            let mut encoder = self.encoder.lock().unwrap();
            let outputs = encoder.run(ort::inputs![
                "input_ids" => Tensor::from_array(input_ids)?,
                "attention_mask" => Tensor::from_array(attention_mask)?
            ])?;
            if let Some(output) = outputs.get("last_hidden_state") {
                extract_f32!(output)
            } else {
                let output = outputs.values().next()
                    .ok_or_else(|| anyhow::anyhow!("batched encoder output missing"))?;
                extract_f32!(output)
            }
        };
        let batch_hs_array = batch_hs_array
            .slice(s![0..1, 0..seq_len, ..]).to_owned().into_dyn();
        let batch_hs = Tensor::from_array(batch_hs_array)?.into_dyn();
        let mut batch_trace = TailDiagnosticTrace::default();
        let (batch_entities, _, _) = self.run_tail_iobinding(
            text, &record, None, &batch_hs, Some(&mut batch_trace))
            .map_err(|error| anyhow::anyhow!(error))?;

        let old_input = standalone_trace.tensors.first()
            .ok_or_else(|| anyhow::anyhow!("standalone tail input was not captured"))?;
        let new_input = batch_trace.tensors.first()
            .ok_or_else(|| anyhow::anyhow!("batch tail input was not captured"))?;
        let mut report = String::new();
        report.push_str(&format!(
            "TAIL_INPUT standalone_value_type={} standalone_memory={} standalone_contiguous={} standalone_source=original_run_binding_encoder_DynValue standalone_bind=IoBinding::bind_input(last_hidden_state,DynValue)\n",
            old_input.value_type, old_input.memory, old_input.contiguous));
        report.push_str(&format!(
            "TAIL_INPUT batch_value_type={} batch_memory={} batch_contiguous={} batch_source=Session::run_f32_extract_slice_to_owned_Tensor::from_array_into_dyn batch_bind=IoBinding::bind_input(last_hidden_state,DynValue)\n",
            new_input.value_type, new_input.memory, new_input.contiguous));

        let mut first_divergence = None;
        let common_count = standalone_trace.tensors.len().min(batch_trace.tensors.len());
        for index in 0..common_count {
            let old = &standalone_trace.tensors[index];
            let new = &batch_trace.tensors[index];
            if old.name != new.name {
                let line = format!("STAGE_ORDER index={} standalone={} batch={}", index, old.name, new.name);
                report.push_str(&line);
                report.push('\n');
                if first_divergence.is_none() { first_divergence = Some(line); }
                continue;
            }
            let (line, differs) = diagnostic_tensor_comparison(old, new);
            report.push_str(&line);
            report.push('\n');
            if differs && first_divergence.is_none() { first_divergence = Some(line); }
        }
        if standalone_trace.tensors.len() != batch_trace.tensors.len() {
            let line = format!("STAGE_COUNT standalone={} batch={}",
                standalone_trace.tensors.len(), batch_trace.tensors.len());
            report.push_str(&line);
            report.push('\n');
            if first_divergence.is_none() { first_divergence = Some(line); }
        }
        if !standalone_trace.tensors.iter().any(|stage| stage.name.starts_with("classifier["))
                && !batch_trace.tensors.iter().any(|stage| stage.name.starts_with("classifier[")) {
            report.push_str("STAGE classifier not_applicable_for_entities_task\n");
        }
        let (candidate_line, candidate_differs) = diagnostic_entity_comparison(
            "pre_postprocessing_candidates", &standalone_trace.candidates, &batch_trace.candidates);
        report.push_str(&candidate_line);
        report.push('\n');
        if candidate_differs && first_divergence.is_none() {
            first_divergence = Some(candidate_line);
        }
        let (final_line, final_differs) = diagnostic_entity_comparison(
            "final_entities", &standalone_entities, &batch_entities);
        report.push_str(&final_line);
        report.push('\n');
        if final_differs && first_divergence.is_none() {
            first_divergence = Some(final_line);
        }
        report.push_str(&format!("FIRST_DIVERGENCE {}\n",
            first_divergence.unwrap_or_else(|| "none".to_owned())));
        Ok(report)
    }

    pub fn localize_batch_paths_b2(
        &self, texts: &[String], tasks: &[SchemaTask],
    ) -> Result<String> {
        if texts.len() != 2 {
            anyhow::bail!("B=2 localization requires exactly two samples");
        }
        let transformer = SchemaTransformer::new(self.tokenizer.clone());
        let records: Vec<ProcessedRecord> = texts.iter()
            .map(|text| transformer.transform(text, tasks)).collect::<Result<_>>()?;
        let sequence_lengths: Vec<usize> = records.iter()
            .map(|record| record.input_ids.len()).collect();
        let max_seq_len = *sequence_lengths.iter().max().unwrap_or(&0);
        let mut input_ids = Array2::<i64>::zeros((2, max_seq_len));
        let mut attention_mask = Array2::<i64>::zeros((2, max_seq_len));
        for (sample_index, record) in records.iter().enumerate() {
            for (token_index, &token) in record.input_ids.iter().enumerate() {
                input_ids[[sample_index, token_index]] = token;
            }
            for (token_index, &mask) in record.attention_mask.iter().enumerate() {
                attention_mask[[sample_index, token_index]] = mask;
            }
        }

        let mut report = format!(
            "B2_INPUT sample0_seq_len={} sample1_seq_len={} smax={} input_ids_shape=[2,{}] attention_mask_shape=[2,{}]\n",
            sequence_lengths[0], sequence_lengths[1], max_seq_len, max_seq_len, max_seq_len);
        for sample_index in 0..2 {
            let row: Vec<i64> = attention_mask.row(sample_index).iter().copied().collect();
            report.push_str(&format!("B2_ATTENTION_MASK sample={} row={:?}\n", sample_index, row));
            report.push_str(&format!(
                "B2_SAMPLE sample={} record_address={:p} record_input_len={} record_words={} slice=[{},{};0,{};all_hidden] padded_positions_excluded={}\n",
                sample_index, &records[sample_index], records[sample_index].input_ids.len(),
                records[sample_index].word_to_token_maps.len(), sample_index, sample_index + 1,
                sequence_lengths[sample_index], sequence_lengths[sample_index] <= max_seq_len));
        }
        report.push_str("B2_OWNERSHIP prepared_record_per_sample=true tail_uses_matching_sample_record=true result_vector_per_sample=true dedup_scope=per_sample offset_base_scope=prepared_chunk\n");

        let mut standalone_hs = Vec::with_capacity(2);
        for record in &records {
            standalone_hs.push(self.run_encoder_iobinding(record)
                .map_err(|error| anyhow::anyhow!(error))?);
        }
        let batched_hs = {
            let mut encoder = self.encoder.lock().unwrap();
            let outputs = encoder.run(ort::inputs![
                "input_ids" => Tensor::from_array(input_ids)?,
                "attention_mask" => Tensor::from_array(attention_mask)?
            ])?;
            if let Some(output) = outputs.get("last_hidden_state") {
                extract_f32!(output)
            } else {
                let output = outputs.values().next()
                    .ok_or_else(|| anyhow::anyhow!("batched encoder output missing"))?;
                extract_f32!(output)
            }
        };
        report.push_str(&format!("B2_ENCODER_BATCH_OUTPUT shape={:?} dtype=f32\n", batched_hs.shape()));

        let mut batch_hs = Vec::with_capacity(2);
        let mut encoder_first_divergence = None;
        for sample_index in 0..2 {
            let seq_len = sequence_lengths[sample_index];
            let sample_array = batched_hs
                .slice(s![sample_index..sample_index + 1, 0..seq_len, ..])
                .to_owned().into_dyn();
            let sample_value = Tensor::from_array(sample_array)?.into_dyn();
            let old = diagnostic_tensor_snapshot(
                format!("encoder_sample_{}", sample_index), &standalone_hs[sample_index])?;
            let new = diagnostic_tensor_snapshot(
                format!("encoder_sample_{}", sample_index), &sample_value)?;
            let (line, differs) = diagnostic_tensor_comparison(&old, &new);
            report.push_str(&line);
            report.push('\n');
            report.push_str(&format!(
                "B2_ENCODER_SLICE sample={} range=[{},{};0,{};all_hidden] excludes_padding={}\n",
                sample_index, sample_index, sample_index + 1, seq_len, seq_len < max_seq_len));
            if differs && encoder_first_divergence.is_none() {
                let first = old.values.iter().zip(new.values.iter()).enumerate()
                    .find(|(_, (left, right))| left != right);
                if let Some((flat_index, (&left, &right))) = first {
                    let hidden_size = *old.shape.last().unwrap_or(&1);
                    let token_index = (flat_index / hidden_size) % seq_len;
                    let hidden_index = flat_index % hidden_size;
                    encoder_first_divergence = Some(format!(
                        "sample={} token_index={} hidden_index={} flat_index={} standalone={} batch={} source_owner=batched_encoder_boundary",
                        sample_index, token_index, hidden_index, flat_index, left, right));
                } else {
                    encoder_first_divergence = Some(format!(
                        "sample={} shape_or_dtype source_owner=batched_encoder_boundary", sample_index));
                }
            }
            batch_hs.push(sample_value);
        }
        if let Some(first) = encoder_first_divergence {
            report.push_str(&format!("FIRST_DIVERGENCE {}\nTAIL_SKIPPED encoder_outputs_not_equivalent=true\n", first));
            return Ok(report);
        }

        let params = Some(InferenceParams { threshold: 0.0, flat_ner: false });
        let mut standalone_traces = Vec::with_capacity(2);
        let mut standalone_entities = Vec::with_capacity(2);
        let mut batch_traces = Vec::with_capacity(2);
        let mut batch_tail_entities = Vec::with_capacity(2);
        for sample_index in 0..2 {
            let mut old_trace = TailDiagnosticTrace::default();
            let (old_entities, _, _) = self.run_tail_iobinding(
                &texts[sample_index], &records[sample_index], params.clone(),
                &standalone_hs[sample_index], Some(&mut old_trace))
                .map_err(|error| anyhow::anyhow!(error))?;
            let mut new_trace = TailDiagnosticTrace::default();
            let (new_entities, _, _) = self.run_tail_iobinding(
                &texts[sample_index], &records[sample_index], params.clone(),
                &batch_hs[sample_index], Some(&mut new_trace))
                .map_err(|error| anyhow::anyhow!(error))?;
            standalone_traces.push(old_trace);
            standalone_entities.push(old_entities);
            batch_traces.push(new_trace);
            batch_tail_entities.push(new_entities);
        }
        let batch_aggregated: Vec<Vec<ExtractedEntity>> = batch_tail_entities.iter().cloned()
            .map(dedup_entities_by_identity).collect();

        for sample_index in 0..2 {
            let old_trace = &standalone_traces[sample_index];
            let new_trace = &batch_traces[sample_index];
            for stage_index in 1..old_trace.tensors.len().min(new_trace.tensors.len()) {
                let old = &old_trace.tensors[stage_index];
                let new = &new_trace.tensors[stage_index];
                let (line, differs) = diagnostic_tensor_comparison(old, new);
                report.push_str(&format!("B2_SAMPLE {} {}\n", sample_index, line));
                if differs {
                    report.push_str(&format!(
                        "FIRST_DIVERGENCE sample={} {} source_owner=shared_iobinding_tail\n",
                        sample_index, line));
                    return Ok(report);
                }
            }
            let (line, differs) = diagnostic_entity_comparison(
                "pre_postprocessing_candidates", &old_trace.candidates, &new_trace.candidates);
            report.push_str(&format!("B2_SAMPLE {} {}\n", sample_index, line));
            if differs {
                report.push_str(&format!(
                    "FIRST_DIVERGENCE sample={} {} source_owner=shared_tail_postprocessing\n",
                    sample_index, line));
                return Ok(report);
            }
            let (line, differs) = diagnostic_entity_comparison(
                "shared_tail_entities", &standalone_entities[sample_index],
                &batch_tail_entities[sample_index]);
            report.push_str(&format!("B2_SAMPLE {} {}\n", sample_index, line));
            if differs {
                report.push_str(&format!(
                    "FIRST_DIVERGENCE sample={} {} source_owner=shared_tail_entities\n",
                    sample_index, line));
                return Ok(report);
            }
            let (line, differs) = diagnostic_entity_comparison(
                "per_sample_batch_aggregation", &standalone_entities[sample_index],
                &batch_aggregated[sample_index]);
            report.push_str(&format!("B2_SAMPLE {} {}\n", sample_index, line));
            if differs {
                if sample_index == 0 {
                    let extra = batch_aggregated[0].iter().find(|entity| {
                        !standalone_entities[0].iter().any(|candidate|
                            candidate.start_char == entity.start_char
                                && candidate.end_char == entity.end_char
                                && candidate.label == entity.label)
                    });
                    if let Some(extra) = extra {
                        let origin = batch_traces[0].candidates.iter().find(|candidate|
                            candidate.start_char == extra.start_char
                                && candidate.end_char == extra.end_char
                                && candidate.label == extra.label);
                        let appears_sample1 = batch_aggregated[1].iter().any(|entity|
                            entity.text == extra.text && entity.label == extra.label);
                        let in_old_candidates = standalone_traces[0].candidates.iter().any(|candidate|
                            candidate.start_char == extra.start_char
                                && candidate.end_char == extra.end_char
                                && candidate.label == extra.label);
                        report.push_str(&format!(
                            "EXTRA_ENTITY label={} char=[{},{}] confidence={} text={:?} origin={:?} appears_in_sample1={} exists_in_sample0_standalone_candidates={}\n",
                            extra.label, extra.start_char, extra.end_char, extra.score, extra.text,
                            origin.map(|item| (item.start_tok, item.end_tok, item.score)),
                            appears_sample1, in_old_candidates));
                    } else {
                        report.push_str("EXTRA_ENTITY no_new_identity_found=true\n");
                    }
                }
                report.push_str(&format!(
                    "FIRST_DIVERGENCE sample={} {} source_owner=per_sample_batch_aggregation\n",
                    sample_index, line));
                return Ok(report);
            }
        }
        report.push_str("FIRST_DIVERGENCE none\n");
        Ok(report)
    }

    /// Public entry point. Bounds every real encoder invocation to this
    /// pinned checkpoint's trained/validated window
    /// (`microsoft/mdeberta-v3-base`'s `max_position_embeddings`, matching
    /// this model family's own `max_length: 512` training configuration -
    /// see Training/config/training-v0.1.json) - see extract_with_progress() below
    /// for the chunking this enforces. A caller never sees a difference for
    /// input that already fits in one window; longer input is chunked,
    /// remapped and merged transparently.
    pub fn extract(
        &self,
        text: &str,
        tasks: &[SchemaTask],
        params: Option<InferenceParams>,
    ) -> Result<(Vec<ExtractedEntity>, Vec<ExtractedRelation>, Vec<ExtractedClassification>)> {
        self.extract_with_progress(text, tasks, params, &mut |_, _| {})
    }

    /// Runs ONE already-tokenized, already-in-budget chunk through the real
    /// engine: tries IOBinding, falls back silently to Standard on OOM -
    /// unchanged behavior from the pre-chunking extract(), just parameterized
    /// on a caller-supplied `record` instead of tokenizing `text` itself, so
    /// extract_with_progress() below can call this once per chunk without a second,
    /// divergent copy of the IOBinding/Standard dispatch logic.
    fn extract_one_chunk(
        &self,
        text: &str,
        record: &ProcessedRecord,
        params: Option<InferenceParams>,
    ) -> Result<(Vec<ExtractedEntity>, Vec<ExtractedRelation>, Vec<ExtractedClassification>)> {
        #[cfg(test)]
        CHUNK_CALL_COUNT.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        let mode = *self.execution_mode.read().unwrap_or_else(|p| p.into_inner());
        if diag_enabled() {
            eprintln!("RUSTMEMDIAG execute_mode_at_call_start={:?} utf8_bytes={} tokens={}",
                mode, text.len(), record.input_ids.len());
        }
        match mode {
            ExecutionMode::IoBinding => {
                match self.extract_iobinding(text, record, params) {
                    Ok(res) => Ok(res),
                    Err(GlinerError::OomDeviceBinding(msg)) => {
                        eprintln!(
                            "[GLiNER2-v2] OOM IOBinding, fallback Standard. Dettagli: {}",
                            msg
                        );
                        *self.execution_mode.write().unwrap_or_else(|p| p.into_inner()) = ExecutionMode::Standard;
                        self.extract_standard(text, record, params, None)
                    }
                    Err(other) => Err(anyhow::anyhow!(other)),
                }
            }
            ExecutionMode::Standard => self.extract_standard(text, record, params, None),
        }
    }

    /// Bounds every real encoder invocation to MAX_ENCODER_POSITIONS tokens
    /// (phase24_1d memory-growth investigation: an unbounded seq_len - this
    /// checkpoint has been fed thousands of tokens with nothing anywhere in
    /// this codebase or the pinned tokenizer's own config enforcing a limit -
    /// is what turned a normal research-loop prompt into multi-GB-per-call
    /// growth; feeding a transformer encoder far past its trained window is
    /// wrong on its own terms before any allocator behavior is even
    /// considered).
    ///
    /// Tokenizes the FULL text once (cheap - this is CPU-side subword
    /// tokenization, not the O(seq_len^2) attention pass that made the
    /// original unbounded call expensive) to get real per-word token counts
    /// from the actual SchemaTransformer/tokenizer path, then - only if the
    /// whole thing does not already fit - splits on WORD boundaries (GLiNER2
    /// entity spans are word-based, never token-based) into chunks whose own
    /// schema-overhead-plus-text encoding is provably <= the window, with
    /// max_width-1 words of overlap between consecutive chunks so an entity
    /// of the model's own maximum supported width (max_width words) can never
    /// be split across a boundary without appearing whole in at least one
    /// chunk. Each chunk is re-tokenized from its own text substring (not
    /// sliced out of the full tokenization) and run through the unmodified,
    /// real extract_one_chunk() - this is chunking the INPUT, never touching
    /// the ONNX sessions/ORT configuration themselves. Every returned entity
    /// offset is remapped from chunk-local back to the original text's own
    /// UTF-8 byte offsets before merging; overlap-duplicated detections
    /// (identical start/end/label) are deterministically collapsed to the
    /// single highest-confidence one. Nothing here persists any chunk-local
    /// state beyond this call's own stack.
    pub fn extract_with_progress(
        &self,
        text: &str,
        tasks: &[SchemaTask],
        params: Option<InferenceParams>,
        progress: &mut dyn FnMut(usize, usize),
    ) -> Result<(Vec<ExtractedEntity>, Vec<ExtractedRelation>, Vec<ExtractedClassification>)> {
        diag_checkpoint("before_preprocessing");
        let transformer = SchemaTransformer::new(self.tokenizer.clone());
        let full_record = transformer.transform(text, tasks)?;
        diag_checkpoint("after_preprocessing");

        // Everything in combined_tokens before the first real text word -
        // the schema prompt, every requested label/field, and the
        // [SEP_STRUCT]/[SEP_TEXT] separators - occupies a FIXED number of
        // encoder tokens no chunk can avoid paying: ProcessedRecord::text_start
        // is exactly that count in final subword-token space (see
        // processor.rs::transform()). A schema this large already leaves no
        // room for any text is a real, distinct failure - not "truncate the
        // text to nothing" - so it fails loudly here.
        let schema_overhead_tokens = full_record.text_start;
        if schema_overhead_tokens >= MAX_ENCODER_POSITIONS {
            anyhow::bail!(
                "This GLiNER2 schema (prompt + requested labels/fields + separators) alone encodes to {} tokens, at or beyond this checkpoint's {}-token encoder window ({} words of text would get zero budget). Reduce the number of requested labels/fields for this call.",
                schema_overhead_tokens, MAX_ENCODER_POSITIONS, full_record.word_to_token_maps.len());
        }
        let text_token_budget = MAX_ENCODER_POSITIONS - schema_overhead_tokens;
        let num_words = full_record.word_to_token_maps.len();

        if diag_enabled() {
            eprintln!("RUSTMEMDIAG [bounded] full_text utf8_bytes={} words={} schema_overhead_tokens={} full_encoder_tokens={} budget_for_text={}",
                text.len(), num_words, schema_overhead_tokens, full_record.input_ids.len(), text_token_budget);
        }

        if full_record.input_ids.len() <= MAX_ENCODER_POSITIONS {
            // Fits in one window - identical behavior/output to the
            // pre-chunking implementation: one real inference call, on the
            // exact same tokenization already computed above (never a
            // second, possibly-divergent re-tokenization for the common
            // case).
            SEMANTIC_CHUNKS_EXECUTED_TOTAL.fetch_add(1, Ordering::Relaxed);
            progress(0, 1);
            let result = self.extract_one_chunk(text, &full_record, params)?;
            progress(1, 1);
            return Ok(result);
        }
        if num_words == 0 {
            return Ok((Vec::new(), Vec::new(), Vec::new()));
        }

        // Word-boundary chunk plan: each chunk's word range [start, end) is
        // the longest run starting at `start` whose own per-word token
        // counts (from the SAME full tokenization, so this is exact, not
        // estimated) sum to <= text_token_budget. Consecutive chunks overlap
        // by (max_width - 1) words - the model's own contract: an entity
        // spans at most max_width words (Gliner2Config::max_width, pinned to
        // 8 - see SpanRep's span_idx construction above), so an entity that
        // starts within the last (max_width - 1) words of one chunk is still
        // fully contained within the NEXT chunk's word range, guaranteeing
        // every possible max-width span is whole in at least one chunk.
        let overlap_words = self.config.max_width.saturating_sub(1);
        let word_token_lens: Vec<usize> = full_record.word_to_token_maps.iter()
            .map(|&(s, e)| e - s).collect();
        let chunk_word_ranges = plan_word_chunks(
            &word_token_lens, text_token_budget, overlap_words, schema_overhead_tokens)?;

        if diag_enabled() {
            eprintln!("RUSTMEMDIAG [bounded] chunk_plan chunks={} overlap_words={}",
                chunk_word_ranges.len(), overlap_words);
        }

        let mut all_entities: Vec<ExtractedEntity> = Vec::new();
        let mut all_relations: Vec<ExtractedRelation> = Vec::new();
        let mut all_classifications: Vec<ExtractedClassification> = Vec::new();

        progress(0, chunk_word_ranges.len());
        for (chunk_index, &(start_word, end_word)) in chunk_word_ranges.iter().enumerate() {
            let char_start = full_record.word_to_char_maps[start_word].0;
            let char_end = full_record.word_to_char_maps[end_word - 1].1;
            // Always a valid UTF-8 boundary: word_to_char_maps comes from
            // WhitespaceTokenSplitter's regex match spans over the original
            // `text`, which regex guarantees land on char boundaries.
            let chunk_text = &text[char_start..char_end];
            diag_checkpoint("before_chunk_preprocessing");
            let chunk_record = transformer.transform(chunk_text, tasks)?;
            diag_checkpoint("after_chunk_preprocessing");
            if diag_enabled() {
                eprintln!("RUSTMEMDIAG [bounded] chunk {} words=[{},{}) chars=[{},{}) encoder_tokens={}",
                    chunk_index, start_word, end_word, char_start, char_end, chunk_record.input_ids.len());
            }
            if chunk_record.input_ids.len() > MAX_ENCODER_POSITIONS {
                // The word-level token counts above were measured against
                // the full-document tokenization; re-tokenizing the same
                // word range in isolation must reproduce the identical
                // per-word subword counts (tokenization here is applied one
                // word at a time with no cross-word context - see
                // processor.rs). Refuse rather than silently submit an
                // oversized sequence if that invariant is ever violated.
                anyhow::bail!(
                    "internal error: chunk {} re-tokenized to {} tokens, over the {}-token encoder window the chunk plan computed it would fit within",
                    chunk_index, chunk_record.input_ids.len(), MAX_ENCODER_POSITIONS);
            }

            SEMANTIC_CHUNKS_EXECUTED_TOTAL.fetch_add(1, Ordering::Relaxed);
            let (entities, relations, classifications) =
                self.extract_one_chunk(chunk_text, &chunk_record, params.clone())?;
            for mut entity in entities {
                // start_char/end_char are the offsets a caller actually sees
                // (see Gliner2Adapter.cpp: ModelEntity::start/length are
                // built from these) - remap to the original text. start_tok/
                // end_tok are chunk-local token indices used only inside NMS
                // above and never read past this function's return.
                entity.start_char += char_start;
                entity.end_char += char_start;
                all_entities.push(entity);
            }
            all_relations.extend(relations);
            all_classifications.extend(classifications);
            progress(chunk_index + 1, chunk_word_ranges.len());
        }

        Ok((dedup_entities_by_identity(all_entities), all_relations, all_classifications))
    }

    /// Runs the encoder with its existing dynamic batch axis, then splits and
    /// trims each [B,Smax,H] result back to [1,Si,H]. The existing Standard
    /// batch=1 tail consumes each sample independently with its own canonical
    /// token/word/schema metadata. Texts are never concatenated.
    pub fn extract_batch_with_progress(
        &self,
        texts: &[String],
        tasks: &[SchemaTask],
        params: Option<InferenceParams>,
        microbatch_size: usize,
        progress: &mut dyn FnMut(usize, usize),
    ) -> Result<Vec<ExtractionResult>> {
        if microbatch_size == 0 {
            anyhow::bail!("GLiNER2 encoder microbatch size must be positive");
        }
        let transformer = SchemaTransformer::new(self.tokenizer.clone());
        let mut prepared = Vec::<PreparedBatchChunk>::new();
        for (sample_index, text) in texts.iter().enumerate() {
            let full_record = transformer.transform(text, tasks)?;
            let schema_overhead_tokens = full_record.text_start;
            if schema_overhead_tokens >= MAX_ENCODER_POSITIONS {
                anyhow::bail!(
                    "This GLiNER2 schema encodes to {} tokens, at or beyond the {}-token encoder window",
                    schema_overhead_tokens, MAX_ENCODER_POSITIONS);
            }
            if full_record.input_ids.len() <= MAX_ENCODER_POSITIONS {
                if !full_record.word_to_token_maps.is_empty() {
                    prepared.push(PreparedBatchChunk {
                        sample_index, chunk_index: 0, char_start: 0,
                        text: text.clone(), record: full_record,
                    });
                }
                continue;
            }
            let text_token_budget = MAX_ENCODER_POSITIONS - schema_overhead_tokens;
            let overlap_words = self.config.max_width.saturating_sub(1);
            let word_token_lens: Vec<usize> = full_record.word_to_token_maps.iter()
                .map(|&(start, end)| end - start).collect();
            let ranges = plan_word_chunks(
                &word_token_lens, text_token_budget, overlap_words, schema_overhead_tokens)?;
            for (chunk_index, &(start_word, end_word)) in ranges.iter().enumerate() {
                let char_start = full_record.word_to_char_maps[start_word].0;
                let char_end = full_record.word_to_char_maps[end_word - 1].1;
                let chunk_text = text[char_start..char_end].to_owned();
                let record = transformer.transform(&chunk_text, tasks)?;
                if record.input_ids.len() > MAX_ENCODER_POSITIONS {
                    anyhow::bail!(
                        "internal error: batch chunk {} re-tokenized to {} tokens, over the {}-token encoder window",
                        chunk_index, record.input_ids.len(), MAX_ENCODER_POSITIONS);
                }
                prepared.push(PreparedBatchChunk {
                    sample_index, chunk_index, char_start, text: chunk_text, record,
                });
            }
        }

        let total_chunks = prepared.len();
        if std::env::var("SINGULARITY_GLINER2_SEQUENCE_HISTOGRAM_ONLY").as_deref() == Ok("1") {
            let mut histogram = std::collections::BTreeMap::<usize, usize>::new();
            for sample in &prepared {
                *histogram.entry(sample.record.input_ids.len()).or_default() += 1;
            }
            let histogram_text = histogram.iter()
                .map(|(length, count)| format!("{}:{}", length, count))
                .collect::<Vec<_>>().join(",");
            eprintln!(
                "GLINER2_SEQUENCE_HISTOGRAM semantic_samples={} semantic_chunks={} unique_lengths={} histogram={}",
                texts.len(), total_chunks, histogram.len(), histogram_text);
            anyhow::bail!("SINGULARITY_GLINER2_SEQUENCE_HISTOGRAM_CAPTURED");
        }
        let mut results: Vec<ExtractionResult> =
            (0..texts.len()).map(|_| (Vec::new(), Vec::new(), Vec::new())).collect();
        progress(0, total_chunks);
        let mut completed = 0usize;
        // Preserve canonical first-occurrence order both between groups and
        // inside each group. The HashMap is lookup-only: its iteration order
        // never controls execution or result order.
        let mut group_lookup = std::collections::HashMap::<ExactLengthBatchKey, usize>::new();
        let mut exact_length_groups = Vec::<Vec<usize>>::new();
        for (prepared_index, sample) in prepared.iter().enumerate() {
            let key = ExactLengthBatchKey {
                sequence_length: sample.record.input_ids.len(),
                schema_tasks: tasks.to_vec(),
                models_dir: self.config.models_dir.clone(),
                max_width: self.config.max_width,
                model_type: self.config.model_type.clone(),
            };
            let group_index = match group_lookup.get(&key) {
                Some(&existing) => existing,
                None => {
                    let created = exact_length_groups.len();
                    group_lookup.insert(key, created);
                    exact_length_groups.push(Vec::new());
                    created
                }
            };
            exact_length_groups[group_index].push(prepared_index);
        }

        for group in exact_length_groups {
          for batch_indices in group.chunks(microbatch_size) {
            // A singleton never enters the batch encoder: retain the complete
            // pre-batching single-sample dispatch and fallback semantics.
            if batch_indices.len() == 1 {
                let sample = &prepared[batch_indices[0]];
                let (entities, relations, classifications) =
                    self.extract_one_chunk(&sample.text, &sample.record, params.clone())?;
                let target = &mut results[sample.sample_index];
                for mut entity in entities {
                    entity.start_char += sample.char_start;
                    entity.end_char += sample.char_start;
                    target.0.push(entity);
                }
                target.1.extend(relations);
                target.2.extend(classifications);
                completed += 1;
                SEMANTIC_CHUNKS_EXECUTED_TOTAL.fetch_add(1, Ordering::Relaxed);
                progress(completed, total_chunks);
                continue;
            }

            let batch_size = batch_indices.len();
            let sequence_length = prepared[batch_indices[0]].record.input_ids.len();
            if batch_indices.iter().any(|&index|
                    prepared[index].record.input_ids.len() != sequence_length) {
                anyhow::bail!("internal error: exact-length encoder batch contains unequal sequence lengths");
            }
            let mut input_ids = Array2::<i64>::zeros((batch_size, sequence_length));
            let mut attention_mask = Array2::<i64>::zeros((batch_size, sequence_length));
            for (batch_index, &prepared_index) in batch_indices.iter().enumerate() {
                let sample = &prepared[prepared_index];
                for (token_index, &token) in sample.record.input_ids.iter().enumerate() {
                    input_ids[[batch_index, token_index]] = token;
                }
                for (token_index, &mask) in sample.record.attention_mask.iter().enumerate() {
                    attention_mask[[batch_index, token_index]] = mask;
                }
            }

            diag_checkpoint("before_batched_encoder_run");
            let batched_hs = {
                let mut encoder = self.encoder.lock().unwrap();
                let encoder_output = encoder.run(ort::inputs![
                    "input_ids" => Tensor::from_array(input_ids)?,
                    "attention_mask" => Tensor::from_array(attention_mask)?
                ])?;
                ENCODER_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
                ENCODER_BATCHES_TOTAL.fetch_add(1, Ordering::Relaxed);
                diag_checkpoint("after_batched_encoder_run");
                let mut found = None;
                for name in ["hidden_states", "last_hidden_state", "output"] {
                    if let Some(value) = encoder_output.get(name) {
                        found = Some(extract_f32!(value));
                        break;
                    }
                }
                match found {
                    Some(value) => value,
                    None => {
                        let value = encoder_output.values().next()
                            .ok_or_else(|| anyhow::anyhow!("batched encoder: no output found"))?;
                        extract_f32!(value)
                    }
                }
            };
            if batched_hs.ndim() != 3 || batched_hs.shape()[0] != batch_size
                    || batched_hs.shape()[1] != sequence_length {
                anyhow::bail!("batched encoder returned an unexpected output shape {:?}", batched_hs.shape());
            }

            for (batch_index, &prepared_index) in batch_indices.iter().enumerate() {
                let sample = &prepared[prepared_index];
                let sample_hs = batched_hs
                    .slice(s![batch_index..batch_index + 1, .., ..])
                    .to_owned().into_dyn();
                let sample_hs = Tensor::from_array(sample_hs)?.into_dyn();
                let (entities, relations, classifications) = self.run_tail_iobinding(
                    &sample.text, &sample.record, params.clone(), &sample_hs, None)?;
                let target = &mut results[sample.sample_index];
                for mut entity in entities {
                    entity.start_char += sample.char_start;
                    entity.end_char += sample.char_start;
                    target.0.push(entity);
                }
                target.1.extend(relations);
                target.2.extend(classifications);
                completed += 1;
                SEMANTIC_CHUNKS_EXECUTED_TOTAL.fetch_add(1, Ordering::Relaxed);
                progress(completed, total_chunks);
                if diag_enabled() {
                    eprintln!("RUSTMEMDIAG batched_sample_complete leaf={} chunk={} seq_len={} batch={}",
                        sample.sample_index, sample.chunk_index, sequence_length, batch_size);
                }
            }
          }
        }
        for result in &mut results {
            result.0 = dedup_entities_by_identity(std::mem::take(&mut result.0));
        }
        Ok(results)
    }

    // ── IOBinding ────────────────────────────────────────────────────────────

    /// Esegue la pipeline v2 con IOBinding Zero-Copy.
    ///
    /// I tensori float intermedi restano in memoria device (VRAM/NPU) tra un
    /// layer e il successivo. Gli unici trasferimenti device→host sono:
    ///   - `pred_count` (8 byte int64) per ogni task
    ///   - `entity_scores` (per NMS + soglia su CPU)
    ///
    /// Se qualsiasi passo di binding o esecuzione fallisce (dispositivo
    /// non disponibile, OOM, EP non supporta IOBinding), l'errore viene
    /// convertito in `GlinerError::OomDeviceBinding` e `extract()` effettua
    /// automaticamente il fallback a `extract_standard()`.
    ///
    /// # Nota ROCm
    /// Attualmente prova prima CUDA (`AllocationDevice::CUDA`).
    /// Su macchine ROCm-only il primo `run()` fallisce → fallback Standard.
    /// TODO: rilevare l'EP attivo e usare `AllocationDevice::HIP` per ROCm.
    // `record` must already be within this engine's MAX_ENCODER_POSITIONS
    // budget - extract_with_progress() (the only caller reachable from the public
    // extract()) guarantees this by chunking before ever calling here; this
    // function's own guard below is the last-resort refusal, not the
    // enforcement point.
    fn extract_iobinding(
        &self,
        text: &str,
        record: &ProcessedRecord,
        params: Option<InferenceParams>,
    ) -> Result<(Vec<ExtractedEntity>, Vec<ExtractedRelation>, Vec<ExtractedClassification>), GlinerError> {
        let seq_len = record.input_ids.len();
        if seq_len > MAX_ENCODER_POSITIONS {
            return Err(GlinerError::Other(anyhow::anyhow!(
                "internal error: a {}-token chunk reached extract_iobinding(), over the {}-token encoder window - extract_with_progress()'s own chunking invariant was violated",
                seq_len, MAX_ENCODER_POSITIONS)));
        }
        let num_words = record.word_to_token_maps.len();
        if num_words == 0 {
            return Ok((Vec::new(), Vec::new(), Vec::new()));
        }
        if diag_enabled() {
            eprintln!("RUSTMEMDIAG [iobinding] chunk: utf8_bytes={} seq_len(tokens)={} num_words={} tasks={}",
                text.len(), seq_len, num_words, record.tasks.len());
        }

        let hs_val = self.run_encoder_iobinding(record)?;
        self.run_tail_iobinding(text, record, params, &hs_val, None)
    }

    fn run_encoder_iobinding(
        &self,
        record: &ProcessedRecord,
    ) -> Result<DynValue, GlinerError> {
        let mut g_encoder = self.encoder.lock().unwrap();
        macro_rules! oe {
            ($expr:expr, $ctx:literal) => {
                $expr.map_err(|e| GlinerError::OomDeviceBinding(
                    format!("{}: {}", $ctx, e)
                ))?
            };
        }

        let device_mem = oe!(
            MemoryInfo::new(AllocationDevice::CUDA, 0, AllocatorType::Device, MemoryType::Default),
            "device MemoryInfo"
        );
        let seq_len = record.input_ids.len();
        let input_ids_arr = Array2::from_shape_vec((1, seq_len), record.input_ids.clone())
            .map_err(|e| GlinerError::Other(anyhow::anyhow!(e)))?;
        let attn_mask_arr = Array2::from_shape_vec((1, seq_len), record.attention_mask.clone())
            .map_err(|e| GlinerError::Other(anyhow::anyhow!(e)))?;
        diag_shape("[iobinding] encoder_input_ids", input_ids_arr.shape());

        let input_ids_t = oe!(Tensor::from_array(input_ids_arr), "encoder input_ids tensor");
        let attn_mask_t = oe!(Tensor::from_array(attn_mask_arr), "encoder attn_mask tensor");
        let enc_out_name = g_encoder.outputs().get(0)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("encoder: nessun output registrato".into()))?;
        let mut b_enc = oe!(g_encoder.create_binding(), "encoder create_binding");
        oe!(b_enc.bind_input("input_ids", &input_ids_t), "encoder bind input_ids");
        oe!(b_enc.bind_input("attention_mask", &attn_mask_t), "encoder bind attention_mask");
        oe!(b_enc.bind_output_to_device(&enc_out_name, &device_mem), "encoder bind output");

        diag_checkpoint("[iobinding] before_encoder_run_binding");
        let mut out = oe!(g_encoder.run_binding(&b_enc), "encoder run");
        ENCODER_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
        let hs_val = out.remove(enc_out_name.as_str())
            .ok_or_else(|| GlinerError::OomDeviceBinding(
                format!("encoder: output '{}' non trovato", enc_out_name)
            ))?;
        diag_checkpoint("[iobinding] after_encoder_run_binding (device_mem tagged AllocationDevice::CUDA)");
        Ok(hs_val)
    }

    fn run_tail_iobinding(
        &self,
        text: &str,
        record: &ProcessedRecord,
        params: Option<InferenceParams>,
        hs_val: &DynValue,
        mut diagnostic_trace: Option<&mut TailDiagnosticTrace>,
    ) -> Result<(Vec<ExtractedEntity>, Vec<ExtractedRelation>, Vec<ExtractedClassification>), GlinerError> {
        // Un solo lock per downstream sessione, preso qui: Mutex non rientrante.
        let mut g_token_gather = self.token_gather.lock().unwrap();
        let mut g_span_rep = self.span_rep.lock().unwrap();
        let mut g_schema_gather = self.schema_gather.lock().unwrap();
        let mut g_count_pred_argmax = self.count_pred_argmax.lock().unwrap();
        let mut g_count_lstm_fixed = self.count_lstm_fixed.lock().unwrap();
        let mut g_scorer = self.scorer.lock().unwrap();
        let mut g_classifier = self.classifier.lock().unwrap();
        let p = params.unwrap_or_default();
        let threshold = p.threshold;
        let flat_ner = p.flat_ner;
        // Mappa errori ORT → OomDeviceBinding (qualsiasi fallimento qui → fallback Standard)
        macro_rules! oe {
            ($expr:expr, $ctx:literal) => {
                $expr.map_err(|e| GlinerError::OomDeviceBinding(
                    format!("{}: {}", $ctx, e)
                ))?
            };
        }

        diag_checkpoint("[iobinding] extract_iobinding_entry");
        // ── MemoryInfo ────────────────────────────────────────────────────────
        // device_mem: tensori intermedi restano su GPU/NPU
        // cpu_out_mem: tensori che devono rientrare su CPU (pred_count, scores)
        //
        // TEMPORARY DIAGNOSTIC NOTE (phase24_1d memory-growth investigation):
        // this unconditionally tags device_mem AllocationDevice::CUDA - on a
        // machine with no CUDA device (this build's execution providers are
        // CoreML+CPU on macOS, see Session::builder() above), every
        // bind_output_to_device(&device_mem) call below is asking ORT to
        // place an output on a device/allocator combination that does not
        // exist on this hardware. Not changed yet - this run exists to find
        // out where in the 8-session sequence that mismatch's memory cost
        // actually lands.
        let device_mem = oe!(
            MemoryInfo::new(AllocationDevice::CUDA, 0, AllocatorType::Device, MemoryType::Default),
            "device MemoryInfo"
        );
        let cpu_out_mem = oe!(
            MemoryInfo::new(AllocationDevice::CPU, 0, AllocatorType::Device, MemoryType::CPUOutput),
            "cpu_out MemoryInfo"
        );
        diag_checkpoint("[iobinding] after_memoryinfo_construction");

        // ── Tokenizzazione: già calcolata dal chiamante ─────────────────────
        let num_words = record.word_to_token_maps.len();
        diag_checkpoint("[iobinding] after_memoryinfo_and_tokenize");
        capture_diagnostic_tensor(&mut diagnostic_trace, "tail_input", hs_val)?;

        // ── Step 2: TokenGather ───────────────────────────────────────────────
        let word_starts: Vec<i64> = record.word_to_token_maps.iter()
            .map(|&(s, _)| s as i64).collect();
        let word_idx_t = oe!(
            Tensor::from_array(Array1::from_vec(word_starts)),
            "token_gather word_idx tensor"
        );

        let tg_out_name = g_token_gather.outputs().get(0)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("token_gather: nessun output".into()))?;

        let mut b_tg = oe!(g_token_gather.create_binding(), "token_gather create_binding");
        oe!(b_tg.bind_input("last_hidden_state", hs_val), "token_gather bind last_hidden_state");
        oe!(b_tg.bind_input("word_indices", &word_idx_t), "token_gather bind word_indices");
        oe!(b_tg.bind_output_to_device(&tg_out_name, &device_mem), "token_gather bind output");

        diag_checkpoint("[iobinding] before_token_gather_run_binding");
        let text_embs_val = {
            let mut out = oe!(g_token_gather.run_binding(&b_tg), "token_gather run");
            out.remove(tg_out_name.as_str())
                .ok_or_else(|| GlinerError::OomDeviceBinding(
                    format!("token_gather: output '{}' non trovato", tg_out_name)
                ))?
        };
        capture_diagnostic_tensor(&mut diagnostic_trace, "token_gather", &text_embs_val)?;
        diag_checkpoint("[iobinding] after_token_gather_run_binding");
        DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);

        // ── Step 3: SpanRep ───────────────────────────────────────────────────
        let num_spans = num_words * self.config.max_width;
        let mut span_idx_data = Vec::with_capacity(num_spans * 2);
        for start in 0..num_words {
            for width in 0..self.config.max_width {
                let end = start + width;
                if end >= num_words {
                    span_idx_data.extend_from_slice(&[0i64, 0i64]);
                } else {
                    span_idx_data.push(start as i64);
                    span_idx_data.push(end as i64);
                }
            }
        }
        let span_idx_arr = Array3::from_shape_vec((1, num_spans, 2), span_idx_data)
            .map_err(|e| GlinerError::Other(anyhow::anyhow!(e)))?;
        let span_idx_t = oe!(Tensor::from_array(span_idx_arr), "span_rep span_idx tensor");

        let sr_out_name = g_span_rep.outputs().get(0)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("span_rep: nessun output".into()))?;

        let mut b_sr = oe!(g_span_rep.create_binding(), "span_rep create_binding");
        oe!(b_sr.bind_input("hidden_states", &text_embs_val), "span_rep bind hidden_states");
        oe!(b_sr.bind_input("span_idx", &span_idx_t), "span_rep bind span_idx");
        oe!(b_sr.bind_output_to_device(&sr_out_name, &device_mem), "span_rep bind output");

        diag_shape("[iobinding] span_rep_input_span_idx", &[1, num_spans, 2]);
        diag_checkpoint("[iobinding] before_span_rep_run_binding");
        let span_embs_val = {
            let mut out = oe!(g_span_rep.run_binding(&b_sr), "span_rep run");
            out.remove(sr_out_name.as_str())
                .ok_or_else(|| GlinerError::OomDeviceBinding(
                    format!("span_rep: output '{}' non trovato", sr_out_name)
                ))?
        };
        capture_diagnostic_tensor(&mut diagnostic_trace, "span_rep", &span_embs_val)?;
        diag_checkpoint("[iobinding] after_span_rep_run_binding");
        DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);

        // ── Nomi output sessioni per-task (raccolti una sola volta) ───────────
        let sg_pc_name = g_schema_gather.outputs().get(0)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("schema_gather: nessun output 0".into()))?;
        let sg_fe_name = g_schema_gather.outputs().get(1)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("schema_gather: nessun output 1".into()))?;
        let cp_out_name = g_count_pred_argmax.outputs().get(0)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("count_pred_argmax: nessun output".into()))?;
        let cl_out_name = g_count_lstm_fixed.outputs().get(0)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("count_lstm_fixed: nessun output".into()))?;
        let sc_out_name = g_scorer.outputs().get(0)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("scorer: nessun output".into()))?;
        let cls_out_name = g_classifier.outputs().get(0)
            .map(|o| o.name().to_string())
            .ok_or_else(|| GlinerError::OomDeviceBinding("classifier: nessun output".into()))?;

        // ── Step 4: Loop per-task ─────────────────────────────────────────────
        let mut final_entities       = Vec::new();
        let mut final_relations      = Vec::new();
        let mut final_classifications = Vec::new();

        for task_map in &record.tasks {
            let num_labels = task_map.labels.len();
            let is_cls = task_map.task_type == "classifications";

            // 4a. SchemaGather
            let schema_indices: Vec<i64> =
                std::iter::once(task_map.prompt_tok_idx as i64)
                .chain(task_map.field_tok_indices.iter().map(|&i| i as i64))
                .collect();
            let schema_idx_t = oe!(
                Tensor::from_array(Array1::from_vec(schema_indices)),
                "schema_gather schema_indices tensor"
            );

            // field_embs: device per entity, CPU per classification (serve sul host per padding)
            let field_mem: &MemoryInfo = if is_cls { &cpu_out_mem } else { &device_mem };

            let mut b_sg = oe!(g_schema_gather.create_binding(), "schema_gather create_binding");
            oe!(b_sg.bind_input("last_hidden_state", hs_val), "schema_gather bind last_hidden_state");
            oe!(b_sg.bind_input("schema_indices", &schema_idx_t), "schema_gather bind schema_indices");
            oe!(b_sg.bind_output_to_device(&sg_pc_name, &device_mem), "schema_gather bind pc_emb");
            oe!(b_sg.bind_output_to_device(&sg_fe_name, field_mem), "schema_gather bind field_embs");

            diag_checkpoint("[iobinding] before_schema_gather_run_binding");
            let (pc_emb_val, field_embs_val) = {
                let mut out = oe!(g_schema_gather.run_binding(&b_sg), "schema_gather run");
                let pc = out.remove(sg_pc_name.as_str())
                    .ok_or_else(|| GlinerError::OomDeviceBinding(
                        format!("schema_gather: output '{}' non trovato", sg_pc_name)
                    ))?;
                let fe = out.remove(sg_fe_name.as_str())
                    .ok_or_else(|| GlinerError::OomDeviceBinding(
                        format!("schema_gather: output '{}' non trovato", sg_fe_name)
                    ))?;
                (pc, fe)
            };
            capture_diagnostic_tensor(&mut diagnostic_trace,
                format!("schema_gather[{}].pc_emb", task_map.task_name), &pc_emb_val)?;
            capture_diagnostic_tensor(&mut diagnostic_trace,
                format!("schema_gather[{}].field_embs", task_map.task_name), &field_embs_val)?;
            diag_checkpoint("[iobinding] after_schema_gather_run_binding");
            DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);

            // 4b. CountPredArgmax → output su CPU (int64 scalare, 8 byte)
            let mut b_cp = oe!(g_count_pred_argmax.create_binding(), "count_pred_argmax create_binding");
            oe!(b_cp.bind_input("pc_emb", &pc_emb_val), "count_pred_argmax bind pc_emb");
            oe!(b_cp.bind_output_to_device(&cp_out_name, &cpu_out_mem), "count_pred_argmax bind output");

            diag_checkpoint("[iobinding] before_count_pred_argmax_run_binding");
            let pred_count: usize = {
                let mut out = oe!(g_count_pred_argmax.run_binding(&b_cp), "count_pred_argmax run");
                let val = out.remove(cp_out_name.as_str())
                    .ok_or_else(|| GlinerError::OomDeviceBinding(
                        format!("count_pred_argmax: output '{}' non trovato", cp_out_name)
                    ))?;
                capture_diagnostic_tensor(&mut diagnostic_trace,
                    format!("count_pred[{}]", task_map.task_name), &val)?;
                oe!(
                    val.try_extract_array::<i64>().map_err(|e| anyhow::anyhow!(e)),
                    "count_pred_argmax extract i64"
                ).into_owned()[[0]] as usize
            };
            diag_checkpoint("[iobinding] after_count_pred_argmax_run_binding");
            DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
            if diag_enabled() { eprintln!("RUSTMEMDIAG   [iobinding] pred_count={}", pred_count); }

            if pred_count == 0 {
                continue;
            }

            // 4c. Task classificazione — field_embs è già su CPU (cpu_out_mem sopra)
            if is_cls {
                let fe_arr = if let Ok(t) = field_embs_val.try_extract_array::<f32>() {
                    t.into_owned()
                } else if let Ok(t) = field_embs_val.try_extract_array::<half::f16>() {
                    t.into_owned().mapv(|x| x.to_f32())
                } else {
                    return Err(GlinerError::OomDeviceBinding("field_embs format error".into()));
                };
                let hidden_size = fe_arr.shape()[1];

                let mut padded = Array4::<half::f16>::from_elem((1, num_labels, self.config.max_width, hidden_size), half::f16::from_f32(0.0));
                for m in 0..num_labels {
                    for d in 0..hidden_size {
                        padded[[0, m, 0, d]] = half::f16::from_f32(fe_arr[[m, d]]);
                    }
                }
                let padded_t = oe!(Tensor::from_array(padded), "cls padded tensor");

                let mut b_cls = oe!(g_classifier.create_binding(), "classifier create_binding");
                oe!(b_cls.bind_input("span_embeddings", &padded_t), "classifier bind span_embeddings");
                oe!(b_cls.bind_output_to_device(&cls_out_name, &cpu_out_mem), "classifier bind output");

                diag_checkpoint("[iobinding] before_classifier_run_binding");
                let logits = {
                    let mut out = oe!(g_classifier.run_binding(&b_cls), "classifier run");
                    let val = out.remove(cls_out_name.as_str())
                        .ok_or_else(|| GlinerError::OomDeviceBinding(
                            format!("classifier: output '{}' non trovato", cls_out_name)
                        ))?;
                    capture_diagnostic_tensor(&mut diagnostic_trace,
                        format!("classifier[{}]", task_map.task_name), &val)?;
                    if let Ok(t) = val.try_extract_array::<f32>() {
                        t.into_owned()
                    } else if let Ok(t) = val.try_extract_array::<half::f16>() {
                        t.into_owned().mapv(|x| x.to_f32())
                    } else {
                        return Err(GlinerError::OomDeviceBinding("classifier extract: type error".into()));
                    }
                };
                diag_checkpoint("[iobinding] after_classifier_run_binding");
                DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);

                let mut exp_sum = 0.0f32;
                let exps: Vec<f32> = (0..num_labels).map(|m| {
                    let e = logits[[0, m, 0, 0]].exp();
                    exp_sum += e;
                    e
                }).collect();

                let (best_idx, best_score) = exps.iter().enumerate()
                    .map(|(i, &e)| (i, e / exp_sum))
                    .max_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal))
                    .unwrap_or((0, 0.0));

                final_classifications.push(ExtractedClassification {
                    task_name: task_map.task_name.clone(),
                    label:     task_map.labels[best_idx].clone(),
                    score:     best_score,
                });
                continue;
            }

            // 4d. CountLSTMFixed → struct_proj su device
            let mut b_cl = oe!(g_count_lstm_fixed.create_binding(), "count_lstm_fixed create_binding");
            oe!(b_cl.bind_input("field_embs", &field_embs_val), "count_lstm_fixed bind field_embs");
            oe!(b_cl.bind_output_to_device(&cl_out_name, &device_mem), "count_lstm_fixed bind output");

            diag_checkpoint("[iobinding] before_count_lstm_fixed_run_binding");
            let struct_proj_val = {
                let mut out = oe!(g_count_lstm_fixed.run_binding(&b_cl), "count_lstm_fixed run");
                out.remove(cl_out_name.as_str())
                    .ok_or_else(|| GlinerError::OomDeviceBinding(
                        format!("count_lstm_fixed: output '{}' non trovato", cl_out_name)
                    ))?
            };
            capture_diagnostic_tensor(&mut diagnostic_trace,
                format!("count_lstm_fixed[{}]", task_map.task_name), &struct_proj_val)?;
            diag_checkpoint("[iobinding] after_count_lstm_fixed_run_binding");
            DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);

            // 4e. Scorer → entity_scores su CPU (per NMS)
            let mut b_sc = oe!(g_scorer.create_binding(), "scorer create_binding");
            oe!(b_sc.bind_input("span_embeddings", &span_embs_val), "scorer bind span_embeddings");
            oe!(b_sc.bind_input("struct_proj", &struct_proj_val), "scorer bind struct_proj");
            oe!(b_sc.bind_output_to_device(&sc_out_name, &cpu_out_mem), "scorer bind output");

            diag_checkpoint("[iobinding] before_scorer_run_binding");
            let scores = {
                let mut out = oe!(g_scorer.run_binding(&b_sc), "scorer run");
                let val = out.remove(sc_out_name.as_str())
                    .ok_or_else(|| GlinerError::OomDeviceBinding(
                        format!("scorer: output '{}' non trovato", sc_out_name)
                    ))?;
                capture_diagnostic_tensor(&mut diagnostic_trace,
                    format!("scorer[{}]", task_map.task_name), &val)?;
                if let Ok(t) = val.try_extract_array::<f32>() {
                    t.into_owned()
                } else if let Ok(t) = val.try_extract_array::<half::f16>() {
                    t.into_owned().mapv(|x| x.to_f32())
                } else {
                    return Err(GlinerError::OomDeviceBinding("scorer extract: type error".into()));
                }
            };
            diag_checkpoint("[iobinding] after_scorer_run_binding (scores extracted CPU-side)");
            DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
            diag_shape("[iobinding] scorer_output_scores", scores.shape());
            // scores: [MAX_COUNT, num_words, max_width, M]  (sigmoid già applicato nell'ONNX)

            // 4f. NMS greedy + soglia (identico a extract_standard)
            diag_checkpoint("[iobinding] before_post_processing");
            let mut all_matches: Vec<ExtractedEntity> = Vec::new();

            // Pinned GLiNER2 1.3.2 decodes entity extraction from count slot
            // zero only; predicted-count slots are used by structured/relation
            // tasks. Combining every slot changes spans and confidence scores.
            let count_slots = if task_map.task_type == "entities" { 1 } else { pred_count };
            for c_idx in 0..count_slots {
                for start in 0..num_words {
                    for width_idx in 0..self.config.max_width {
                        let end = std::cmp::min(start + width_idx + 1, num_words);
                        for m in 0..num_labels {
                            let prob = scores[[c_idx, start, width_idx, m]];
                            if prob >= threshold {
                                let char_start = record.word_to_char_maps[start].0;
                                let char_end   = record.word_to_char_maps[end - 1].1;
                                let orig_start = record.word_to_token_maps[start].0;
                                let orig_end   = record.word_to_token_maps[end - 1].1;

                                if char_start <= char_end && char_end <= text.len() {
                                    let entity_text = text[char_start..char_end].trim().to_string();
                                    if !entity_text.is_empty() {
                                        all_matches.push(ExtractedEntity {
                                            score:     prob,
                                            label:     task_map.labels[m].clone(),
                                            text:      entity_text,
                                            start_tok: orig_start,
                                            end_tok:   orig_end,
                                            start_char: char_start,
                                            end_char: char_end,
                                        });
                                    }
                                }
                            }
                        }
                    }
                }
            }

            all_matches.sort_by(|a, b| {
                b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal)
            });
            if let Some(trace) = diagnostic_trace.as_deref_mut() {
                trace.candidates.extend(all_matches.iter().cloned());
            }
            let mut selected: Vec<ExtractedEntity> = Vec::new();
            for candidate in all_matches {
                let overlap = selected.iter().any(|s| {
                    let spans_overlap = !(candidate.end_tok <= s.start_tok || candidate.start_tok >= s.end_tok);
                    spans_overlap && (flat_ner || s.label == candidate.label)
                });
                if !overlap {
                    selected.push(candidate);
                }
            }

            if task_map.task_type == "entities" {
                final_entities.extend(selected);
            } else if task_map.task_type == "relations" {
                let head = selected.iter().find(|x| x.label == "head").cloned();
                let tail = selected.iter().find(|x| x.label == "tail").cloned();
                if let (Some(h), Some(t)) = (head, tail) {
                    final_relations.push(ExtractedRelation {
                        head:          h,
                        tail:          t,
                        relation_type: task_map.labels[0].clone(),
                    });
                }
            }
            diag_checkpoint("[iobinding] after_post_processing");
        }

        if let Some(trace) = diagnostic_trace.as_deref_mut() {
            trace.final_entities = final_entities.clone();
        }
        Ok((final_entities, final_relations, final_classifications))
    }

    // ── Standard mode (identico al v1) ───────────────────────────────────────

    /// Esegue la pipeline con trasferimenti CPU↔device espliciti tra ogni layer.
    /// Usa le sessioni v2 (8 modelli) ma con il pattern Standard del v1:
    /// ogni output viene estratto su CPU e passato come input al layer successivo.
    ///
    /// Questa implementazione garantisce correttezza e retrocompatibilità
    /// indipendentemente dall'hardware disponibile.
    // `record` must already be within MAX_ENCODER_POSITIONS - see the same
    // note on extract_iobinding() above; extract_with_progress() is the only
    // caller reachable from the public extract().
    pub fn extract_standard(
        &self,
        text: &str,
        record: &ProcessedRecord,
        params: Option<InferenceParams>,
        precomputed_hs: Option<ArrayD<f32>>,
    ) -> Result<(Vec<ExtractedEntity>, Vec<ExtractedRelation>, Vec<ExtractedClassification>)> {
        // Un solo lock per sessione, preso qui: Mutex non rientrante.
        let mut g_token_gather = self.token_gather.lock().unwrap();
        let mut g_span_rep = self.span_rep.lock().unwrap();
        let mut g_schema_gather = self.schema_gather.lock().unwrap();
        let mut g_count_pred_argmax = self.count_pred_argmax.lock().unwrap();
        let mut g_count_lstm_fixed = self.count_lstm_fixed.lock().unwrap();
        let mut g_scorer = self.scorer.lock().unwrap();
        let mut g_classifier = self.classifier.lock().unwrap();
        let p = params.unwrap_or_default();
        let threshold = p.threshold;
        let flat_ner = p.flat_ner;
        let seq_len = record.input_ids.len();
        if seq_len > MAX_ENCODER_POSITIONS {
            anyhow::bail!(
                "internal error: a {}-token chunk reached extract_standard(), over the {}-token encoder window - extract_with_progress()'s own chunking invariant was violated",
                seq_len, MAX_ENCODER_POSITIONS);
        }
        if diag_enabled() {
            eprintln!("RUSTMEMDIAG chunk: utf8_bytes={} seq_len(tokens)={} num_words={} tasks={}",
                text.len(), seq_len, record.word_to_token_maps.len(), record.tasks.len());
        }

        // 1. Encoder. A batch caller may supply one already-split [1,S,H]
        // sample; every downstream fragment below remains the same batch=1
        // path and uses only this sample's own ProcessedRecord metadata.
        let hs = if let Some(hs) = precomputed_hs {
            hs
        } else {
            let mut g_encoder = self.encoder.lock().unwrap();
            diag_checkpoint("before_encoder_run");
            let input_ids = Array2::from_shape_vec((1, seq_len), record.input_ids.clone())?;
            let attn_mask = Array2::from_shape_vec((1, seq_len), record.attention_mask.clone())?;
            diag_shape("encoder_input_ids", input_ids.shape());
            diag_shape("encoder_attention_mask", attn_mask.shape());
            let enc_out = g_encoder.run(ort::inputs![
                "input_ids"      => Tensor::from_array(input_ids)?,
                "attention_mask" => Tensor::from_array(attn_mask)?
            ])?;
            ENCODER_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
            diag_checkpoint("after_encoder_run");
            let mut found = None;
            for name in ["hidden_states", "last_hidden_state", "output"] {
                if let Some(v) = enc_out.get(name) {
                    found = Some(extract_f32!(v));
                    break;
                }
            }
            match found {
                Some(a) => a,
                None => {
                    let v_fallback = enc_out.values().next()
                        .ok_or_else(|| anyhow::anyhow!("encoder: nessun output trovato"))?;
                    extract_f32!(v_fallback)
                }
            }
        };

        let num_words = record.word_to_token_maps.len();
        if num_words == 0 {
            return Ok((Vec::new(), Vec::new(), Vec::new()));
        }
        let hidden_size = hs.shape()[2];
        diag_shape("encoder_output_hidden_states", hs.shape());

        // 2. TokenGather — estrae embedding word-level da hs
        diag_checkpoint("before_token_gather_run");
        let word_starts: Vec<i64> = record.word_to_token_maps.iter()
            .map(|&(s, _)| s as i64).collect();
        let word_idx_arr = Array1::from_vec(word_starts);
        let tg_out = g_token_gather.run(ort::inputs![
            "last_hidden_state" => Tensor::from_array(hs.clone())?,
            "word_indices"      => Tensor::from_array(word_idx_arr)?
        ])?;
        diag_checkpoint("after_token_gather_run");
        DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
        let text_embs = {
            let v = tg_out.values().next()
                .ok_or_else(|| anyhow::anyhow!("token_gather: nessun output"))?;
            extract_f32!(v)
        };
        diag_shape("token_gather_output_text_embs", text_embs.shape());

        // 3. SpanRep
        let num_spans = num_words * self.config.max_width;
        let mut span_idx_data = Vec::with_capacity(num_spans * 2);
        for start in 0..num_words {
            for width in 0..self.config.max_width {
                let end = start + width;
                if end >= num_words {
                    span_idx_data.extend_from_slice(&[0i64, 0i64]);
                } else {
                    span_idx_data.push(start as i64);
                    span_idx_data.push(end as i64);
                }
            }
        }
        let span_idx_arr = Array3::from_shape_vec((1, num_spans, 2), span_idx_data)?;
        diag_checkpoint("before_span_rep_run");
        diag_shape("span_rep_input_span_idx", span_idx_arr.shape());
        let sr_out = g_span_rep.run(ort::inputs![
            "hidden_states" => Tensor::from_array(text_embs)?,
            "span_idx"      => Tensor::from_array(span_idx_arr)?
        ])?;
        diag_checkpoint("after_span_rep_run");
        DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
        let span_embs = {
            let v = sr_out.values().next()
                .ok_or_else(|| anyhow::anyhow!("span_rep: nessun output"))?;
            extract_f32!(v)
        };
        diag_shape("span_rep_output_span_embs", span_embs.shape());

        // 4. Loop per-task
        let mut final_entities       = Vec::new();
        let mut final_relations      = Vec::new();
        let mut final_classifications = Vec::new();

        for task_map in &record.tasks {
            let num_labels = task_map.labels.len();

            // 4a. SchemaGather — un solo Gather per prompt + campi
            let schema_indices: Vec<i64> =
                std::iter::once(task_map.prompt_tok_idx as i64)
                .chain(task_map.field_tok_indices.iter().map(|&i| i as i64))
                .collect();
            let schema_idx_arr = Array1::from_vec(schema_indices);
            diag_checkpoint("before_schema_gather_run");
            let sg_out = g_schema_gather.run(ort::inputs![
                "last_hidden_state" => Tensor::from_array(hs.clone())?,
                "schema_indices"    => Tensor::from_array(schema_idx_arr)?
            ])?;
            diag_checkpoint("after_schema_gather_run");
            DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
            let mut sg_iter = sg_out.values();
            let pc_emb = {
                let v = sg_iter.next()
                    .ok_or_else(|| anyhow::anyhow!("schema_gather: pc_emb mancante"))?;
                extract_f32!(v)   // [1, H]
            };
            let field_embs = {
                let v = sg_iter.next()
                    .ok_or_else(|| anyhow::anyhow!("schema_gather: field_embs mancante"))?;
                extract_f32!(v)   // [M, H]
            };
            diag_shape("schema_gather_output_pc_emb", pc_emb.shape());
            diag_shape("schema_gather_output_field_embs", field_embs.shape());

            // 4b. CountPredArgmax — restituisce int64
            diag_checkpoint("before_count_pred_argmax_run");
            let cp_out = g_count_pred_argmax.run(ort::inputs![
                "pc_emb" => Tensor::from_array(pc_emb)?
            ])?;
            diag_checkpoint("after_count_pred_argmax_run");
            DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
            let pred_count = cp_out.values().next()
                .ok_or_else(|| anyhow::anyhow!("count_pred_argmax: nessun output"))?
                .try_extract_array::<i64>()?.into_owned()[[0]] as usize;
            if diag_enabled() { eprintln!("RUSTMEMDIAG   pred_count={}", pred_count); }

            if pred_count == 0 {
                continue;
            }

            // 4c. Task classificazione: usa classifier invece di scorer
            if task_map.task_type == "classifications" {
                let _span_emb_shape = span_embs.shape();
                let mut padded = ndarray::Array4::<half::f16>::from_elem(
                    (1, num_labels, self.config.max_width, hidden_size), half::f16::from_f32(0.0)
                );
                for m in 0..num_labels {
                    for d in 0..hidden_size {
                        padded[[0, m, 0, d]] = half::f16::from_f32(field_embs[[m, d]]);
                    }
                }
                diag_checkpoint("before_classifier_run");
                let cls_out = g_classifier.run(ort::inputs![
                    "span_embeddings" => Tensor::from_array(padded)?
                ])?;
                diag_checkpoint("after_classifier_run");
                DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
                let logits = {
                    let v = cls_out.values().next()
                        .ok_or_else(|| anyhow::anyhow!("classifier: nessun output"))?;
                    extract_f32!(v)
                };

                let mut exp_sum = 0.0f32;
                let exps: Vec<f32> = (0..num_labels).map(|m| {
                    let e = logits[[0, m, 0, 0]].exp();
                    exp_sum += e;
                    e
                }).collect();

                let (best_idx, best_score) = exps.iter().enumerate()
                    .map(|(i, &e)| (i, e / exp_sum))
                    .max_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal))
                    .unwrap_or((0, 0.0));

                final_classifications.push(ExtractedClassification {
                    task_name: task_map.task_name.clone(),
                    label: task_map.labels[best_idx].clone(),
                    score: best_score,
                });
                continue;
            }

            // 4d. CountLSTMFixed — output sempre [MAX_COUNT, M, H]
            diag_checkpoint("before_count_lstm_fixed_run");
            diag_shape("count_lstm_fixed_input_field_embs", field_embs.shape());
            let cl_out = g_count_lstm_fixed.run(ort::inputs![
                "field_embs" => Tensor::from_array(field_embs)?
            ])?;
            diag_checkpoint("after_count_lstm_fixed_run");
            DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
            let struct_proj = {
                let v = cl_out.values().next()
                    .ok_or_else(|| anyhow::anyhow!("count_lstm_fixed: nessun output"))?;
                extract_f32!(v)
            };
            diag_shape("count_lstm_fixed_output_struct_proj", struct_proj.shape());
            // struct_proj: [MAX_COUNT, M, H] — usiamo solo [:pred_count]

            // 4e. Scorer — restituisce probabilità sigmoid già calcolate
            //     entity_scores: [MAX_COUNT, num_words, max_width, M]
            diag_checkpoint("before_scorer_run");
            let sc_out = g_scorer.run(ort::inputs![
                "span_embeddings" => Tensor::from_array(span_embs.clone())?,
                "struct_proj"     => Tensor::from_array(struct_proj)?
            ])?;
            diag_checkpoint("after_scorer_run");
            DOWNSTREAM_FORWARD_CALLS_TOTAL.fetch_add(1, Ordering::Relaxed);
            let scores = {
                let v = sc_out.values().next()
                    .ok_or_else(|| anyhow::anyhow!("scorer: nessun output"))?;
                extract_f32!(v)
            };
            diag_shape("scorer_output_scores", scores.shape());
            // scores: [MAX_COUNT, num_words, max_width, M]

            // 4f. NMS + soglia (identico al v1, ma ora scores sono già sigmoid)
            diag_checkpoint("before_post_processing");
            let mut all_matches: Vec<ExtractedEntity> = Vec::new();

            let count_slots = if task_map.task_type == "entities" { 1 } else { pred_count };
            for c_idx in 0..count_slots {
                for start in 0..num_words {
                    for width_idx in 0..self.config.max_width {
                        let end = std::cmp::min(start + width_idx + 1, num_words);
                        for m in 0..num_labels {
                            let prob = scores[[c_idx, start, width_idx, m]];
                            if prob >= threshold {
                                let char_start = record.word_to_char_maps[start].0;
                                let char_end   = record.word_to_char_maps[end - 1].1;
                                let orig_start = record.word_to_token_maps[start].0;
                                let orig_end   = record.word_to_token_maps[end - 1].1;

                                if char_start <= char_end && char_end <= text.len() {
                                    let entity_text = text[char_start..char_end]
                                        .trim().to_string();
                                    if !entity_text.is_empty() {
                                        all_matches.push(ExtractedEntity {
                                            score: prob,
                                            label: task_map.labels[m].clone(),
                                            text:  entity_text,
                                            start_tok: orig_start,
                                            end_tok:   orig_end,
                                            start_char: char_start,
                                            end_char: char_end,
                                        });
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // NMS greedy per score decrescente
            all_matches.sort_by(|a, b| {
                b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal)
            });
            let mut selected: Vec<ExtractedEntity> = Vec::new();
            for m in all_matches {
                let overlap = selected.iter().any(|s| {
                    let spans_overlap = !(m.end_tok <= s.start_tok || m.start_tok >= s.end_tok);
                    spans_overlap && (flat_ner || s.label == m.label)
                });
                if !overlap {
                    selected.push(m);
                }
            }

            if task_map.task_type == "entities" {
                final_entities.extend(selected);
            } else if task_map.task_type == "relations" {
                let head = selected.iter().find(|x| x.label == "head").cloned();
                let tail = selected.iter().find(|x| x.label == "tail").cloned();
                if let (Some(h), Some(t)) = (head, tail) {
                    final_relations.push(ExtractedRelation {
                        head: h,
                        tail: t,
                        relation_type: task_map.labels[0].clone(),
                    });
                }
            }
            diag_checkpoint("after_post_processing");
        }

        Ok((final_entities, final_relations, final_classifications))
    }
}

// MAX_COUNT deve coincidere con il parametro max_count usato durante l'export.
// Baked nei modelli count_lstm_fixed (output shape fisso [MAX_COUNT, M, H]).
#[allow(dead_code)]
const MAX_COUNT: usize = 20;

#[cfg(test)]
static CHUNK_CALL_COUNT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

#[cfg(test)]
mod bounded_inference_tests {
    use super::*;

    // ── Pure, structural tests: no tokenizer, no ONNX, no model package ────
    // ── needed - these hold for ANY word_token_lens/budget/overlap input. ──

    fn make_entity(start_char: usize, end_char: usize, label: &str, score: f32) -> ExtractedEntity {
        ExtractedEntity {
            text: String::new(),
            label: label.to_string(),
            score,
            start_tok: 0,
            end_tok: 0,
            start_char,
            end_char,
        }
    }

    /// Property: every planned chunk's own word-token sum is <= the budget -
    /// i.e. no chunk plan_word_chunks() ever returns could produce an
    /// encoder invocation over the window, for any input.
    #[test]
    fn every_chunk_respects_the_token_budget() {
        let word_token_lens: Vec<usize> = (0..500).map(|i| 1 + (i % 4)).collect(); // realistic 1-4 subwords/word
        let budget = 480usize; // MAX_ENCODER_POSITIONS(512) minus a representative 32-token schema overhead
        let overlap = 7usize; // max_width(8) - 1
        let ranges = plan_word_chunks(&word_token_lens, budget, overlap, 32).unwrap();
        assert!(ranges.len() > 1, "this input should not fit in a single chunk");
        for &(start, end) in &ranges {
            let sum: usize = word_token_lens[start..end].iter().sum();
            assert!(sum <= budget, "chunk [{start},{end}) sums to {sum} tokens, over budget {budget}");
        }
    }

    /// Property: no gap - every word index is covered by at least one
    /// chunk's range, and the last chunk always reaches the end (no tail of
    /// the document is left unexamined).
    #[test]
    fn chunk_plan_covers_every_word_with_no_gap_and_reaches_the_end() {
        let word_token_lens: Vec<usize> = vec![2; 300];
        let ranges = plan_word_chunks(&word_token_lens, 100, 7, 0).unwrap();
        assert_eq!(ranges.last().unwrap().1, 300, "the last chunk must reach the final word");
        let mut covered = vec![false; 300];
        for &(start, end) in &ranges {
            for w in start..end { covered[w] = true; }
        }
        assert!(covered.iter().all(|&c| c), "every word index must be covered by some chunk");
    }

    /// Property (schema overhead is included in the budget): a schema large
    /// enough to leave zero room for even one word fails loudly rather than
    /// silently producing an empty/truncated result.
    #[test]
    fn oversized_schema_overhead_fails_loudly_instead_of_silently_truncating() {
        let word_token_lens: Vec<usize> = vec![3, 3, 3];
        let result = plan_word_chunks(&word_token_lens, 0, 7, 512);
        assert!(result.is_err(), "zero text budget (schema consumed the whole window) must be an error, not an empty Ok");
    }

    /// Property: short input fits in one chunk (real-world equivalent of
    /// "short input still uses exactly one inference and preserves existing
    /// output" - this half is the structural guarantee; the real-model half
    /// is short_input_uses_exactly_one_real_inference_call() below).
    #[test]
    fn input_within_budget_produces_exactly_one_chunk() {
        let word_token_lens: Vec<usize> = vec![1, 2, 1, 3, 1];
        let ranges = plan_word_chunks(&word_token_lens, 512, 7, 0).unwrap();
        assert_eq!(ranges, vec![(0, 5)]);
    }

    /// Property (the model's own max_width contract is honored): for every
    /// possible entity start word and every possible width up to max_width,
    /// the resulting span [start, start+width) is fully contained within at
    /// least one planned chunk - i.e. an entity crossing a naive chunk
    /// boundary is still preserved whole, purely because of the computed
    /// overlap. This is the structural proof of "entity crossing a chunk
    /// boundary is preserved because of overlap" - true regardless of what
    /// the model actually detects at any particular boundary.
    #[test]
    fn every_max_width_span_is_fully_contained_in_at_least_one_chunk() {
        const MAX_WIDTH: usize = 8;
        let overlap_words = MAX_WIDTH - 1;
        let word_token_lens: Vec<usize> = (0..1000).map(|i| 1 + (i % 3)).collect();
        let budget = 60usize; // small budget forces many chunk boundaries, stress-testing coverage
        let ranges = plan_word_chunks(&word_token_lens, budget, overlap_words, 0).unwrap();
        let num_words = word_token_lens.len();
        for span_start in 0..num_words {
            for width in 1..=MAX_WIDTH {
                if span_start + width > num_words { continue; }
                let span_end = span_start + width;
                let contained = ranges.iter().any(|&(cs, ce)| cs <= span_start && span_end <= ce);
                assert!(contained,
                    "span [{span_start},{span_end}) (width {width}) is not fully contained in any chunk - overlap is insufficient");
            }
        }
    }

    /// Property (duplicate entities from overlap collapse deterministically):
    /// identical (start_char, end_char, label) keeps only the
    /// highest-confidence detection, regardless of insertion order.
    #[test]
    fn dedup_keeps_highest_confidence_for_identical_span_and_label() {
        let entities = vec![
            make_entity(10, 20, "person", 0.55),
            make_entity(10, 20, "person", 0.91), // same identity, higher score - must win
            make_entity(30, 40, "organization", 0.70),
        ];
        let deduped = dedup_entities_by_identity(entities);
        assert_eq!(deduped.len(), 2);
        let person = deduped.iter().find(|e| e.label == "person").unwrap();
        assert!((person.score - 0.91).abs() < f32::EPSILON);
        assert!(deduped.iter().any(|e| e.label == "organization" && e.start_char == 30));
    }

    /// Property: an entity entirely inside one chunk (no overlap ambiguity)
    /// is not affected by dedup - distinct spans/labels all survive.
    #[test]
    fn distinct_entities_all_survive_dedup() {
        let entities = vec![
            make_entity(0, 5, "person", 0.9),
            make_entity(6, 12, "organization", 0.8),
            make_entity(13, 20, "location", 0.7),
        ];
        let deduped = dedup_entities_by_identity(entities);
        assert_eq!(deduped.len(), 3);
    }

    // ── Real-model integration tests: gated on SINGULARITY_PAPINIAN_MODEL_DIR ──
    // ── pointing at the actual installed Fastino/mdeberta-v3-base package. ─
    // ── Skip (not fail) when unset, matching this repo's other real-model ──
    // ── hardware-dependent test conventions.                              ──

    fn real_engine() -> Option<Gliner2EngineV2> {
        let root = std::env::var("SINGULARITY_PAPINIAN_MODEL_DIR").ok()?;
        if root.is_empty() { return None; }
        Some(Gliner2EngineV2::new(Gliner2Config {
            models_dir: root,
            max_width: 8,
            model_type: crate::ModelType::HuggingFace,
        }).expect("real GLiNER2 V2 engine must construct from the installed package"))
    }

    fn labels() -> Vec<String> {
        vec!["person".to_string(), "organization".to_string(), "location".to_string()]
    }

    fn russian_legal_sentence() -> &'static str {
        "Верховный Суд Российской Федерации в определении по делу N А40-123456/2023 указал, \
         что общество с ограниченной ответственностью \"Северный Мост\" не проявило должной \
         осмотрительности при выборе контрагента. "
    }

    #[test]
    fn short_input_uses_exactly_one_real_inference_call_and_succeeds() {
        let Some(engine) = real_engine() else {
            eprintln!("skipped: SINGULARITY_PAPINIAN_MODEL_DIR not set");
            return;
        };
        CHUNK_CALL_COUNT.store(0, std::sync::atomic::Ordering::SeqCst);
        let text = russian_legal_sentence();
        let (entities, _, _) = engine
            .extract(text, &[SchemaTask::Entities(labels())], None)
            .expect("short input must succeed");
        assert_eq!(CHUNK_CALL_COUNT.load(std::sync::atomic::Ordering::SeqCst), 1,
            "input well within the 512-token window must trigger exactly one real inference call");
        assert!(!entities.is_empty(), "this sentence contains real, detectable entities");
    }

    #[test]
    fn text_exceeding_one_window_produces_multiple_bounded_calls_and_succeeds() {
        let Some(engine) = real_engine() else {
            eprintln!("skipped: SINGULARITY_PAPINIAN_MODEL_DIR not set");
            return;
        };
        CHUNK_CALL_COUNT.store(0, std::sync::atomic::Ordering::SeqCst);
        let long_text = russian_legal_sentence().repeat(40); // far beyond 512 tokens
        let mut updates = Vec::new();
        let (entities, _, _) = engine
            .extract_with_progress(&long_text, &[SchemaTask::Entities(labels())], None,
                &mut |completed, total| updates.push((completed, total)))
            .expect("long input must still succeed via chunking, never error just for being long");
        let calls = CHUNK_CALL_COUNT.load(std::sync::atomic::Ordering::SeqCst);
        assert!(calls > 1, "text well beyond one window must trigger more than one real inference call, got {calls}");
        assert_eq!(updates.len(), calls + 1);
        for (index, &(completed, total)) in updates.iter().enumerate() {
            assert_eq!(completed, index);
            assert_eq!(total, calls);
        }
        assert!(!entities.is_empty(), "entities repeated throughout this long text must still be detected");
    }

    #[test]
    fn offsets_beyond_the_first_chunk_map_exactly_to_original_utf8_bytes() {
        let Some(engine) = real_engine() else {
            eprintln!("skipped: SINGULARITY_PAPINIAN_MODEL_DIR not set");
            return;
        };
        // Padding forces the marker organization name into a later chunk;
        // includes a non-BMP emoji before the marker so its multi-byte UTF-8
        // encoding is exercised in the offset arithmetic leading up to it.
        let padding = russian_legal_sentence().repeat(35);
        let marker = "Общество \u{1F4C4} \"Балтийская Верфь\" заключило договор поставки. ";
        let text = format!("{padding}{marker}{padding}");
        let (entities, _, _) = engine
            .extract(&text, &[SchemaTask::Entities(labels())], None)
            .expect("long padded input must succeed");
        let found = entities.iter().find(|e| text.get(e.start_char..e.end_char)
            .map(|slice| slice.contains("Балтийская")).unwrap_or(false));
        assert!(found.is_some(),
            "an entity naming the marker organization must be found with offsets that slice back to it exactly in the ORIGINAL text");
        // Confirms the slice at these offsets is valid UTF-8 landing on real
        // character boundaries (String::get returns None otherwise) - the
        // non-BMP emoji earlier in the padding did not corrupt byte-offset
        // arithmetic for entities detected after it.
        let entity = found.unwrap();
        assert!(text.get(entity.start_char..entity.end_char).is_some(),
            "entity offsets must land on valid UTF-8 character boundaries in the original text");
    }

    #[test]
    fn an_entity_near_the_end_of_a_long_document_is_still_detected() {
        let Some(engine) = real_engine() else {
            eprintln!("skipped: SINGULARITY_PAPINIAN_MODEL_DIR not set");
            return;
        };
        let padding = russian_legal_sentence().repeat(35);
        let tail_marker = "В заключение договор с \"Уральская Сталь\" был расторгнут. ";
        let text = format!("{padding}{tail_marker}");
        let (entities, _, _) = engine
            .extract(&text, &[SchemaTask::Entities(labels())], None)
            .expect("long input ending with a marker must succeed");
        let found = entities.iter().any(|e| text.get(e.start_char..e.end_char)
            .map(|slice| slice.contains("Уральская")).unwrap_or(false));
        assert!(found, "an entity in the document's final chunk must not be left unexamined");
    }
}
