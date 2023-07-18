/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_HERMES_JSI_H
#define HERMES_HERMES_JSI_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jsi_host_object_s *jsi_host_object;
typedef struct jsi_host_function_s *jsi_host_function;
typedef struct JsiNativeState_s *JsiNativeState;
typedef struct JsiMutableBuffer_s *JsiMutableBuffer;
typedef struct JsiScopeState_s *JsiScopeState;
typedef struct abi_string_s *abi_string;

#ifdef __cplusplus
enum class JsiValueKind {
  Undefined,
  Null,
  Boolean,
  Number,
  Symbol,
  BigInt,
  String,
  Object,
};
#else
enum JsiValueKind {
  JsiValueUndefined,
  JsiValueNull,
  JsiValueBoolean,
  JsiValueNumber,
  JsiValueSymbol,
  JsiValueBigInt,
  JsiValueString,
  JsiValueObject,
};
#endif

struct JsiValue {
  JsiValueKind kind;
  uint64_t data;
};

#ifdef __cplusplus
enum class JsiErrorType {
  JSError,
  NativeException,
};
#else
enum JsiErrorType {
  JsiErrorTypeJSError,
  JsiErrorTypeNativeException,
};
#endif

#define JSICALL __cdecl

enum jsi_status {
  jsi_status_ok,
  jsi_status_error,
};

struct JsiRuntime;
struct JsiBuffer;
struct JsiPreparedJavaScript;
struct JsiHostObject;
struct JsiHostFunction;
struct JsiError;
struct JsiPointer;

struct JsiPointerVTable {
  jsi_status(JSICALL *release)(const JsiPointer *pointer);
};

struct JsiPointer {
  const JsiPointerVTable *vtable;

  jsi_status release() const {
    return vtable->release(this);
  }
};

// TODO: express it differently in C API
struct JsiSymbol : JsiPointer {};
struct JsiBigInt : JsiPointer {};
struct JsiString : JsiPointer {};
struct JsiObject : JsiPointer {};
struct JsiPropNameID : JsiPointer {};
struct JsiWeakObject : JsiPointer {};

typedef void(JSICALL *JsiToUtf8Callback)(
    const uint8_t *utf8,
    size_t size,
    void *receiver);
typedef void(JSICALL *JsiPropNameIDSpanCallback)(
    const JsiPropNameID **data,
    size_t size,
    void *receiver);
typedef void(JSICALL *JsiDeleter)(void *data);

struct JsiBufferVTable {
  jsi_status(JSICALL *destroy)(const JsiBuffer *buffer);
  jsi_status(JSICALL *getSpan)(
      const JsiBuffer *buffer,
      const uint8_t **data,
      size_t *size);
};

#ifdef __cplusplus
struct JsiBuffer {
  JsiBuffer(const JsiBufferVTable *vtable) : vtable(vtable) {}

  jsi_status destroy() const {
    return vtable->destroy(this);
  }

  jsi_status getSpan(const uint8_t **data, size_t *size) const {
    return vtable->getSpan(this, data, size);
  }

 private:
  const JsiBufferVTable *vtable;
};
#endif

struct JsiPreparedJavaScriptVTable {
  jsi_status(JSICALL *destroy)(const JsiPreparedJavaScript *script);
};

#ifdef __cplusplus
struct JsiPreparedJavaScript {
  JsiPreparedJavaScript(const JsiPreparedJavaScriptVTable *vtable)
      : vtable(vtable) {}

  jsi_status destroy() const {
    return vtable->destroy(this);
  }

 private:
  const JsiPreparedJavaScriptVTable *vtable;
};
#endif

struct JsiErrorVTable {
  jsi_status(JSICALL *destroy)(const JsiError *error);
  jsi_status(JSICALL *errorType)(const JsiError *error, JsiErrorType *result);
  jsi_status(JSICALL *message)(const JsiError *error, const char **result);
  jsi_status(JSICALL *value)(const JsiError *error, const JsiValue **result);
};

#ifdef __cplusplus
struct JsiError {
  JsiError(const JsiErrorVTable *vtable) : vtable(vtable) {}

  jsi_status destroy() const {
    return vtable->destroy(this);
  }

  jsi_status errorType(JsiErrorType *result) const {
    return vtable->errorType(this, result);
  }

  jsi_status message(const char **result) const {
    return vtable->message(this, result);
  }

  jsi_status value(const JsiValue **result) const {
    return vtable->value(this, result);
  }

 private:
  const JsiErrorVTable *vtable;
};

struct IJsiError {
  virtual jsi_status JSICALL destroy() const = 0;
  virtual jsi_status JSICALL errorType(JsiErrorType *result) const = 0;
  virtual jsi_status JSICALL message(const char **result) const = 0;
  virtual jsi_status JSICALL value(const JsiValue **result) const = 0;
};

#else
struct JsiError {
  const JsiErrorVTable *vtable;
};
#endif

