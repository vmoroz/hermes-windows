/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HermesSandboxRuntime.h"

#include "external/hermes_sandbox_impl_compiled.h"

using namespace facebook::jsi;

namespace {

struct SandboxManagedPointer;
struct SandboxBuffer;
struct SandboxGrowableBuffer;
struct SandboxHostFunction;
struct SandboxPropNameIDList;
struct SandboxHostObject;
struct SandboxNativeState;

/// Mirror the types defined in hermes_abi.h but in the form they would have in
/// the sandbox. For instance, pointers and size_t are replaced with uint32.

struct SandboxManagedPointerVTable {
  /// Pointer to the function that should be invoked when this reference is
  /// released.
  /* void (*)(HermesABIManagedPointer *) */ u32 invalidate;
};
struct SandboxManagedPointer {
  /* const HermesABIManagedPointerVTable * */ u32 vtable;
};

enum SandboxErrorCode {
  SandboxErrorCodeNativeException,
  SandboxErrorCodeJSError,
};

static_assert(
    alignof(SandboxManagedPointer) % 4 == 0,
    "SandboxManagedPointer must be at least aligned to pointer size.");

/// Define simple wrappers for the different pointer types.
#define SANDBOX_POINTER_TYPES(V) \
  V(Object)                      \
  V(Array)                       \
  V(String)                      \
  V(BigInt)                      \
  V(Symbol)                      \
  V(Function)                    \
  V(ArrayBuffer)                 \
  V(PropNameID)                  \
  V(WeakObject)

#define DECLARE_SANDBOX_POINTER_TYPE(name)      \
  struct Sandbox##name {                        \
    /* HermesABIManagedPointer* */ u32 pointer; \
  };                                            \
  struct Sandbox##name##OrError {               \
    u32 ptr_or_error;                           \
  };

SANDBOX_POINTER_TYPES(DECLARE_SANDBOX_POINTER_TYPE)
#undef DECLARE_SANDBOX_POINTER_TYPE

struct SandboxVoidOrError {
  u32 void_or_error;
};

struct SandboxBoolOrError {
  u32 bool_or_error;
};

struct SandboxUint8PtrOrError {
  bool is_error;
  union {
    u32 val;
    uint16_t error;
  } data;
};

struct SandboxSizeTOrError {
  bool is_error;
  union {
    u32 val;
    uint16_t error;
  } data;
};

struct SandboxPropNameIDListPtrOrError {
  u32 ptr_or_error;
};

/// Always set the top bit for pointers so they can be easily checked.
#define SANDBOX_POINTER_MASK (1 << 31)

enum SandboxValueKind {
  SandboxValueKindUndefined = 0,
  SandboxValueKindNull = 1,
  SandboxValueKindBoolean = 2,
  SandboxValueKindError = 3,
  SandboxValueKindNumber = 4,
  SandboxValueKindSymbol = 5 | SANDBOX_POINTER_MASK,
  SandboxValueKindBigInt = 6 | SANDBOX_POINTER_MASK,
  SandboxValueKindString = 7 | SANDBOX_POINTER_MASK,
  SandboxValueKindObject = 9 | SANDBOX_POINTER_MASK,
};

struct SandboxValue {
  SandboxValueKind kind;
  union {
    bool boolean;
    alignas(8) double number;
    /* HermesABIManagedPointer* */ u32 pointer;
    SandboxErrorCode error;
  } data;
};
struct SandboxValueOrError {
  SandboxValue value;
};

struct SandboxBufferVTable {
  /* void (*)(HermesABIBuffer *) */ u32 release;
};
struct SandboxBuffer {
  /* const HermesABIBufferVTable* */ u32 vtable;
  /* const uint8_t * */ u32 data;
  /* size_t */ u32 size;
};

struct SandboxGrowableBufferVTable {
  /* void (*)(struct HermesABIGrowableBuffer *, size_t) */ u32 try_grow_to;
};
struct SandboxGrowableBuffer {
  /* const struct HermesABIGrowableBufferVTable * */ u32 vtable;
  /* uint8_t * */ u32 data;
  /* size_t */ u32 size;
  /* size_t */ u32 used;
};

