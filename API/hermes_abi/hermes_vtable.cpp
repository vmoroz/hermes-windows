/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_abi/hermes_abi.h"

#include "hermes/ADT/ManagedChunkedList.h"
#include "hermes/BCGen/HBC/BytecodeProviderFromSrc.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/Runtime.h"
#include "hermes_abi/HermesABIHelpers.h"

#define ABI_PREFIX static

using namespace hermes;
using namespace facebook::hermes;

namespace {

class BufferWrapper : public hermes::Buffer {
  HermesABIBuffer *buffer_;

 public:
  explicit BufferWrapper(HermesABIBuffer *buffer)
      : hermes::Buffer(buffer->data, buffer->size), buffer_(buffer) {}

  ~BufferWrapper() override {
    buffer_->vtable->release(buffer_);
  }
};

/// A ManagedChunkedList element that indicates whether it's occupied based on
/// a refcount.
template <typename T>
class ManagedValue : public HermesABIManagedPointer {
  static void invalidate(HermesABIManagedPointer *ptr) {
    static_cast<ManagedValue<T> *>(ptr)->dec();
  }
  static constexpr HermesABIManagedPointerVTable vt{invalidate};

 public:
  ManagedValue() : HermesABIManagedPointer{&vt}, refCount_(0) {}

  /// Determine whether the element is occupied by inspecting the refcount.
  bool isFree() const {
    return refCount_.load(std::memory_order_relaxed) == 0;
  }

  /// Store a value and start the refcount at 1. After invocation, this
  /// instance is occupied with a value, and the "nextFree" methods should
  /// not be used until the value is released.
  template <typename... Args>
  void emplace(Args &&...args) {
    assert(isFree() && "Emplacing already occupied value");
    refCount_.store(1, std::memory_order_relaxed);
    new (&value_) T(std::forward<Args>(args)...);
  }

  /// Get the next free element. Must not be called when this instance is
  /// occupied with a value.
  ManagedValue<T> *getNextFree() {
    assert(isFree() && "Free pointer unusable while occupied");
    return nextFree_;
  }

  /// Set the next free element. Must not be called when this instance is
  /// occupied with a value.
  void setNextFree(ManagedValue<T> *nextFree) {
    assert(isFree() && "Free pointer unusable while occupied");
    nextFree_ = nextFree;
  }

  T &value() {
    assert(!isFree() && "Value not present");
    return value_;
  }

  const T &value() const {
    assert(!isFree() && "Value not present");
    return value_;
  }

  void inc() {
    // It is always safe to use relaxed operations for incrementing the
    // reference count, because the only operation that may occur concurrently
    // with it is decrementing the reference count, and we do not need to
    // enforce any ordering between the two.
    auto oldCount = refCount_.fetch_add(1, std::memory_order_relaxed);
    assert(oldCount && "Cannot resurrect a pointer");
    assert(oldCount + 1 != 0 && "Ref count overflow");
    (void)oldCount;
  }

  void dec() {
    // It is safe to use relaxed operations here because decrementing the
    // reference count is the only access that may be performed without proper
    // synchronisation. As a result, the only ordering we need to enforce when
    // decrementing is that the vtable pointer used to call \c invalidate is
    // loaded from before the decrement, in case the decrement ends up causing
    // this value to be freed. We get this ordering from the fact that the
    // vtable read and the reference count update form a load-store control
    // dependency, which preserves their ordering on any reasonable hardware.
    auto oldCount = refCount_.fetch_sub(1, std::memory_order_relaxed);
    assert(oldCount > 0 && "Ref count underflow");
    (void)oldCount;
  }

 private:
  std::atomic<uint32_t> refCount_;
  union {
    T value_;
    ManagedValue<T> *nextFree_;
  };
};

/// Helper functions to create a handle from a HermesABI reference.
template <typename T = vm::HermesValue>
vm::Handle<T> toHandle(HermesABIManagedPointer *value) {
  return vm::Handle<T>::vmcast(
      &static_cast<ManagedValue<vm::PinnedHermesValue> *>(value)->value());
}
vm::Handle<vm::JSObject> toHandle(HermesABIObject obj) {
  return toHandle<vm::JSObject>(obj.pointer);
}
vm::Handle<vm::StringPrimitive> toHandle(HermesABIString str) {
  return toHandle<vm::StringPrimitive>(str.pointer);
}
vm::Handle<vm::SymbolID> toHandle(HermesABISymbol sym) {
  return toHandle<vm::SymbolID>(sym.pointer);
}
vm::Handle<vm::SymbolID> toHandle(HermesABIPropNameID sym) {
  return toHandle<vm::SymbolID>(sym.pointer);
}
vm::Handle<vm::JSArray> toHandle(HermesABIArray arr) {
  return toHandle<vm::JSArray>(arr.pointer);
}
vm::Handle<vm::BigIntPrimitive> toHandle(HermesABIBigInt bi) {
  return toHandle<vm::BigIntPrimitive>(bi.pointer);
}
vm::Handle<vm::Callable> toHandle(HermesABIFunction fn) {
  return toHandle<vm::Callable>(fn.pointer);
}
vm::Handle<vm::JSArrayBuffer> toHandle(HermesABIArrayBuffer ab) {
  return toHandle<vm::JSArrayBuffer>(ab.pointer);
}

vm::HermesValue toHermesValue(const HermesABIValue &val) {
  switch (abi::getValueKind(val)) {
    case HermesABIValueKindUndefined:
      return vm::HermesValue::encodeUndefinedValue();
    case HermesABIValueKindNull:
      return vm::HermesValue::encodeNullValue();
    case HermesABIValueKindBoolean:
      return vm::HermesValue::encodeBoolValue(abi::getBoolValue(val));
    case HermesABIValueKindNumber:
      return vm::HermesValue::encodeUntrustedNumberValue(
          abi::getNumberValue(val));
    case HermesABIValueKindString:
    case HermesABIValueKindObject:
    case HermesABIValueKindSymbol:
    case HermesABIValueKindBigInt:
      return *toHandle<>(val.data.pointer);
    default:
      // This incoming value is either an error, or from a newer version of the
      // ABI, which violates our expectations.
      hermes_fatal("Value has an unexpected tag.");
  }
}

} // namespace

