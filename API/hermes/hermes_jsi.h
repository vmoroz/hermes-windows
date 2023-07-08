/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_HERMES_JSI_H
#define HERMES_HERMES_JSI_H

#include "js_runtime_api.h"

EXTERN_C_START

typedef struct jsi_runtime_s *jsi_runtime;
typedef struct jsi_prepared_script_s *jsi_prepared_script;
typedef struct jsi_symbol_s *jsi_symbol;
typedef struct jsi_bigint_s *jsi_bigint;
typedef struct jsi_string_s *jsi_string;
typedef struct jsi_object_s *jsi_object;
typedef struct jsi_weak_object_s *jsi_weak_object;
typedef struct jsi_property_id_s *jsi_property_id;

enum jsi_value_kind {
  jsi_value_kind_undefined,
  jsi_value_kind_null,
  jsi_value_kind_boolean,
  jsi_value_kind_number,
  jsi_value_kind_symbol,
  jsi_value_kind_bigint,
  jsi_value_kind_string,
  jsi_value_kind_object,
};

struct jsi_value {
  uint64_t data;
  jsi_value_kind kind;
};

JSR_API jsi_evaluate_script(
    jsi_runtime runtime,
    const uint8_t *script_data,
    size_t script_length,
    jsr_data_delete_cb script_delete_cb,
    void *deleter_data,
    const char *source_url,
    jsi_value *result);

JSR_API jsi_create_prepared_script(
    jsi_runtime runtime,
    const uint8_t *script_data,
    size_t script_length,
    jsr_data_delete_cb script_delete_cb,
    void *deleter_data,
    const char *source_url,
    jsi_prepared_script *result);

JSR_API jsi_delete_prepared_script(
    jsi_runtime runtime,
    jsi_prepared_script prepared_script);

// Run the prepared script.
JSR_API jsi_evaluate_prepared_script(
    jsi_runtime runtime,
    jsi_prepared_script prepared_script,
    jsi_value *result);

JSR_API jsi_drain_microtasks(jsi_runtime runtime, int32_t max_count_hint, bool* result);

JSR_API jsi_get_global(jsi_runtime runtime, jsi_object_pointer* result);

JSR_API jsi_get_description(jsi_runtime runtime, const char** result);

JSR_API jsi_is_inspectable(jsi_runtime runtime, bool* result);

JSR_API jsi_clone_symbol(jsi_runtime runtime, jsi_symbol_pointer symbol, jsi_symbol_pointer* result);
JSR_API jsi_clone_bigint(jsi_runtime runtime, jsi_bigint_pointer bigint, jsi_bigint_pointer* result);
JSR_API jsi_clone_string(jsi_runtime runtime, jsi_string_pointer str, jsi_string_pointer* result);
JSR_API jsi_clone_object(jsi_runtime runtime, jsi_object_pointer obj, jsi_object_pointer* result);
JSR_API jsi_clone_property_id(jsi_runtime runtime, jsi_property_id property_id, jsi_property_id* result);

JSR_API jsi_create_property_id_from_ascii(jsi_runtime runtime, const char* ascii, size_t length, jsi_property_id* result);
JSR_API jsi_create_property_id_from_utf8(jsi_runtime runtime, const uint8_t* utf8, size_t length, jsi_property_id* result);
JSR_API jsi_create_property_id_from_string(jsi_runtime runtime, jsi_string_pointer str, jsi_property_id* result);
JSR_API jsi_create_property_id_from_symbol(jsi_runtime runtime, jsi_symbol_pointer symbol, jsi_property_id* result);

JSR_API jsi_create_property_id_to_string(jsi_runtime runtime, jsi_symbol_pointer symbol, jsi_property_id* result);
JSR_API jsi_property_id_to_utf8(jsi_runtime runtime, jsi_property_id property_id, uint8_t *str, size_t* size);

JSR_API jsi_property_id_equals(jsi_runtime runtime, jsi_property_id left, jsi_property_id right, bool *result);

JSR_API jsi_symbol_to_utf8(jsi_runtime runtime, jsi_symbol_pointer symbol, uint8_t *str, size_t* size);

JSR_API jsi_create_bigint_from_int64(jsi_runtime runtime, int64_t value, jsi_bigint_pointer* result);
JSR_API jsi_create_bigint_from_uint64(jsi_runtime runtime, uint64_t value, jsi_bigint_pointer* result);
JSR_API jsi_bigint_is_int64(jsi_runtime runtime, jsi_bigint_pointer value, bool* result);
JSR_API jsi_bigint_is_uint64(jsi_runtime runtime, jsi_bigint_pointer value, bool* result);
JSR_API jsi_truncate_bigint(jsi_runtime runtime, jsi_bigint_pointer value, uint64_t* result);
JSR_API jsi_bigint_to_string(jsi_runtime runtime, jsi_bigint_pointer value, int32_t radix, jsi_string_pointer* result);

JSR_API jsi_create_string_from_ascii(jsi_runtime runtime, const char* ascii, size_t length, jsi_string_pointer* result);
JSR_API jsi_create_string_from_utf8(jsi_runtime runtime, const uint8_t* ascii, size_t length, jsi_string_pointer* result);
JSR_API jsi_string_to_utf8(jsi_runtime runtime, jsi_string_pointer string, uint8_t *str, size_t* size);

JSR_API jsi_create_value_from_json(jsi_runtime runtime, const uint8_t* json, size_t length, jsi_value* result);