struct JsiHostObjectVTable {
  jsi_status(JSICALL *destroy)(JsiHostObject *hostObject);
  jsi_status(JSICALL *get)(
      JsiHostObject *hostObject,
      JsiRuntime *runtime,
      JsiPropNameID *name,
      JsiValue *result);
  jsi_status(JSICALL *set)(
      JsiHostObject *hostObject,
      JsiRuntime *runtime,
      JsiPropNameID *name,
      JsiValue *value);
  jsi_status(JSICALL *getPropertyNames)(
      JsiHostObject *hostObject,
      JsiRuntime *runtime,
      JsiPropNameIDSpanCallback getNames,
      void *receiver);
};

#ifdef __cplusplus
struct JsiHostObject {
  JsiHostObject(const JsiHostObjectVTable *vtable) : vtable(vtable) {}

  jsi_status destroy() {
    return vtable->destroy(this);
  }

  jsi_status get(JsiRuntime *runtime, JsiPropNameID *name, JsiValue *result) {
    return vtable->get(this, runtime, name, result);
  }

  jsi_status set(JsiRuntime *runtime, JsiPropNameID *name, JsiValue *value) {
    return vtable->set(this, runtime, name, value);
  }

  jsi_status getPropertyNames(
      JsiRuntime *runtime,
      JsiPropNameIDSpanCallback getNames,
      void *receiver) {
    return vtable->getPropertyNames(this, runtime, getNames, receiver);
  }

 private:
  const JsiHostObjectVTable *vtable;
};
#endif

struct JsiHostFunctionVTable {
  jsi_status(
      JSICALL *runtime)(JsiHostFunction *hostFunction, JsiRuntime **result);
  jsi_status(JSICALL *destroy)(JsiHostFunction *hostFunction);
  jsi_status(JSICALL *invoke)(
      JsiHostFunction *hostFunction,
      JsiRuntime *runtime,
      const JsiValue *thisArg,
      const JsiValue *args,
      size_t argCount,
      JsiValue *result);
};

#ifdef __cplusplus
struct JsiHostFunction {
  JsiHostFunction(const JsiHostFunctionVTable *vtable) : vtable(vtable) {}

  jsi_status runtime(JsiRuntime **result) {
    return vtable->runtime(this, result);
  }

  jsi_status destroy() {
    return vtable->destroy(this);
  }

  jsi_status invoke(
      JsiRuntime *runtime,
      const JsiValue *thisArg,
      const JsiValue *args,
      size_t argCount,
      JsiValue *result) {
    return vtable->invoke(this, runtime, thisArg, args, argCount, result);
  }

 private:
  const JsiHostFunctionVTable *vtable;
};
#endif

struct JsiRuntimeVTable {
  jsi_status(JSICALL *evaluateJavaScript)(
      JsiRuntime *runtime,
      const JsiBuffer *buffer,
      const char *source_url,
      JsiValue *result);

  jsi_status(JSICALL *prepareJavaScript)(
      JsiRuntime *runtime,
      const JsiBuffer *buffer,
      const char *sourceUrl,
      JsiPreparedJavaScript **result);

  jsi_status(JSICALL *evaluatePreparedJavaScript)(
      JsiRuntime *runtime,
      const JsiPreparedJavaScript *prepared_script,
      JsiValue *result);

  jsi_status(JSICALL *drainMicrotasks)(
      JsiRuntime *runtime,
      int32_t max_count_hint,
      bool *result);

  jsi_status(JSICALL *getGlobal)(JsiRuntime *runtime, JsiObject **result);

  jsi_status(JSICALL *getDescription)(JsiRuntime *runtime, const char **result);

  jsi_status(JSICALL *isInspectable)(JsiRuntime *runtime, bool *result);

