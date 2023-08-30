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

#ifndef HERMES_CALL
#ifdef _WIN32
#define HERMES_CALL __stdcall
#else
#define HERMES_CALL
#endif
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
  void(HERMES_CALL *invalidate)(HermesABIManagedPointer *);
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
  void(HERMES_CALL *release)(HermesABIBuffer *);
};
struct HermesABIBuffer {
  const HermesABIBufferVTable *vtable;
  const uint8_t *data;
  size_t size;
};

struct HermesABIMutableBufferVTable {
  void(HERMES_CALL *release)(HermesABIMutableBuffer *);
};
struct HermesABIMutableBuffer {
  const HermesABIMutableBufferVTable *vtable;
  uint8_t *data;
  size_t size;
};

struct HermesABIByteBufferVTable {
  void(HERMES_CALL *grow_by)(HermesABIByteBuffer *, size_t);
};
struct HermesABIByteBuffer {
  const HermesABIByteBufferVTable *vtable;
  uint8_t *data;
  size_t available;
};

/// Define the structure for host functions. This is designed to recreate the
/// functionality of jsi::HostFunction.
struct HermesABIHostFunctionVTable {
  HermesABIValueOrError(HERMES_CALL *call)(
      HermesABIHostFunction *,
      HermesABIContext *,
      const HermesABIValue *,
      const HermesABIValue *,
      size_t);
  void(HERMES_CALL *release)(HermesABIHostFunction *);
};
struct HermesABIHostFunction {
  const HermesABIHostFunctionVTable *vtable;
};

/// Define the structure for lists of PropNameIDs, so that they can be returned
/// by get_property_names on a HostObject.
struct HermesABIPropNameIDListVTable {
  void(HERMES_CALL *release)(HermesABIPropNameIDList *);
};
struct HermesABIPropNameIDList {
  const HermesABIPropNameIDListVTable *vtable;
  const HermesABIPropNameID *props;
  size_t size;
};

/// Define the structure for host objects. This is designed to recreate the
/// functionality of jsi::HostObject.
struct HermesABIHostObjectVTable {
  HermesABIValueOrError(HERMES_CALL *get)(
      HermesABIHostObject *,
      HermesABIContext *,
      HermesABIPropNameID);
  HermesABIVoidOrError(HERMES_CALL *set)(
      HermesABIHostObject *,
      HermesABIContext *,
      HermesABIPropNameID,
      const HermesABIValue *);
  HermesABIPropNameIDListPtrOrError(HERMES_CALL *get_property_names)(
      HermesABIHostObject *,
      HermesABIContext *);
  void(HERMES_CALL *release)(HermesABIHostObject *);
};
struct HermesABIHostObject {
  const HermesABIHostObjectVTable *vtable;
};

struct HermesABINativeStateVTable {
  void(HERMES_CALL *release)(HermesABINativeState *);
};
struct HermesABINativeState {
  const HermesABINativeStateVTable *vtable;
};

struct HermesABIVTable {
  /// Create a new instance of a Hermes Runtime, and return a pointer to its
  /// associated context. The context must be released with
  /// release_hermes_runtime when it is no longer needed.
  HermesABIContext *(HERMES_CALL *make_hermes_runtime)(
      const HermesABIRuntimeConfig *);
  /// Release the Hermes Runtime associated with the given context.
  void(HERMES_CALL *release_hermes_runtime)(HermesABIContext *);

  /// Methods for retrieving and clearing exceptions. An exception should be
  /// retrieved if and only if some method returned an error value.
  /// Get and clear the stored JS exception value. The returned value is
  /// guaranteed to not be another exception. This should be called exactly once
  /// after an exception is thrown.
  HermesABIValue(HERMES_CALL *get_and_clear_js_error_value)(HermesABIContext *);
  HermesABIByteRef(HERMES_CALL *get_native_exception_message)(
      HermesABIContext *);
  /// Clear the stored native exception message. This should be called exactly
  /// once after the message is retrieved.
  void(HERMES_CALL *clear_native_exception_message)(HermesABIContext *);

  /// Set the current error before returning control to the ABI. These are
  /// intended to be used to throw exceptions from HostFunctions and
  /// HostObjects.
  void(HERMES_CALL *set_js_error_value)(
      HermesABIContext *,
      const HermesABIValue *);
  void(HERMES_CALL *set_native_exception_message)(
      HermesABIContext *,
      const char *,
      size_t);

