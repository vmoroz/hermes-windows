/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_abi/HermesABIRuntime.h"

#include "hermes_abi/HermesABIHelpers.h"
#include "hermes_abi/hermes_abi.h"

#include "hermes/ADT/ManagedChunkedList.h"

#include <atomic>

using namespace facebook::jsi;
using namespace facebook::hermes;

namespace {
class BufferWrapper : public HermesABIBuffer {
  std::shared_ptr<const Buffer> buf_;

  static size_t size(const HermesABIBuffer *buf) {
    return static_cast<const BufferWrapper *>(buf)->buf_->size();
  }
  static const uint8_t *data(const HermesABIBuffer *buf) {
    return static_cast<const BufferWrapper *>(buf)->buf_->data();
  }
  static void release(HermesABIBuffer *buf) {
    delete static_cast<const BufferWrapper *>(buf);
  }
  static constexpr HermesABIBufferVTable vt{
      release,
  };

 public:
  explicit BufferWrapper(std::shared_ptr<const Buffer> buf)
      : HermesABIBuffer{&vt, buf->data(), buf->size()}, buf_(std::move(buf)) {}
};

/// Helper class to save and restore a value on exiting a scope.
template <typename T>
class SaveAndRestore {
  T &target_;
  T oldVal_;

 public:
  SaveAndRestore(T &target) : target_(target), oldVal_(target) {}
  ~SaveAndRestore() {
    target_ = oldVal_;
  }
};

#define throwUnimplemented() \
  throw JSINativeException(std::string("Unimplemented function ") + __func__)

class HermesABIRuntime : public Runtime {
  class ManagedPointerHolder;

  const HermesABIVTable *vtable_;
  HermesABIContext *ctx_;
  hermes::ManagedChunkedList<ManagedPointerHolder> managedPointers_;
  bool activeJSError_ = false;

  /// A ManagedChunkedList element that indicates whether it's occupied based on
  /// a refcount. This is just a temporary measure until we replace
  /// jsi::PointerValue with something like HermesABIManagedPointer, so we can
  /// directly invalidate values.
  class ManagedPointerHolder : public PointerValue {
    std::atomic<uint32_t> refCount_;
    union {
      HermesABIManagedPointer *managedPointer_;
      ManagedPointerHolder *nextFree_;
    };

   public:
    ManagedPointerHolder() : refCount_(0) {}

    /// Determine whether the element is occupied by inspecting the refcount.
    bool isFree() const {
      return refCount_.load(std::memory_order_relaxed) == 0;
    }

    /// Store a value and start the refcount at 1. After invocation, this
    /// instance is occupied with a value, and the "nextFree" methods should
    /// not be used until the value is released.
    void emplace(HermesABIManagedPointer *managedPointer) {
      assert(isFree() && "Emplacing already occupied value");
      refCount_.store(1, std::memory_order_relaxed);
      managedPointer_ = managedPointer;
    }

    /// Get the next free element. Must not be called when this instance is
    /// occupied with a value.
    ManagedPointerHolder *getNextFree() {
      assert(isFree() && "Free pointer unusuable while occupied");
      return nextFree_;
    }

    /// Set the next free element. Must not be called when this instance is
    /// occupied with a value.
    void setNextFree(ManagedPointerHolder *nextFree) {
      assert(isFree() && "Free pointer unusuable while occupied");
      nextFree_ = nextFree;
    }

    HermesABIManagedPointer *getManagedPointer() const {
      assert(!isFree() && "Value not present");
      return managedPointer_;
    }

    void invalidate() override {
      dec();
    }

    void inc() {
      // See comments in hermes_abi.cpp for why we use relaxed operations here.
      auto oldCount = refCount_.fetch_add(1, std::memory_order_relaxed);
      assert(oldCount && "Cannot resurrect a pointer");
      assert(oldCount + 1 != 0 && "Ref count overflow");
      (void)oldCount;
    }

    void dec() {
      // See comments in hermes_abi.cpp for why we use relaxed operations here.
      auto oldCount = refCount_.fetch_sub(1, std::memory_order_relaxed);
      assert(oldCount > 0 && "Ref count underflow");
      // This was the last decrement of this holder, so we can invalidate the
      // underlying pointer.
      if (oldCount == 1)
        managedPointer_->vtable->invalidate(managedPointer_);
    }
  };