  jsi_status(JSICALL *cloneSymbol)(
      JsiRuntime *runtime,
      const JsiSymbol *symbol,
      JsiSymbol **result);
  jsi_status(JSICALL *cloneBigInt)(
      JsiRuntime *runtime,
      const JsiBigInt *bigint,
      JsiBigInt **result);
  jsi_status(JSICALL *cloneString)(
      JsiRuntime *runtime,
      const JsiString *str,
      JsiString **result);
  jsi_status(JSICALL *cloneObject)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      JsiObject **result);
  jsi_status(JSICALL *clonePropNameID)(
      JsiRuntime *runtime,
      const JsiPropNameID *property_id,
      JsiPropNameID **result);

  jsi_status(JSICALL *createPropNameIDFromAscii)(
      JsiRuntime *runtime,
      const char *ascii,
      size_t length,
      JsiPropNameID **result);
  jsi_status(JSICALL *createPropNameIDFromUtf8)(
      JsiRuntime *runtime,
      const uint8_t *utf8,
      size_t length,
      JsiPropNameID **result);
  jsi_status(JSICALL *createPropNameIDFromString)(
      JsiRuntime *runtime,
      const JsiString *str,
      JsiPropNameID **result);
  jsi_status(JSICALL *createPropNameIDFromSymbol)(
      JsiRuntime *runtime,
      const JsiSymbol *symbol,
      JsiPropNameID **result);
  jsi_status(JSICALL *propNameIDToUtf8)(
      JsiRuntime *runtime,
      const JsiPropNameID *propertyId,
      JsiToUtf8Callback toUtf8,
      void *receiver);
  jsi_status(JSICALL *propNameIDEquals)(
      JsiRuntime *runtime,
      const JsiPropNameID *left,
      const JsiPropNameID *right,
      bool *result);

  jsi_status(JSICALL *symbolToUtf8)(
      JsiRuntime *runtime,
      const JsiSymbol *symbol,
      JsiToUtf8Callback toUtf8,
      void *receiver);

  jsi_status(JSICALL *createBigIntFromInt64)(
      JsiRuntime *runtime,
      int64_t value,
      JsiBigInt **result);
  jsi_status(JSICALL *createBigIntFromUInt64)(
      JsiRuntime *runtime,
      uint64_t value,
      JsiBigInt **result);
  jsi_status(JSICALL *bigIntIsInt64)(
      JsiRuntime *runtime,
      const JsiBigInt *value,
      bool *result);
  jsi_status(JSICALL *bigIntIsUInt64)(
      JsiRuntime *runtime,
      const JsiBigInt *value,
      bool *result);
  jsi_status(JSICALL *truncateBigInt)(
      JsiRuntime *runtime,
      const JsiBigInt *value,
      uint64_t *result);
  jsi_status(JSICALL *bigIntToString)(
      JsiRuntime *runtime,
      const JsiBigInt *value,
      int32_t radix,
      JsiString **result);

  jsi_status(JSICALL *createStringFromAscii)(
      JsiRuntime *runtime,
      const char *ascii,
      size_t length,
      JsiString **result);
  jsi_status(JSICALL *createStringFromUtf8)(
      JsiRuntime *runtime,
      const uint8_t *utf8,
      size_t length,
      JsiString **result);
  jsi_status(JSICALL *stringToUtf8)(
      JsiRuntime *runtime,
      const JsiString *string,
      JsiToUtf8Callback toUtf8,
      void *receiver);

  jsi_status(JSICALL *createValueFromJsonUtf8)(
      JsiRuntime *runtime,
      const uint8_t *json,
      size_t length,
      JsiValue *result);

  jsi_status(JSICALL *createObject)(JsiRuntime *runtime, JsiObject **result);
  jsi_status(JSICALL *createObjectWithHostObject)(
      JsiRuntime *runtime,
      JsiHostObject *host_object,
      JsiObject **result);
  jsi_status(JSICALL *getHostObject)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      JsiHostObject **result);
  jsi_status(JSICALL *getHostFunction)(
      JsiRuntime *runtime,
      const JsiObject *func,
      JsiHostFunction **result);

  jsi_status(JSICALL *hasNativeState)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      bool *result);
  jsi_status(JSICALL *getNativeState)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      JsiNativeState *result);
  jsi_status(JSICALL *setNativeState)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      JsiNativeState state,
      JsiDeleter deleter);

  jsi_status(JSICALL *getProperty)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      const JsiPropNameID *name,
      JsiValue *result);
  jsi_status(JSICALL *getPropertyWithStringKey)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      const JsiString *name,
      JsiValue *result);
  jsi_status(JSICALL *hasProperty)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      const JsiPropNameID *name,
      bool *result);
  jsi_status(JSICALL *hasPropertyWithStringKey)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      const JsiString *name,
      bool *result);
  jsi_status(JSICALL *setProperty)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      const JsiPropNameID *name,
      const JsiValue *value);
  jsi_status(JSICALL *setPropertyWithStringKey)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      const JsiString *name,
      const JsiValue *value);

  jsi_status(JSICALL *isArray)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      bool *result);
  jsi_status(JSICALL *isArrayBuffer)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      bool *result);
  jsi_status(JSICALL *isFunction)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      bool *result);
  jsi_status(JSICALL *isHostObject)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      bool *result);
  jsi_status(JSICALL *isHostFunction)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      bool *result);

  jsi_status(JSICALL *getPropertyNames)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      JsiObject **result);

  jsi_status(JSICALL *createWeakObject)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      JsiWeakObject **result);
  jsi_status(JSICALL *lockWeakObject)(
      JsiRuntime *runtime,
      const JsiWeakObject *obj,
      JsiValue *result);

  jsi_status(JSICALL *createArray)(
      JsiRuntime *runtime,
      size_t length,
      JsiObject **result);
  jsi_status(JSICALL *createArrayBuffer)(
      JsiRuntime *runtime,
      JsiMutableBuffer buffer,
      uint8_t *data,
      size_t size,
      JsiDeleter deleter,
      JsiObject **result);
  jsi_status(JSICALL *getArraySize)(
      JsiRuntime *runtime,
      const JsiObject *array,
      size_t *result);
  jsi_status(JSICALL *getArrayBufferSize)(
      JsiRuntime *runtime,
      const JsiObject *array_buffer,
      size_t *result);
  jsi_status(JSICALL *getArrayBufferData)(
      JsiRuntime *runtime,
      const JsiObject *array_buffer,
      uint8_t **result);
  jsi_status(JSICALL *getValueAtIndex)(
      JsiRuntime *runtime,
      const JsiObject *array,
      size_t index,
      JsiValue *result);
  jsi_status(JSICALL *setValueAtIndex)(
      JsiRuntime *runtime,
      const JsiObject *array,
      size_t index,
      const JsiValue *value);

  jsi_status(JSICALL *createFunction)(
      JsiRuntime *runtime,
      const JsiPropNameID *name,
      uint32_t param_count,
      JsiHostFunction *host_function,
      JsiObject **result);
  jsi_status(JSICALL *call)(
      JsiRuntime *runtime,
      const JsiObject *func,
      const JsiValue *this_arg,
      const JsiValue *args,
      size_t arg_count,
      JsiValue *result);
  jsi_status(JSICALL *callAsConstructor)(
      JsiRuntime *runtime,
      const JsiObject *func,
      const JsiValue *args,
      size_t arg_count,
      JsiValue *result);

  jsi_status(JSICALL *pushScope)(JsiRuntime *runtime, JsiScopeState *result);
  jsi_status(JSICALL *popScope)(JsiRuntime *runtime, JsiScopeState scope_state);

  jsi_status(JSICALL *symbolStrictEquals)(
      JsiRuntime *runtime,
      const JsiSymbol *left,
      const JsiSymbol *right,
      bool *result);
  jsi_status(JSICALL *bigintStrictEquals)(
      JsiRuntime *runtime,
      const JsiBigInt *left,
      const JsiBigInt *right,
      bool *result);
  jsi_status(JSICALL *stringStrictEquals)(
      JsiRuntime *runtime,
      const JsiString *left,
      const JsiString *right,
      bool *result);
  jsi_status(JSICALL *objectStrictEquals)(
      JsiRuntime *runtime,
      const JsiObject *left,
      const JsiObject *right,
      bool *result);

  jsi_status(JSICALL *instanceOf)(
      JsiRuntime *runtime,
      const JsiObject *obj,
      const JsiObject *constructor,
      bool *result);

  jsi_status(
      JSICALL *getAndClearLastError)(JsiRuntime *runtime, JsiError **result);

  jsi_status(JSICALL *setError)(
      JsiRuntime *runtime,
      JsiErrorType error_kind,
      const char *error_details,
      const JsiValue *value);

  jsi_status(JSICALL *raiseJSError)(JsiRuntime *runtime, const JsiValue *value);
};