struct SandboxHostFunctionVTable {
  /* void (*)(struct HermesABIHostFunction *) */ u32 release;
  /* struct HermesABIValueOrError (*)(
         struct HermesABIHostFunction *,
         struct HermesABIRuntime *rt,
         const struct HermesABIValue *,
         const struct HermesABIValue *,
         size_t) */
  u32 call;
};
struct SandboxHostFunction {
  /* const struct HermesABIHostFunctionVTable * */ u32 vtable;
};

struct SandboxPropNameIDListVTable {
  /* void (*)(struct HermesABIPropNameIDList *) */ u32 release;
};
struct SandboxPropNameIDList {
  /* const struct HermesABIPropNameIDListVTable * */ u32 vtable;
  /* const struct HermesABIPropNameID * */ u32 props;
  /* size_t */ u32 size;
};

struct SandboxHostObjectVTable {
  /* void (*)(struct HermesABIHostObject *) */ u32 release;
  /* struct HermesABIValueOrError (*)(
         struct HermesABIHostObject *,
         struct HermesABIRuntime *rt,
         struct HermesABIPropNameID) */
  u32 get;
  /* struct HermesABIVoidOrError (*)(
         struct HermesABIHostObject *,
         struct HermesABIRuntime *rt,
         struct HermesABIPropNameID,
         const struct HermesABIValue *) */
  u32 set;
  /* struct HermesABIPropNameIDListPtrOrError (*)(
         struct HermesABIHostObject *,
         struct HermesABIRuntime *) */
  u32 get_own_keys;
};
struct SandboxHostObject {
  /* const struct HermesABIHostObjectVTable * */ u32 vtable;
};

struct SandboxNativeStateVTable {
  /* void (*)(struct HermesABINativeState *) */ u32 release;
};
struct SandboxNativeState {
  /* const struct HermesABINativeStateVTable *vtable */ u32 vtable;
};