  HermesABIPropNameID(
      HERMES_CALL *clone_prop_name_id)(HermesABIContext *, HermesABIPropNameID);
  HermesABIString(
      HERMES_CALL *clone_string)(HermesABIContext *, HermesABIString);
  HermesABISymbol(
      HERMES_CALL *clone_symbol)(HermesABIContext *, HermesABISymbol);
  HermesABIObject(
      HERMES_CALL *clone_object)(HermesABIContext *, HermesABIObject);
  HermesABIBigInt(
      HERMES_CALL *clone_big_int)(HermesABIContext *, HermesABIBigInt);

  /// Check if the given buffer contains Hermes bytecode.
  bool(HERMES_CALL *is_hermes_bytecode)(const uint8_t *, size_t);

  /// Evaluate the given JavaScript source or Hermes bytecode with an associated
  /// source URL in the given context, and return the result. The caller must
  /// ensure that bytecode passed to \c evaluate_hermes_bytecode is valid
  /// bytecode.
  HermesABIValueOrError(HERMES_CALL *evaluate_javascript_source)(
      HermesABIContext *,
      HermesABIBuffer *,
      const char *,
      size_t);
  HermesABIValueOrError(HERMES_CALL *evaluate_hermes_bytecode)(
      HermesABIContext *,
      HermesABIBuffer *,
      const char *,
      size_t);

  HermesABIObject(HERMES_CALL *get_global_object)(HermesABIContext *);
  HermesABIStringOrError(HERMES_CALL *create_string_from_ascii)(
      HermesABIContext *,
      const char *,
      size_t);
  HermesABIStringOrError(HERMES_CALL *create_string_from_utf8)(
      HermesABIContext *,
      const uint8_t *,
      size_t);

  HermesABIObjectOrError(HERMES_CALL *create_object)(HermesABIContext *);

