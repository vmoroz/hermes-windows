/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_HERMES_API_H
#define HERMES_HERMES_API_H

#include "js_native_ext_api.h"

// TODO: use NAPI_CDECL after we update Node-API files.
#if _M_IX86
#define HERMES_CDECL __cdecl
#else
#define HERMES_CDECL
#endif

#define HERMES_API NAPI_EXTERN hermes_status HERMES_CDECL

EXTERN_C_START

enum hermes_status {
  hermes_ok,
  hermes_error,
};

typedef struct hermes_runtime_s *hermes_runtime;
typedef struct hermes_config_s *hermes_config;
typedef struct hermes_local_connection_s *hermes_local_connection;
typedef struct hermes_remote_connection_s *hermes_remote_connection;

typedef void(
    HERMES_CDECL *hermes_data_delete_cb)(void *data, void *deleter_data);

//=============================================================================
// hermes_runtime
//=============================================================================

HERMES_API hermes_create_runtime(hermes_config config, hermes_runtime *runtime);
HERMES_API hermes_delete_runtime(hermes_runtime runtime);
HERMES_API hermes_get_node_api_env(hermes_runtime runtime, napi_env *env);
HERMES_API hermes_dump_crash_data(hermes_runtime runtime, int32_t fd);
HERMES_API hermes_sampling_profiler_enable();
HERMES_API hermes_sampling_profiler_disable();
HERMES_API hermes_sampling_profiler_add(hermes_runtime runtime);
HERMES_API hermes_sampling_profiler_remove(hermes_runtime runtime);
HERMES_API hermes_sampling_profiler_dump_to_file(const char *filename);

//=============================================================================
// hermes_config
//=============================================================================

HERMES_API hermes_create_config(hermes_config *config);
HERMES_API hermes_delete_config(hermes_config config);
HERMES_API hermes_config_enable_default_crash_handler(
    hermes_config config,
    bool value);
HERMES_API hermes_config_enable_debugger(hermes_config config, bool value);
HERMES_API hermes_config_set_debugger_runtime_name(
    hermes_config config,
    const char *name);
HERMES_API hermes_config_set_debugger_port(hermes_config config, uint16_t port);
HERMES_API hermes_config_set_debugger_break_on_start(
    hermes_config config,
    bool value);

//=============================================================================
// hermes_config task runner
//=============================================================================

// A callback to run task
typedef void(HERMES_CDECL *hermes_task_run_cb)(void *task_data);

// A callback to post task to the task runner
typedef void(HERMES_CDECL *hermes_task_runner_post_task_cb)(
    void *task_runner_data,
    void *task_data,
    hermes_task_run_cb task_run_cb,
    hermes_data_delete_cb task_data_delete_cb,
    void *deleter_data);

HERMES_API hermes_config_set_task_runner(
    hermes_config config,
    void *task_runner_data,
    hermes_task_runner_post_task_cb task_runner_post_task_cb,
    hermes_data_delete_cb task_runner_data_delete_cb,
    void *deleter_data);

//=============================================================================
// hermes_config script cache
//=============================================================================

typedef struct {
  const char *source_url;
  uint64_t source_hash;
  const char *runtime_name;
  uint64_t runtime_version;
  const char *tag;
} hermes_script_cache_metadata;

typedef void(HERMES_CDECL *hermes_script_cache_load_cb)(
    void *script_cache_data,
    hermes_script_cache_metadata *script_metadata,
    const uint8_t **buffer,
    size_t *buffer_size,
    hermes_data_delete_cb *buffer_delete_cb,
    void **deleter_data);

typedef void(HERMES_CDECL *hermes_script_cache_store_cb)(
    void *script_cache_data,
    hermes_script_cache_metadata *script_metadata,
    const uint8_t *buffer,
    size_t buffer_size,
    hermes_data_delete_cb buffer_delete_cb,
    void *deleter_data);

HERMES_API hermes_config_set_script_cache(
    hermes_config config,
    void *script_cache_data,
    hermes_script_cache_load_cb script_cache_load_cb,
    hermes_script_cache_store_cb script_cache_store_cb,
    hermes_data_delete_cb script_cache_data_delete_cb,
    void *deleter_data);

//=============================================================================
// Setting inspector singleton
//=============================================================================

typedef int32_t(HERMES_CDECL *hermes_inspector_add_page_cb)(
    const char *title,
    const char *vm,
    void *connectFunc);

typedef void(HERMES_CDECL *hermes_inspector_remove_page_cb)(int32_t page_id);

HERMES_API hermes_set_inspector(
    hermes_inspector_add_page_cb add_page_cb,
    hermes_inspector_remove_page_cb remove_page_cb);

//=============================================================================
// Local and remote inspector connections.
// Local is defined in Hermes VM, Remote is defined by inspector outside of VM.
//=============================================================================

typedef void(HERMES_CDECL *hermes_remote_connection_send_message_cb)(
    hermes_remote_connection remote_connection,
    const char *message);

typedef void(HERMES_CDECL *hermes_remote_connection_disconnect_cb)(
    hermes_remote_connection remote_connection);

HERMES_API hermes_create_local_connection(
    void *page_data,
    hermes_remote_connection remote_connection,
    hermes_remote_connection_send_message_cb on_send_message_cb,
    hermes_remote_connection_disconnect_cb on_disconnect_cb,
    hermes_data_delete_cb on_delete_cb,
    void *deleter_data,
    hermes_local_connection *local_connection);

HERMES_API hermes_delete_local_connection(
    hermes_local_connection local_connection);

HERMES_API hermes_local_connection_send_message(
    hermes_local_connection local_connection,
    const char *message);

HERMES_API hermes_local_connection_disconnect(
    hermes_local_connection local_connection);

EXTERN_C_END

#endif // HERMES_HERMES_API_H
