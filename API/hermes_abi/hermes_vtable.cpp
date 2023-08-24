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
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/Runtime.h"
#include "hermes_abi/HermesABIHelpers.h"

#define ABI_PREFIX static

using namespace hermes;
using namespace facebook::hermes;

namespace {

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
  };
  return &abiVtable;
}
} // extern "C"