/// List all of the vtable functions with their signatures.
#define SANDBOX_CONTEXT_VTABLE_FUNCTIONS(F)                                   \
  F(release, void, (w2c_hermes *, u32 srt))                                   \
  F(get_and_clear_js_error_value, void, (w2c_hermes *, u32 resVal, u32 srt))  \
  F(get_and_clear_native_exception_message,                                   \
    void,                                                                     \
    (w2c_hermes *, u32 srt, u32 buf))                                         \
  F(set_js_error_value, void, (w2c_hermes *, u32 srt, u32 val))               \
  F(set_native_exception_message,                                             \
    void,                                                                     \
    (w2c_hermes *, u32 srt, u32 buf, u32 len))                                \
  F(clone_propnameid, u32, (w2c_hermes *, u32 srt, u32 propnameid))           \
  F(clone_string, u32, (w2c_hermes *, u32 srt, u32 str))                      \
  F(clone_symbol, u32, (w2c_hermes *, u32 srt, u32 sym))                      \
  F(clone_object, u32, (w2c_hermes *, u32 srt, u32 obj))                      \
  F(clone_bigint, u32, (w2c_hermes *, u32 srt, u32 bi))                       \
  F(evaluate_javascript_source,                                               \
    void,                                                                     \
    (w2c_hermes *,                                                            \
     u32 resValOrError,                                                       \
     u32 srt,                                                                 \
     u32 buf,                                                                 \
     u32 sourceUrl,                                                           \
     u32 sourceUrlLength))                                                    \
  F(evaluate_hermes_bytecode,                                                 \
    void,                                                                     \
    (w2c_hermes *,                                                            \
     u32 resValueOrError,                                                     \
     u32 srt,                                                                 \
     u32 buf,                                                                 \
     u32 sourceUrl,                                                           \
     u32 sourceUrlLength))                                                    \
  F(get_global_object, u32, (w2c_hermes *, u32 srt))                          \
  F(create_string_from_utf8, u32, (w2c_hermes *, u32 srt, u32 buf, u32 len))  \
  F(create_object, u32, (w2c_hermes *, u32 srt))                              \
  F(has_object_property_from_value, u32, (w2c_hermes *, u32, u32, u32))       \
  F(has_object_property_from_propnameid, u32, (w2c_hermes *, u32, u32, u32))  \
  F(get_object_property_from_value, void, (w2c_hermes *, u32, u32, u32, u32)) \
  F(get_object_property_from_propnameid,                                      \
    void,                                                                     \
    (w2c_hermes *, u32, u32, u32, u32))                                       \
  F(set_object_property_from_value, u32, (w2c_hermes *, u32, u32, u32, u32))  \
  F(set_object_property_from_propnameid,                                      \
    u32,                                                                      \
    (w2c_hermes *, u32, u32, u32, u32))                                       \
  F(get_object_property_names, u32, (w2c_hermes *, u32, u32))                 \
  F(set_object_external_memory_pressure, u32, (w2c_hermes *, u32, u32, u32))  \
  F(create_array, u32, (w2c_hermes *, u32, u32))                              \
  F(get_array_length, u32, (w2c_hermes *, u32, u32))                          \
  F(create_arraybuffer_from_external_data, u32, (w2c_hermes *, u32, u32))     \
  F(get_arraybuffer_data, void, (w2c_hermes *, u32, u32, u32))                \
  F(get_arraybuffer_size, void, (w2c_hermes *, u32, u32, u32))                \
  F(create_propnameid_from_string, u32, (w2c_hermes *, u32, u32))             \
  F(create_propnameid_from_symbol, u32, (w2c_hermes *, u32, u32))             \
  F(prop_name_id_equals, u32, (w2c_hermes *, u32, u32, u32))                  \
  F(call, void, (w2c_hermes *, u32, u32, u32, u32, u32, u32))                 \
  F(call_as_constructor, void, (w2c_hermes *, u32, u32, u32, u32, u32))       \
  F(create_function_from_host_function,                                       \
    u32,                                                                      \
    (w2c_hermes *, u32, u32, u32, u32))                                       \
  F(get_host_function, u32, (w2c_hermes *, u32, u32))                         \
  F(create_object_from_host_object, u32, (w2c_hermes *, u32, u32))            \
  F(get_host_object, u32, (w2c_hermes *, u32, u32))                           \
  F(get_native_state, u32, (w2c_hermes *, u32, u32))                          \
  F(set_native_state, u32, (w2c_hermes *, u32, u32, u32))                     \
  F(object_is_array, u32, (w2c_hermes *, u32, u32))                           \
  F(object_is_arraybuffer, u32, (w2c_hermes *, u32, u32))                     \
  F(object_is_function, u32, (w2c_hermes *, u32, u32))                        \
  F(create_weak_object, u32, (w2c_hermes *, u32, u32))                        \
  F(lock_weak_object, void, (w2c_hermes *, u32, u32, u32))                    \
  F(get_utf8_from_string, void, (w2c_hermes *, u32, u32, u32))                \
  F(get_utf8_from_propnameid, void, (w2c_hermes *, u32, u32, u32))            \
  F(get_utf8_from_symbol, void, (w2c_hermes *, u32, u32, u32))                \
  F(instance_of, u32, (w2c_hermes *, u32, u32, u32))                          \
  F(strict_equals_symbol, u32, (w2c_hermes *, u32, u32, u32))                 \
  F(strict_equals_bigint, u32, (w2c_hermes *, u32, u32, u32))                 \
  F(strict_equals_string, u32, (w2c_hermes *, u32, u32, u32))                 \
  F(strict_equals_object, u32, (w2c_hermes *, u32, u32, u32))                 \
  F(drain_microtasks, u32, (w2c_hermes *, u32, u32))                          \
  F(create_bigint_from_int64, u32, (w2c_hermes *, u32, u64))                  \
  F(create_bigint_from_uint64, u32, (w2c_hermes *, u32, u64))                 \
  F(bigint_is_int64, u32, (w2c_hermes *, u32, u32))                           \
  F(bigint_is_uint64, u32, (w2c_hermes *, u32, u32))                          \
  F(bigint_truncate_to_uint64, u64, (w2c_hermes *, u32, u32))                 \
  F(bigint_to_string, u32, (w2c_hermes *, u32, u32, u32))

