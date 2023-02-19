/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_HERMES_API_H
#define HERMES_HERMES_API_H

#include "js_native_ext_api.h"

#if _M_IX86
#define HERMES_CDECL __cdecl
#else
#define HERMES_CDECL
#endif

#define HERMES_API NAPI_EXTERN hermes_status HERMES_CDECL

enum hermes_status {
  hermes_ok,
  hermes_error,
};

typedef struct hermes_runtime_s *hermes_runtime;

EXTERN_C_START

HERMES_API hermes_create_runtime(hermes_runtime *runtime);
HERMES_API hermes_create_runtime_with_wer(hermes_runtime *runtime);
HERMES_API hermes_delete_runtime(hermes_runtime runtime);
HERMES_API hermes_get_non_abi_safe_runtime(
    hermes_runtime runtime,
    void **non_abi_safe_runtime);

HERMES_API hermes_dump_crash_data(hermes_runtime runtime, int32_t fd);

HERMES_API hermes_sampling_profiler_enable();
HERMES_API hermes_sampling_profiler_disable();
HERMES_API hermes_sampling_profiler_add(hermes_runtime runtime);
HERMES_API hermes_sampling_profiler_remove(hermes_runtime runtime);
HERMES_API hermes_sampling_profiler_dump_to_file(const char *filename);

HERMES_API hermes_get_napi_env(hermes_runtime runtime, napi_env *env);

EXTERN_C_END

#endif // HERMES_HERMES_API_H