JSR_API jsi_create_object(jsi_runtime runtime, jsi_object* result);
JSR_API jsi_create_object_for_host_object(jsi_runtime runtime, jsi_host_object host_object, jsi_object* result);
JSR_API jsi_get_host_object(jsi_runtime runtime, jsi_object obj, jsi_host_object* result);
JSR_API jsi_get_host_function(jsi_runtime runtime, jsi_object obj, jsi_host_function* result);

JSR_API jsi_has_native_state(jsi_runtime runtime, jsi_object obj, bool* result);
JSR_API jsi_get_native_state(jsi_runtime runtime, jsi_object obj, jsi_native_state* result);
JSR_API jsi_set_native_state(jsi_runtime runtime, jsi_object obj, jsi_native_state result);

JSR_API jsi_get_property(jsi_runtime runtime, jsi_object obj, jsi_property_id property_id, jsi_value* result);
JSR_API jsi_get_property_by_name(jsi_runtime runtime, jsi_object obj, jsi_string property_name, jsi_value* result);
JSR_API jsi_has_property(jsi_runtime runtime, jsi_object obj, jsi_property_id property_id, jsi_value* result);
JSR_API jsi_has_property_by_name(jsi_runtime runtime, jsi_object obj, jsi_string property_name, jsi_value* result);
JSR_API jsi_set_property(jsi_runtime runtime, jsi_object obj, jsi_property_id property_id, jsi_value* result);
JSR_API jsi_set_property_by_name(jsi_runtime runtime, jsi_object obj, jsi_string property_name, jsi_value* result);

JSR_API jsi_is_array(jsi_runtime runtime, jsi_object obj, bool* result);
JSR_API jsi_is_array_buffer(jsi_runtime runtime, jsi_object obj, bool* result);
JSR_API jsi_is_function(jsi_runtime runtime, jsi_object obj, bool* result);
JSR_API jsi_is_host_object(jsi_runtime runtime, jsi_object obj, bool* result);
JSR_API jsi_is_host_function(jsi_runtime runtime, jsi_object obj, bool* result);

JSR_API jsi_get_property_names(jsi_runtime runtime, jsi_object* result);

JSR_API jsi_create_weak_object(jsi_runtime runtime, jsi_object obj, jsi_weak_object* result);
JSR_API jsi_lock_weak_object(jsi_runtime runtime, jsi_object obj, jsi_value* result);

JSR_API jsi_create_array(jsi_runtime runtime, size_t length, jsi_object* result);
JSR_API jsi_create_array_buffer(jsi_runtime runtime, jsi_mutable_buffer buffer, jsi_object* result);
JSR_API jsi_get_array_size(jsi_runtime runtime, jsi_object array, size_t* result);
JSR_API jsi_get_array_buffer_size(jsi_runtime runtime, jsi_object array_buffer, size_t* result);
JSR_API jsi_get_array_buffer_data(jsi_runtime runtime, jsi_object array_buffer, uint8_t** result);
JSR_API jsi_get_value_at_index(jsi_runtime runtime, jsi_object array, jsi_value* result);
JSR_API jsi_set_value_at_index(jsi_runtime runtime, jsi_object array, jsi_value result);

JSR_API jsi_create_function_from_host_function(jsi_runtime runtime, jsi_property_id name, uint32_t param_count, jsi_host_function host_function, jsi_object* result);
JSR_API jsi_call_function(jsi_runtime runtime, jsi_object func, jsi_value this_arg, size_t arg_count, jsi_value* args, jsi_value* result);
JSR_API jsi_call_as_constructor(jsi_runtime runtime, jsi_object func, size_t arg_count, jsi_value* args, jsi_value* result);

JSR_API jsi_push_scope(jsi_runtime runtime, jsi_scope_state* result);
JSR_API jsi_pop_scope(jsi_runtime runtime, jsi_scope_state scope_state);

JSR_API jsi_symbol_strict_equals(jsi_runtime runtime, jsi_symbol left, jsi_symbol right, bool* result);
JSR_API jsi_bigint_strict_equals(jsi_runtime runtime, jsi_bigint left, jsi_bigint right, bool* result);
JSR_API jsi_string_strict_equals(jsi_runtime runtime, jsi_string left, jsi_string right, bool* result);
JSR_API jsi_object_strict_equals(jsi_runtime runtime, jsi_object left, jsi_object right, bool* result);

JSR_API jsi_instance_of(jsi_runtime runtime, jsi_object obj, jsi_object constructor, bool* result);

JSR_API jsi_release_symbol(jsi_runtime runtime, jsi_symbol symbol);
JSR_API jsi_release_bigint(jsi_runtime runtime, jsi_bigint bigint);
JSR_API jsi_release_string(jsi_runtime runtime, jsi_string string);
JSR_API jsi_release_object(jsi_runtime runtime, jsi_bigint object);
JSR_API jsi_release_property_id(jsi_runtime runtime, jsi_property_id property_id);

JSR_API jsi_get_and_clear_last_error(jsi_runtime runtime, jsi_error_type* error_type,
    abi_string* error_details,
    abi_string* message,
    abi_string* stack,
    jsi_value* value);

JSR_API jsi_set_error(jsi_runtime runtime, jsi_error_type error_type, abi_string error_details, jsi_value value);

EXTERN_C_END

#endif // !HERMES_HERMES_JSI_H