/// Declare the vtable structure as it appears in the sandbox, with u32 as the
/// function pointer type.
struct SandboxRuntimeVTable {
#define DECLARE_SANDBOX_VTABLE_FUNCTION(name, ret, args) u32 name;
  SANDBOX_CONTEXT_VTABLE_FUNCTIONS(DECLARE_SANDBOX_VTABLE_FUNCTION)
#undef DECLARE_SANDBOX_VTABLE_FUNCTION
};

/// Declare the vtable structure with actual C function pointers. The vtable
/// pointers from the SandboxRuntimeVTable are converted and copied into this
/// structure, to make the functions easier and more efficient to call.
struct SandboxRuntimeVTableMirror {
#define DECLARE_SANDBOX_VTABLE_FUNCTION(name, ret, args) ret(*name) args;
  SANDBOX_CONTEXT_VTABLE_FUNCTIONS(DECLARE_SANDBOX_VTABLE_FUNCTION)
#undef DECLARE_SANDBOX_VTABLE_FUNCTION
};

struct SandboxRuntime {
  u32 vtable;
};

struct SandboxVTable {
  /* struct HermesABIRuntime *(*make_hermes_runtime)
     (const struct HermesABIRuntimeConfig *config); */
  u32 make_hermes_runtime;
  /* bool (*is_hermes_bytecode)(const uint8_t *buf, size_t len) */
  u32 is_hermes_bytecode;
};

namespace sb {

/// Mirror the helper functions from HermesABIHelpers.h.
#define DECLARE_SANDBOX_POINTER_HELPERS(name)           \
  Sandbox##name create##name(u32 ptr) {                 \
    return {ptr};                                       \
  }                                                     \
  bool isError(Sandbox##name##OrError p) {              \
    return p.ptr_or_error & 1;                          \
  }                                                     \
  SandboxErrorCode getError(Sandbox##name##OrError p) { \
    assert(isError(p));                                 \
    return (SandboxErrorCode)(p.ptr_or_error >> 2);     \
  }                                                     \
  Sandbox##name get##name(Sandbox##name##OrError p) {   \
    assert(!isError(p));                                \
    return create##name(p.ptr_or_error);                \
  }
SANDBOX_POINTER_TYPES(DECLARE_SANDBOX_POINTER_HELPERS)
#undef DECLARE_SANDBOX_POINTER_HELPERS

SandboxVoidOrError createVoidOrError(void) {
  return {0};
}
SandboxVoidOrError createVoidOrError(SandboxErrorCode err) {
  return {(u32)((err << 2) | 1)};
}
bool isError(SandboxVoidOrError v) {
  return v.void_or_error & 1;
}
SandboxErrorCode getError(SandboxVoidOrError v) {
  assert(isError(v));
  return (SandboxErrorCode)(v.void_or_error >> 2);
}

bool isError(const SandboxBoolOrError &p) {
  return p.bool_or_error & 1;
}
SandboxErrorCode getError(const SandboxBoolOrError &p) {
  return (SandboxErrorCode)(p.bool_or_error >> 2);
}
bool getBool(const SandboxBoolOrError &p) {
  return p.bool_or_error >> 2;
}

bool isError(const SandboxUint8PtrOrError &p) {
  return p.is_error;
}
SandboxErrorCode getError(const SandboxUint8PtrOrError &p) {
  return (SandboxErrorCode)p.data.error;
}
u32 getUint8Ptr(SandboxUint8PtrOrError p) {
  return p.data.val;
}

bool isError(const SandboxSizeTOrError &p) {
  return p.is_error;
}
SandboxErrorCode getError(const SandboxSizeTOrError &p) {
  return (SandboxErrorCode)p.data.error;
}
size_t getSizeT(const SandboxSizeTOrError &p) {
  return p.data.val;
}

SandboxPropNameIDListPtrOrError createPropNameIDListPtrOrError(u32 ptr) {
  return {ptr};
}
SandboxPropNameIDListPtrOrError createPropNameIDListPtrOrError(
    SandboxErrorCode err) {
  return {static_cast<u32>((err << 2) | 1)};
}

SandboxValue createUndefinedValue() {
  SandboxValue val;
  val.kind = SandboxValueKindUndefined;
  return val;
}
SandboxValue createNullValue() {
  SandboxValue val;
  val.kind = SandboxValueKindNull;
  return val;
}
SandboxValue createBoolValue(bool b) {
  SandboxValue val;
  val.kind = SandboxValueKindBoolean;
  val.data.boolean = b;
  return val;
}
SandboxValue createNumberValue(double d) {
  SandboxValue val;
  val.kind = SandboxValueKindNumber;
  val.data.number = d;
  return val;
}