#ifdef __cplusplus
struct JsiRuntime {
  const struct JsiRuntimeVTable *vtable;

  jsi_status evaluateJavaScript(
      const JsiBuffer *buffer,
      const char *source_url,
      JsiValue *result) {
    return vtable->evaluateJavaScript(this, buffer, source_url, result);
  }

  jsi_status prepareJavaScript(
      const JsiBuffer *buffer,
      const char *sourceUrl,
      JsiPreparedJavaScript **result) {
    return vtable->prepareJavaScript(this, buffer, sourceUrl, result);
  }

  jsi_status evaluatePreparedJavaScript(
      const JsiPreparedJavaScript *prepared_script,
      JsiValue *result) {
    return vtable->evaluatePreparedJavaScript(this, prepared_script, result);
  }

  jsi_status drainMicrotasks(int32_t maxMicrotasksHint, bool *result) {
    return vtable->drainMicrotasks(this, maxMicrotasksHint, result);
  }

  jsi_status getGlobal(JsiObject **result) {
    return vtable->getGlobal(this, result);
  }

  jsi_status getDescription(const char **result) {
    return vtable->getDescription(this, result);
  }

  jsi_status isInspectable(bool *result) {
    return vtable->isInspectable(this, result);
  }

  jsi_status cloneSymbol(const JsiSymbol *symbol, JsiSymbol **result) {
    return vtable->cloneSymbol(this, symbol, result);
  }

  jsi_status cloneBigInt(const JsiBigInt *bigint, JsiBigInt **result) {
    return vtable->cloneBigInt(this, bigint, result);
  }

  jsi_status cloneString(const JsiString *str, JsiString **result) {
    return vtable->cloneString(this, str, result);
  }

  jsi_status cloneObject(const JsiObject *obj, JsiObject **result) {
    return vtable->cloneObject(this, obj, result);
  }

  jsi_status clonePropNameID(
      const JsiPropNameID *property_id,
      JsiPropNameID **result) {
    return vtable->clonePropNameID(this, property_id, result);
  }