/// A thin wrapper around vm::Runtime to provide additional state for things
/// like pointer management. It is intended to provide a small number of helper
/// functions, with the core logic being kept in the actual API functions below,
/// which can directly manipulate the vm::Runtime.
struct HermesABIContext {
  std::shared_ptr<::hermes::vm::Runtime> rt;
  ManagedChunkedList<ManagedValue<vm::PinnedHermesValue>> hermesValues;
  ManagedChunkedList<ManagedValue<vm::WeakRoot<vm::JSObject>>> weakHermesValues;

  /// This holds the message for cases where we throw a native exception.
  std::string nativeExceptionMessage{};

  explicit HermesABIContext(const hermes::vm::RuntimeConfig &runtimeConfig)
      : rt(hermes::vm::Runtime::create(runtimeConfig)),
        hermesValues(runtimeConfig.getGCConfig().getOccupancyTarget(), 0.5),
        weakHermesValues(
            runtimeConfig.getGCConfig().getOccupancyTarget(),
            0.5) {
    // Add custom roots functions to the runtime to expose references retained
    // through the API as roots.
    rt->addCustomRootsFunction([this](vm::GC *, vm::RootAcceptor &acceptor) {
      hermesValues.forEach(
          [&acceptor](auto &element) { acceptor.accept(element.value()); });
    });
    rt->addCustomWeakRootsFunction(
        [this](vm::GC *, vm::WeakRootAcceptor &acceptor) {
          weakHermesValues.forEach([&acceptor](auto &element) {
            acceptor.acceptWeak(element.value());
          });
        });
  }

  ~HermesABIContext() {
    // Release the runtime to make sure that any remaining references held by
    // things like HostObject are freed.
    rt.reset();
    assert(hermesValues.sizeForTests() == 0 && "Dangling references.");
    assert(weakHermesValues.sizeForTests() == 0 && "Dangling references.");
  }

  HermesABIValue createValue(vm::HermesValue hv) {
    switch (hv.getETag()) {
      case vm::HermesValue::ETag::Undefined:
        return abi::createUndefinedValue();
      case vm::HermesValue::ETag::Null:
        return abi::createNullValue();
      case vm::HermesValue::ETag::Bool:
        return abi::createBoolValue(hv.getBool());
      case vm::HermesValue::ETag::Symbol:
        return abi::createSymbolValue(&hermesValues.add(hv));
      case vm::HermesValue::ETag::Str1:
      case vm::HermesValue::ETag::Str2:
        return abi::createStringValue(&hermesValues.add(hv));
      case vm::HermesValue::ETag::BigInt1:
      case vm::HermesValue::ETag::BigInt2:
        return abi::createBigIntValue(&hermesValues.add(hv));
      case vm::HermesValue::ETag::Object1:
      case vm::HermesValue::ETag::Object2:
        return abi::createObjectValue(&hermesValues.add(hv));
      default:
        assert(hv.isNumber() && "No other types are permitted in the API.");
        return abi::createNumberValue(hv.getNumber());
    }
  }
  HermesABIValueOrError createValueOrError(vm::HermesValue hv) {
    return abi::createValueOrError(createValue(hv));
  }

  template <typename T>
  HermesABIManagedPointer *createPointerImpl(vm::HermesValue hv) {
    if constexpr (!std::is_same_v<T, HermesABIWeakObject>)
      return &hermesValues.add(hv);

    return &weakHermesValues.add(
        vm::WeakRoot<vm::JSObject>(vm::vmcast<vm::JSObject>(hv), *rt));
  }

#define DECLARE_HERMES_ABI_POINTER_HELPERS(name)                               \
  HermesABI##name create##name(vm::HermesValue hv) {                           \
    return abi::create##name(createPointerImpl<HermesABI##name>(hv));          \
  }                                                                            \
  HermesABI##name##OrError create##name##OrError(vm::HermesValue hv) {         \
    return abi::create##name##OrError(createPointerImpl<HermesABI##name>(hv)); \
  }
  HERMES_ABI_POINTER_TYPES(DECLARE_HERMES_ABI_POINTER_HELPERS)
#undef DECLARE_HERMES_ABI_POINTER_HELPERS

  vm::Handle<> makeHandle(const HermesABIValue &val) {
    switch (abi::getValueKind(val)) {
      case HermesABIValueKindUndefined:
        return vm::Runtime::getUndefinedValue();
      case HermesABIValueKindNull:
        return vm::Runtime::getNullValue();
      case HermesABIValueKindBoolean:
        return vm::Runtime::getBoolValue(abi::getBoolValue(val));
      case HermesABIValueKindNumber:
        return rt->makeHandle(vm::HermesValue::encodeUntrustedNumberValue(
            abi::getNumberValue(val)));
      case HermesABIValueKindString:
      case HermesABIValueKindObject:
      case HermesABIValueKindSymbol:
      case HermesABIValueKindBigInt:
        return toHandle<>(val.data.pointer);
      default:
        // This incoming value is either an error, or from a newer version of
        // the ABI, which violates our expectations.
        hermes_fatal("Value has an unexpected tag.");
    }
  }

  vm::ExecutionStatus raiseError(HermesABIErrorCode err) {
    if (err == HermesABIErrorCodeJSError)
      return hermes::vm::ExecutionStatus::EXCEPTION;

    if (err == HermesABIErrorCodeNativeException) {
      auto msg = std::exchange(nativeExceptionMessage, {});
      return rt->raiseError(llvh::StringRef{msg});
    }

    return rt->raiseError("Native code threw an unknown exception.");
  }
};

ABI_PREFIX HermesABIContext *make_hermes_runtime(
    const HermesABIRuntimeConfig *config) {
  return new HermesABIContext({});
}

ABI_PREFIX void release_hermes_runtime(HermesABIContext *runtime) {
  delete runtime;
}

ABI_PREFIX HermesABIValue get_and_clear_js_error_value(HermesABIContext *ctx) {
  auto ret = ctx->createValue(ctx->rt->getThrownValue());
  ctx->rt->clearThrownValue();
  return ret;
}

ABI_PREFIX HermesABIByteRef
get_native_exception_message(HermesABIContext *ctx) {
  return {
      (const uint8_t *)ctx->nativeExceptionMessage.data(),
      ctx->nativeExceptionMessage.size()};
}
ABI_PREFIX void clear_native_exception_message(HermesABIContext *ctx) {
  ctx->nativeExceptionMessage.clear();
  ctx->nativeExceptionMessage.shrink_to_fit();
}

ABI_PREFIX void set_js_error_value(
    HermesABIContext *ctx,
    const HermesABIValue *val) {
  ctx->rt->setThrownValue(toHermesValue(*val));
}
ABI_PREFIX void set_native_exception_message(
    HermesABIContext *ctx,
    const char *message,
    size_t length) {
  ctx->nativeExceptionMessage.assign(message, length);
}