  PointerValue *clone(const PointerValue *pv) {
    // TODO: Evaluate whether to keep this null check. It is currently here for
    //       compatibility with hermes' API, but it is odd that it is the only
    //       API that allows null.
    if (!pv)
      return nullptr;

    auto *nonConst = const_cast<PointerValue *>(pv);
    static_cast<ManagedPointerHolder *>(nonConst)->inc();
    return nonConst;
  }

  [[noreturn]] void throwError(HermesABIErrorCode err) const {
    if (err == HermesABIErrorCodeNativeException) {
      auto ref = vtable_->get_native_exception_message(ctx_);
      std::string msg{(const char *)ref.data, ref.length};
      vtable_->clear_native_exception_message(ctx_);
      throw JSINativeException(std::move(msg));
    } else {
      throw JSINativeException("ABI threw an unknown error.");
    }
  }
  [[noreturn]] void throwError(HermesABIErrorCode err) {
    if (err == HermesABIErrorCodeJSError) {
      // We have to get and clear the error regardless of whether it is used.
      auto errVal = intoJSIValue(vtable_->get_and_clear_js_error_value(ctx_));

      // If we are already in the process of creating a JSError, it means that
      // something in JSError's constructor is throwing. We cannot handle this
      // gracefully, so bail.
      if (activeJSError_)
        throw JSINativeException("Error thrown while handling error.");

      // Record the fact that we are in the process of creating a JSError.
      SaveAndRestore s(activeJSError_);
      activeJSError_ = true;
      throw JSError(*this, std::move(errVal));
    }
    static_cast<const HermesABIRuntime *>(this)->throwError(err);
  }

#define DECLARE_POINTER_CONVERSIONS(name)                              \
  name intoJSI##name(const HermesABI##name &p) {                         \
    return make<name>(&managedPointers_.add(p.pointer));               \
  }                                                                    \
  name intoJSI##name(const HermesABI##name##OrError &p) {                \
    return intoJSI##name(unwrap(p));                                     \
  }                                                                    \
  HermesABI##name toABI##name(const name &p) const {                   \
    return abi::create##name(                                          \
        static_cast<const ManagedPointerHolder *>(getPointerValue(p))  \
            ->getManagedPointer());                                    \
  }                                                                    \
  HermesABI##name unwrap(const HermesABI##name##OrError &p) {          \
    if (p.ptrOrError & 1)                                              \
      throwError(static_cast<HermesABIErrorCode>(p.ptrOrError >> 2));  \
    return abi::create##name((HermesABIManagedPointer *)p.ptrOrError); \
  }

  HERMES_ABI_POINTER_TYPES(DECLARE_POINTER_CONVERSIONS)
#undef DECLARE_POINTER_CONVERSIONS

  PropNameID cloneToJSIPropNameID(HermesABIPropNameID name) {
    return intoJSIPropNameID(vtable_->clone_prop_name_id(ctx_, name));
  }

#define DECLARE_TRIVIAL_OR_ERROR_CONVERSIONS(name, type) \
  type unwrap(const HermesABI##name##OrError &p) {       \
    if (p.is_error)                                      \
      throwError((HermesABIErrorCode)p.data.error);      \
    return p.data.val;                                   \
  }                                                      \
  type unwrap(const HermesABI##name##OrError &p) const { \
    if (p.is_error)                                      \
      throwError((HermesABIErrorCode)p.data.error);      \
    return p.data.val;                                   \
  }

  HERMES_ABI_TRIVIAL_OR_ERROR_TYPES(DECLARE_TRIVIAL_OR_ERROR_CONVERSIONS)
