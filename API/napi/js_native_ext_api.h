// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#define NAPI_EXPERIMENTAL
#include "js_native_api.h"

//
// N-API extensions required for JavaScript engine hosting.
//
// It is a very early version of the APIs which we consider to be experimental.
// These APIs are not stable yet and are subject to change while we continue
// their development. After some time we will stabilize the APIs and make them
// "officially stable".
//

EXTERN_C_START

// TODO: Revise relevance. Likely obsoleted by napi_ext_env_settings::flags
typedef enum {
  napi_ext_env_attribute_none = 0x00000000,
  napi_ext_env_attribute_enable_gc_api = 0x00000001,
  napi_ext_env_attribute_ignore_unhandled_promises = 0x00000002,
} napi_ext_env_attributes;

typedef struct napi_ext_env_scope__ *napi_ext_env_scope;
typedef struct napi_ext_ref__ *napi_ext_ref;

// A callback to run task
typedef void (__cdecl *napi_ext_task_callback)(napi_env env, void *task_data);

// A callback to schedule a task
typedef void (__cdecl *napi_ext_schedule_task_callback)(
    napi_env env,
    napi_ext_task_callback task_cb,
    void *task_data,
    uint32_t delay_in_msec,
    napi_finalize finalize_cb,
    void *finalize_hint);

typedef struct _napi_ext_env_settings {
  // Size of this struct to allow extending it in future.
  size_t this_size;

  // Custom scheduler of the foreground JavaScript tasks.
  napi_ext_schedule_task_callback foreground_scheduler;

  // The environment attributes.
  napi_ext_env_attributes attributes;

  uint16_t inspector_port{9223};

  size_t initial_heap_size_in_bytes{0};
  size_t maximum_heap_size_in_bytes{0};

  // Custom data associated with the environment.
  void *data;

  // The callback to call to destroy the custom data.
  napi_finalize finalize_data_cb;

  // Additional data for the finalize callback.
  void *finalize_data_hint;

  // NOTE: Keep in sync with v8runtime::V8RuntimeArgs::flags
  // Padded to allow adding boolean flags without breaking the ABI
  union {
    struct {
      bool track_gc_object_stats : 1;
      bool enable_jit_tracing : 1;
      bool enable_message_tracing : 1;
      bool enable_gc_tracing : 1;
      bool enable_inspector : 1;
      bool wait_for_debugger : 1;
      bool enable_gc_api : 1;
      bool ignore_unhandled_promises : 1;
      bool enable_system_instrumentation : 1;

      // Experimental flags (for memory-constrained optimization testing)
      bool sparkplug : 1; // https://v8.dev/blog/sparkplug
      bool
          predictable : 1; // take a big CPU hit to reduce the number of threads
      bool optimize_for_size : 1; // enables optimizations which favor memory
                                  // size over execution speed
      bool always_compact : 1; // perform compaction on every full GC
      bool jitless : 1; // disable JIT entirely
    } flags;
    uint32_t _flagspad{0};
  };

} napi_ext_env_settings;

// Increments the napi_env ref count by 1.
NAPI_EXTERN napi_status __cdecl napi_ext_env_ref(napi_env env);

// Decrements the napi_env ref count by 1. If the ref count becomes 0, then the
// napi_env is deleted.
NAPI_EXTERN napi_status __cdecl napi_ext_env_unref(napi_env env);

// Opens the napi_env in the current thread.
// Calling N-API functions without the opened scope may cause a failure.
// The scope must be closed by the napi_ext_close_env_scope call.
NAPI_EXTERN napi_status __cdecl napi_ext_open_env_scope(
    napi_env env,
    napi_ext_env_scope *result);

// Closes the napi_env in the current thread. It must match to the
// napi_ext_open_env_scope call.
NAPI_EXTERN napi_status __cdecl napi_ext_close_env_scope(
    napi_env env,
    napi_ext_env_scope scope);

// Provides a hint to run garbage collection.
// It is typically used for unit tests.
NAPI_EXTERN napi_status __cdecl napi_ext_collect_garbage(napi_env env);

// Checks if the environment has an unhandled promise rejection.
NAPI_EXTERN napi_status __cdecl napi_ext_has_unhandled_promise_rejection(
    napi_env env,
    bool *result);

// Gets and clears the last unhandled promise rejection.
NAPI_EXTERN
napi_status __cdecl napi_get_and_clear_last_unhandled_promise_rejection(
    napi_env env,
    napi_value *result);

// Use to enable fast string equality check by comparing napi_refs as addresses.
// The caller is responsible for calling napi_reference_unref on the result
// after the use. The caller must not call the napi_delete_reference.
NAPI_EXTERN napi_status __cdecl napi_ext_get_unique_string_utf8_ref(
    napi_env env,
    const char *str,
    size_t length,
    napi_ext_ref *result);