ABI_PREFIX HermesABIPropNameID
clone_prop_name_id(HermesABIContext *ctx, HermesABIPropNameID name) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(name.pointer)->inc();
  return abi::createPropNameID(name.pointer);
}
ABI_PREFIX HermesABIString
clone_string(HermesABIContext *ctx, HermesABIString str) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(str.pointer)->inc();
  return abi::createString(str.pointer);
}
ABI_PREFIX HermesABISymbol
clone_symbol(HermesABIContext *ctx, HermesABISymbol sym) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(sym.pointer)->inc();
  return abi::createSymbol(sym.pointer);
}
ABI_PREFIX HermesABIObject
clone_object(HermesABIContext *ctx, HermesABIObject obj) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(obj.pointer)->inc();
  return abi::createObject(obj.pointer);
}
ABI_PREFIX HermesABIBigInt
clone_big_int(HermesABIContext *ctx, HermesABIBigInt bi) {
  static_cast<ManagedValue<vm::PinnedHermesValue> *>(bi.pointer)->inc();
  return abi::createBigInt(bi.pointer);
}

ABI_PREFIX bool is_hermes_bytecode(const uint8_t *data, size_t len) {
  return hbc::BCProviderFromBuffer::isBytecodeStream(
      llvh::ArrayRef<uint8_t>(data, len));
}

ABI_PREFIX HermesABIValueOrError evaluate_javascript_source(
    HermesABIContext *ctx,
    HermesABIBuffer *source,
    const char *sourceURL,
    size_t sourceURLLength) {
  llvh::StringRef sourceURLRef(sourceURL, sourceURLLength);
  std::pair<std::unique_ptr<hbc::BCProvider>, std::string> bcErr{};
#if defined(HERMESVM_LEAN)
  bcErr.second = "source compilation not supported";
#else
  bcErr = hbc::BCProviderFromSrc::createBCProviderFromSrc(
      std::make_unique<BufferWrapper>(source),
      sourceURLRef,
      /* sourceMap */ {},
      /* compileFlags */ {});
#endif
  if (!bcErr.first) {
    ctx->nativeExceptionMessage = std::move(bcErr.second);
    return abi::createValueOrError(HermesABIErrorCodeNativeException);
  }
  auto &runtime = *ctx->rt;
  vm::RuntimeModuleFlags runtimeFlags{};
  vm::GCScope gcScope(runtime);
  auto res = runtime.runBytecode(
      std::move(bcErr.first),
      runtimeFlags,
      sourceURLRef,
      vm::Runtime::makeNullHandle<vm::Environment>());
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);

  return ctx->createValueOrError(*res);
}

ABI_PREFIX HermesABIValueOrError evaluate_hermes_bytecode(
    HermesABIContext *ctx,
    HermesABIBuffer *bytecode,
    const char *sourceURL,
    size_t sourceURLLength) {
  assert(is_hermes_bytecode(bytecode->data, bytecode->size));
  auto bcErr = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::make_unique<BufferWrapper>(bytecode));
  if (!bcErr.first) {
    ctx->nativeExceptionMessage = std::move(bcErr.second);
    return abi::createValueOrError(HermesABIErrorCodeNativeException);
  }

  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  llvh::StringRef sourceURLRef(sourceURL, sourceURLLength);
  vm::RuntimeModuleFlags runtimeFlags{};
  auto res = runtime.runBytecode(
      std::move(bcErr.first),
      runtimeFlags,
      sourceURLRef,
      vm::Runtime::makeNullHandle<vm::Environment>());
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);

  return ctx->createValueOrError(*res);
}

ABI_PREFIX HermesABIObject get_global_object(HermesABIContext *ctx) {
  return ctx->createObject(ctx->rt->getGlobal().getHermesValue());
}

ABI_PREFIX HermesABIStringOrError create_string_from_ascii(
    HermesABIContext *ctx,
    const char *str,
    size_t length) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto strRes = vm::StringPrimitive::createEfficient(
      runtime, llvh::makeArrayRef(str, length));
  if (strRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createStringOrError(HermesABIErrorCodeJSError);
  return ctx->createStringOrError(*strRes);
}

ABI_PREFIX HermesABIStringOrError create_string_from_utf8(
    HermesABIContext *ctx,
    const uint8_t *utf8,
    size_t length) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto strRes = vm::StringPrimitive::createEfficient(
      runtime, llvh::makeArrayRef(utf8, length), /* IgnoreInputErrors */ true);
  if (strRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createStringOrError(HermesABIErrorCodeJSError);
  return ctx->createStringOrError(*strRes);
}

ABI_PREFIX HermesABIObjectOrError create_object(HermesABIContext *ctx) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  return ctx->createObjectOrError(
      vm::JSObject::create(runtime).getHermesValue());
}

ABI_PREFIX HermesABIBoolOrError has_object_property_from_string(
    HermesABIContext *ctx,
    HermesABIObject obj,
    HermesABIString str) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto res = vm::JSObject::hasComputed(toHandle(obj), runtime, toHandle(str));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createBoolOrError(HermesABIErrorCodeJSError);
  return abi::createBoolOrError(*res);
}

ABI_PREFIX HermesABIBoolOrError has_object_property_from_prop_name_id(
    HermesABIContext *ctx,
    HermesABIObject obj,
    HermesABIPropNameID name) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto res =
      vm::JSObject::hasNamedOrIndexed(toHandle(obj), runtime, *toHandle(name));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createBoolOrError(HermesABIErrorCodeJSError);
  return abi::createBoolOrError(*res);
}

ABI_PREFIX HermesABIValueOrError get_object_property_from_string(
    HermesABIContext *ctx,
    HermesABIObject object,
    HermesABIString str) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto res =
      vm::JSObject::getComputed_RJS(toHandle(object), runtime, toHandle(str));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  return ctx->createValueOrError(res->get());
}

ABI_PREFIX HermesABIValueOrError get_object_property_from_prop_name_id(
    HermesABIContext *ctx,
    HermesABIObject object,
    HermesABIPropNameID sym) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto res = vm::JSObject::getNamedOrIndexed(
      toHandle(object), runtime, *toHandle(sym));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  return ctx->createValueOrError(res->get());
}

