/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_ABI_HERMES_ABI_H
#define HERMES_ABI_HERMES_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct HermesABIRuntimeConfig;
struct HermesABIContext;
struct HermesABIManagedPointer;
struct HermesABIBuffer;
struct HermesABIMutableBuffer;
struct HermesABIByteBuffer;
struct HermesABIHostFunction;
struct HermesABIPropNameIDList;
struct HermesABIHostObject;
struct HermesABINativeState;

/// Define the structure for references to pointer types in JS (e.g. string,
/// object, BigInt).
/// TODO: Replace jsi::PointerValue itself with this C implementation to
/// eliminate pointer management overhead in the JSI wrapper.
struct HermesABIManagedPointerVTable {
  /// Pointer to the function that should be invoked when this reference is
  /// released.
  void (*invalidate)(HermesABIManagedPointer *);
};
struct HermesABIManagedPointer {
  const HermesABIManagedPointerVTable *vtable;
};

enum HermesABIErrorCode {
  HermesABIErrorCodeNativeException,
  HermesABIErrorCodeJSError,
};

/// Define simple wrappers for the different pointer types.
#define HERMES_ABI_POINTER_TYPES(V) \
  V(Object)                         \
  V(Array)                          \
  V(String)                         \
  V(BigInt)                         \
  V(Symbol)                         \
  V(Function)                       \
  V(ArrayBuffer)                    \
  V(PropNameID)                     \
  V(WeakObject)

#define DECLARE_HERMES_ABI_POINTER_TYPE(name) \
  struct HermesABI##name {                    \
    HermesABIManagedPointer *pointer;         \
  };                                          \
  struct HermesABI##name##OrError {           \
    uintptr_t ptrOrError;                     \
  };

HERMES_ABI_POINTER_TYPES(DECLARE_HERMES_ABI_POINTER_TYPE)
#undef DECLARE_HERMES_ABI_POINTER_TYPE

#define HERMES_ABI_TRIVIAL_OR_ERROR_TYPES(V) \
  V(Bool, bool)                              \
  V(Uint64, uint64_t)                        \
  V(Uint8Ptr, uint8_t *)                     \
  V(SizeT, size_t)                           \
  V(PropNameIDListPtr, HermesABIPropNameIDList *)

#define DECLARE_HERMES_ABI_TRIVIAL_OR_ERROR_TYPES(name, type) \
  struct HermesABI##name##OrError {                           \
    bool is_error;                                            \
    union {                                                   \
      type val;                                               \
      uint16_t error;                                         \
    } data;                                                   \
  };

HERMES_ABI_TRIVIAL_OR_ERROR_TYPES(DECLARE_HERMES_ABI_TRIVIAL_OR_ERROR_TYPES)
#undef DECLARE_HERMES_ABI_TRIVIAL_OR_ERROR_TYPES

struct HermesABIVoidOrError {
  bool is_error;
  uint16_t error;
};

/// Always set the top bit for pointers so they can be easily checked.
#define HERMES_ABI_POINTER_MASK (1 << (sizeof(int) * 8 - 1))

enum HermesABIValueKind {
  HermesABIValueKindUndefined = 0,
  HermesABIValueKindNull = 1,
  HermesABIValueKindBoolean = 2,
  HermesABIValueKindError = 3,
  HermesABIValueKindNumber = 4,
  HermesABIValueKindSymbol = 5 | HERMES_ABI_POINTER_MASK,
  HermesABIValueKindBigInt = 6 | HERMES_ABI_POINTER_MASK,
  HermesABIValueKindString = 7 | HERMES_ABI_POINTER_MASK,
  HermesABIValueKindObject = 9 | HERMES_ABI_POINTER_MASK,
};

struct HermesABIValue {
  HermesABIValueKind kind;
  union {
    bool boolean;
    double number;
    HermesABIManagedPointer *pointer;
    HermesABIErrorCode error;
  } data;
};
struct HermesABIValueOrError {
  HermesABIValue value;
};

struct HermesABIByteRef {
  const uint8_t *data;
  size_t length;
};

/// Define the structure for buffers containing JS source or bytecode. This is
/// designed to mirror the functionality of jsi::Buffer.
struct HermesABIBufferVTable {
  void (*release)(HermesABIBuffer *);
};
struct HermesABIBuffer {
  const HermesABIBufferVTable *vtable;
  const uint8_t *data;
  size_t size;
};

struct HermesABIMutableBufferVTable {
  void (*release)(HermesABIMutableBuffer *);
};
struct HermesABIMutableBuffer {
  const HermesABIMutableBufferVTable *vtable;
  uint8_t *data;
  size_t size;
};

struct HermesABIByteBufferVTable {
  void (*grow_by)(HermesABIByteBuffer *, size_t);
};
struct HermesABIByteBuffer {
  const HermesABIByteBufferVTable *vtable;
  uint8_t *data;
  size_t available;
};

