/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_abi/HermesABIRuntime.h"

#include "hermes_abi/HermesABIHelpers.h"
#include "hermes_abi/hermes_abi.h"
#include "jsi/jsilib.h"

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

class MutableBufferWrapper : public HermesABIMutableBuffer {
  std::shared_ptr<MutableBuffer> buf_;

  static void release(HermesABIMutableBuffer *buf) {
    delete static_cast<const MutableBufferWrapper *>(buf);
  }
  static constexpr HermesABIMutableBufferVTable vt{
      release,
  };

 public:
  explicit MutableBufferWrapper(std::shared_ptr<MutableBuffer> buf)
      : HermesABIMutableBuffer{&vt, buf->data(), buf->size()},
        buf_(std::move(buf)) {}
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

  /// An implementation of HermesABIByteBuffer that uses a string as its
  /// internal storage. This can be used to conveniently construct a std::string
  /// from ABI functions that return strings.
  class StringByteBuffer : public HermesABIByteBuffer {
    std::string buf_;

    static void grow_by(HermesABIByteBuffer *buf, size_t amount) {
      auto *self = static_cast<StringByteBuffer *>(buf);
      self->buf_.resize(self->buf_.size() + amount);
      self->data = (uint8_t *)self->buf_.data();
      self->available += amount;
    }

    static constexpr HermesABIByteBufferVTable vt{
        grow_by,
    };

   public:
    explicit StringByteBuffer() : HermesABIByteBuffer{&vt, nullptr, 0} {
      // Make the small string storage available for use without needing to call
      // grow_by.
      buf_.resize(buf_.capacity());
      data = (uint8_t *)buf_.data();
      available = buf_.size();
    }

    std::string get() && {
      // Trim off any unused bytes at the end.
      buf_.resize(buf_.size() - available);
      return std::move(buf_);
    }
  };

  class HostFunctionWrapper : public HermesABIHostFunction {
    HermesABIRuntime &rt_;
    HostFunctionType hf_;

    static HermesABIValueOrError call(
        HermesABIHostFunction *hf,
        HermesABIContext *ctx,
        const HermesABIValue *thisArg,
        const HermesABIValue *args,
        size_t count) {
      auto *self = static_cast<HostFunctionWrapper *>(hf);
      auto &rt = self->rt_;
      try {
        std::vector<Value> jsiArgs;
        jsiArgs.reserve(count);
        for (size_t i = 0; i < count; ++i)
          jsiArgs.emplace_back(rt.cloneToJSIValue(args[i]));

        auto jsiThisArg = rt.cloneToJSIValue(*thisArg);
        return abi::createValueOrError(rt.cloneToABIValue(
            self->hf_(rt, jsiThisArg, jsiArgs.data(), count)));
      } catch (const JSError &e) {
        auto abiVal = toABIValue(e.value());
        rt.vtable_->set_js_error_value(ctx, &abiVal);
        return abi::createValueOrError(HermesABIErrorCodeJSError);
      } catch (const std::exception &e) {
        auto what = std::string("Exception in HostFunction: ") + e.what();
        rt.vtable_->set_native_exception_message(
            ctx, what.c_str(), what.size());
        return abi::createValueOrError(HermesABIErrorCodeNativeException);
      } catch (...) {
        std::string err = "An unknown exception occurred in HostFunction.";
        rt.vtable_->set_native_exception_message(ctx, err.c_str(), err.size());
        return abi::createValueOrError(HermesABIErrorCodeNativeException);
      }
    }
    static void release(HermesABIHostFunction *hf) {
      delete static_cast<HostFunctionWrapper *>(hf);
    }

   public:
    static constexpr HermesABIHostFunctionVTable vt{
        call,
        release,
    };

    HostFunctionWrapper(HermesABIRuntime &rt, HostFunctionType hf)
        : HermesABIHostFunction{&vt}, rt_{rt}, hf_{std::move(hf)} {}

    HostFunctionType &getHostFunction() {
      return hf_;
    }
  };

  class HostObjectWrapper : public HermesABIHostObject {
    HermesABIRuntime &rt_;
    std::shared_ptr<HostObject> ho_;