  HermesABIBoolOrError(HERMES_CALL *has_object_property_from_string)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIString);
  HermesABIBoolOrError(HERMES_CALL *has_object_property_from_prop_name_id)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIPropNameID);

  HermesABIValueOrError(HERMES_CALL *get_object_property_from_string)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIString);
  HermesABIValueOrError(HERMES_CALL *get_object_property_from_prop_name_id)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIPropNameID);
  HermesABIVoidOrError(HERMES_CALL *set_object_property_from_string)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIString,
      const HermesABIValue *);
  HermesABIVoidOrError(HERMES_CALL *set_object_property_from_prop_name_id)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIPropNameID,
      const HermesABIValue *);

  HermesABIArrayOrError(HERMES_CALL *get_object_property_names)(
      HermesABIContext *,
      HermesABIObject);

  HermesABIArrayOrError(HERMES_CALL *create_array)(HermesABIContext *, size_t);
  size_t(HERMES_CALL *get_array_length)(HermesABIContext *, HermesABIArray);
  HermesABIValueOrError(HERMES_CALL *get_array_value_at_index)(
      HermesABIContext *,
      HermesABIArray,
      size_t);
  HermesABIVoidOrError(HERMES_CALL *set_array_value_at_index)(
      HermesABIContext *,
      HermesABIArray,
      size_t,
      const HermesABIValue *);

  HermesABIArrayBufferOrError(
      HERMES_CALL *create_array_buffer_from_external_data)(
      HermesABIContext *,
      HermesABIMutableBuffer *);
  HermesABIUint8PtrOrError(HERMES_CALL *get_array_buffer_data)(
      HermesABIContext *,
      HermesABIArrayBuffer);
  HermesABISizeTOrError(HERMES_CALL *get_array_buffer_size)(
      HermesABIContext *,
      HermesABIArrayBuffer);

  HermesABIPropNameIDOrError(HERMES_CALL *create_prop_name_id_from_ascii)(
      HermesABIContext *,
      const char *,
      size_t);
  HermesABIPropNameIDOrError(HERMES_CALL *create_prop_name_id_from_utf8)(
      HermesABIContext *,
      const uint8_t *,
      size_t);
  HermesABIPropNameIDOrError(HERMES_CALL *create_prop_name_id_from_string)(
      HermesABIContext *,
      HermesABIString);
  HermesABIPropNameIDOrError(HERMES_CALL *create_prop_name_id_from_symbol)(
      HermesABIContext *,
      HermesABISymbol);
  bool(HERMES_CALL *prop_name_id_equals)(
      HermesABIContext *,
      HermesABIPropNameID,
      HermesABIPropNameID);

  bool(HERMES_CALL *object_is_array)(HermesABIContext *, HermesABIObject);
  bool(
      HERMES_CALL *object_is_array_buffer)(HermesABIContext *, HermesABIObject);
  bool(HERMES_CALL *object_is_function)(HermesABIContext *, HermesABIObject);
  bool(HERMES_CALL *object_is_host_object)(HermesABIContext *, HermesABIObject);
  bool(HERMES_CALL *function_is_host_function)(
      HermesABIContext *,
      HermesABIFunction);

  HermesABIValueOrError(HERMES_CALL *call)(
      HermesABIContext *,
      HermesABIFunction,
      const HermesABIValue *,
      const HermesABIValue *,
      size_t);
  HermesABIValueOrError(HERMES_CALL *call_as_constructor)(
      HermesABIContext *,
      HermesABIFunction,
      const HermesABIValue *,
      size_t);

  HermesABIFunctionOrError(HERMES_CALL *create_function_from_host_function)(
      HermesABIContext *,
      HermesABIPropNameID,
      unsigned int,
      HermesABIHostFunction *);
  HermesABIHostFunction *(
      HERMES_CALL *get_host_function)(HermesABIContext *, HermesABIFunction);

  HermesABIObjectOrError(HERMES_CALL *create_object_from_host_object)(
      HermesABIContext *,
      HermesABIHostObject *);
  HermesABIHostObject *(
      HERMES_CALL *get_host_object)(HermesABIContext *, HermesABIObject);

  bool(HERMES_CALL *has_native_state)(HermesABIContext *, HermesABIObject);
  HermesABINativeState *(
      HERMES_CALL *get_native_state)(HermesABIContext *, HermesABIObject);
  HermesABIVoidOrError(HERMES_CALL *set_native_state)(
      HermesABIContext *,
      HermesABIObject,
      HermesABINativeState *);

  HermesABIWeakObjectOrError(
      HERMES_CALL *create_weak_object)(HermesABIContext *, HermesABIObject);
  HermesABIValue(
      HERMES_CALL *lock_weak_object)(HermesABIContext *, HermesABIWeakObject);

  void(HERMES_CALL *get_utf8_from_string)(
      HermesABIContext *,
      HermesABIString,
      HermesABIByteBuffer *);
  void(HERMES_CALL *get_utf8_from_prop_name_id)(
      HermesABIContext *,
      HermesABIPropNameID,
      HermesABIByteBuffer *);
  void(HERMES_CALL *get_utf8_from_symbol)(
      HermesABIContext *,
      HermesABISymbol,
      HermesABIByteBuffer *);

  HermesABIBoolOrError(HERMES_CALL *instance_of)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIFunction);

  bool(HERMES_CALL *strict_equals_symbol)(
      HermesABIContext *,
      HermesABISymbol,
      HermesABISymbol);
  bool(HERMES_CALL *strict_equals_bigint)(
      HermesABIContext *,
      HermesABIBigInt,
      HermesABIBigInt);
  bool(HERMES_CALL *strict_equals_string)(
      HermesABIContext *,
      HermesABIString,
      HermesABIString);
  bool(HERMES_CALL *strict_equals_object)(
      HermesABIContext *,
      HermesABIObject,
      HermesABIObject);

  HermesABIBoolOrError(HERMES_CALL *drain_microtasks)(HermesABIContext *, int);

  HermesABIBigIntOrError(
      HERMES_CALL *create_bigint_from_int64)(HermesABIContext *, int64_t);
  HermesABIBigIntOrError(
      HERMES_CALL *create_bigint_from_uint64)(HermesABIContext *, uint64_t);
  bool(HERMES_CALL *bigint_is_int64)(HermesABIContext *, HermesABIBigInt);
  bool(HERMES_CALL *bigint_is_uint64)(HermesABIContext *, HermesABIBigInt);
  uint64_t(HERMES_CALL *bigint_truncate_to_uint64)(
      HermesABIContext *,
      HermesABIBigInt);
  HermesABIStringOrError(HERMES_CALL *bigint_to_string)(
      HermesABIContext *,
      HermesABIBigInt,
      unsigned);
};

#endif