/// Helpers to create a SandboxValue from a pointer to a ManagedPointer.
SandboxValue createObjectValue(u32 ptr) {
  SandboxValue val;
  val.kind = SandboxValueKindObject;
  val.data.pointer = ptr;
  return val;
}
SandboxValue createStringValue(u32 ptr) {
  SandboxValue val;
  val.kind = SandboxValueKindString;
  val.data.pointer = ptr;
  return val;
}
SandboxValue createBigIntValue(u32 ptr) {
  SandboxValue val;
  val.kind = SandboxValueKindBigInt;
  val.data.pointer = ptr;
  return val;
}
SandboxValue createSymbolValue(u32 ptr) {
  SandboxValue val;
  val.kind = SandboxValueKindSymbol;
  val.data.pointer = ptr;
  return val;
}

SandboxValueKind getValueKind(const SandboxValue &val) {
  return val.kind;
}

bool isBoolValue(const SandboxValue &val) {
  return getValueKind(val) == SandboxValueKindBoolean;
}
bool isNumberValue(const SandboxValue &val) {
  return getValueKind(val) == SandboxValueKindNumber;
}
bool isObjectValue(const SandboxValue &val) {
  return getValueKind(val) == SandboxValueKindObject;
}
bool isStringValue(const SandboxValue &val) {
  return getValueKind(val) == SandboxValueKindString;
}
bool isBigIntValue(const SandboxValue &val) {
  return getValueKind(val) == SandboxValueKindBigInt;
}
bool isSymbolValue(const SandboxValue &val) {
  return getValueKind(val) == SandboxValueKindSymbol;
}

bool getBoolValue(const SandboxValue &val) {
  (void)isBoolValue;
  assert(isBoolValue(val));
  return val.data.boolean;
}
double getNumberValue(const SandboxValue &val) {
  (void)isNumberValue;
  assert(isNumberValue(val));
  return val.data.number;
}
SandboxObject getObjectValue(const SandboxValue &val) {
  (void)isObjectValue;
  assert(isObjectValue(val));
  return createObject(val.data.pointer);
}
SandboxString getStringValue(const SandboxValue &val) {
  (void)isStringValue;
  assert(isStringValue(val));
  return createString(val.data.pointer);
}
SandboxBigInt getBigIntValue(const SandboxValue &val) {
  (void)isBigIntValue;
  assert(isBigIntValue(val));
  return createBigInt(val.data.pointer);
}
SandboxSymbol getSymbolValue(const SandboxValue &val) {
  (void)isSymbolValue;
  assert(isSymbolValue(val));
  return createSymbol(val.data.pointer);
}
u32 getPointerValue(const SandboxValue &val) {
  assert(getValueKind(val) | SANDBOX_POINTER_MASK);
  return val.data.pointer;
}

SandboxValueOrError createValueOrError(SandboxValue val) {
  SandboxValueOrError res;
  res.value = val;
  return res;
}
SandboxValueOrError createValueOrError(SandboxErrorCode err) {
  SandboxValueOrError res;
  res.value.kind = SandboxValueKindError;
  res.value.data.error = err;
  return res;
}
bool isError(const SandboxValueOrError &val) {
  return getValueKind(val.value) == SandboxValueKindError;
}
SandboxValue getValue(const SandboxValueOrError &val) {
  assert(!isError(val));
  return val.value;
}
SandboxErrorCode getError(const SandboxValueOrError &val) {
  assert(isError(val));
  return val.value.data.error;
}

} // namespace sb

/// Define a helper macro to throw an exception for unimplemented methods. The
/// actual throw is kept in a separate function because throwing generates a lot
/// of code.
[[noreturn]] void throwUnimplementedImpl(const char *name) {
  throw JSINativeException(std::string("Unimplemented function ") + name);
}

#define THROW_UNIMPLEMENTED() throwUnimplementedImpl(__func__)