    static HermesABIValueOrError get(
        HermesABIHostObject *ho,
        HermesABIContext *ctx,
        HermesABIPropNameID name) {
      auto *self = static_cast<HostObjectWrapper *>(ho);
      auto &rt = self->rt_;
      try {
        auto jsiName = rt.cloneToJSIPropNameID(name);
        return abi::createValueOrError(
            rt.cloneToABIValue(self->ho_->get(rt, jsiName)));
      } catch (const JSError &e) {
        auto abiVal = toABIValue(e.value());
        rt.vtable_->set_js_error_value(ctx, &abiVal);
        return abi::createValueOrError(HermesABIErrorCodeJSError);
      } catch (const std::exception &e) {
        auto *what = e.what();
        rt.vtable_->set_native_exception_message(ctx, what, strlen(what));
        return abi::createValueOrError(HermesABIErrorCodeNativeException);
      } catch (...) {
        std::string err = "An unknown exception occurred in HostObject::get";
        rt.vtable_->set_native_exception_message(ctx, err.c_str(), err.size());
        return abi::createValueOrError(HermesABIErrorCodeNativeException);
      }
    }

    static HermesABIVoidOrError set(
        HermesABIHostObject *ho,
        HermesABIContext *ctx,
        HermesABIPropNameID name,
        const HermesABIValue *value) {
      auto *self = static_cast<HostObjectWrapper *>(ho);
      auto &rt = self->rt_;
      try {
        auto jsiName = rt.cloneToJSIPropNameID(name);
        auto jsiValue = rt.cloneToJSIValue(*value);
        self->ho_->set(rt, jsiName, jsiValue);
        return abi::createVoidOrError();
      } catch (const JSError &e) {
        auto abiVal = toABIValue(e.value());
        rt.vtable_->set_js_error_value(ctx, &abiVal);
        return abi::createVoidOrError(HermesABIErrorCodeJSError);
      } catch (const std::exception &e) {
        auto what = std::string("Exception in HostObject: ") + e.what();
        rt.vtable_->set_native_exception_message(
            ctx, what.c_str(), what.size());
        return abi::createVoidOrError(HermesABIErrorCodeNativeException);
      } catch (...) {
        std::string err = "An unknown exception occurred in HostObject::set";
        rt.vtable_->set_native_exception_message(ctx, err.c_str(), err.size());
        return abi::createVoidOrError(HermesABIErrorCodeNativeException);
      }
    }

    class PropNameIDListWrapper : public HermesABIPropNameIDList {
      std::vector<PropNameID> jsiPropsVec_;
      std::vector<HermesABIPropNameID> abiPropsVec_;

      static void release(HermesABIPropNameIDList *self) {
        delete static_cast<PropNameIDListWrapper *>(self);
      }
      static constexpr HermesABIPropNameIDListVTable vt{
          release,
      };

     public:
      PropNameIDListWrapper(
          std::vector<PropNameID> jsiPropsVec,
          std::vector<HermesABIPropNameID> abiPropsVec) {
        vtable = &vt;
        jsiPropsVec_ = std::move(jsiPropsVec);
        abiPropsVec_ = std::move(abiPropsVec);
        props = abiPropsVec_.data();
        size = abiPropsVec_.size();
      }
    };

    static HermesABIPropNameIDListPtrOrError getPropertyNames(
        HermesABIHostObject *ho,
        HermesABIContext *ctx) {
      auto *self = static_cast<HostObjectWrapper *>(ho);
      auto &rt = self->rt_;

      try {
        auto res = self->ho_->getPropertyNames(rt);
        std::vector<HermesABIPropNameID> v;
        for (auto &p : res)
          v.push_back(rt.toABIPropNameID(p));
        return abi::createPropNameIDListPtrOrError(
            new PropNameIDListWrapper(std::move(res), std::move(v)));
      } catch (const JSError &e) {
        auto abiVal = toABIValue(e.value());
        rt.vtable_->set_js_error_value(ctx, &abiVal);
        return abi::createPropNameIDListPtrOrError(HermesABIErrorCodeJSError);
      } catch (const std::exception &e) {
        auto what = std::string("Exception in HostObject: ") + e.what();
        rt.vtable_->set_native_exception_message(
            ctx, what.c_str(), what.size());
        return abi::createPropNameIDListPtrOrError(
            HermesABIErrorCodeNativeException);
      } catch (...) {
        std::string err =
            "An unknown exception occurred in HostObject::getPropertyNames";
        rt.vtable_->set_native_exception_message(ctx, err.c_str(), err.size());
        return abi::createPropNameIDListPtrOrError(
            HermesABIErrorCodeNativeException);
      }
    }