#undef DECLARE_TRIVIAL_OR_ERROR_CONVERSIONS

  void unwrap(const HermesABIVoidOrError &v) {
    if (v.is_error)
      throwError((HermesABIErrorCode)v.error);
  }

  /// Take ownership of the given value \p v and wrap it in a jsi::Value that
  /// will now manage its lifetime.
  Value intoJSIValue(const HermesABIValue &v) {
    switch (abi::getValueKind(v)) {
      case HermesABIValueKindUndefined:
        return Value::undefined();
      case HermesABIValueKindNull:
        return Value::null();
      case HermesABIValueKindBoolean:
        return Value(abi::getBoolValue(v));
      case HermesABIValueKindNumber:
        return Value(abi::getNumberValue(v));
      case HermesABIValueKindString:
        return make<String>(&managedPointers_.add(abi::getPointerValue(v)));
      case HermesABIValueKindObject:
        return make<Object>(&managedPointers_.add(abi::getPointerValue(v)));
      case HermesABIValueKindSymbol:
        return make<Symbol>(&managedPointers_.add(abi::getPointerValue(v)));
      case HermesABIValueKindBigInt:
        return make<BigInt>(&managedPointers_.add(abi::getPointerValue(v)));
      default:
        // We aren't able to construct an equivalent jsi::Value, just release
        // the value that was passed in.
        abi::releaseValue(v);
        throw JSINativeException("ABI returned an unknown value kind.");
    }
  }
  Value intoJSIValue(const HermesABIValueOrError &val) {
    if (abi::isError(val))
      throwError(abi::getError(val));
    return intoJSIValue(abi::getValue(val));
  }

  /// Create a jsi::Value from the given HermesABIValue without taking ownership
  /// of it. This will clone any underlying pointers if needed.
  Value cloneToJSIValue(const HermesABIValue &v) {
    switch (abi::getValueKind(v)) {
      case HermesABIValueKindUndefined:
        return Value::undefined();
      case HermesABIValueKindNull:
        return Value::null();
      case HermesABIValueKindBoolean:
        return Value(abi::getBoolValue(v));
      case HermesABIValueKindNumber:
        return Value(abi::getNumberValue(v));
      case HermesABIValueKindString:
        return intoJSIString(vtable_->clone_string(ctx_, abi::getStringValue(v)));
      case HermesABIValueKindObject:
        return intoJSIObject(vtable_->clone_object(ctx_, abi::getObjectValue(v)));
      case HermesABIValueKindSymbol:
        return intoJSISymbol(vtable_->clone_symbol(ctx_, abi::getSymbolValue(v)));
      case HermesABIValueKindBigInt:
        return intoJSIBigInt(
            vtable_->clone_big_int(ctx_, abi::getBigIntValue(v)));
      default:
        // Don't release the value here, since we didn't take ownership.
        throw JSINativeException("ABI returned an unknown value kind.");
    }
  }
  Value cloneToJSIValue(const HermesABIValueOrError &val) {
    if (abi::isError(val))
      throwError(abi::getError(val));
    return cloneToJSIValue(abi::getValue(val));
  }

  static HermesABIValue toABIValue(const Value &v) {
    if (v.isUndefined())
      return abi::createUndefinedValue();
    if (v.isNull())
      return abi::createNullValue();
    if (v.isBool())
      return abi::createBoolValue(v.getBool());
    if (v.isNumber())
      return abi::createNumberValue(v.getNumber());

    HermesABIManagedPointer *mp =
        static_cast<const ManagedPointerHolder *>(getPointerValue(v))
            ->getManagedPointer();
    if (v.isString())
      return abi::createStringValue(mp);
    if (v.isObject())
      return abi::createObjectValue(mp);
    if (v.isSymbol())
      return abi::createSymbolValue(mp);
    if (v.isBigInt())
      return abi::createBigIntValue(mp);

    assert(false && "Unexpected value type.");
  }

  HermesABIValue cloneToABIValue(const Value &v) {
    if (v.isUndefined())
      return abi::createUndefinedValue();
    if (v.isNull())
      return abi::createNullValue();
    if (v.isBool())
      return abi::createBoolValue(v.getBool());
    if (v.isNumber())
      return abi::createNumberValue(v.getNumber());

    HermesABIManagedPointer *mp =
        static_cast<const ManagedPointerHolder *>(getPointerValue(v))
            ->getManagedPointer();
    if (v.isString())
      return abi::createStringValue(
          vtable_->clone_string(ctx_, abi::createString(mp)));
    if (v.isObject())
      return abi::createObjectValue(
          vtable_->clone_object(ctx_, abi::createObject(mp)));
    if (v.isSymbol())
      return abi::createSymbolValue(
          vtable_->clone_symbol(ctx_, abi::createSymbol(mp)));
    if (v.isBigInt())
      return abi::createBigIntValue(
          vtable_->clone_big_int(ctx_, abi::createBigInt(mp)));

    assert(false && "Unexpected value type.");
  }

 public:
  HermesABIRuntime(
      const HermesABIVTable *vtable,
      const ::hermes::vm::RuntimeConfig &runtimeConfig)
      : vtable_(vtable),
        managedPointers_(
            runtimeConfig.getGCConfig().getOccupancyTarget(),
            0.5) {
    ctx_ = vtable_->make_hermes_runtime(nullptr);
  }
  ~HermesABIRuntime() override {
    vtable_->release_hermes_runtime(ctx_);
    assert(managedPointers_.sizeForTests() == 0 && "Dangling references.");
  }

  Value evaluateJavaScript(
      const std::shared_ptr<const Buffer> &buffer,
      const std::string &sourceURL) override {
    auto *bw = new BufferWrapper(buffer);
    if (vtable_->is_hermes_bytecode(buffer->data(), buffer->size()))
      return intoJSIValue(vtable_->evaluate_hermes_bytecode(
          ctx_, bw, sourceURL.c_str(), sourceURL.size()));

    return intoJSIValue(vtable_->evaluate_javascript_source(
        ctx_, bw, sourceURL.c_str(), sourceURL.size()));
  }

  std::shared_ptr<const PreparedJavaScript> prepareJavaScript(
      const std::shared_ptr<const Buffer> &buffer,
      std::string sourceURL) override {
    throwUnimplemented();
  }

  Value evaluatePreparedJavaScript(
      const std::shared_ptr<const PreparedJavaScript> &js) override {
    throwUnimplemented();
  }

  bool drainMicrotasks(int maxMicrotasksHint = -1) override {
    throwUnimplemented();
  }

  Object global() override {
    throwUnimplemented();
  }

  std::string description() override {
    throwUnimplemented();
  }

  bool isInspectable() override {
    throwUnimplemented();
  }

  Instrumentation &instrumentation() override {
    throwUnimplemented();
  }

 protected:
  PointerValue *cloneSymbol(const Runtime::PointerValue *pv) override {
    return clone(pv);
  }
  PointerValue *cloneBigInt(const Runtime::PointerValue *pv) override {
    return clone(pv);
  }
  PointerValue *cloneString(const Runtime::PointerValue *pv) override {
    return clone(pv);
  }
  PointerValue *cloneObject(const Runtime::PointerValue *pv) override {
    return clone(pv);
  }
  PointerValue *clonePropNameID(const Runtime::PointerValue *pv) override {
    return clone(pv);
  }

  PropNameID createPropNameIDFromAscii(const char *str, size_t length)
      override {
    throwUnimplemented();
  }
  PropNameID createPropNameIDFromUtf8(const uint8_t *utf8, size_t length)
      override {
    throwUnimplemented();
  }
  PropNameID createPropNameIDFromString(const String &str) override {
    throwUnimplemented();
  }
  PropNameID createPropNameIDFromSymbol(const Symbol &sym) override {
    throwUnimplemented();
  }
  std::string utf8(const PropNameID &) override {
    throwUnimplemented();
  }
  bool compare(const PropNameID &, const PropNameID &) override {
    throwUnimplemented();
  }

  std::string symbolToString(const Symbol &) override {
    throwUnimplemented();
  }

  BigInt createBigIntFromInt64(int64_t) override {
    throwUnimplemented();
  }
  BigInt createBigIntFromUint64(uint64_t) override {
    throwUnimplemented();
  }
  bool bigintIsInt64(const BigInt &) override {
    throwUnimplemented();
  }
  bool bigintIsUint64(const BigInt &) override {
    throwUnimplemented();
  }
  uint64_t truncate(const BigInt &) override {
    throwUnimplemented();
  }
  String bigintToString(const BigInt &, int) override {
    throwUnimplemented();
  }

  String createStringFromAscii(const char *str, size_t length) override {
    throwUnimplemented();
  }
  String createStringFromUtf8(const uint8_t *utf8, size_t length) override {
    throwUnimplemented();
  }
  std::string utf8(const String &) override {
    throwUnimplemented();
  }

  Object createObject() override {
    throwUnimplemented();
  }
  Object createObject(std::shared_ptr<HostObject> ho) override {
    throwUnimplemented();
  }
  std::shared_ptr<HostObject> getHostObject(const Object &) override {
    throwUnimplemented();
  }
  HostFunctionType &getHostFunction(const Function &) override {
    throwUnimplemented();
  }

  bool hasNativeState(const Object &) override {
    throwUnimplemented();
  }
  std::shared_ptr<NativeState> getNativeState(const Object &) override {
    throwUnimplemented();
  }
  void setNativeState(const Object &, std::shared_ptr<NativeState> state)
      override {
    throwUnimplemented();
  }

  Value getProperty(const Object &, const PropNameID &name) override {
    throwUnimplemented();
  }
  Value getProperty(const Object &, const String &name) override {
    throwUnimplemented();
  }
  bool hasProperty(const Object &, const PropNameID &name) override {
    throwUnimplemented();
  }
  bool hasProperty(const Object &, const String &name) override {
    throwUnimplemented();
  }
  void setPropertyValue(
      const Object &,
      const PropNameID &name,
      const Value &value) override {
    throwUnimplemented();
  }
  void setPropertyValue(const Object &, const String &name, const Value &value)
      override {
    throwUnimplemented();
  }

  bool isArray(const Object &) const override {
    throwUnimplemented();
  }
  bool isArrayBuffer(const Object &) const override {
    throwUnimplemented();
  }
  bool isFunction(const Object &) const override {
    throwUnimplemented();
  }
  bool isHostObject(const Object &) const override {
    throwUnimplemented();
  }
  bool isHostFunction(const Function &) const override {
    throwUnimplemented();
  }
  Array getPropertyNames(const Object &) override {
    throwUnimplemented();
  }

  WeakObject createWeakObject(const Object &) override {
    throwUnimplemented();
  }
  Value lockWeakObject(const WeakObject &) override {
    throwUnimplemented();
  }

  Array createArray(size_t length) override {
    throwUnimplemented();
  }
  ArrayBuffer createArrayBuffer(
      std::shared_ptr<MutableBuffer> buffer) override {
    throwUnimplemented();
  }
  size_t size(const Array &) override {
    throwUnimplemented();
  }
  size_t size(const ArrayBuffer &) override {
    throwUnimplemented();
  }
  uint8_t *data(const ArrayBuffer &) override {
    throwUnimplemented();
  }
  Value getValueAtIndex(const Array &, size_t i) override {
    throwUnimplemented();
  }
  void setValueAtIndexImpl(const Array &, size_t i, const Value &value)
      override {
    throwUnimplemented();
  }

  Function createFunctionFromHostFunction(
      const PropNameID &name,
      unsigned int paramCount,
      HostFunctionType func) override {
    throwUnimplemented();
  }
  Value call(
      const Function &,
      const Value &jsThis,
      const Value *args,
      size_t count) override {
    throwUnimplemented();
  }
  Value callAsConstructor(const Function &, const Value *args, size_t count)
      override {
    throwUnimplemented();
  }

  bool strictEquals(const Symbol &a, const Symbol &b) const override {
    throwUnimplemented();
  }
  bool strictEquals(const BigInt &a, const BigInt &b) const override {
    throwUnimplemented();
  }
  bool strictEquals(const String &a, const String &b) const override {
    throwUnimplemented();
  }
  bool strictEquals(const Object &a, const Object &b) const override {
    throwUnimplemented();
  }

  bool instanceOf(const Object &o, const Function &f) override {
    throwUnimplemented();
  }
};

} // namespace

namespace facebook::hermes {
std::unique_ptr<facebook::jsi::Runtime> makeHermesABIRuntime(
    const HermesABIVTable *vtable,
    const ::hermes::vm::RuntimeConfig &runtimeConfig) {
  return std::make_unique<HermesABIRuntime>(vtable, runtimeConfig);
}
} // namespace facebook::hermes