class HermesSandboxRuntimeImpl : public facebook::hermes::HermesSandboxRuntime {
 public:
  HermesSandboxRuntimeImpl() {}
  ~HermesSandboxRuntimeImpl() override {}

  Value evaluateJavaScript(
      const std::shared_ptr<const Buffer> &buffer,
      const std::string &sourceURL) override {
    THROW_UNIMPLEMENTED();
  }

  Value evaluateHermesBytecode(
      const std::shared_ptr<const Buffer> &buffer,
      const std::string &sourceURL) override {
    THROW_UNIMPLEMENTED();
  }

  std::shared_ptr<const PreparedJavaScript> prepareJavaScript(
      const std::shared_ptr<const Buffer> &buffer,
      std::string sourceURL) override {
    THROW_UNIMPLEMENTED();
  }

  Value evaluatePreparedJavaScript(
      const std::shared_ptr<const PreparedJavaScript> &js) override {
    THROW_UNIMPLEMENTED();
  }

  bool drainMicrotasks(int maxMicrotasksHint = -1) override {
    THROW_UNIMPLEMENTED();
  }

  Object global() override {
    THROW_UNIMPLEMENTED();
  }

  std::string description() override {
    THROW_UNIMPLEMENTED();
  }

  bool isInspectable() override {
    THROW_UNIMPLEMENTED();
  }

  Instrumentation &instrumentation() override {
    THROW_UNIMPLEMENTED();
  }

 protected:
  PointerValue *cloneSymbol(const Runtime::PointerValue *pv) override {
    THROW_UNIMPLEMENTED();
  }
  PointerValue *cloneBigInt(const Runtime::PointerValue *pv) override {
    THROW_UNIMPLEMENTED();
  }
  PointerValue *cloneString(const Runtime::PointerValue *pv) override {
    THROW_UNIMPLEMENTED();
  }
  PointerValue *cloneObject(const Runtime::PointerValue *pv) override {
    THROW_UNIMPLEMENTED();
  }
  PointerValue *clonePropNameID(const Runtime::PointerValue *pv) override {
    THROW_UNIMPLEMENTED();
  }

  PropNameID createPropNameIDFromAscii(const char *str, size_t length)
      override {
    THROW_UNIMPLEMENTED();
  }
  PropNameID createPropNameIDFromUtf8(const uint8_t *utf8, size_t length)
      override {
    THROW_UNIMPLEMENTED();
  }
  PropNameID createPropNameIDFromString(const String &str) override {
    THROW_UNIMPLEMENTED();
  }
  PropNameID createPropNameIDFromSymbol(const Symbol &sym) override {
    THROW_UNIMPLEMENTED();
  }
  std::string utf8(const PropNameID &) override {
    THROW_UNIMPLEMENTED();
  }
  bool compare(const PropNameID &, const PropNameID &) override {
    THROW_UNIMPLEMENTED();
  }

  std::string symbolToString(const Symbol &) override {
    THROW_UNIMPLEMENTED();
  }

  BigInt createBigIntFromInt64(int64_t) override {
    THROW_UNIMPLEMENTED();
  }
  BigInt createBigIntFromUint64(uint64_t) override {
    THROW_UNIMPLEMENTED();
  }
  bool bigintIsInt64(const BigInt &) override {
    THROW_UNIMPLEMENTED();
  }
  bool bigintIsUint64(const BigInt &) override {
    THROW_UNIMPLEMENTED();
  }
  uint64_t truncate(const BigInt &) override {
    THROW_UNIMPLEMENTED();
  }
  String bigintToString(const BigInt &, int) override {
    THROW_UNIMPLEMENTED();
  }

  String createStringFromAscii(const char *str, size_t length) override {
    THROW_UNIMPLEMENTED();
  }
  String createStringFromUtf8(const uint8_t *utf8, size_t length) override {
    THROW_UNIMPLEMENTED();
  }
  std::string utf8(const String &) override {
    THROW_UNIMPLEMENTED();
  }

  Object createObject() override {
    THROW_UNIMPLEMENTED();
  }
  Object createObject(std::shared_ptr<HostObject> ho) override {
    THROW_UNIMPLEMENTED();
  }
  std::shared_ptr<HostObject> getHostObject(const Object &) override {
    THROW_UNIMPLEMENTED();
  }
  HostFunctionType &getHostFunction(const Function &) override {
    THROW_UNIMPLEMENTED();
  }