    static void release(HermesABIHostObject *ho) {
      delete static_cast<HostObjectWrapper *>(ho);
    }

   public:
    static constexpr HermesABIHostObjectVTable vt{
        get,
        set,
        getPropertyNames,
        release,
    };

    HostObjectWrapper(HermesABIRuntime &rt, std::shared_ptr<HostObject> ho)
        : HermesABIHostObject{&vt}, rt_{rt}, ho_{std::move(ho)} {}

    std::shared_ptr<HostObject> getHostObject() const {
      return ho_;
    }
  };

  class NativeStateWrapper : public HermesABINativeState {
    std::shared_ptr<NativeState> nativeState_;

    static void release(HermesABINativeState *self) {
      delete static_cast<NativeStateWrapper *>(self);
    }

   public:
    static constexpr HermesABINativeStateVTable vt{
        release,
    };

    NativeStateWrapper(std::shared_ptr<NativeState> nativeState)
        : HermesABINativeState{&vt}, nativeState_{std::move(nativeState)} {}

    std::shared_ptr<NativeState> getNativeState() const {
      return nativeState_;
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
    return std::make_shared<const SourceJavaScriptPreparation>(
        buffer, std::move(sourceURL));
  }

  Value evaluatePreparedJavaScript(
      const std::shared_ptr<const PreparedJavaScript> &js) override {
    assert(dynamic_cast<const SourceJavaScriptPreparation *>(js.get()));
    auto sjp = std::static_pointer_cast<const SourceJavaScriptPreparation>(js);
    return evaluateJavaScript(sjp, sjp->sourceURL());
  }

  bool drainMicrotasks(int maxMicrotasksHint = -1) override {
    return unwrap(vtable_->drain_microtasks(ctx_, maxMicrotasksHint));
  }

  Object global() override {
    return intoJSIObject(vtable_->get_global_object(ctx_));
  }

  std::string description() override {
    return "HermesABIRuntime";
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
    return intoJSIPropNameID(
        vtable_->create_prop_name_id_from_ascii(ctx_, str, length));
  }
  PropNameID createPropNameIDFromUtf8(const uint8_t *utf8, size_t length)
      override {
    return intoJSIPropNameID(
        vtable_->create_prop_name_id_from_utf8(ctx_, utf8, length));
  }
  PropNameID createPropNameIDFromString(const String &str) override {
    return intoJSIPropNameID(
        vtable_->create_prop_name_id_from_string(ctx_, toABIString(str)));
  }
  PropNameID createPropNameIDFromSymbol(const Symbol &sym) override {
    return intoJSIPropNameID(
        vtable_->create_prop_name_id_from_symbol(ctx_, toABISymbol(sym)));
  }
  std::string utf8(const PropNameID &name) override {
    StringByteBuffer buffer;
    vtable_->get_utf8_from_prop_name_id(ctx_, toABIPropNameID(name), &buffer);
    return std::move(buffer).get();
  }
  bool compare(const PropNameID &a, const PropNameID &b) override {
    return vtable_->prop_name_id_equals(
        ctx_, toABIPropNameID(a), toABIPropNameID(b));
  }

  std::string symbolToString(const Symbol &sym) override {
    StringByteBuffer buffer;
    vtable_->get_utf8_from_symbol(ctx_, toABISymbol(sym), &buffer);
    return std::move(buffer).get();
  }

  BigInt createBigIntFromInt64(int64_t value) override {
    return intoJSIBigInt(vtable_->create_bigint_from_int64(ctx_, value));
  }
  BigInt createBigIntFromUint64(uint64_t value) override {
    return intoJSIBigInt(vtable_->create_bigint_from_uint64(ctx_, value));
  }
  bool bigintIsInt64(const BigInt &bigint) override {
    return vtable_->bigint_is_int64(ctx_, toABIBigInt(bigint));
  }
  bool bigintIsUint64(const BigInt &bigint) override {
    return vtable_->bigint_is_uint64(ctx_, toABIBigInt(bigint));
  }
  uint64_t truncate(const BigInt &bigint) override {
    return vtable_->bigint_truncate_to_uint64(ctx_, toABIBigInt(bigint));
  }
  String bigintToString(const BigInt &bigint, int radix) override {
    // Note that the ABI takes the radix as unsigned, but it is safe to pass in
    // the signed value without a check because values <2 or >36 will be
    // rejected anyway.
    return intoJSIString(
        vtable_->bigint_to_string(ctx_, toABIBigInt(bigint), (unsigned)radix));
  }