ABI_PREFIX HermesABIVoidOrError set_object_property_from_string(
    HermesABIContext *ctx,
    HermesABIObject obj,
    HermesABIString str,
    const HermesABIValue *val) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);

  auto res = vm::JSObject::putComputed_RJS(
                 toHandle(obj),
                 runtime,
                 toHandle(str),
                 ctx->makeHandle(*val),
                 vm::PropOpFlags().plusThrowOnError())
                 .getStatus();
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createVoidOrError(HermesABIErrorCodeJSError);
  return abi::createVoidOrError();
}

ABI_PREFIX HermesABIVoidOrError set_object_property_from_prop_name_id(
    HermesABIContext *ctx,
    HermesABIObject obj,
    HermesABIPropNameID name,
    const HermesABIValue *val) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);

  auto res = vm::JSObject::putNamedOrIndexed(
                 toHandle(obj),
                 runtime,
                 *toHandle(name),
                 ctx->makeHandle(*val),
                 vm::PropOpFlags().plusThrowOnError())
                 .getStatus();
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createVoidOrError(HermesABIErrorCodeJSError);
  return abi::createVoidOrError();
}

ABI_PREFIX HermesABIArrayOrError
get_object_property_names(HermesABIContext *ctx, HermesABIObject obj) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  uint32_t beginIndex;
  uint32_t endIndex;
  auto objHandle = toHandle(obj);

  auto propsRes =
      vm::getForInPropertyNames(runtime, objHandle, beginIndex, endIndex);
  if (propsRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createArrayOrError(HermesABIErrorCodeJSError);

  vm::Handle<vm::SegmentedArray> props = *propsRes;
  size_t length = endIndex - beginIndex;

  auto retRes = vm::JSArray::create(runtime, length, length);
  if (retRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createArrayOrError(HermesABIErrorCodeJSError);
  vm::Handle<vm::JSArray> ret = *retRes;
  vm::JSArray::setStorageEndIndex(ret, runtime, length);

  for (size_t i = 0; i < length; ++i) {
    vm::HermesValue name = props->at(runtime, beginIndex + i);
    vm::StringPrimitive *asString;
    if (name.isString()) {
      asString = name.getString();
    } else {
      assert(name.isNumber());
      auto asStrRes = vm::toString_RJS(runtime, runtime.makeHandle(name));
      if (asStrRes == vm::ExecutionStatus::EXCEPTION)
        return abi::createArrayOrError(HermesABIErrorCodeJSError);
      asString = asStrRes->get();
    }
    vm::JSArray::unsafeSetExistingElementAt(
        *ret,
        runtime,
        i,
        vm::SmallHermesValue::encodeStringValue(asString, runtime));
  }

  return ctx->createArrayOrError(ret.getHermesValue());
}

ABI_PREFIX HermesABIArrayOrError
create_array(HermesABIContext *ctx, size_t length) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto result = vm::JSArray::create(runtime, length, length);
  if (result == vm::ExecutionStatus::EXCEPTION)
    return abi::createArrayOrError(HermesABIErrorCodeJSError);
  return ctx->createArrayOrError(result->getHermesValue());
}

size_t get_array_length(HermesABIContext *ctx, HermesABIArray arr) {
  auto &runtime = *ctx->rt;
  return vm::JSArray::getLength(*toHandle(arr), runtime);
}
ABI_PREFIX HermesABIValueOrError
get_array_value_at_index(HermesABIContext *ctx, HermesABIArray arr, size_t i) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto len = vm::JSArray::getLength(*toHandle(arr), runtime);
  if (LLVM_UNLIKELY(i >= len)) {
    (void)runtime.raiseError("Array index out of bounds.");
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  }

  auto res = vm::JSObject::getComputed_RJS(
      toHandle(arr),
      runtime,
      runtime.makeHandle(vm::HermesValue::encodeUntrustedNumberValue(i)));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);

  return ctx->createValueOrError(res->get());
}

ABI_PREFIX HermesABIVoidOrError set_array_value_at_index(
    HermesABIContext *ctx,
    HermesABIArray arr,
    size_t i,
    const HermesABIValue *val) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto len = vm::JSArray::getLength(*toHandle(arr), runtime);
  if (LLVM_UNLIKELY(i >= len)) {
    (void)runtime.raiseError("Array index out of bounds.");
    return abi::createVoidOrError(HermesABIErrorCodeJSError);
  }

  auto res = vm::JSObject::putComputed_RJS(
      toHandle(arr),
      runtime,
      runtime.makeHandle(vm::HermesValue::encodeTrustedNumberValue(i)),
      ctx->makeHandle(*val));
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createVoidOrError(HermesABIErrorCodeJSError);
  return abi::createVoidOrError();
}

ABI_PREFIX HermesABIArrayBufferOrError create_array_buffer_from_external_data(
    HermesABIContext *ctx,
    HermesABIMutableBuffer *buf) {
  auto &runtime = *ctx->rt;

  vm::GCScope gcScope(runtime);
  auto arrayBuffer = runtime.makeHandle(vm::JSArrayBuffer::create(
      runtime,
      vm::Handle<vm::JSObject>::vmcast(&runtime.arrayBufferPrototype)));
  auto size = buf->size;
  auto *data = buf->data;
  auto finalize = [](void *buf) {
    auto *self = static_cast<HermesABIMutableBuffer *>(buf);
    self->vtable->release(self);
  };
  auto res = vm::JSArrayBuffer::setExternalDataBlock(
      runtime, arrayBuffer, data, size, buf, finalize);
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createArrayBufferOrError(HermesABIErrorCodeJSError);
  return ctx->createArrayBufferOrError(arrayBuffer.getHermesValue());
}

ABI_PREFIX HermesABIUint8PtrOrError
get_array_buffer_data(HermesABIContext *ctx, HermesABIArrayBuffer buf) {
  auto &runtime = *ctx->rt;
  auto ab = toHandle(buf);
  if (!ab->attached()) {
    ctx->nativeExceptionMessage =
        "Cannot get data block of detached ArrayBuffer.";
    return abi::createUint8PtrOrError(HermesABIErrorCodeNativeException);
  }
  return abi::createUint8PtrOrError(ab->getDataBlock(runtime));
}

ABI_PREFIX HermesABISizeTOrError
get_array_buffer_size(HermesABIContext *ctx, HermesABIArrayBuffer buf) {
  auto ab = toHandle(buf);
  if (!ab->attached()) {
    ctx->nativeExceptionMessage = "Cannot get size of detached ArrayBuffer.";
    return abi::createSizeTOrError(HermesABIErrorCodeNativeException);
  }
  return abi::createSizeTOrError(ab->size());
}