  bool hasNativeState(const Object &) override {
    THROW_UNIMPLEMENTED();
  }
  std::shared_ptr<NativeState> getNativeState(const Object &) override {
    THROW_UNIMPLEMENTED();
  }
  void setNativeState(const Object &, std::shared_ptr<NativeState> state)
      override {
    THROW_UNIMPLEMENTED();
  }

  Value getProperty(const Object &, const PropNameID &name) override {
    THROW_UNIMPLEMENTED();
  }
  Value getProperty(const Object &, const String &name) override {
    THROW_UNIMPLEMENTED();
  }
  bool hasProperty(const Object &, const PropNameID &name) override {
    THROW_UNIMPLEMENTED();
  }
  bool hasProperty(const Object &, const String &name) override {
    THROW_UNIMPLEMENTED();
  }
  void setPropertyValue(
      const Object &,
      const PropNameID &name,
      const Value &value) override {
    THROW_UNIMPLEMENTED();
  }
  void setPropertyValue(const Object &, const String &name, const Value &value)
      override {
    THROW_UNIMPLEMENTED();
  }

  bool isArray(const Object &) const override {
    THROW_UNIMPLEMENTED();
  }
  bool isArrayBuffer(const Object &) const override {
    THROW_UNIMPLEMENTED();
  }
  bool isFunction(const Object &) const override {
    THROW_UNIMPLEMENTED();
  }
  bool isHostObject(const Object &) const override {
    THROW_UNIMPLEMENTED();
  }
  bool isHostFunction(const Function &) const override {
    THROW_UNIMPLEMENTED();
  }
  Array getPropertyNames(const Object &) override {
    THROW_UNIMPLEMENTED();
  }

  WeakObject createWeakObject(const Object &) override {
    THROW_UNIMPLEMENTED();
  }
  Value lockWeakObject(const WeakObject &) override {
    THROW_UNIMPLEMENTED();
  }

  Array createArray(size_t length) override {
    THROW_UNIMPLEMENTED();
  }
  ArrayBuffer createArrayBuffer(
      std::shared_ptr<MutableBuffer> buffer) override {
    THROW_UNIMPLEMENTED();
  }
  size_t size(const Array &) override {
    THROW_UNIMPLEMENTED();
  }
  size_t size(const ArrayBuffer &) override {
    THROW_UNIMPLEMENTED();
  }
  uint8_t *data(const ArrayBuffer &) override {
    THROW_UNIMPLEMENTED();
  }
  Value getValueAtIndex(const Array &, size_t i) override {
    THROW_UNIMPLEMENTED();
  }
  void setValueAtIndexImpl(const Array &, size_t i, const Value &value)
      override {
    THROW_UNIMPLEMENTED();
  }

  Function createFunctionFromHostFunction(
      const PropNameID &name,
      unsigned int paramCount,
      HostFunctionType func) override {
    THROW_UNIMPLEMENTED();
  }
  Value call(
      const Function &,
      const Value &jsThis,
      const Value *args,
      size_t count) override {
    THROW_UNIMPLEMENTED();
  }
  Value callAsConstructor(const Function &, const Value *args, size_t count)
      override {
    THROW_UNIMPLEMENTED();
  }

  bool strictEquals(const Symbol &a, const Symbol &b) const override {
    THROW_UNIMPLEMENTED();
  }
  bool strictEquals(const BigInt &a, const BigInt &b) const override {
    THROW_UNIMPLEMENTED();
  }
  bool strictEquals(const String &a, const String &b) const override {
    THROW_UNIMPLEMENTED();
  }
  bool strictEquals(const Object &a, const Object &b) const override {
    THROW_UNIMPLEMENTED();
  }

  bool instanceOf(const Object &o, const Function &f) override {
    THROW_UNIMPLEMENTED();
  }

  void setExternalMemoryPressure(const Object &obj, size_t amount) override {}
};

} // namespace

/// Provide implementations for the functions imported by the sandbox.

