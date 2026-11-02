use gliner2_inference::{downstream_forward_calls_total, encoder_batches_total, encoder_forward_calls_total,
    semantic_chunks_executed_total, Gliner2Config, Gliner2EngineV2,
    InferenceParams, ModelType, SchemaTask, MAX_ENCODER_POSITIONS};
use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::ptr;
use std::sync::{OnceLock, atomic::{AtomicUsize, Ordering}};

static ENGINE_INSTANCES_ALIVE: AtomicUsize = AtomicUsize::new(0);
static BOUNDED_RUNTIME_ID: &[u8] = b"gliner2-bounded-v1-max512\0";

#[repr(C)]
pub struct LmGliner2Entity {
    pub start_byte: usize,
    pub end_byte: usize,
    pub confidence: f32,
    pub label: *mut c_char,
}

#[repr(C)]
pub struct LmGliner2EntityList {
    pub entities: *mut LmGliner2Entity,
    pub len: usize,
}

#[repr(C)]
pub struct LmGliner2EntityBatch {
    pub samples: *mut LmGliner2EntityList,
    pub len: usize,
}

#[repr(C)]
pub struct LmGliner2EncoderComparison {
    pub old_shape: [usize; 3],
    pub new_shape: [usize; 3],
    pub old_dtype: *const c_char,
    pub new_dtype: *const c_char,
    pub max_absolute_difference: f64,
    pub mean_absolute_difference: f64,
}

static DTYPE_F16: &[u8] = b"f16\0";
static DTYPE_F32: &[u8] = b"f32\0";

fn error_string(message: impl ToString) -> *mut c_char {
    let sanitized = message.to_string().replace('\0', " ");
    CString::new(sanitized).unwrap_or_default().into_raw()
}

unsafe fn required_utf8<'a>(value: *const c_char, name: &str) -> Result<&'a str, String> {
    if value.is_null() {
        return Err(format!("{name} is required"));
    }
    CStr::from_ptr(value)
        .to_str()
        .map_err(|_| format!("{name} is not valid UTF-8"))
}

fn bundled_ort_path() -> Result<PathBuf, String> {
    if let Some(configured) = std::env::var_os("ORT_DYLIB_PATH").filter(|value| !value.is_empty()) {
        return Ok(PathBuf::from(configured));
    }

    let executable = std::env::current_exe()
        .map_err(|error| format!("cannot locate the Singularity executable: {error}"))?;
    let executable_dir = executable
        .parent()
        .ok_or_else(|| "Singularity executable has no parent directory".to_owned())?;

    #[cfg(any(target_os = "macos", target_os = "ios"))]
    let candidates = [
        executable_dir.join("libonnxruntime.1.23.2.dylib"),
        executable_dir.join("../Frameworks/libonnxruntime.1.23.2.dylib"),
    ];
    #[cfg(target_os = "windows")]
    let candidates = [executable_dir.join("onnxruntime.dll")];
    #[cfg(all(unix, not(any(target_os = "macos", target_os = "ios"))))]
    let candidates = [executable_dir.join("libonnxruntime.so.1.23.2")];

    candidates
        .into_iter()
        .find(|candidate| candidate.is_file())
        .ok_or_else(|| "pinned bundled ONNX Runtime 1.23.2 library is missing".to_owned())
}