ABI_PREFIX HermesABIPropNameIDOrError create_prop_name_id_from_ascii(
    HermesABIContext *ctx,
    const char *str,
    size_t len) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto cr = vm::stringToSymbolID(
      runtime,
      vm::StringPrimitive::createNoThrow(runtime, llvh::StringRef(str, len)));
  if (cr == vm::ExecutionStatus::EXCEPTION)
    return abi::createPropNameIDOrError(HermesABIErrorCodeJSError);
  return ctx->createPropNameIDOrError(cr->getHermesValue());
}

ABI_PREFIX HermesABIPropNameIDOrError create_prop_name_id_from_utf8(
    HermesABIContext *ctx,
    const uint8_t *utf8,
    size_t length) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto strRes = vm::StringPrimitive::createEfficient(
      runtime, llvh::makeArrayRef(utf8, length), /* IgnoreInputErrors */ true);
  if (strRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createPropNameIDOrError(HermesABIErrorCodeJSError);

  auto cr = vm::stringToSymbolID(
      runtime, vm::createPseudoHandle(strRes->getString()));
  if (cr == vm::ExecutionStatus::EXCEPTION)
    return abi::createPropNameIDOrError(HermesABIErrorCodeJSError);
  return ctx->createPropNameIDOrError(cr->getHermesValue());
}

ABI_PREFIX HermesABIPropNameIDOrError
create_prop_name_id_from_string(HermesABIContext *ctx, HermesABIString str) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto cr =
      vm::stringToSymbolID(runtime, vm::createPseudoHandle(*toHandle(str)));
  if (cr == vm::ExecutionStatus::EXCEPTION)
    return abi::createPropNameIDOrError(HermesABIErrorCodeJSError);
  return ctx->createPropNameIDOrError(cr->getHermesValue());
}

ABI_PREFIX HermesABIPropNameIDOrError
create_prop_name_id_from_symbol(HermesABIContext *ctx, HermesABISymbol sym) {
  return ctx->createPropNameIDOrError(toHandle(sym).getHermesValue());
}

ABI_PREFIX bool prop_name_id_equals(
    HermesABIContext *,
    HermesABIPropNameID a,
    HermesABIPropNameID b) {
  return *toHandle(a) == *toHandle(b);
}

ABI_PREFIX bool object_is_array(HermesABIContext *, HermesABIObject object) {
  return vm::vmisa<vm::JSArray>(*toHandle(object));
}
ABI_PREFIX bool object_is_array_buffer(
    HermesABIContext *,
    HermesABIObject object) {
  return vm::vmisa<vm::JSArrayBuffer>(*toHandle(object));
}
ABI_PREFIX bool object_is_function(HermesABIContext *, HermesABIObject object) {
  return vm::vmisa<vm::Callable>(*toHandle(object));
}
ABI_PREFIX bool object_is_host_object(
    HermesABIContext *,
    HermesABIObject object) {
  return vm::vmisa<vm::HostObject>(*toHandle(object));
}
ABI_PREFIX bool function_is_host_function(
    HermesABIContext *,
    HermesABIFunction fn) {
  return vm::vmisa<vm::FinalizableNativeFunction>(*toHandle(fn));
}

ABI_PREFIX HermesABIValueOrError call(
    HermesABIContext *ctx,
    HermesABIFunction func,
    const HermesABIValue *jsThis,
    const HermesABIValue *args,
    size_t count) {
  if (count > std::numeric_limits<uint32_t>::max()) {
    ctx->nativeExceptionMessage = "Too many arguments to call";
    return abi::createValueOrError(HermesABIErrorCodeNativeException);
  }

  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  vm::Handle<vm::Callable> funcHandle = toHandle(func);

  vm::ScopedNativeCallFrame newFrame{
      runtime,
      static_cast<uint32_t>(count),
      funcHandle.getHermesValue(),
      vm::HermesValue::encodeUndefinedValue(),
      toHermesValue(*jsThis)};
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    (void)runtime.raiseStackOverflow(
        ::hermes::vm::Runtime::StackOverflowKind::NativeStack);
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  }

  for (uint32_t i = 0; i != count; ++i)
    newFrame->getArgRef(i) = toHermesValue(args[i]);

  auto callRes = vm::Callable::call(funcHandle, runtime);
  if (callRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);

  return ctx->createValueOrError(callRes->get());
}

ABI_PREFIX HermesABIValueOrError call_as_constructor(
    HermesABIContext *ctx,
    HermesABIFunction fn,
    const HermesABIValue *args,
    size_t count) {
  if (count > std::numeric_limits<uint32_t>::max()) {
    ctx->nativeExceptionMessage = "Too many arguments to call";
    return abi::createValueOrError(HermesABIErrorCodeNativeException);
  }

  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  vm::Handle<vm::Callable> funcHandle = toHandle(fn);

  auto thisRes = vm::Callable::createThisForConstruct_RJS(funcHandle, runtime);
  // Save the this param to the constructor call in case the function does not
  // return an object.
  auto objHandle = runtime.makeHandle<vm::JSObject>(std::move(*thisRes));

  vm::ScopedNativeCallFrame newFrame{
      runtime,
      static_cast<uint32_t>(count),
      funcHandle.getHermesValue(),
      funcHandle.getHermesValue(),
      objHandle.getHermesValue()};
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    (void)runtime.raiseStackOverflow(
        ::hermes::vm::Runtime::StackOverflowKind::NativeStack);
    return abi::createValueOrError(HermesABIErrorCodeJSError);
  }
  for (uint32_t i = 0; i != count; ++i)
    newFrame->getArgRef(i) = toHermesValue(args[i]);

  // The last parameter indicates that this call should construct an object.
  auto callRes = vm::Callable::call(funcHandle, runtime);
  if (callRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createValueOrError(HermesABIErrorCodeJSError);

  // If the result is not an object, return the this parameter.
  auto res = callRes->get();
  return ctx->createValueOrError(
      res.isObject() ? res : objHandle.getHermesValue());
}

namespace {
class HostFunctionWrapper {
  HermesABIContext *ctx_;
  HermesABIHostFunction *func_;

 public:
  HostFunctionWrapper(HermesABIContext *ctx, HermesABIHostFunction *func)
      : ctx_(ctx), func_(func) {}

  ~HostFunctionWrapper() {
    func_->vtable->release(func_);
  }

  HermesABIHostFunction *getFunc() {
    return func_;
  }