// Gets an unique string reference.
NAPI_EXTERN
napi_status __cdecl napi_ext_get_unique_string_ref(
    napi_env env,
    napi_value str_value,
    napi_ext_ref *result);

// Methods to control object lifespan.
// The NAPI's napi_ref can be used only for objects.
// The napi_ext_ref can be used for any value type.

// Creates new napi_ext_ref with ref counter set to 1.
NAPI_EXTERN napi_status __cdecl napi_ext_create_reference(
    napi_env env,
    napi_value value,
    napi_ext_ref *result);

// Creates new napi_ext_ref and associates native data with the reference.
// The ref counter is set to 1.
NAPI_EXTERN napi_status __cdecl napi_ext_create_reference_with_data(
    napi_env env,
    napi_value value,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ext_ref *result);

// Creates new napi_ext_ref with ref counter set to 1.
// The napi_ext_ref wraps up a weak reference to the value.
// Even if the napi_ext_ref ref counter is more than 0, the internal weak
// reference can exptire and the napi_ext_get_reference_value for this
// napi_ext_ref may return nullptr.
NAPI_EXTERN napi_status __cdecl napi_ext_create_weak_reference(
    napi_env env,
    napi_value value,
    napi_ext_ref *result);

// Increments the reference count.
NAPI_EXTERN napi_status __cdecl napi_ext_reference_ref(
    napi_env env,
    napi_ext_ref ref);

// Decrements the reference count.
// The provided ref must not be used after this call because it could be deleted
// if the internal ref counter became zero.
NAPI_EXTERN napi_status __cdecl napi_ext_reference_unref(
    napi_env env,
    napi_ext_ref ref);

// Gets the referenced value.
NAPI_EXTERN napi_status __cdecl napi_ext_get_reference_value(
    napi_env env,
    napi_ext_ref ref,
    napi_value *result);

//=============================================================================
// Script running, preparing, and serialization.
//
// Script is usually converted to byte code, or in other words - prepared - for
// execution. The APIs below allow not only running the script, but also control
// its preparation phase where we can explicitly prepare the script for running,
// run the prepared script, and serialize or deserialize the prepared script.
//=============================================================================

typedef struct napi_ext_prepared_script__ *napi_ext_prepared_script;

typedef struct {
  void *data;
  size_t byte_length;
  napi_finalize finalize_cb;
  void *finalize_hint;
} napi_ext_buffer;

// A callback to return buffer synchronously.
typedef void (__cdecl *napi_ext_buffer_callback)(
    napi_env env,
    const uint8_t *buffer,
    size_t buffer_length,
    void *buffer_hint);

// [DEPRECATED] - use napi_ext_run_script_with_source_map method.
// Run script with the provided source_url origin.
NAPI_EXTERN napi_status __cdecl napi_ext_run_script(
    napi_env env,
    napi_value source,
    const char *source_url,
    napi_value *result);

// [DEPRECATED] - use napi_ext_prepare_script_with_source_map with
// napi_ext_run_prepared_script methods.
// Deserialize prepared script and run it.
NAPI_EXTERN napi_status __cdecl napi_ext_run_serialized_script(
    napi_env env,
    const uint8_t *buffer,
    size_t buffer_length,
    napi_value source,
    const char *source_url,
    napi_value *result);

// [DEPRECATED] - use napi_ext_prepare_script_with_source_map with
// napi_ext_serialize_prepared_script methods.
// Prepare the script and serialize it into a buffer.
NAPI_EXTERN napi_status __cdecl napi_ext_serialize_script(
    napi_env env,
    napi_value source,
    const char *source_url,
    napi_ext_buffer_callback buffer_cb,
    void *buffer_hint);

// Run the script with the source map that can be used for the script debugging.
NAPI_EXTERN napi_status __cdecl napi_ext_run_script_with_source_map(
    napi_env env,
    napi_ext_buffer script,
    napi_ext_buffer source_map,
    const char *source_url,
    napi_value *result);

// Prepare the script for running.
NAPI_EXTERN napi_status __cdecl napi_ext_prepare_script_with_source_map(
    napi_env env,
    napi_ext_buffer script,
    napi_ext_buffer source_map,
    const char *source_url,
    napi_ext_prepared_script *prepared_script);

// Run the prepared script.
NAPI_EXTERN napi_status __cdecl napi_ext_run_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script,
    napi_value *result);

// Delete the prepared script.
NAPI_EXTERN napi_status __cdecl napi_ext_delete_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script);

// Serialize the prepared script.
NAPI_EXTERN napi_status __cdecl napi_ext_serialize_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script,
    napi_ext_buffer_callback buffer_cb,
    void *buffer_hint);

EXTERN_C_END