  jsi_status createPropNameIDFromAscii(
      const char *ascii,
      size_t length,
      JsiPropNameID **result) {
    return vtable->createPropNameIDFromAscii(this, ascii, length, result);
  }

  jsi_status createPropNameIDFromUtf8(
      const uint8_t *utf8,
      size_t length,
      JsiPropNameID **result) {
    return vtable->createPropNameIDFromUtf8(this, utf8, length, result);
  }

  jsi_status createPropNameIDFromString(
      const JsiString *str,
      JsiPropNameID **result) {
    return vtable->createPropNameIDFromString(this, str, result);
  }

  jsi_status createPropNameIDFromSymbol(
      const JsiSymbol *symbol,
      JsiPropNameID **result) {
    return vtable->createPropNameIDFromSymbol(this, symbol, result);
  }

  jsi_status propNameIDToUtf8(
      const JsiPropNameID *propertyId,
      JsiToUtf8Callback toUtf8,
      void *receiver) {
    return vtable->propNameIDToUtf8(this, propertyId, toUtf8, receiver);
  }

  jsi_status propNameIDEquals(
      const JsiPropNameID *left,
      const JsiPropNameID *right,
      bool *result) {
    return vtable->propNameIDEquals(this, left, right, result);
  }

  jsi_status symbolToUtf8(
      const JsiSymbol *symbol,
      JsiToUtf8Callback toUtf8,
      void *receiver) {
    return vtable->symbolToUtf8(this, symbol, toUtf8, receiver);
  }

  jsi_status createBigIntFromInt64(int64_t value, JsiBigInt **result) {
    return vtable->createBigIntFromInt64(this, value, result);
  }

  jsi_status createBigIntFromUInt64(uint64_t value, JsiBigInt **result) {
    return vtable->createBigIntFromUInt64(this, value, result);
  }

  jsi_status bigIntIsInt64(const JsiBigInt *value, bool *result) {
    return vtable->bigIntIsInt64(this, value, result);
  }

  jsi_status bigIntIsUInt64(const JsiBigInt *value, bool *result) {
    return vtable->bigIntIsUInt64(this, value, result);
  }

  jsi_status truncateBigInt(const JsiBigInt *value, uint64_t *result) {
    return vtable->truncateBigInt(this, value, result);
  }

  jsi_status
  bigIntToString(const JsiBigInt *value, int32_t radix, JsiString **result) {
    return vtable->bigIntToString(this, value, radix, result);
  }

  jsi_status
  createStringFromAscii(const char *ascii, size_t length, JsiString **result) {
    return vtable->createStringFromAscii(this, ascii, length, result);
  }

  jsi_status
  createStringFromUtf8(const uint8_t *utf8, size_t length, JsiString **result) {
    return vtable->createStringFromUtf8(this, utf8, length, result);
  }

  jsi_status stringToUtf8(
      const JsiString *string,
      JsiToUtf8Callback toUtf8,
      void *receiver) {
    return vtable->stringToUtf8(this, string, toUtf8, receiver);
  }

  jsi_status createValueFromJsonUtf8(
      const uint8_t *json,
      size_t length,
      JsiValue *result) {
    return vtable->createValueFromJsonUtf8(this, json, length, result);
  }

  jsi_status createObject(JsiObject **result) {
    return vtable->createObject(this, result);
  }

  jsi_status createObjectWithHostObject(
      JsiHostObject *host_object,
      JsiObject **result) {
    return vtable->createObjectWithHostObject(this, host_object, result);
  }

  jsi_status getHostObject(const JsiObject *obj, JsiHostObject **result) {
    return vtable->getHostObject(this, obj, result);
  }

  jsi_status getHostFunction(const JsiObject *func, JsiHostFunction **result) {
    return vtable->getHostFunction(this, func, result);
  }

  jsi_status hasNativeState(const JsiObject *obj, bool *result) {
    return vtable->hasNativeState(this, obj, result);
  }

  jsi_status getNativeState(const JsiObject *obj, JsiNativeState *result) {
    return vtable->getNativeState(this, obj, result);
  }

  jsi_status setNativeState(
      const JsiObject *obj,
      JsiNativeState state,
      JsiDeleter deleter) {
    return vtable->setNativeState(this, obj, state, deleter);
  }

  jsi_status getProperty(
      const JsiObject *obj,
      const JsiPropNameID *name,
      JsiValue *result) {
    return vtable->getProperty(this, obj, name, result);
  }

  jsi_status getPropertyWithStringKey(
      const JsiObject *obj,
      const JsiString *name,
      JsiValue *result) {
    return vtable->getPropertyWithStringKey(this, obj, name, result);
  }

  jsi_status
  hasProperty(const JsiObject *obj, const JsiPropNameID *name, bool *result) {
    return vtable->hasProperty(this, obj, name, result);
  }