  String createStringFromAscii(const char *str, size_t length) override {
    return intoJSIString(vtable_->create_string_from_ascii(ctx_, str, length));
  }
  String createStringFromUtf8(const uint8_t *utf8, size_t length) override {
    return intoJSIString(vtable_->create_string_from_utf8(ctx_, utf8, length));
  }
  std::string utf8(const String &str) override {
    StringByteBuffer buffer;
    vtable_->get_utf8_from_string(ctx_, toABIString(str), &buffer);
    return std::move(buffer).get();
  }

  Object createObject() override {
    return intoJSIObject(vtable_->create_object(ctx_));
  }
  Object createObject(std::shared_ptr<HostObject> ho) override {
    return intoJSIObject(vtable_->create_object_from_host_object(
        ctx_, new HostObjectWrapper(*this, std::move(ho))));
  }
  std::shared_ptr<HostObject> getHostObject(const Object &o) override {
    return static_cast<HostObjectWrapper *>(
               vtable_->get_host_object(ctx_, toABIObject(o)))
        ->getHostObject();
  }
  HostFunctionType &getHostFunction(const Function &f) override {
    return static_cast<HostFunctionWrapper *>(
               vtable_->get_host_function(ctx_, toABIFunction(f)))
        ->getHostFunction();
  }

  bool hasNativeState(const Object &obj) override {
    bool hasNS = vtable_->has_native_state(ctx_, toABIObject(obj));
    if (!hasNS)
      return false;

    auto *ns = vtable_->get_native_state(ctx_, toABIObject(obj));
    return ns->vtable == &NativeStateWrapper::vt;
  }
  std::shared_ptr<NativeState> getNativeState(const Object &obj) override {
    auto *ns = vtable_->get_native_state(ctx_, toABIObject(obj));
    return static_cast<NativeStateWrapper *>(ns)->getNativeState();
  }
  void setNativeState(const Object &obj, std::shared_ptr<NativeState> state)
      override {
    unwrap(vtable_->set_native_state(
        ctx_, toABIObject(obj), new NativeStateWrapper(std::move(state))));
  }

  Value getProperty(const Object &obj, const PropNameID &name) override {
    return intoJSIValue(vtable_->get_object_property_from_prop_name_id(
        ctx_, toABIObject(obj), toABIPropNameID(name)));
  }
  Value getProperty(const Object &obj, const String &name) override {
    return intoJSIValue(vtable_->get_object_property_from_string(
        ctx_, toABIObject(obj), toABIString(name)));
  }
  bool hasProperty(const Object &obj, const PropNameID &name) override {
    return unwrap(vtable_->has_object_property_from_prop_name_id(
        ctx_, toABIObject(obj), toABIPropNameID(name)));
  }
  bool hasProperty(const Object &obj, const String &name) override {
    return unwrap(vtable_->has_object_property_from_string(
        ctx_, toABIObject(obj), toABIString(name)));
  }
  void setPropertyValue(
      const Object &obj,
      const PropNameID &name,
      const Value &value) override {
    auto abiVal = toABIValue(value);
    unwrap(vtable_->set_object_property_from_prop_name_id(
        ctx_, toABIObject(obj), toABIPropNameID(name), &abiVal));
  }
  void setPropertyValue(
      const Object &obj,
      const String &name,
      const Value &value) override {
    auto abiVal = toABIValue(value);
    unwrap(vtable_->set_object_property_from_string(
        ctx_, toABIObject(obj), toABIString(name), &abiVal));
  }

  bool isArray(const Object &obj) const override {
    return vtable_->object_is_array(ctx_, toABIObject(obj));
  }
  bool isArrayBuffer(const Object &obj) const override {
    return vtable_->object_is_array_buffer(ctx_, toABIObject(obj));
  }
  bool isFunction(const Object &obj) const override {
    return vtable_->object_is_function(ctx_, toABIObject(obj));
  }
  bool isHostObject(const Object &obj) const override {
    // First check if it is considered a HostObject by the ABI.
    bool isHO = vtable_->object_is_host_object(ctx_, toABIObject(obj));
    if (!isHO)
      return false;

    // Now check if the HostObject was created by this C++ wrapper.
    auto ho = vtable_->get_host_object(ctx_, toABIObject(obj));
    return ho->vtable == &HostObjectWrapper::vt;
  }
  bool isHostFunction(const Function &fn) const override {
    // First check if it is considered a HostFunction by the ABI.
    bool isHF = vtable_->function_is_host_function(ctx_, toABIFunction(fn));
    if (!isHF)
      return false;
    // Now check if the HostFunction was created by this C++ wrapper.
    auto hf = vtable_->get_host_function(ctx_, toABIFunction(fn));
    return hf->vtable == &HostFunctionWrapper::vt;
  }
  Array getPropertyNames(const Object &obj) override {
    return intoJSIArray(
        vtable_->get_object_property_names(ctx_, toABIObject(obj)));
  }