/// Define the structure for host functions. This is designed to recreate the
/// functionality of jsi::HostFunction.
struct HermesABIHostFunctionVTable {
  HermesABIValueOrError (*call)(
      HermesABIHostFunction *,
      HermesABIContext *,
      const HermesABIValue *,
      const HermesABIValue *,
      size_t);
  void (*release)(HermesABIHostFunction *);
};
struct HermesABIHostFunction {
  const HermesABIHostFunctionVTable *vtable;
};

/// Define the structure for lists of PropNameIDs, so that they can be returned
/// by get_property_names on a HostObject.
struct HermesABIPropNameIDListVTable {
  void (*release)(HermesABIPropNameIDList *);
};
struct HermesABIPropNameIDList {
  const HermesABIPropNameIDListVTable *vtable;
  const HermesABIPropNameID *props;
  size_t size;
};

/// Define the structure for host objects. This is designed to recreate the
/// functionality of jsi::HostObject.
struct HermesABIHostObjectVTable {
  HermesABIValueOrError (
      *get)(HermesABIHostObject *, HermesABIContext *, HermesABIPropNameID);
  HermesABIVoidOrError (*set)(
      HermesABIHostObject *,
      HermesABIContext *,
      HermesABIPropNameID,
      const HermesABIValue *);
  HermesABIPropNameIDListPtrOrError (
      *get_property_names)(HermesABIHostObject *, HermesABIContext *);
  void (*release)(HermesABIHostObject *);
};
struct HermesABIHostObject {
  const HermesABIHostObjectVTable *vtable;
};

struct HermesABINativeStateVTable {
  void (*release)(HermesABINativeState *);
};
struct HermesABINativeState {
  const HermesABINativeStateVTable *vtable;
};

struct HermesABIVTable {
  /// Create a new instance of a Hermes Runtime, and return a pointer to its
  /// associated context. The context must be released with
  /// release_hermes_runtime when it is no longer needed.
  HermesABIContext *(*make_hermes_runtime)(const HermesABIRuntimeConfig *);
  /// Release the Hermes Runtime associated with the given context.
  void (*release_hermes_runtime)(HermesABIContext *);

  /// Methods for retrieving and clearing exceptions. An exception should be
  /// retrieved if and only if some method returned an error value.
  /// Get and clear the stored JS exception value. The returned value is
  /// guaranteed to not be another exception. This should be called exactly once
  /// after an exception is thrown.
  HermesABIValue (*get_and_clear_js_error_value)(HermesABIContext *);
  HermesABIByteRef (*get_native_exception_message)(HermesABIContext *);
  /// Clear the stored native exception message. This should be called exactly
  /// once after the message is retrieved.
  void (*clear_native_exception_message)(HermesABIContext *);

  /// Set the current error before returning control to the ABI. These are
  /// intended to be used to throw exceptions from HostFunctions and
  /// HostObjects.
  void (*set_js_error_value)(HermesABIContext *, const HermesABIValue *);
  void (
      *set_native_exception_message)(HermesABIContext *, const char *, size_t);

  HermesABIPropNameID (
      *clone_prop_name_id)(HermesABIContext *, HermesABIPropNameID);
  HermesABIString (*clone_string)(HermesABIContext *, HermesABIString);
  HermesABISymbol (*clone_symbol)(HermesABIContext *, HermesABISymbol);
  HermesABIObject (*clone_object)(HermesABIContext *, HermesABIObject);
  HermesABIBigInt (*clone_big_int)(HermesABIContext *, HermesABIBigInt);

  /// Check if the given buffer contains Hermes bytecode.
  bool (*is_hermes_bytecode)(const uint8_t *, size_t);

  /// Evaluate the given JavaScript source or Hermes bytecode with an associated
  /// source URL in the given context, and return the result. The caller must
  /// ensure that bytecode passed to \c evaluate_hermes_bytecode is valid
  /// bytecode.
  HermesABIValueOrError (*evaluate_javascript_source)(
      HermesABIContext *,
      HermesABIBuffer *,
      const char *,
      size_t);
  HermesABIValueOrError (*evaluate_hermes_bytecode)(
      HermesABIContext *,
      HermesABIBuffer *,
      const char *,
      size_t);

  HermesABIObject (*get_global_object)(HermesABIContext *);
  HermesABIStringOrError (
      *create_string_from_ascii)(HermesABIContext *, const char *, size_t);
  HermesABIStringOrError (
      *create_string_from_utf8)(HermesABIContext *, const uint8_t *, size_t);

  HermesABIObjectOrError (*create_object)(HermesABIContext *);