  jsi_status hasPropertyWithStringKey(
      const JsiObject *obj,
      const JsiString *name,
      bool *result) {
    return vtable->hasPropertyWithStringKey(this, obj, name, result);
  }

  jsi_status setProperty(
      const JsiObject *obj,
      const JsiPropNameID *name,
      const JsiValue *value) {
    return vtable->setProperty(this, obj, name, value);
  }

  jsi_status setPropertyWithStringKey(
      const JsiObject *obj,
      const JsiString *name,
      const JsiValue *value) {
    return vtable->setPropertyWithStringKey(this, obj, name, value);
  }

  jsi_status isArray(const JsiObject *obj, bool *result) {
    return vtable->isArray(this, obj, result);
  }

  jsi_status isArrayBuffer(const JsiObject *obj, bool *result) {
    return vtable->isArrayBuffer(this, obj, result);
  }

  jsi_status isFunction(const JsiObject *obj, bool *result) {
    return vtable->isFunction(this, obj, result);
  }

  jsi_status isHostObject(const JsiObject *obj, bool *result) {
    return vtable->isHostObject(this, obj, result);
  }

  jsi_status isHostFunction(const JsiObject *obj, bool *result) {
    return vtable->isHostFunction(this, obj, result);
  }

  jsi_status getPropertyNames(const JsiObject *obj, JsiObject **result) {
    return vtable->getPropertyNames(this, obj, result);
  }

  jsi_status createWeakObject(const JsiObject *obj, JsiWeakObject **result) {
    return vtable->createWeakObject(this, obj, result);
  }

  jsi_status lockWeakObject(const JsiWeakObject *obj, JsiValue *result) {
    return vtable->lockWeakObject(this, obj, result);
  }

  jsi_status createArray(size_t length, JsiObject **result) {
    return vtable->createArray(this, length, result);
  }

  jsi_status createArrayBuffer(
      JsiMutableBuffer buffer,
      uint8_t *data,
      size_t size,
      JsiDeleter deleter,
      JsiObject **result) {
    return vtable->createArrayBuffer(this, buffer, data, size, deleter, result);
  }

  jsi_status getArraySize(const JsiObject *array, size_t *result) {
    return vtable->getArraySize(this, array, result);
  }

  jsi_status getArrayBufferSize(const JsiObject *array_buffer, size_t *result) {
    return vtable->getArrayBufferSize(this, array_buffer, result);
  }

  jsi_status getArrayBufferData(
      const JsiObject *array_buffer,
      uint8_t **result) {
    return vtable->getArrayBufferData(this, array_buffer, result);
  }

  jsi_status
  getValueAtIndex(const JsiObject *array, size_t index, JsiValue *result) {
    return vtable->getValueAtIndex(this, array, index, result);
  }

  jsi_status
  setValueAtIndex(const JsiObject *array, size_t index, const JsiValue *value) {
    return vtable->setValueAtIndex(this, array, index, value);
  }

  jsi_status createFunction(
      const JsiPropNameID *name,
      uint32_t param_count,
      JsiHostFunction *host_function,
      JsiObject **result) {
    return vtable->createFunction(
        this, name, param_count, host_function, result);
  }

  jsi_status call(
      const JsiObject *func,
      const JsiValue *this_arg,
      const JsiValue *args,
      size_t arg_count,
      JsiValue *result) {
    return vtable->call(this, func, this_arg, args, arg_count, result);
  }

  jsi_status callAsConstructor(
      const JsiObject *func,
      const JsiValue *args,
      size_t arg_count,
      JsiValue *result) {
    return vtable->callAsConstructor(this, func, args, arg_count, result);
  }

  jsi_status pushScope(JsiScopeState *result) {
    return vtable->pushScope(this, result);
  }

  jsi_status popScope(JsiScopeState scope_state) {
    return vtable->popScope(this, scope_state);
  }

  jsi_status symbolStrictEquals(
      const JsiSymbol *left,
      const JsiSymbol *right,
      bool *result) {
    return vtable->symbolStrictEquals(this, left, right, result);
  }

  jsi_status bigintStrictEquals(
      const JsiBigInt *left,
      const JsiBigInt *right,
      bool *result) {
    return vtable->bigintStrictEquals(this, left, right, result);
  }

  jsi_status stringStrictEquals(
      const JsiString *left,
      const JsiString *right,
      bool *result) {
    return vtable->stringStrictEquals(this, left, right, result);
  }

  jsi_status objectStrictEquals(
      const JsiObject *left,
      const JsiObject *right,
      bool *result) {
    return vtable->objectStrictEquals(this, left, right, result);
  }

  jsi_status
  instanceOf(const JsiObject *obj, const JsiObject *constructor, bool *result) {
    return vtable->instanceOf(this, obj, constructor, result);
  }

  jsi_status getAndClearLastError(JsiError **result) {
    return vtable->getAndClearLastError(this, result);
  }

