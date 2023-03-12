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

// Methods to control object lifespan.
// The NAPI's napi_ref can be used only for objects.
// The napi_ext_ref can be used for any value type.

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