extern "C" {

/// These definitions comes from wasi/api.h in Emscripten.
#define WASI_EINVAL 28
#define WASI_ENOSYS 52
#define WASI_CLOCKID_REALTIME 0
#define WASI_CLOCKID_MONOTONIC 1

/* import: 'env' '__syscall_lstat64' */
u32 w2c_env_0x5F_syscall_lstat64(struct w2c_env *, u32, u32) {
  return WASI_ENOSYS;
}

/* import: 'env' '__syscall_newfstatat' */
u32 w2c_env_0x5F_syscall_newfstatat(struct w2c_env *, u32, u32, u32, u32) {
  return WASI_ENOSYS;
}

/* import: 'env' '__syscall_stat64' */
u32 w2c_env_0x5F_syscall_stat64(struct w2c_env *, u32, u32) {
  return WASI_ENOSYS;
}

/* import: 'env' '__syscall_unlinkat' */
u32 w2c_env_0x5F_syscall_unlinkat(struct w2c_env *, u32, u32, u32) {
  return WASI_ENOSYS;
}

/* import: 'wasi_snapshot_preview1' 'environ_get' */
u32 w2c_wasi__snapshot__preview1_environ_get(
    struct w2c_wasi__snapshot__preview1 *,
    u32,
    u32) {
  return WASI_ENOSYS;
}

/* import: 'wasi_snapshot_preview1' 'environ_sizes_get' */
u32 w2c_wasi__snapshot__preview1_environ_sizes_get(
    struct w2c_wasi__snapshot__preview1 *,
    u32,
    u32) {
  return WASI_ENOSYS;
}

/* import: 'wasi_snapshot_preview1' 'fd_close' */
u32 w2c_wasi__snapshot__preview1_fd_close(
    struct w2c_wasi__snapshot__preview1 *,
    u32) {
  return WASI_ENOSYS;
}

/* import: 'wasi_snapshot_preview1' 'fd_fdstat_get' */
u32 w2c_wasi__snapshot__preview1_fd_fdstat_get(
    struct w2c_wasi__snapshot__preview1 *,
    u32,
    u32) {
  return WASI_ENOSYS;
}

/* import: 'wasi_snapshot_preview1' 'fd_seek' */
u32 w2c_wasi__snapshot__preview1_fd_seek(
    struct w2c_wasi__snapshot__preview1 *,
    u32,
    u64,
    u32,
    u32) {
  return WASI_ENOSYS;
}

/* import: 'wasi_snapshot_preview1' 'fd_write' */
u32 w2c_wasi__snapshot__preview1_fd_write(
    struct w2c_wasi__snapshot__preview1 *,
    u32,
    u32,
    u32,
    u32) {
  return WASI_ENOSYS;
}

/* import: 'wasi_snapshot_preview1' 'clock_time_get' */
u32 w2c_wasi__snapshot__preview1_clock_time_get(
    struct w2c_wasi__snapshot__preview1 *,
    u32,
    u64,
    u32) {
  return WASI_ENOSYS;
}

/* import: 'env' 'emscripten_notify_memory_growth' */
void w2c_env_emscripten_notify_memory_growth(struct w2c_env *, u32) {}

/* import: 'hermes_import' 'getentropy' */
u32 w2c_hermes__import_getentropy(struct w2c_hermes__import *, u32, u32) {
  return WASI_ENOSYS;
}

/* import: 'wasi_snapshot_preview1' 'proc_exit' */
void w2c_wasi__snapshot__preview1_proc_exit(
    struct w2c_wasi__snapshot__preview1 *,
    u32) {
  abort();
}
}

namespace facebook {
namespace hermes {

/*static*/ bool HermesSandboxRuntime::isHermesBytecode(
    const uint8_t *data,
    size_t len) {
  // "Hermes" in ancient Greek encoded in UTF-16BE and truncated to 8 bytes.
  constexpr uint64_t MAGIC = 0x1F1903C103BC1FC6;
  return (len >= sizeof(MAGIC) && memcmp(data, &MAGIC, sizeof(MAGIC)) == 0);
}

std::unique_ptr<HermesSandboxRuntime> makeHermesSandboxRuntime() {
  return std::make_unique<HermesSandboxRuntimeImpl>();
}

} // namespace hermes
} // namespace facebook