  jsi_status setError(
      JsiErrorType error_kind,
      const char *error_details,
      const JsiValue *value) {
    return vtable->setError(this, error_kind, error_details, value);
  }

  jsi_status raiseJSError(const JsiValue *value) {
    return vtable->raiseJSError(this, value);
  }
};
#endif

#ifdef __cplusplus
} // extern "C"
#endif

struct IJsiRuntime {
  virtual jsi_status JSICALL evaluateJavaScript(
      const JsiBuffer *buffer,
      const char *source_url,
      JsiValue *result) = 0;
  virtual jsi_status JSICALL prepareJavaScript(
      const JsiBuffer *buffer,
      const char *sourceUrl,
      JsiPreparedJavaScript **result) = 0;
  virtual jsi_status JSICALL evaluatePreparedJavaScript(
      const JsiPreparedJavaScript *prepared_script,
      JsiValue *result) = 0;
  virtual jsi_status JSICALL
  drainMicrotasks(int32_t maxMicrotasksHint, bool *result) = 0;
  virtual jsi_status JSICALL getGlobal(JsiObject **result) = 0;
  virtual jsi_status JSICALL getDescription(const char **result) = 0;
  virtual jsi_status JSICALL isInspectable(bool *result) = 0;
  virtual jsi_status JSICALL
  cloneSymbol(const JsiSymbol *symbol, JsiSymbol **result) = 0;
  virtual jsi_status JSICALL
  cloneBigInt(const JsiBigInt *bigint, JsiBigInt **result) = 0;
  virtual jsi_status JSICALL
  cloneString(const JsiString *str, JsiString **result) = 0;
  virtual jsi_status JSICALL
  cloneObject(const JsiObject *obj, JsiObject **result) = 0;
  virtual jsi_status JSICALL
  clonePropNameID(const JsiPropNameID *property_id, JsiPropNameID **result) = 0;
  virtual jsi_status JSICALL createPropNameIDFromAscii(
      const char *ascii,
      size_t length,
      JsiPropNameID **result) = 0;
  virtual jsi_status JSICALL createPropNameIDFromUtf8(
      const uint8_t *utf8,
      size_t length,
      JsiPropNameID **result) = 0;
  virtual jsi_status JSICALL
  createPropNameIDFromString(const JsiString *str, JsiPropNameID **result) = 0;
  virtual jsi_status JSICALL createPropNameIDFromSymbol(
      const JsiSymbol *symbol,
      JsiPropNameID **result) = 0;
  virtual jsi_status JSICALL propNameIDToUtf8(
      const JsiPropNameID *propertyId,
      JsiToUtf8Callback toUtf8,
      void *receiver) = 0;
  virtual jsi_status JSICALL propNameIDEquals(
      const JsiPropNameID *left,
      const JsiPropNameID *right,
      bool *result) = 0;
  virtual jsi_status JSICALL symbolToUtf8(
      const JsiSymbol *symbol,
      JsiToUtf8Callback toUtf8,
      void *receiver) = 0;
  virtual jsi_status JSICALL
  createBigIntFromInt64(int64_t value, JsiBigInt **result) = 0;
  virtual jsi_status JSICALL
  createBigIntFromUInt64(uint64_t value, JsiBigInt **result) = 0;
  virtual jsi_status JSICALL
  bigIntIsInt64(const JsiBigInt *value, bool *result) = 0;
  virtual jsi_status JSICALL
  bigIntIsUInt64(const JsiBigInt *value, bool *result) = 0;
  virtual jsi_status JSICALL
  truncateBigInt(const JsiBigInt *value, uint64_t *result) = 0;
  virtual jsi_status JSICALL
  bigIntToString(const JsiBigInt *value, int32_t radix, JsiString **result) = 0;
  virtual jsi_status JSICALL createStringFromAscii(
      const char *ascii,
      size_t length,
      JsiString **result) = 0;
  virtual jsi_status JSICALL createStringFromUtf8(
      const uint8_t *utf8,
      size_t length,
      JsiString **result) = 0;
  virtual jsi_status JSICALL stringToUtf8(
      const JsiString *string,
      JsiToUtf8Callback toUtf8,
      void *receiver) = 0;
  virtual jsi_status JSICALL createValueFromJsonUtf8(
      const uint8_t *json,
      size_t length,
      JsiValue *result) = 0;
  virtual jsi_status JSICALL createObject(JsiObject **result) = 0;
  virtual jsi_status JSICALL createObjectWithHostObject(
      JsiHostObject *host_object,
      JsiObject **result) = 0;
  virtual jsi_status JSICALL
  getHostObject(const JsiObject *obj, JsiHostObject **result) = 0;
  virtual jsi_status JSICALL
  getHostFunction(const JsiObject *func, JsiHostFunction **result) = 0;
  virtual jsi_status JSICALL
  hasNativeState(const JsiObject *obj, bool *result) = 0;
  virtual jsi_status JSICALL
  getNativeState(const JsiObject *obj, JsiNativeState *result) = 0;
  virtual jsi_status JSICALL setNativeState(
      const JsiObject *obj,
      JsiNativeState state,
      JsiDeleter deleter) = 0;
  virtual jsi_status JSICALL getProperty(
      const JsiObject *obj,
      const JsiPropNameID *name,
      JsiValue *result) = 0;
  virtual jsi_status JSICALL getPropertyWithStringKey(
      const JsiObject *obj,
      const JsiString *name,
      JsiValue *result) = 0;
  virtual jsi_status JSICALL hasProperty(
      const JsiObject *obj,
      const JsiPropNameID *name,
      bool *result) = 0;
  virtual jsi_status JSICALL hasPropertyWithStringKey(
      const JsiObject *obj,
      const JsiString *name,
      bool *result) = 0;
  virtual jsi_status JSICALL setProperty(
      const JsiObject *obj,
      const JsiPropNameID *name,
      const JsiValue *value) = 0;
  virtual jsi_status JSICALL setPropertyWithStringKey(
      const JsiObject *obj,
      const JsiString *name,
      const JsiValue *value) = 0;
  virtual jsi_status JSICALL isArray(const JsiObject *obj, bool *result) = 0;
  virtual jsi_status JSICALL
  isArrayBuffer(const JsiObject *obj, bool *result) = 0;
  virtual jsi_status JSICALL isFunction(const JsiObject *obj, bool *result) = 0;
  virtual jsi_status JSICALL
  isHostObject(const JsiObject *obj, bool *result) = 0;
  virtual jsi_status JSICALL
  isHostFunction(const JsiObject *obj, bool *result) = 0;
  virtual jsi_status JSICALL
  getPropertyNames(const JsiObject *obj, JsiObject **result) = 0;
  virtual jsi_status JSICALL
  createWeakObject(const JsiObject *obj, JsiWeakObject **result) = 0;
  virtual jsi_status JSICALL
  lockWeakObject(const JsiWeakObject *obj, JsiValue *result) = 0;
  virtual jsi_status JSICALL createArray(size_t length, JsiObject **result) = 0;
  virtual jsi_status JSICALL createArrayBuffer(
      JsiMutableBuffer buffer,
      uint8_t *data,
      size_t size,
      JsiDeleter deleter,
      JsiObject **result) = 0;
  virtual jsi_status JSICALL
  getArraySize(const JsiObject *array, size_t *result) = 0;
  virtual jsi_status JSICALL
  getArrayBufferSize(const JsiObject *array_buffer, size_t *result) = 0;
  virtual jsi_status JSICALL
  getArrayBufferData(const JsiObject *array_buffer, uint8_t **result) = 0;
  virtual jsi_status JSICALL
  getValueAtIndex(const JsiObject *array, size_t index, JsiValue *result) = 0;
  virtual jsi_status JSICALL setValueAtIndex(
      const JsiObject *array,
      size_t index,
      const JsiValue *value) = 0;
  virtual jsi_status JSICALL createFunction(
      const JsiPropNameID *name,
      uint32_t param_count,
      JsiHostFunction *host_function,
      JsiObject **result) = 0;
  virtual jsi_status JSICALL call(
      const JsiObject *func,
      const JsiValue *this_arg,
      const JsiValue *args,
      size_t arg_count,
      JsiValue *result) = 0;
  virtual jsi_status JSICALL callAsConstructor(
      const JsiObject *func,
      const JsiValue *args,
      size_t arg_count,
      JsiValue *result) = 0;
  virtual jsi_status JSICALL pushScope(JsiScopeState *result) = 0;
  virtual jsi_status JSICALL popScope(JsiScopeState scope_state) = 0;
  virtual jsi_status JSICALL symbolStrictEquals(
      const JsiSymbol *left,
      const JsiSymbol *right,
      bool *result) = 0;
  virtual jsi_status JSICALL bigintStrictEquals(
      const JsiBigInt *left,
      const JsiBigInt *right,
      bool *result) = 0;
  virtual jsi_status JSICALL stringStrictEquals(
      const JsiString *left,
      const JsiString *right,
      bool *result) = 0;
  virtual jsi_status JSICALL objectStrictEquals(
      const JsiObject *left,
      const JsiObject *right,
      bool *result) = 0;
  virtual jsi_status JSICALL instanceOf(
      const JsiObject *obj,
      const JsiObject *constructor,
      bool *result) = 0;
  virtual jsi_status JSICALL getAndClearLastError(JsiError **result) = 0;
  virtual jsi_status JSICALL setError(
      JsiErrorType error_kind,
      const char *error_details,
      const JsiValue *value) = 0;
  virtual jsi_status JSICALL raiseJSError(const JsiValue *value) = 0;
};

#endif // !HERMES_HERMES_JSI_H