  HermesABIBoolOrError (*has_object_property_from_string)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIString);
  HermesABIBoolOrError (*has_object_property_from_prop_name_id)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIPropNameID);

  HermesABIValueOrError (*get_object_property_from_string)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIString);
  HermesABIValueOrError (*get_object_property_from_prop_name_id)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIPropNameID);
  HermesABIVoidOrError (*set_object_property_from_string)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIString,
      const HermesABIValue *);
  HermesABIVoidOrError (*set_object_property_from_prop_name_id)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIPropNameID,
      const HermesABIValue *);

  HermesABIArrayOrError (
      *get_object_property_names)(HermesABIContext *, HermesABIObject);

  HermesABIArrayOrError (*create_array)(HermesABIContext *, size_t);
  size_t (*get_array_length)(HermesABIContext *, HermesABIArray);
  HermesABIValueOrError (
      *get_array_value_at_index)(HermesABIContext *, HermesABIArray, size_t);
  HermesABIVoidOrError (*set_array_value_at_index)(
      HermesABIContext *,
      HermesABIArray,
      size_t,
      const HermesABIValue *);

  HermesABIArrayBufferOrError (*create_array_buffer_from_external_data)(
      HermesABIContext *,
      HermesABIMutableBuffer *);
  HermesABIUint8PtrOrError (
      *get_array_buffer_data)(HermesABIContext *, HermesABIArrayBuffer);
  HermesABISizeTOrError (
      *get_array_buffer_size)(HermesABIContext *, HermesABIArrayBuffer);

  HermesABIPropNameIDOrError (*create_prop_name_id_from_ascii)(
      HermesABIContext *,
      const char *,
      size_t);
  HermesABIPropNameIDOrError (*create_prop_name_id_from_utf8)(
      HermesABIContext *,
      const uint8_t *,
      size_t);
  HermesABIPropNameIDOrError (
      *create_prop_name_id_from_string)(HermesABIContext *, HermesABIString);
  HermesABIPropNameIDOrError (
      *create_prop_name_id_from_symbol)(HermesABIContext *, HermesABISymbol);
  bool (*prop_name_id_equals)(
      HermesABIContext *,
      HermesABIPropNameID,
      HermesABIPropNameID);

  bool (*object_is_array)(HermesABIContext *, HermesABIObject);
  bool (*object_is_array_buffer)(HermesABIContext *, HermesABIObject);
  bool (*object_is_function)(HermesABIContext *, HermesABIObject);
  bool (*object_is_host_object)(HermesABIContext *, HermesABIObject);
  bool (*function_is_host_function)(HermesABIContext *, HermesABIFunction);

  HermesABIValueOrError (*call)(
      HermesABIContext *,
      HermesABIFunction,
      const HermesABIValue *,
      const HermesABIValue *,
      size_t);
  HermesABIValueOrError (*call_as_constructor)(
      HermesABIContext *,
      HermesABIFunction,
      const HermesABIValue *,
      size_t);

  HermesABIFunctionOrError (*create_function_from_host_function)(
      HermesABIContext *,
      HermesABIPropNameID,
      unsigned int,
      HermesABIHostFunction *);
  HermesABIHostFunction *(
      *get_host_function)(HermesABIContext *, HermesABIFunction);

  HermesABIObjectOrError (*create_object_from_host_object)(
      HermesABIContext *,
      HermesABIHostObject *);
  HermesABIHostObject *(*get_host_object)(HermesABIContext *, HermesABIObject);

  bool (*has_native_state)(HermesABIContext *, HermesABIObject);
  HermesABINativeState *(
      *get_native_state)(HermesABIContext *, HermesABIObject);
  HermesABIVoidOrError (*set_native_state)(
      HermesABIContext *,
      HermesABIObject,
      HermesABINativeState *);

  HermesABIWeakObjectOrError (
      *create_weak_object)(HermesABIContext *, HermesABIObject);
  HermesABIValue (*lock_weak_object)(HermesABIContext *, HermesABIWeakObject);

  void (*get_utf8_from_string)(
      HermesABIContext *,
      HermesABIString,
      HermesABIByteBuffer *);
  void (*get_utf8_from_prop_name_id)(
      HermesABIContext *,
      HermesABIPropNameID,
      HermesABIByteBuffer *);
  void (*get_utf8_from_symbol)(
      HermesABIContext *,
      HermesABISymbol,
      HermesABIByteBuffer *);

  HermesABIBoolOrError (
      *instance_of)(HermesABIContext *, HermesABIObject, HermesABIFunction);

  bool (*strict_equals_symbol)(
      HermesABIContext *,
      HermesABISymbol,
      HermesABISymbol);
  bool (*strict_equals_bigint)(
      HermesABIContext *,
      HermesABIBigInt,
      HermesABIBigInt);
  bool (*strict_equals_string)(
      HermesABIContext *,
      HermesABIString,
      HermesABIString);
  bool (*strict_equals_object)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIObject);

  HermesABIBoolOrError (*drain_microtasks)(HermesABIContext *, int);

  HermesABIBigIntOrError (
      *create_bigint_from_int64)(HermesABIContext *, int64_t);
  HermesABIBigIntOrError (
      *create_bigint_from_uint64)(HermesABIContext *, uint64_t);
  bool (*bigint_is_int64)(HermesABIContext *, HermesABIBigInt);
  bool (*bigint_is_uint64)(HermesABIContext *, HermesABIBigInt);
  uint64_t (*bigint_truncate_to_uint64)(HermesABIContext *, HermesABIBigInt);
  HermesABIStringOrError (
      *bigint_to_string)(HermesABIContext *, HermesABIBigInt, unsigned);
};

#ifdef __cplusplus
}
#endif

#endif // !HERMES_ABI_HERMES_ABI_H