  static vm::CallResult<vm::HermesValue>
  call(void *hfCtx, vm::Runtime &runtime, vm::NativeArgs hvArgs) {
    auto *self = static_cast<HostFunctionWrapper *>(hfCtx);
    HermesABIContext *ctx = self->ctx_;
    assert(&runtime == ctx->rt.get());

    llvh::SmallVector<HermesABIValue, 8> apiArgs;
    for (vm::HermesValue hv : hvArgs)
      apiArgs.push_back(ctx->createValue(hv));

    const HermesABIValue *args = apiArgs.empty() ? nullptr : &apiArgs.front();
    HermesABIValue thisArg = ctx->createValue(hvArgs.getThisArg());

    auto retOrError = (self->func_->vtable->call)(
        self->func_, ctx, &thisArg, args, apiArgs.size());

    for (const auto &arg : apiArgs)
      abi::releaseValue(arg);
    abi::releaseValue(thisArg);

    // Error values do not need to be "released" so we can return early.
    if (abi::isError(retOrError))
      return ctx->raiseError(abi::getError(retOrError));

    auto ret = abi::getValue(retOrError);
    auto retHV = toHermesValue(ret);
    abi::releaseValue(ret);
    return retHV;
  }
  static void release(void *data) {
    delete static_cast<HostFunctionWrapper *>(data);
  }
};
} // namespace

ABI_PREFIX HermesABIFunctionOrError create_function_from_host_function(
    HermesABIContext *ctx,
    HermesABIPropNameID name,
    unsigned int paramCount,
    HermesABIHostFunction *func) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto *hfw = new HostFunctionWrapper(ctx, func);
  auto funcRes = vm::FinalizableNativeFunction::createWithoutPrototype(
      runtime,
      hfw,
      HostFunctionWrapper::call,
      HostFunctionWrapper::release,
      *toHandle(name),
      paramCount);
  assert(
      funcRes != vm::ExecutionStatus::EXCEPTION &&
      "Failed to create HostFunction");
  return ctx->createFunctionOrError(*funcRes);
}

ABI_PREFIX HermesABIHostFunction *get_host_function(
    HermesABIContext *,
    HermesABIFunction fn) {
  auto h = vm::Handle<vm::FinalizableNativeFunction>::vmcast(toHandle(fn));
  return static_cast<HostFunctionWrapper *>(h->getContext())->getFunc();
}

namespace {
class HostObjectWrapper : public vm::HostObjectProxy {
  HermesABIContext *ctx_;
  HermesABIHostObject *ho_;

 public:
  HostObjectWrapper(HermesABIContext *ctx, HermesABIHostObject *ho)
      : ctx_(ctx), ho_(ho) {}

  // This is called when the object is finalized.
  ~HostObjectWrapper() {
    ho_->vtable->release(ho_);
  }

  HermesABIHostObject *getHostObject() {
    return ho_;
  }

  // This is called to fetch a property value by name.
  vm::CallResult<vm::HermesValue> get(vm::SymbolID sym) override {
    HermesABIPropNameID name =
        ctx_->createPropNameID(vm::HermesValue::encodeSymbolValue(sym));
    auto retOrErr = ho_->vtable->get(ho_, ctx_, name);
    abi::releasePointer(name.pointer);

    if (abi::isError(retOrErr))
      return ctx_->raiseError(abi::getError(retOrErr));

    auto ret = abi::getValue(retOrErr);
    auto retHV = toHermesValue(ret);
    abi::releaseValue(ret);
    return retHV;
  }

  // This is called to set a property value by name.  It will return
  // \c ExecutionStatus, and set the runtime's thrown value as appropriate.
  vm::CallResult<bool> set(vm::SymbolID sym, vm::HermesValue value) override {
    HermesABIPropNameID name =
        ctx_->createPropNameID(vm::HermesValue::encodeSymbolValue(sym));
    auto abiVal = ctx_->createValue(value);
    auto ret = ho_->vtable->set(ho_, ctx_, name, &abiVal);
    abi::releasePointer(name.pointer);
    abi::releaseValue(abiVal);
    if (abi::isError(ret))
      return ctx_->raiseError(abi::getError(ret));
    return true;
  }

  // This is called to query names of properties.  In case of failure it will
  // return \c ExecutionStatus::EXCEPTION, and set the runtime's thrown Value
  // as appropriate.
  vm::CallResult<vm::Handle<vm::JSArray>> getHostPropertyNames() override {
    auto ret = ho_->vtable->get_property_names(ho_, ctx_);
    if (abi::isError(ret))
      return ctx_->raiseError(abi::getError(ret));
    auto *abiNames = abi::getPropNameIDListPtr(ret);
    const HermesABIPropNameID *names = abiNames->props;
    size_t size = abiNames->size;
    auto &runtime = *ctx_->rt;
    auto arrayRes = vm::JSArray::create(runtime, size, size);
    if (arrayRes == vm::ExecutionStatus::EXCEPTION) {
      abiNames->vtable->release(abiNames);
      return vm::ExecutionStatus::EXCEPTION;
    }
    vm::Handle<vm::JSArray> arrayHandle = *arrayRes;
    vm::JSArray::setStorageEndIndex(arrayHandle, runtime, size);
    for (size_t i = 0; i < size; ++i) {
      auto shv = vm::SmallHermesValue::encodeSymbolValue(*toHandle(names[i]));
      vm::JSArray::unsafeSetExistingElementAt(*arrayHandle, runtime, i, shv);
    }
    abiNames->vtable->release(abiNames);
    return arrayHandle;
  }
};
} // namespace

ABI_PREFIX HermesABIObjectOrError
create_object_from_host_object(HermesABIContext *ctx, HermesABIHostObject *ho) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto objRes = vm::HostObject::createWithoutPrototype(
      runtime, std::make_unique<HostObjectWrapper>(ctx, ho));
  assert(
      objRes != vm::ExecutionStatus::EXCEPTION &&
      "Failed to create HostObject");
  return ctx->createObjectOrError(*objRes);
}

ABI_PREFIX HermesABIHostObject *get_host_object(
    HermesABIContext *,
    HermesABIObject obj) {
  auto h = vm::Handle<vm::HostObject>::vmcast(toHandle(obj));
  return static_cast<HostObjectWrapper *>(h->getProxy())->getHostObject();
}