  WeakObject createWeakObject(const Object &obj) override {
    return intoJSIWeakObject(
        vtable_->create_weak_object(ctx_, toABIObject(obj)));
  }
  Value lockWeakObject(const WeakObject &wo) override {
    return intoJSIValue(vtable_->lock_weak_object(ctx_, toABIWeakObject(wo)));
  }

  Array createArray(size_t length) override {
    return intoJSIArray(vtable_->create_array(ctx_, length));
  }
  ArrayBuffer createArrayBuffer(
      std::shared_ptr<MutableBuffer> buffer) override {
    return intoJSIArrayBuffer(
        vtable_->create_array_buffer_from_external_data(
            ctx_, new MutableBufferWrapper(std::move(buffer))));
  }
  size_t size(const Array &arr) override {
    return vtable_->get_array_length(ctx_, toABIArray(arr));
  }
  size_t size(const ArrayBuffer &ab) override {
    return unwrap(vtable_->get_array_buffer_size(ctx_, toABIArrayBuffer(ab)));
  }
  uint8_t *data(const ArrayBuffer &ab) override {
    return unwrap(vtable_->get_array_buffer_data(ctx_, toABIArrayBuffer(ab)));
  }
  Value getValueAtIndex(const Array &arr, size_t i) override {
    return intoJSIValue(
        vtable_->get_array_value_at_index(ctx_, toABIArray(arr), i));
  }
  void setValueAtIndexImpl(const Array &arr, size_t i, const Value &value)
      override {
    auto abiVal = toABIValue(value);
    unwrap(
        vtable_->set_array_value_at_index(ctx_, toABIArray(arr), i, &abiVal));
  }

  Function createFunctionFromHostFunction(
      const PropNameID &name,
      unsigned int paramCount,
      HostFunctionType func) override {
    return intoJSIFunction(vtable_->create_function_from_host_function(
        ctx_,
        toABIPropNameID(name),
        paramCount,
        new HostFunctionWrapper(*this, func)));
  }
  Value call(
      const Function &fn,
      const Value &jsThis,
      const Value *args,
      size_t count) override {
    std::vector<HermesABIValue> abiArgs;
    for (size_t i = 0; i < count; ++i)
      abiArgs.push_back(toABIValue(args[i]));
    HermesABIValue abiThis = toABIValue(jsThis);
    return intoJSIValue(vtable_->call(
        ctx_, toABIFunction(fn), &abiThis, abiArgs.data(), abiArgs.size()));
  }
  Value callAsConstructor(const Function &fn, const Value *args, size_t count)
      override {
    std::vector<HermesABIValue> abiArgs;
    for (size_t i = 0; i < count; ++i)
      abiArgs.push_back(toABIValue(args[i]));
    return intoJSIValue(vtable_->call_as_constructor(
        ctx_, toABIFunction(fn), abiArgs.data(), abiArgs.size()));
  }

  bool strictEquals(const Symbol &a, const Symbol &b) const override {
    return vtable_->strict_equals_symbol(ctx_, toABISymbol(a), toABISymbol(b));
  }
  bool strictEquals(const BigInt &a, const BigInt &b) const override {
    return vtable_->strict_equals_bigint(ctx_, toABIBigInt(a), toABIBigInt(b));
  }
  bool strictEquals(const String &a, const String &b) const override {
    return vtable_->strict_equals_string(ctx_, toABIString(a), toABIString(b));
  }
  bool strictEquals(const Object &a, const Object &b) const override {
    return vtable_->strict_equals_object(ctx_, toABIObject(a), toABIObject(b));
  }

  bool instanceOf(const Object &o, const Function &f) override {
    return unwrap(vtable_->instance_of(ctx_, toABIObject(o), toABIFunction(f)));
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