fn initialize_ort() -> Result<(), String> {
    static INITIALIZED: OnceLock<Result<(), String>> = OnceLock::new();
    INITIALIZED
        .get_or_init(|| {
            let runtime_path = bundled_ort_path()?;
            ort::init_from(&runtime_path)
                .map_err(|error| {
                    format!(
                        "failed to load pinned ONNX Runtime from {}: {error}",
                        runtime_path.display()
                    )
                })?
                .with_name("Singularity Papinian Guard")
                .commit();
            Ok(())
        })
        .clone()
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_engine_create(
    models_dir: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut c_void {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<*mut c_void, String> {
        let models_dir = required_utf8(models_dir, "GLiNER2 models directory")?;
        initialize_ort()?;
        let engine = Gliner2EngineV2::new(Gliner2Config {
            models_dir: models_dir.to_owned(),
            max_width: 8,
            model_type: ModelType::HuggingFace,
        })
        .map_err(|error| format!("GLiNER2 V2 session construction failed: {error}"))?;
        ENGINE_INSTANCES_ALIVE.fetch_add(1, Ordering::Relaxed);
        Ok(Box::into_raw(Box::new(engine)).cast())
    }));

    match result {
        Ok(Ok(engine)) => engine,
        Ok(Err(error)) => {
            if !error_out.is_null() {
                *error_out = error_string(error);
            }
            ptr::null_mut()
        }
        Err(_) => {
            if !error_out.is_null() {
                *error_out = error_string("panic while constructing GLiNER2 V2 engine");
            }
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_engine_destroy(engine: *mut c_void) {
    if !engine.is_null() {
        drop(Box::from_raw(engine.cast::<Gliner2EngineV2>()));
        ENGINE_INSTANCES_ALIVE.fetch_sub(1, Ordering::Relaxed);
    }
}

#[no_mangle]
pub extern "C" fn lm_gliner2_max_encoder_positions() -> usize {
    MAX_ENCODER_POSITIONS
}

#[no_mangle]
pub extern "C" fn lm_gliner2_bounded_runtime_id() -> *const c_char {
    BOUNDED_RUNTIME_ID.as_ptr().cast()
}

#[no_mangle]
pub extern "C" fn lm_gliner2_engine_instances_alive() -> usize {
    ENGINE_INSTANCES_ALIVE.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn lm_gliner2_semantic_chunks_executed_total() -> usize {
    semantic_chunks_executed_total()
}

#[no_mangle]
pub extern "C" fn lm_gliner2_encoder_forward_calls_total() -> usize {
    encoder_forward_calls_total()
}

#[no_mangle]
pub extern "C" fn lm_gliner2_encoder_batches_total() -> usize {
    encoder_batches_total()
}

#[no_mangle]
pub extern "C" fn lm_gliner2_downstream_forward_calls_total() -> usize {
    downstream_forward_calls_total()
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_engine_compare_encoder_b1(
    engine: *mut c_void, text: *const c_char, labels: *const *const c_char,
    label_count: usize, result_out: *mut LmGliner2EncoderComparison,
    error_out: *mut *mut c_char,
) -> bool {
    if !error_out.is_null() { *error_out = ptr::null_mut(); }
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<LmGliner2EncoderComparison, String> {
        if engine.is_null() || result_out.is_null() {
            return Err("GLiNER2 engine and encoder comparison output are required".to_owned());
        }
        let text = required_utf8(text, "GLiNER2 encoder comparison text")?;
        if label_count > 0 && labels.is_null() {
            return Err("GLiNER2 label array is required".to_owned());
        }
        let label_pointers = std::slice::from_raw_parts(labels, label_count);
        let owned_labels: Vec<String> = label_pointers.iter().enumerate()
            .map(|(index, value)| required_utf8(*value, &format!("GLiNER2 label {index}"))
                .map(str::to_owned)).collect::<Result<_, _>>()?;
        let report = (&*engine.cast::<Gliner2EngineV2>())
            .compare_encoder_paths_b1(text, &[SchemaTask::Entities(owned_labels)])
            .map_err(|error| format!("GLiNER2 encoder comparison failed: {error}"))?;
        Ok(LmGliner2EncoderComparison {
            old_shape: report.old_shape,
            new_shape: report.new_shape,
            old_dtype: if report.old_dtype == "f16" { DTYPE_F16.as_ptr().cast() }
                else { DTYPE_F32.as_ptr().cast() },
            new_dtype: if report.new_dtype == "f16" { DTYPE_F16.as_ptr().cast() }
                else { DTYPE_F32.as_ptr().cast() },
            max_absolute_difference: report.max_absolute_difference,
            mean_absolute_difference: report.mean_absolute_difference,
        })
    }));
    match result {
        Ok(Ok(report)) => { *result_out = report; true }
        Ok(Err(error)) => {
            if !error_out.is_null() { *error_out = error_string(error); }
            false
        }
        Err(_) => {
            if !error_out.is_null() { *error_out = error_string("panic during encoder comparison"); }
            false
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_engine_compare_tail_b1(
    engine: *mut c_void, text: *const c_char, labels: *const *const c_char,
    label_count: usize, report_out: *mut *mut c_char, error_out: *mut *mut c_char,
) -> bool {
    if !report_out.is_null() { *report_out = ptr::null_mut(); }
    if !error_out.is_null() { *error_out = ptr::null_mut(); }
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<String, String> {
        if engine.is_null() || report_out.is_null() {
            return Err("GLiNER2 engine and tail comparison output are required".to_owned());
        }
        let text = required_utf8(text, "GLiNER2 tail comparison text")?;
        if label_count > 0 && labels.is_null() {
            return Err("GLiNER2 label array is required".to_owned());
        }
        let label_pointers = std::slice::from_raw_parts(labels, label_count);
        let owned_labels: Vec<String> = label_pointers.iter().enumerate()
            .map(|(index, value)| required_utf8(*value, &format!("GLiNER2 label {index}"))
                .map(str::to_owned)).collect::<Result<_, _>>()?;
        (&*engine.cast::<Gliner2EngineV2>())
            .compare_tail_paths_b1(text, &[SchemaTask::Entities(owned_labels)])
            .map_err(|error| format!("GLiNER2 tail comparison failed: {error}"))
    }));
    match result {
        Ok(Ok(report)) => {
            *report_out = error_string(report);
            true
        }
        Ok(Err(error)) => {
            if !error_out.is_null() { *error_out = error_string(error); }
            false
        }
        Err(_) => {
            if !error_out.is_null() { *error_out = error_string("panic during tail comparison"); }
            false
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_engine_localize_batch_b2(
    engine: *mut c_void, texts: *const *const c_char, text_count: usize,
    labels: *const *const c_char, label_count: usize,
    report_out: *mut *mut c_char, error_out: *mut *mut c_char,
) -> bool {
    if !report_out.is_null() { *report_out = ptr::null_mut(); }
    if !error_out.is_null() { *error_out = ptr::null_mut(); }
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<String, String> {
        if engine.is_null() || report_out.is_null() || texts.is_null() || text_count != 2 {
            return Err("GLiNER2 engine, output, and exactly two texts are required".to_owned());
        }
        if label_count > 0 && labels.is_null() {
            return Err("GLiNER2 label array is required".to_owned());
        }
        let text_pointers = std::slice::from_raw_parts(texts, text_count);
        let owned_texts: Vec<String> = text_pointers.iter().enumerate()
            .map(|(index, value)| required_utf8(*value, &format!("GLiNER2 input text {index}"))
                .map(str::to_owned)).collect::<Result<_, _>>()?;
        let label_pointers = std::slice::from_raw_parts(labels, label_count);
        let owned_labels: Vec<String> = label_pointers.iter().enumerate()
            .map(|(index, value)| required_utf8(*value, &format!("GLiNER2 label {index}"))
                .map(str::to_owned)).collect::<Result<_, _>>()?;
        (&*engine.cast::<Gliner2EngineV2>())
            .localize_batch_paths_b2(&owned_texts, &[SchemaTask::Entities(owned_labels)])
            .map_err(|error| format!("GLiNER2 B=2 localization failed: {error}"))
    }));
    match result {
        Ok(Ok(report)) => {
            *report_out = error_string(report);
            true
        }
        Ok(Err(error)) => {
            if !error_out.is_null() { *error_out = error_string(error); }
            false
        }
        Err(_) => {
            if !error_out.is_null() { *error_out = error_string("panic during B=2 localization"); }
            false
        }
    }
}

fn native_entity_list(entities: Vec<gliner2_inference::ExtractedEntity>) -> LmGliner2EntityList {
    let mut output: Vec<LmGliner2Entity> = entities.into_iter().map(|entity| LmGliner2Entity {
        start_byte: entity.start_char,
        end_byte: entity.end_char,
        confidence: entity.score,
        label: CString::new(entity.label.replace('\0', " ")).unwrap_or_default().into_raw(),
    }).collect();
    let result = LmGliner2EntityList { entities: output.as_mut_ptr(), len: output.len() };
    std::mem::forget(output);
    result
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_engine_infer(
    engine: *mut c_void, text: *const c_char, labels: *const *const c_char,
    label_count: usize, result_out: *mut LmGliner2EntityList, error_out: *mut *mut c_char,
) -> bool {
    lm_gliner2_engine_infer_with_progress(engine, text, labels, label_count,
        result_out, error_out, None, ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_engine_infer_with_progress(
    engine: *mut c_void, text: *const c_char, labels: *const *const c_char,
    label_count: usize, result_out: *mut LmGliner2EntityList, error_out: *mut *mut c_char,
    progress: Option<extern "C" fn(usize, usize, *mut c_void)>, user_data: *mut c_void,
) -> bool {
    if !result_out.is_null() {
        (*result_out).entities = ptr::null_mut();
        (*result_out).len = 0;
    }
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }
    let result = catch_unwind(AssertUnwindSafe(
        || -> Result<LmGliner2EntityList, String> {
            if engine.is_null() || result_out.is_null() {
                return Err("GLiNER2 engine and result output are required".to_owned());
            }
            let text = required_utf8(text, "GLiNER2 input text")?;
            if label_count > 0 && labels.is_null() {
                return Err("GLiNER2 label array is required".to_owned());
            }
            let label_pointers = std::slice::from_raw_parts(labels, label_count);
            let mut owned_labels = Vec::with_capacity(label_count);
            for (index, label) in label_pointers.iter().enumerate() {
                owned_labels
                    .push(required_utf8(*label, &format!("GLiNER2 label {index}"))?.to_owned());
            }

            let engine = &*engine.cast::<Gliner2EngineV2>();
            let (entities, _, _) = engine
                .extract_with_progress(
                    text,
                    &[SchemaTask::Entities(owned_labels)],
                    Some(InferenceParams {
                        threshold: 0.0,
                        flat_ner: false,
                    }),
                    &mut |completed, total| { if let Some(callback) = progress { callback(completed, total, user_data); } },
                )
                .map_err(|error| format!("GLiNER2 V2 inference failed: {error}"))?;

            Ok(native_entity_list(entities))
        },
    ));

    match result {
        Ok(Ok(list)) => {
            *result_out = list;
            true
        }
        Ok(Err(error)) => {
            if !error_out.is_null() {
                *error_out = error_string(error);
            }
            false
        }
        Err(_) => {
            if !error_out.is_null() {
                *error_out = error_string("panic during GLiNER2 V2 inference");
            }
            false
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_engine_infer_batch_with_progress(
    engine: *mut c_void, texts: *const *const c_char, text_count: usize,
    labels: *const *const c_char, label_count: usize, microbatch_size: usize,
    result_out: *mut LmGliner2EntityBatch, error_out: *mut *mut c_char,
    progress: Option<extern "C" fn(usize, usize, *mut c_void)>, user_data: *mut c_void,
) -> bool {
    if !result_out.is_null() {
        (*result_out).samples = ptr::null_mut();
        (*result_out).len = 0;
    }
    if !error_out.is_null() { *error_out = ptr::null_mut(); }
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<LmGliner2EntityBatch, String> {
        if engine.is_null() || result_out.is_null() || (text_count > 0 && texts.is_null()) {
            return Err("GLiNER2 engine, texts, and result output are required".to_owned());
        }
        if label_count > 0 && labels.is_null() {
            return Err("GLiNER2 label array is required".to_owned());
        }
        let text_pointers = std::slice::from_raw_parts(texts, text_count);
        let owned_texts: Vec<String> = text_pointers.iter().enumerate()
            .map(|(index, value)| required_utf8(*value, &format!("GLiNER2 input text {index}"))
                .map(str::to_owned)).collect::<Result<_, _>>()?;
        let label_pointers = std::slice::from_raw_parts(labels, label_count);
        let owned_labels: Vec<String> = label_pointers.iter().enumerate()
            .map(|(index, value)| required_utf8(*value, &format!("GLiNER2 label {index}"))
                .map(str::to_owned)).collect::<Result<_, _>>()?;
        let engine = &*engine.cast::<Gliner2EngineV2>();
        let extractions = engine.extract_batch_with_progress(
            &owned_texts, &[SchemaTask::Entities(owned_labels)],
            Some(InferenceParams { threshold: 0.0, flat_ner: false }), microbatch_size,
            &mut |completed, total| {
                if let Some(callback) = progress { callback(completed, total, user_data); }
            }).map_err(|error| format!("GLiNER2 V2 batch inference failed: {error}"))?;
        let mut samples: Vec<LmGliner2EntityList> = extractions.into_iter()
            .map(|(entities, _, _)| native_entity_list(entities)).collect();
        let batch = LmGliner2EntityBatch { samples: samples.as_mut_ptr(), len: samples.len() };
        std::mem::forget(samples);
        Ok(batch)
    }));
    match result {
        Ok(Ok(batch)) => { *result_out = batch; true }
        Ok(Err(error)) => {
            if !error_out.is_null() { *error_out = error_string(error); }
            false
        }
        Err(_) => {
            if !error_out.is_null() { *error_out = error_string("panic during GLiNER2 V2 batch inference"); }
            false
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_entity_list_free(list: LmGliner2EntityList) {
    if list.entities.is_null() {
        return;
    }
    let entities = Vec::from_raw_parts(list.entities, list.len, list.len);
    for entity in entities {
        if !entity.label.is_null() {
            drop(CString::from_raw(entity.label));
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_entity_batch_free(batch: LmGliner2EntityBatch) {
    if batch.samples.is_null() { return; }
    let samples = Vec::from_raw_parts(batch.samples, batch.len, batch.len);
    for sample in samples { lm_gliner2_entity_list_free(sample); }
}

#[no_mangle]
pub unsafe extern "C" fn lm_gliner2_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(CString::from_raw(value));
    }
}