ABI_PREFIX bool has_native_state(HermesABIContext *ctx, HermesABIObject obj) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto h = toHandle(obj);
  if (h->isProxyObject() || h->isHostObject()) {
    return false;
  }
  vm::NamedPropertyDescriptor desc;
  return vm::JSObject::getOwnNamedDescriptor(
      h,
      runtime,
      vm::Predefined::getSymbolID(vm::Predefined::InternalPropertyNativeState),
      desc);
}
ABI_PREFIX HermesABINativeState *get_native_state(
    HermesABIContext *ctx,
    HermesABIObject obj) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto h = toHandle(obj);
  vm::NamedPropertyDescriptor desc;
  bool exists = vm::JSObject::getOwnNamedDescriptor(
      h,
      runtime,
      vm::Predefined::getSymbolID(vm::Predefined::InternalPropertyNativeState),
      desc);
  (void)exists;
  assert(exists && "Object does not have native state");
  // Raw pointers below.
  vm::NoAllocScope scope(runtime);
  vm::NativeState *ns = vm::vmcast<vm::NativeState>(
      vm::JSObject::getNamedSlotValueUnsafe(*h, runtime, desc)
          .getObject(runtime));
  return static_cast<HermesABINativeState *>(ns->context());
}

ABI_PREFIX HermesABIVoidOrError set_native_state(
    HermesABIContext *ctx,
    HermesABIObject obj,
    HermesABINativeState *abiState) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);

  auto finalize = [](void *state) {
    auto *self = static_cast<HermesABINativeState *>(state);
    self->vtable->release(self);
  };
  // Note that creating the vm::NativeState here takes ownership of abiState, so
  // if the below steps fail, abiState will simply be freed when the
  // vm::NativeState is garbage collected.
  auto ns =
      runtime.makeHandle(vm::NativeState::create(runtime, abiState, finalize));

  auto h = toHandle(obj);
  if (h->isProxyObject()) {
    ctx->nativeExceptionMessage = "Native state is unsupported on Proxy";
    return abi::createVoidOrError(HermesABIErrorCodeNativeException);
  } else if (h->isHostObject()) {
    ctx->nativeExceptionMessage = "Native state is unsupported on HostObject";
    return abi::createVoidOrError(HermesABIErrorCodeNativeException);
  }

  auto res = vm::JSObject::defineOwnProperty(
      h,
      runtime,
      vm::Predefined::getSymbolID(vm::Predefined::InternalPropertyNativeState),
      vm::DefinePropertyFlags::getDefaultNewPropertyFlags(),
      ns);
  if (res == vm::ExecutionStatus::EXCEPTION) {
    return abi::createVoidOrError(HermesABIErrorCodeJSError);
  }
  if (!*res) {
    ctx->nativeExceptionMessage = "Failed to set native state.";
    return abi::createVoidOrError(HermesABIErrorCodeNativeException);
  }
  return abi::createVoidOrError();
}

ABI_PREFIX HermesABIWeakObjectOrError
create_weak_object(HermesABIContext *ctx, HermesABIObject obj) {
  return ctx->createWeakObjectOrError(toHandle(obj).getHermesValue());
}
ABI_PREFIX HermesABIValue
lock_weak_object(HermesABIContext *ctx, HermesABIWeakObject obj) {
  auto &runtime = *ctx->rt;
  const auto &wr =
      static_cast<ManagedValue<vm::WeakRoot<vm::JSObject>> *>(obj.pointer)
          ->value();
  if (const auto ptr = wr.get(runtime, runtime.getHeap()))
    return ctx->createValue(vm::HermesValue::encodeObjectValue(ptr));
  return abi::createUndefinedValue();
}

ABI_PREFIX void get_utf8_from_string(
    HermesABIContext *ctx,
    HermesABIString str,
    HermesABIByteBuffer *buf) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto view = vm::StringPrimitive::createStringView(runtime, toHandle(str));
  std::string convertBuf;
  llvh::StringRef res;
  if (view.isASCII()) {
    res = {view.castToCharPtr(), view.length()};
  } else {
    ::hermes::convertUTF16ToUTF8WithReplacements(
        convertBuf, {view.castToChar16Ptr(), view.length()});
    res = convertBuf;
  }
  if (buf->available < res.size())
    buf->vtable->grow_by(buf, res.size() - buf->available);
  memcpy(buf->data, res.data(), res.size());
  buf->available -= res.size();
}

ABI_PREFIX void get_utf8_from_prop_name_id(
    HermesABIContext *ctx,
    HermesABIPropNameID name,
    HermesABIByteBuffer *buf) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto view =
      runtime.getIdentifierTable().getStringView(runtime, *toHandle(name));
  std::string convertBuf;
  llvh::StringRef res;
  if (view.isASCII()) {
    res = {view.castToCharPtr(), view.length()};
  } else {
    ::hermes::convertUTF16ToUTF8WithReplacements(
        convertBuf, {view.castToChar16Ptr(), view.length()});
    res = convertBuf;
  }
  if (buf->available < res.size())
    buf->vtable->grow_by(buf, res.size() - buf->available);
  memcpy(buf->data, res.data(), res.size());
  buf->available -= res.size();
}

ABI_PREFIX void get_utf8_from_symbol(
    HermesABIContext *ctx,
    HermesABISymbol name,
    HermesABIByteBuffer *buf) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto view =
      runtime.getIdentifierTable().getStringView(runtime, *toHandle(name));
  auto writeToBuf = [buf](llvh::StringRef res) {
    llvh::StringRef prefix{"Symbol("};
    auto *cur = buf->data;
    // Total bytes needed are the size of the string, plus the prefix, plus the
    // closing parenthesis.
    size_t bytesNeeded = res.size() + prefix.size() + 1;
    if (buf->available < bytesNeeded)
      buf->vtable->grow_by(buf, bytesNeeded - buf->available);
    buf->available -= bytesNeeded;

    memcpy(cur, prefix.data(), prefix.size());
    cur += prefix.size();
    memcpy(cur, res.data(), res.size());
    cur += res.size();
    *cur = ')';
  };
  if (view.isASCII()) {
    writeToBuf({view.castToCharPtr(), view.length()});
    return;
  }

  // TODO: Write directly to the output buffer instead of copying.
  std::string ret;
  ::hermes::convertUTF16ToUTF8WithReplacements(
      ret, {view.castToChar16Ptr(), view.length()});
  writeToBuf(ret);
}

