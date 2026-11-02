#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LmGliner2Entity {
  size_t start_byte;
  size_t end_byte;
  float confidence;
  char *label;
} LmGliner2Entity;

typedef struct LmGliner2EntityList {
  LmGliner2Entity *entities;
  size_t len;
} LmGliner2EntityList;

typedef struct LmGliner2EntityBatch {
  LmGliner2EntityList *samples;
  size_t len;
} LmGliner2EntityBatch;

typedef struct LmGliner2EncoderComparison {
  size_t old_shape[3];
  size_t new_shape[3];
  const char *old_dtype;
  const char *new_dtype;
  double max_absolute_difference;
  double mean_absolute_difference;
} LmGliner2EncoderComparison;

void *lm_gliner2_engine_create(const char *models_dir, char **error_out);
void lm_gliner2_engine_destroy(void *engine);
size_t lm_gliner2_max_encoder_positions(void);
const char *lm_gliner2_bounded_runtime_id(void);
size_t lm_gliner2_engine_instances_alive(void);
size_t lm_gliner2_semantic_chunks_executed_total(void);
size_t lm_gliner2_encoder_forward_calls_total(void);
size_t lm_gliner2_encoder_batches_total(void);
size_t lm_gliner2_downstream_forward_calls_total(void);
bool lm_gliner2_engine_compare_encoder_b1(void *engine, const char *text,
                            const char *const *labels, size_t label_count,
                            LmGliner2EncoderComparison *result_out, char **error_out);
bool lm_gliner2_engine_compare_tail_b1(void *engine, const char *text,
                            const char *const *labels, size_t label_count,
                            char **report_out, char **error_out);
bool lm_gliner2_engine_localize_batch_b2(void *engine,
                            const char *const *texts, size_t text_count,
                            const char *const *labels, size_t label_count,
                            char **report_out, char **error_out);
bool lm_gliner2_engine_infer(void *engine, const char *text, const char *const *labels,
                            size_t label_count, LmGliner2EntityList *result_out,
                            char **error_out);
// Called synchronously on the requesting thread; no retained callback state.
typedef void (*LmGliner2Progress)(size_t completed_chunks, size_t total_chunks, void *user_data);
bool lm_gliner2_engine_infer_with_progress(void *engine, const char *text, const char *const *labels,
                            size_t label_count, LmGliner2EntityList *result_out,
                            char **error_out, LmGliner2Progress progress, void *user_data);
bool lm_gliner2_engine_infer_batch_with_progress(void *engine,
                            const char *const *texts, size_t text_count,
                            const char *const *labels, size_t label_count,
                            size_t microbatch_size, LmGliner2EntityBatch *result_out,
                            char **error_out, LmGliner2Progress progress, void *user_data);
void lm_gliner2_entity_list_free(LmGliner2EntityList list);
void lm_gliner2_entity_batch_free(LmGliner2EntityBatch batch);
void lm_gliner2_string_free(char *value);

#ifdef __cplusplus
}
#endif