ABI_PREFIX HermesABIBoolOrError
instance_of(HermesABIContext *ctx, HermesABIObject o, HermesABIFunction f) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto result = vm::instanceOfOperator_RJS(runtime, toHandle(o), toHandle(f));
  if (result == vm::ExecutionStatus::EXCEPTION)
    return abi::createBoolOrError(HermesABIErrorCodeJSError);
  return abi::createBoolOrError(*result);
}

ABI_PREFIX bool strict_equals_symbol(
    HermesABIContext *ctx,
    HermesABISymbol a,
    HermesABISymbol b) {
  return toHandle(a) == toHandle(b);
}
ABI_PREFIX bool strict_equals_bigint(
    HermesABIContext *ctx,
    HermesABIBigInt a,
    HermesABIBigInt b) {
  return toHandle(a)->compare(*toHandle(b)) == 0;
}
ABI_PREFIX bool strict_equals_string(
    HermesABIContext *ctx,
    HermesABIString a,
    HermesABIString b) {
  return toHandle(a)->equals(*toHandle(b));
}
ABI_PREFIX bool strict_equals_object(
    HermesABIContext *ctx,
    HermesABIObject a,
    HermesABIObject b) {
  return toHandle(a) == toHandle(b);
}

ABI_PREFIX HermesABIBoolOrError drain_microtasks(HermesABIContext *ctx, int) {
  auto &runtime = *ctx->rt;
  if (runtime.hasMicrotaskQueue()) {
    auto drainRes = runtime.drainJobs();
    if (drainRes == vm::ExecutionStatus::EXCEPTION)
      return abi::createBoolOrError(HermesABIErrorCodeJSError);
  }
  runtime.clearKeptObjects();

  // drainJobs currently drains the entire queue, unless there is an exception,
  // so always return true.
  // TODO(T89426441): Support maxMicrotaskHint.
  return abi::createBoolOrError(true);
}

ABI_PREFIX HermesABIBigIntOrError
create_bigint_from_int64(HermesABIContext *ctx, int64_t value) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  vm::CallResult<vm::HermesValue> res =
      vm::BigIntPrimitive::fromSigned(runtime, value);
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createBigIntOrError(HermesABIErrorCodeJSError);
  return ctx->createBigIntOrError(*res);
}
ABI_PREFIX HermesABIBigIntOrError
create_bigint_from_uint64(HermesABIContext *ctx, uint64_t value) {
  auto &runtime = *ctx->rt;
  vm::GCScope gcScope(runtime);
  auto res = vm::BigIntPrimitive::fromUnsigned(runtime, value);
  if (res == vm::ExecutionStatus::EXCEPTION)
    return abi::createBigIntOrError(HermesABIErrorCodeJSError);
  return ctx->createBigIntOrError(*res);
}
ABI_PREFIX bool bigint_is_int64(HermesABIContext *, HermesABIBigInt bigint) {
  return toHandle(bigint)->isTruncationToSingleDigitLossless(
      /* signedTruncation */ true);
}
ABI_PREFIX bool bigint_is_uint64(HermesABIContext *, HermesABIBigInt bigint) {
  return toHandle(bigint)->isTruncationToSingleDigitLossless(
      /* signedTruncation */ false);
}
ABI_PREFIX uint64_t
bigint_truncate_to_uint64(HermesABIContext *, HermesABIBigInt bigint) {
  auto digit = toHandle(bigint)->truncateToSingleDigit();
  static_assert(
      sizeof(digit) == sizeof(uint64_t),
      "BigInt digit is no longer sizeof(uint64_t) bytes.");
  return digit;
}
ABI_PREFIX HermesABIStringOrError bigint_to_string(
    HermesABIContext *ctx,
    HermesABIBigInt bigint,
    unsigned radix) {
  auto &runtime = *ctx->rt;
  if (radix < 2 || radix > 36) {
    ctx->nativeExceptionMessage = "Radix must be between 2 and 36";
    return abi::createStringOrError(HermesABIErrorCodeNativeException);
  }

  vm::GCScope gcScope(runtime);
  auto toStringRes = vm::BigIntPrimitive::toString(
      runtime, vm::createPseudoHandle(*toHandle(bigint)), radix);

  if (toStringRes == vm::ExecutionStatus::EXCEPTION)
    return abi::createStringOrError(HermesABIErrorCodeJSError);
  return ctx->createStringOrError(*toStringRes);
}

extern "C" {
#ifdef _MSC_VER
__declspec(dllexport)
#else // _MSC_VER
__attribute__((visibility("default")))
#endif // _MSC_VER
    const HermesABIVTable *get_hermes_abi_vtable() {
  static const HermesABIVTable abiVtable = {
      make_hermes_runtime,
      release_hermes_runtime,
      get_and_clear_js_error_value,
      get_native_exception_message,
      clear_native_exception_message,
      set_js_error_value,
      set_native_exception_message,
      clone_prop_name_id,
      clone_string,
      clone_symbol,
      clone_object,
      clone_big_int,
      is_hermes_bytecode,
      evaluate_javascript_source,
      evaluate_hermes_bytecode,
      get_global_object,
      create_string_from_ascii,
      create_string_from_utf8,
      create_object,
      has_object_property_from_string,
      has_object_property_from_prop_name_id,
      get_object_property_from_string,
      get_object_property_from_prop_name_id,
      set_object_property_from_string,
      set_object_property_from_prop_name_id,
      get_object_property_names,
      create_array,
      get_array_length,
      get_array_value_at_index,
      set_array_value_at_index,
      create_array_buffer_from_external_data,
      get_array_buffer_data,
      get_array_buffer_size,
      create_prop_name_id_from_ascii,
      create_prop_name_id_from_utf8,
      create_prop_name_id_from_string,
      create_prop_name_id_from_symbol,
      prop_name_id_equals,
      object_is_array,
      object_is_array_buffer,
      object_is_function,
      object_is_host_object,
      function_is_host_function,
      call,
      call_as_constructor,
      create_function_from_host_function,
      get_host_function,
      create_object_from_host_object,
      get_host_object,
      has_native_state,
      get_native_state,
      set_native_state,
      create_weak_object,
      lock_weak_object,
      get_utf8_from_string,
      get_utf8_from_prop_name_id,
      get_utf8_from_symbol,
      instance_of,
      strict_equals_symbol,
      strict_equals_bigint,
      strict_equals_string,
      strict_equals_object,
      drain_microtasks,
      create_bigint_from_int64,
      create_bigint_from_uint64,
      bigint_is_int64,
      bigint_is_uint64,
      bigint_truncate_to_uint64,
      bigint_to_string,
  };
  return &abiVtable;
}
} // extern "C"
