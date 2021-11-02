#include <algorithm>
#include <atomic>
#include <climits> // INT_MAX
#include <cmath>
#include <vector>
#define NAPI_EXPERIMENTAL

#include "llvh/Support/Compiler.h"

#include "hermes.h"
#include "hermes/BCGen/HBC/BytecodeDataProvider.h"
#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/BCGen/HBC/BytecodeProviderFromSrc.h"
#include "hermes/VM/Runtime.h"

#include "hermes/Support/SimpleDiagHandler.h"

#include "hermes/SourceMap/SourceMapParser.h"

#include "hermes/VM/Callable.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/StringPrimitive.h"

#include "llvh/Support/ConvertUTF.h"

#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/PrimitiveBox.h"

#include "llvh/ADT/SmallSet.h"

#include "hermes_napi.h"

using ::hermes::hermesLog;

// Android OSS has a bug where exception data can get mangled when going via
// fbjni. This macro can be used to expose the root cause in adb log. It serves
// no purpose other than as a backup.
#ifdef __ANDROID__
#define LOG_EXCEPTION_CAUSE(...) hermesLog("HermesVM", __VA_ARGS__)
#else
#define LOG_EXCEPTION_CAUSE(...) \
  do {                           \
  } while (0)
#endif

#define STATUS_CALL(call)              \
  do {                                 \
    if (napi_status status = (call)) { \
      return status;                   \
    }                                  \
  } while (false)

#define RETURN_STATUS_IF_FALSE(condition, status) \
  do {                                            \
    if (!(condition)) {                           \
      return SetLastError((status));              \
    }                                             \
  } while (false)

#define CHECK_ENV(env)         \
  do {                         \
    if ((env) == nullptr) {    \
      return napi_invalid_arg; \
    }                          \
  } while (false)

#define CHECKED_ENV(env) \
  ((env) == nullptr)     \
      ? napi_invalid_arg \
      : reinterpret_cast<hermes::napi::NodeApiEnvironment *>(env)

#define CHECK_ARG(arg) \
  RETURN_STATUS_IF_FALSE(((arg) != nullptr), napi_invalid_arg)

#define CHECK_TYPED_ARG(arg, isCheck, status) \
  CHECK_ARG(arg);                             \
  RETURN_STATUS_IF_FALSE(phv(arg).isCheck(), status)

#define CHECK_OBJECT_ARG(arg) \
  CHECK_TYPED_ARG((arg), isObject, napi_object_expected)

#define CHECK_EXTERNAL_ARG(arg) \
  CHECK_ARG(arg);               \
  RETURN_STATUS_IF_FALSE(vm::vmisa<vm::HostObject>(phv(arg)), napi_invalid_arg)

//   napi_throw_type_error(
//       env, "ERR_NAPI_CONS_FUNCTION", "Constructor must be a function");

#define CHECK_FUNCTION_ARG(arg)                    \
  do {                                             \
    CHECK_OBJECT_ARG(arg);                         \
    if (vm::vmisa<vm::Callable>(phv(arg))) {       \
      return SetLastError(napi_function_expected); \
    }                                              \
  } while (false)

#define CHECK_STRING_ARG(arg) \
  CHECK_TYPED_ARG((arg), isString, napi_string_expected)

#define CHECK_NUMBER_ARG(arg) \
  CHECK_TYPED_ARG((arg), isNumber, napi_number_expected)

#define CHECK_BOOL_ARG(arg) \
  CHECK_TYPED_ARG((arg), isBool, napi_boolean_expected)

#define CHECK_STATUS(hermesStatus) STATUS_CALL(checkStatus(hermesStatus))

#define CONCAT_IMPL(left, right) left##right
#define CONCAT(left, right) CONCAT_IMPL(left, right)
#define TEMP_VARNAME(tempSuffix) CONCAT(temp_, tempSuffix)
#define ASSIGN_CHECKED_IMPL(var, expr, tempSuffix)    \
  auto TEMP_VARNAME(tempSuffix) = (expr);             \
  CHECK_STATUS(TEMP_VARNAME(tempSuffix).getStatus()); \
  var = *TEMP_VARNAME(tempSuffix);

#define ASSIGN_CHECKED(var, expr) ASSIGN_CHECKED_IMPL(var, expr, __COUNTER__)

namespace hermes {
namespace napi {

struct Marker {
  size_t ChunkIndex{0};
  size_t ItemIndex{0};

  bool IsValid() const {
    return ChunkIndex < std::numeric_limits<size_t>::max();
  }

  static const Marker InvalidMarker;
};

/*static*/ const Marker Marker::InvalidMarker{
    std::numeric_limits<size_t>::max(),
    0};

template <typename T>
struct NonMovableObjStack {
  NonMovableObjStack() {
    // There is always at least one chunk in the storage
    std::vector<T> firstChunk;
    firstChunk.reserve(ChunkSize);
    m_storage.push_back(std::move(firstChunk));
  }

  bool empty() const {
    return m_storage[0].empty();
  }

  template <typename... TArgs>
  void emplace_back(TArgs &&...args) {
    auto &storageChunk = m_storage.back();
    if (storageChunk.size() == storageChunk.capacity()) {
      std::vector<T> newChunk;
      newChunk.reserve(std::min(storageChunk.capacity() * 2, MaxChunkSize));
      m_storage.push_back(std::move(newChunk));
      storageChunk = m_storage.back();
    }
    storageChunk.emplace_back(std::forward<TArgs>(args)...);
  }

  T &back() {
    return m_storage.back().back();
  }

  bool pop_back() {
    auto &storageChunk = m_storage.back();
    if (storageChunk.empty()) {
      return false;
    }

    storageChunk.pop_back();
    if (storageChunk.empty() && m_storage.size() > 1) {
      m_storage.pop_back();
    }

    return true;
  }

  bool pop_marker(const Marker &marker) {
    if (marker.ChunkIndex > m_storage.size()) {
      return false; // Invalid ChunkIndex
    } else if (marker.ChunkIndex == m_storage.size()) {
      // ChunkIndex is valid only if ItemIndex is 0.
      // In that case we have nothing to remove.
      return marker.ItemIndex == 0;
    }

    auto &markerChunk = m_storage[marker.ChunkIndex];
    if (marker.ItemIndex >= markerChunk.size()) {
      return false; // Invalid ItemIndex
    }

    if (marker.ChunkIndex < m_storage.size() - 1) {
      // Delete the whole chunks
      m_storage.erase(
          m_storage.begin() + marker.ChunkIndex + 1, m_storage.end());
    }

    if (marker.ChunkIndex > 0 && marker.ItemIndex == 0) {
      // Delete the last chunk
      m_storage.erase(m_storage.begin() + marker.ChunkIndex, m_storage.end());
    } else {
      // Delete items in the marker chunk
      markerChunk.erase(
          markerChunk.begin() + marker.ItemIndex, markerChunk.end());
    }

    return true;
  }

  // New marker points to a location where to insert a new element.
  // Thus, it always points to an invalid location after the last element.
  Marker create_marker() {
    auto &lastChunk = m_storage.back();
    if (lastChunk.size() < lastChunk.capacity()) {
      return {m_storage.size() - 1, lastChunk.size()};
    } else {
      return {m_storage.size(), 0};
    }
  }

  Marker get_previous_marker(const Marker &marker) {
    if (marker.ItemIndex > 0) {
      return {marker.ChunkIndex, marker.ItemIndex - 1};
    } else if (marker.ChunkIndex > 0) {
      auto prevChunkIndex = marker.ChunkIndex - 1;
      if (m_storage[prevChunkIndex].size() > 0) {
        return {prevChunkIndex, m_storage[prevChunkIndex].size() - 1};
      }
    }
    return Marker::InvalidMarker;
  }

  T *at(const Marker &marker) {
    if (marker.ChunkIndex >= m_storage.size()) {
      return nullptr;
    }
    auto &chunk = m_storage[marker.ChunkIndex];
    if (marker.ItemIndex >= chunk.size()) {
      return nullptr;
    }
    return &chunk[marker.ItemIndex];
  }

  template <typename F>
  void for_each(const F &f) noexcept {
    for (const auto &storageChunk : m_storage) {
      for (const auto &item : storageChunk) {
        f(item);
      }
    }
  }

 private:
  static const size_t ChunkSize = 16;
  static const size_t MaxChunkSize = 4096;
  std::vector<std::vector<T>>
      m_storage; // There is always at least one chunk in the storage
};

struct NodeApiEnvironment;

enum class FinalizeReason {
  Destruction,
  EnvTeardown,
};

struct RefTracker {
  using RefList = RefTracker;

  RefTracker() = default;
  virtual ~RefTracker() = default;
  virtual void finalize(FinalizeReason reason) noexcept {}

  void link(RefList *list) noexcept {
    prev_ = list;
    next_ = list->next_;
    if (next_ != nullptr) {
      next_->prev_ = this;
    }
    list->next_ = this;
  }

  void unlink() noexcept {
    if (prev_ != nullptr) {
      prev_->next_ = next_;
    }
    if (next_ != nullptr) {
      next_->prev_ = prev_;
    }
    prev_ = nullptr;
    next_ = nullptr;
  }

  static void finalizeAll(RefList *list) noexcept {
    while (list->next_ != nullptr) {
      list->next_->finalize(FinalizeReason::EnvTeardown);
    }
  }

 private:
  RefList *next_{nullptr};
  RefList *prev_{nullptr};
};

napi_value napiValue(const vm::PinnedHermesValue *hv) noexcept {
  return reinterpret_cast<napi_value>(const_cast<vm::PinnedHermesValue *>(hv));
}

struct HFContext;

struct CallbackInfo final {
  CallbackInfo(HFContext &context, vm::NativeArgs &hvArgs) noexcept
      : context_(context), hvArgs_(hvArgs) {}

  void Args(napi_value *args, size_t *argCount) noexcept {
    *args = napiValue(hvArgs_.begin());
    *argCount = hvArgs_.getArgCount();
  }

  size_t ArgCount() noexcept {
    return hvArgs_.getArgCount();
  }

  napi_value This() noexcept {
    return napiValue(&hvArgs_.getThisArg());
  }

  void *Data() noexcept;

  napi_value GetNewTarget() noexcept {
    return napiValue(&hvArgs_.getNewTarget());
  }

 private:
  HFContext &context_;
  vm::NativeArgs &hvArgs_;
};

struct NodeApiEnvironment;

struct HFContext final {
  HFContext(NodeApiEnvironment &env, napi_callback hostCallback, void *data)
      : env_(env), hostCallback_(hostCallback), data_(data) {}

  static vm::CallResult<vm::HermesValue>
  func(void *context, vm::Runtime *runtime, vm::NativeArgs hvArgs);

  static void finalize(void *context) {
    delete reinterpret_cast<HFContext *>(context);
  }

  NodeApiEnvironment &env_;
  napi_callback hostCallback_;
  void *data_;
};

void *CallbackInfo::Data() noexcept {
  return context_.data_;
}

struct HermesBuffer;

enum class WrapType { Retrievable, Anonymous };

enum class UnwrapAction { KeepWrap, RemoveWrap };

enum class NapiPredefined {
  UndefinedValue,
  NullValue,
  TrueValue,
  FalseValue,
  WrapSymbol,
  WeakFinalizerSymbol,
  PredefinedCount // a special value that must be last in the enum
};

struct NodeApiEnvironment {
  explicit NodeApiEnvironment(
      const vm::RuntimeConfig &runtimeConfig = {}) noexcept;
  virtual ~NodeApiEnvironment();

  template <typename F>
  napi_status handleExceptions(const F &f) noexcept;

  vm::Handle<> toHandle(napi_value value) noexcept;

  // v8::Isolate* const isolate;  // Shortcut for context()->GetIsolate()
  // v8impl::Persistent<v8::Context> context_persistent;

  // inline v8::Local<v8::Context> context() const {
  //   return v8impl::PersistentToLocal::Strong(context_persistent);
  // }

  napi_status Ref() noexcept;
  napi_status Unref() noexcept;

  // virtual bool can_call_into_js() const { return true; }
  // virtual v8::Maybe<bool> mark_arraybuffer_as_untransferable(
  //     v8::Local<v8::ArrayBuffer> ab) const {
  //   return v8::Just(true);
  // }

  // static inline void
  // HandleThrow(napi_env env, v8::Local<v8::Value> value) {
  //   env->isolate->ThrowException(value);
  // }

  // template <typename T, typename U = decltype(HandleThrow)>
  // inline void CallIntoModule(T&& call, U&& handle_exception = HandleThrow)
  // {
  //   int open_handle_scopes_before = open_handle_scopes;
  //   int open_callback_scopes_before = open_callback_scopes;
  //   napi_clear_last_error(this);
  //   call(this);
  //   CHECK_EQ(open_handle_scopes, open_handle_scopes_before);
  //   CHECK_EQ(open_callback_scopes, open_callback_scopes_before);
  //   if (!last_exception.IsEmpty()) {
  //     handle_exception(this, last_exception.Get(this->isolate));
  //     last_exception.Reset();
  //   }
  // }

  // virtual void CallFinalizer(napi_finalize cb, void* data, void* hint) {
  //   v8::HandleScope handle_scope(isolate);
  //   CallIntoModule([&](napi_env env) {
  //     cb(env, data, hint);
  //   });
  // }

  // v8impl::Persistent<v8::Value> last_exception;

  // We store references in two different lists, depending on whether they
  // have `napi_finalizer` callbacks, because we must first finalize the ones
  // that have such a callback. See `~NodeApiEnvironment()` above for details.
  RefTracker::RefList refList_{};
  RefTracker::RefList finalizingRefList_{};
  napi_extended_error_info lastError_{};
  int openCallbackScopeCount_{};
  void *instanceData_{};

  static vm::PinnedHermesValue &phv(napi_value value) noexcept;
  static vm::Handle<vm::JSObject> toObjectHandle(napi_value value) noexcept;
  static vm::Handle<vm::JSArray> toArrayHandle(napi_value value) noexcept;
  static vm::Handle<vm::HermesValue> stringHandle(napi_value value) noexcept;
  static vm::Handle<vm::JSArray> arrayHandle(napi_value value) noexcept;
  vm::Handle<vm::HermesValue> toHandle(const vm::HermesValue &value) noexcept;

  napi_status SetLastError(
      napi_status error_code,
      uint32_t engine_error_code = 0,
      void *engine_reserved = nullptr) noexcept;
  napi_status ClearLastError() noexcept;

  napi_status GetLastErrorInfo(
      const napi_extended_error_info **result) noexcept;

  napi_status createFunction(
      const char *utf8Name,
      size_t length,
      napi_callback callback,
      void *callbackData,
      napi_value *result) noexcept;

  napi_status defineClass(
      const char *utf8Name,
      size_t length,
      napi_callback constructor,
      void *callbackData,
      size_t propertyCount,
      const napi_property_descriptor *properties,
      napi_value *result) noexcept;

  napi_status getPropertyNames(napi_value object, napi_value *result) noexcept;

  napi_status getAllPropertyNames(
      napi_value object,
      napi_key_collection_mode keyMode,
      napi_key_filter keyFilter,
      napi_key_conversion keyConversion,
      napi_value *result) noexcept;

  napi_status
  setProperty(napi_value object, napi_value key, napi_value value) noexcept;

  napi_status
  hasProperty(napi_value object, napi_value key, bool *result) noexcept;

  napi_status
  getProperty(napi_value object, napi_value key, napi_value *result) noexcept;

  napi_status
  deleteProperty(napi_value object, napi_value key, bool *result) noexcept;

  napi_status
  hasOwnProperty(napi_value object, napi_value key, bool *result) noexcept;

  napi_status setNamedProperty(
      napi_value object,
      const char *utf8Name,
      napi_value value) noexcept;

  napi_status hasNamedProperty(
      napi_value object,
      const char *utf8Name,
      bool *result) noexcept;

  napi_status getNamedProperty(
      napi_value object,
      const char *utf8Name,
      napi_value *result) noexcept;

  napi_status
  setElement(napi_value arr, uint32_t index, napi_value value) noexcept;

  napi_status hasElement(napi_value arr, uint32_t index, bool *result) noexcept;

  napi_status
  getElement(napi_value arr, uint32_t index, napi_value *result) noexcept;

  napi_status
  deleteElement(napi_value arr, uint32_t index, bool *result) noexcept;

  napi_status defineProperties(
      napi_value object,
      size_t propertyCount,
      const napi_property_descriptor *properties) noexcept;

  napi_status objectFreeze(napi_value object) noexcept;

  napi_status objectSeal(napi_value object) noexcept;

  napi_status isArray(napi_value value, bool *result) noexcept;

  napi_status getArrayLength(napi_value value, uint32_t *result) noexcept;

  napi_status
  strictEquals(napi_value lhs, napi_value rhs, bool *result) noexcept;

  napi_status getPrototype(napi_value object, napi_value *result) noexcept;

  napi_status createObject(napi_value *result) noexcept;

  napi_status createArray(napi_value *result) noexcept;

  napi_status createArray(size_t length, napi_value *result) noexcept;

  napi_status CreateStringLatin1(
      const char *str,
      size_t length,
      napi_value *result) noexcept;

  napi_status
  CreateStringUtf8(const char *str, size_t length, napi_value *result) noexcept;

  napi_status CreateStringUtf16(
      const char16_t *str,
      size_t length,
      napi_value *result) noexcept;

  napi_status CreateNumber(double value, napi_value *result) noexcept;

  napi_status CreateNumber(int32_t value, napi_value *result) noexcept;

  napi_status CreateNumber(uint32_t value, napi_value *result) noexcept;

  napi_status CreateNumber(int64_t value, napi_value *result) noexcept;

  napi_status GetBoolean(bool value, napi_value *result) noexcept;

  napi_status createSymbol(napi_value description, napi_value *result) noexcept;

  napi_status
  CreateError(napi_value code, napi_value msg, napi_value *result) noexcept;

  napi_status
  CreateTypeError(napi_value code, napi_value msg, napi_value *result) noexcept;

  napi_status CreateRangeError(
      napi_value code,
      napi_value msg,
      napi_value *result) noexcept;

  napi_status typeOf(napi_value value, napi_valuetype *result) noexcept;

  napi_status getUndefined(napi_value *result) noexcept;

  napi_status getNull(napi_value *result) noexcept;

  napi_status getCallbackInfo(
      CallbackInfo *callbackInfo,
      size_t *argCount,
      napi_value *args,
      napi_value *thisArg,
      void **data) noexcept;

  napi_status getNewTarget(
      CallbackInfo *callbackInfo,
      napi_value *result) noexcept;

  napi_status callFunction(
      napi_value object,
      napi_value func,
      size_t argCount,
      const napi_value *args,
      napi_value *result) noexcept;

  napi_status getGlobal(napi_value *result) noexcept;

  napi_status Throw(napi_value error) noexcept;

  napi_status ThrowError(const char *code, const char *msg) noexcept;

  napi_status ThrowTypeError(const char *code, const char *msg) noexcept;

  napi_status ThrowRangeError(const char *code, const char *msg) noexcept;

  napi_status IsError(napi_value value, bool *result) noexcept;

  napi_status GetNumberValue(napi_value value, double *result) noexcept;

  napi_status GetNumberValue(napi_value value, int32_t *result) noexcept;

  napi_status GetNumberValue(napi_value value, uint32_t *result) noexcept;

  napi_status GetNumberValue(napi_value value, int64_t *result) noexcept;

  napi_status GetBoolValue(napi_value value, bool *result) noexcept;

  napi_status GetValueStringLatin1(
      napi_value value,
      char *buf,
      size_t bufsize,
      size_t *result) noexcept;

  napi_status GetValueStringUtf8(
      napi_value value,
      char *buf,
      size_t bufsize,
      size_t *result) noexcept;

  napi_status GetValueStringUtf16(
      napi_value value,
      char16_t *buf,
      size_t bufsize,
      size_t *result) noexcept;

  napi_status coerceToBool(napi_value value, napi_value *result) noexcept;
  napi_status coerceToNumber(napi_value value, napi_value *result) noexcept;
  napi_status coerceToObject(napi_value value, napi_value *result) noexcept;
  napi_status coerceToString(napi_value value, napi_value *result) noexcept;

  napi_status wrap(
      napi_value object,
      void *nativeObject,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_ref *result) noexcept;

  napi_status unwrap(napi_value object, void **result) noexcept;

  napi_status removeWrap(napi_value object, void **result) noexcept;

  napi_status createExternal(
      void *data,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_value *result) noexcept;

  napi_status typeTagObject(
      napi_value object,
      const napi_type_tag *typeTag) noexcept;

  napi_status checkObjectTypeTag(
      napi_value object,
      const napi_type_tag *typeTag,
      bool *result) noexcept;

  napi_status getValueExternal(napi_value value, void **result) noexcept;

  napi_status createReference(
      napi_value value,
      uint32_t initialRefCount,
      napi_ref *result) noexcept;

  napi_status deleteReference(napi_ref ref) noexcept;

  napi_status referenceRef(napi_ref ref, uint32_t *result) noexcept;

  napi_status referenceUnref(napi_ref ref, uint32_t *result) noexcept;

  napi_status getReferenceValue(napi_ref ref, napi_value *result) noexcept;

  napi_status OpenHandleScope(napi_handle_scope *result) noexcept;

  napi_status CloseHandleScope(napi_handle_scope scope) noexcept;

  napi_status OpenEscapableHandleScope(
      napi_escapable_handle_scope *result) noexcept;

  napi_status CloseEscapableHandleScope(
      napi_escapable_handle_scope scope) noexcept;

  napi_status EscapeHandle(
      napi_escapable_handle_scope scope,
      napi_value escapee,
      napi_value *result) noexcept;

  napi_status NewInstance(
      napi_value constructor,
      size_t argc,
      const napi_value *argv,
      napi_value *result) noexcept;

  napi_status
  InstanceOf(napi_value object, napi_value constructor, bool *result) noexcept;

  napi_status IsExceptionPending(bool *result) noexcept;

  napi_status GetAndClearLastException(napi_value *result) noexcept;

  napi_status isArrayBuffer(napi_value value, bool *result) noexcept;

  napi_status CreateArrayBuffer(
      size_t byte_length,
      void **data,
      napi_value *result) noexcept;

  napi_status CreateExternalArrayBuffer(
      void *external_data,
      size_t byte_length,
      napi_finalize finalize_cb,
      void *finalize_hint,
      napi_value *result) noexcept;

  napi_status GetArrayBufferInfo(
      napi_value arraybuffer,
      void **data,
      size_t *byte_length) noexcept;

  napi_status IsTypedArray(napi_value value, bool *result) noexcept;

  napi_status CreateTypedArray(
      napi_typedarray_type type,
      size_t length,
      napi_value arraybuffer,
      size_t byte_offset,
      napi_value *result) noexcept;

  napi_status GetTypedArrayInfo(
      napi_value typedarray,
      napi_typedarray_type *type,
      size_t *length,
      void **data,
      napi_value *arraybuffer,
      size_t *byte_offset) noexcept;

  napi_status CreateDataView(
      size_t byte_length,
      napi_value arraybuffer,
      size_t byte_offset,
      napi_value *result) noexcept;

  napi_status IsDataView(napi_value value, bool *result) noexcept;

  napi_status GetDataViewInfo(
      napi_value dataview,
      size_t *byte_length,
      void **data,
      napi_value *arraybuffer,
      size_t *byte_offset) noexcept;

  napi_status GetVersion(uint32_t *result) noexcept;

  napi_status CreatePromise(
      napi_deferred *deferred,
      napi_value *promise) noexcept;

  napi_status ResolveDeferred(
      napi_deferred deferred,
      napi_value resolution) noexcept;

  napi_status RejectDeferred(
      napi_deferred deferred,
      napi_value resolution) noexcept;

  napi_status IsPromise(napi_value value, bool *is_promise) noexcept;

  napi_status CreateDate(double time, napi_value *result) noexcept;

  napi_status IsDate(napi_value value, bool *is_date) noexcept;

  napi_status GetDateValue(napi_value value, double *result) noexcept;

  napi_status addFinalizer(
      napi_value object,
      void *nativeObject,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_ref *result) noexcept;

  napi_status AdjustExternalMemory(
      int64_t change_in_bytes,
      int64_t *adjusted_value) noexcept;

  napi_status SetInstanceData(
      void *data,
      napi_finalize finalize_cb,
      void *finalize_hint) noexcept;
  napi_status GetInstanceData(void **data) noexcept;
  napi_status DetachArrayBuffer(napi_value arraybuffer) noexcept;
  napi_status IsDetachedArrayBuffer(
      napi_value arraybuffer,
      bool *result) noexcept;

  // Extensions
  napi_status runScript(
      napi_value source,
      const char *sourceURL,
      napi_value *result) noexcept;

  napi_status runSerializedScript(
      const uint8_t *buffer,
      size_t bufferLength,
      napi_value source,
      const char *sourceURL,
      napi_value *result) noexcept;

  napi_status serializeScript(
      napi_value source,
      const char *sourceURL,
      napi_ext_buffer_callback bufferCallback,
      void *bufferHint) noexcept;

  napi_status runScriptWithSourceMap(
      std::unique_ptr<HermesBuffer> script,
      std::unique_ptr<HermesBuffer> sourceMap,
      const char *sourceURL,
      napi_value *result) noexcept;

  napi_status prepareScriptWithSourceMap(
      std::unique_ptr<HermesBuffer> script,
      std::unique_ptr<HermesBuffer> sourceMap,
      const char *sourceURL,
      napi_ext_prepared_script *preparedScript) noexcept;

  napi_status runPreparedScript(
      napi_ext_prepared_script preparedScript,
      napi_value *result) noexcept;

  napi_status deletePreparedScript(
      napi_ext_prepared_script preparedScript) noexcept;

  napi_status serializePreparedScript(
      napi_ext_prepared_script preparedScript,
      napi_ext_buffer_callback bufferCallback,
      void *bufferHint) noexcept;

  // Utility
  vm::CallResult<vm::HermesValue> stringHVFromAscii(
      const char *str,
      size_t length) noexcept;
  vm::CallResult<vm::HermesValue> stringHVFromLatin1(
      const char *str,
      size_t length) noexcept;
  vm::CallResult<vm::HermesValue> stringHVFromUtf8(
      const uint8_t *utf8,
      size_t length) noexcept;
  vm::CallResult<vm::HermesValue> stringHVFromUtf8(const char *utf8) noexcept;
  napi_value addStackValue(vm::HermesValue value) noexcept;
  napi_status checkStatus(vm::ExecutionStatus status) noexcept;

  napi_status newFunction(
      vm::SymbolID name,
      napi_callback callback,
      void *callbackData,
      napi_value *result) noexcept;

  static bool isHermesBytecode(const uint8_t *data, size_t len) noexcept;

  napi_status symbolIDFromPropertyDescriptor(
      const napi_property_descriptor *p,
      vm::MutableHandle<vm::SymbolID> *result) noexcept;

  template <WrapType wrapType>
  napi_status wrapObject(
      napi_value object,
      void *nativeObject,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_ref *result) noexcept;
  napi_status
  unwrapObject(napi_value object, UnwrapAction action, void **result) noexcept;

  const vm::PinnedHermesValue &getPredefined(
      NapiPredefined predefinedKey) noexcept;

  vm::CallResult<vm::PseudoHandle<>> getPrivate(
      vm::Handle<vm::JSObject> obj,
      NapiPredefined key) noexcept;

  vm::CallResult<bool> setPrivate(
      vm::Handle<vm::JSObject> obj,
      vm::SymbolID propID,
      vm::HermesValue value) noexcept;

  vm::CallResult<bool> deletePrivate(
      vm::Handle<vm::JSObject> obj,
      NapiPredefined key) noexcept;

  // TODO:
  // vm::CallResult<bool> addWeakFinalizer(
  //     vm::Handle<vm::JSObject> object,
  //     IWeakFinalizer *weakFinalizer) noexcept;

  // vm::CallResult<bool> removeWeakFinalizer(
  //     vm::Handle<vm::JSObject> object,
  //     IWeakFinalizer *weakFinalizer) noexcept;

 public:
#ifdef HERMESJSI_ON_STACK
  StackRuntime stackRuntime_;
#else
  std::shared_ptr<vm::Runtime> rt_;
#endif
  vm::Runtime &runtime_;
#ifdef HERMES_ENABLE_DEBUGGER
  friend class debugger::Debugger;
  std::unique_ptr<debugger::Debugger> debugger_;
#endif
  vm::experiments::VMExperimentFlags vmExperimentFlags_{0};
  std::shared_ptr<vm::CrashManager> crashMgr_;

  /// Compilation flags used by prepareJavaScript().
  hbc::CompileFlags compileFlags_{};
  /// The default setting of "emit async break check" in this runtime.
  bool defaultEmitAsyncBreakCheck_{false};

  std::atomic<int> m_refs{1};

  // TODO: use it as a GC root
  vm::PinnedHermesValue lastException_{EmptyHermesValue};
  std::array<
      vm::PinnedHermesValue,
      static_cast<size_t>(NapiPredefined::PredefinedCount)>
      predefinedValues_{};

 public:
  NonMovableObjStack<vm::PinnedHermesValue> m_stackValues;
  NonMovableObjStack<Marker> m_stackMarkers;
  static constexpr uint32_t kEscapeableSentinelNativeValue = 0x35456789;
  static constexpr uint32_t kUsedEscapeableSentinelNativeValue =
      kEscapeableSentinelNativeValue + 1;
  static constexpr vm::HermesValue EmptyHermesValue{
      vm::HermesValue::encodeEmptyValue()};
};

struct HermesBuffer : Buffer {
  HermesBuffer(
      napi_env env,
      napi_ext_buffer buffer,
      napi_ext_get_buffer_range getBufferRange,
      napi_ext_delete_buffer deleteBuffer) noexcept
      : env_(env), buffer_(buffer), deleteBuffer_(deleteBuffer) {
    getBufferRange(env, buffer, &data_, &size_);
  }

  ~HermesBuffer() noexcept {
    if (buffer_ && deleteBuffer_) {
      deleteBuffer_(env_, buffer_);
    }
  }

 private:
  napi_env env_;
  napi_ext_buffer buffer_;
  napi_ext_delete_buffer deleteBuffer_;
};

std::unique_ptr<HermesBuffer> makeHermesBuffer(
    napi_env env,
    napi_ext_buffer buffer,
    napi_ext_get_buffer_range getBufferRange,
    napi_ext_delete_buffer deleteBuffer) noexcept {
  return buffer ? std::make_unique<HermesBuffer>(
                      env, buffer, getBufferRange, deleteBuffer)
                : nullptr;
}

/// An implementation of PreparedJavaScript that wraps a BytecodeProvider.
struct HermesPreparedJavaScript {
  explicit HermesPreparedJavaScript(
      std::unique_ptr<hbc::BCProvider> bcProvider,
      vm::RuntimeModuleFlags runtimeFlags,
      std::string sourceURL,
      bool isBytecode)
      : bcProvider_(std::move(bcProvider)),
        runtimeFlags_(runtimeFlags),
        sourceURL_(std::move(sourceURL)),
        isBytecode_(isBytecode) {}

  std::shared_ptr<hbc::BCProvider> bytecodeProvider() const {
    return bcProvider_;
  }

  vm::RuntimeModuleFlags runtimeFlags() const {
    return runtimeFlags_;
  }

  const std::string &sourceURL() const {
    return sourceURL_;
  }

  bool isBytecode() const {
    return isBytecode_;
  }

 private:
  std::shared_ptr<hbc::BCProvider> bcProvider_;
  vm::RuntimeModuleFlags runtimeFlags_;
  std::string sourceURL_;
  bool isBytecode_{false};
};

// Reference counter implementation.
struct ExtRefCounter : RefTracker {
  ~ExtRefCounter() override {
    unlink();
  }

  void ref() noexcept {
    ++refCount_;
  }

  void unref() noexcept {
    if (--refCount_ == 0) {
      finalize(FinalizeReason::Destruction);
    }
  }

  virtual vm::PinnedHermesValue *get() = 0;

 protected:
  ExtRefCounter(NodeApiEnvironment *env) noexcept {
    link(&env->refList_);
  }

  void finalize(FinalizeReason reason) noexcept override {
    delete this;
  }

 private:
  uint32_t refCount_{1};
};
#if 0
// Wrapper around vm::PinnedHermesValue that implements reference counting.
struct ExtReference : ExtRefCounter {
  static ExtReference *create(vm::PinnedHermesValue value) noexcept {
    return new ExtReference(value);
  }

  vm::PinnedHermesValue *get() override {
    return &value_;
  }

 protected:
  ExtReference(vm::PinnedHermesValue value) noexcept : value_(value) {}

 private:
  vm::PinnedHermesValue value_;
};

// Associates data with ExtReference.
struct ExtReferenceWithData : ExtReference {
  static ExtReferenceWithData *create(
      NodeApiEnvironment *env,
      vm::PinnedHermesValue value,
      void *data,
      napi_finalize finalizeCallback,
      void *finalizeHint) {
    return new ExtReferenceWithData(
        env, value, data, finalizeCallback, finalizeHint);
  }

 protected:
  ExtReferenceWithData(
      NodeApiEnvironment *env,
      vm::PinnedHermesValue value,
      void *data,
      napi_finalize finalizeCallback,
      void *finalizeHint) noexcept
      : ExtReference(value),
        env_(env),
        data_(data),
        finalizeCallback_(finalizeCallback),
        finalizeHint_(finalizeHint) {}

  void finalize(FinalizeReason reason) override {
    if (finalizeCallback_) {
      finalizeCallback_(reinterpret_cast<napi_env>(env_), data_, finalizeHint_);
      finalizeCallback_ = nullptr;
    }
    ExtReference::Finalize(is_env_teardown);
  }

 private:
  NodeApiEnvironment *env_{};
  void *data_{};
  napi_finalize finalizeCallback_{};
  void *finalizeHint_{};
};

// Wrapper around vm::WeakRef<vm::HermesValue> that implements reference
// counting.
struct ExtWeakReference : protected ExtRefCounter {
  static ExtWeakReference *create(vm::WeakRef<vm::HermesValue> value) noexcept {
    return new ExtWeakReference(value);
  }

  ~ExtWeakReference() override {
    napi_delete_reference(env_, weakRef_);
  }

  v8::Local<v8::Value> Get(napi_env env) override {
    napi_value result{};
    napi_get_reference_value(env, weak_ref_, &result);
    return result ? v8impl::V8LocalValueFromJsValue(result)
                  : v8::Local<v8::Value>();
  }

 protected:
  ExtWeakReference(napi_env env, v8::Local<v8::Value> value)
      : ExtRefCounter(env), env_{env} {
    napi_create_reference(
        env, v8impl::JsValueFromV8LocalValue(value), 0, &weak_ref_);
  }

 private:
  napi_env env_{nullptr};
  napi_ref weakRef_{nullptr};
};
#endif
// Adapter for napi_finalize callbacks.
struct Finalizer {
  // Some Finalizers are run during shutdown when the napi_env is destroyed,
  // and some need to keep an explicit reference to the napi_env because they
  // are run independently.
  enum class EnvReferenceMode { NoEnvReference, KeepEnvReference };

 protected:
  Finalizer(
      NodeApiEnvironment *env,
      napi_finalize finalizeCallback,
      void *finalizeData,
      void *finalizeHint,
      EnvReferenceMode refMode = EnvReferenceMode::NoEnvReference)
      : env_(env),
        finalizeCallback_(finalizeCallback),
        finalizeData_(finalizeData),
        finalizeHint_(finalizeHint),
        hasEnvReference_(refMode == EnvReferenceMode::KeepEnvReference) {
    if (hasEnvReference_)
      env_->Ref();
  }

  ~Finalizer() {
    if (hasEnvReference_) {
      env_->Unref();
    }
  }

 public:
  static Finalizer *create(
      NodeApiEnvironment *env,
      napi_finalize finalizeCallback = nullptr,
      void *finalizeData = nullptr,
      void *finalizeHint = nullptr,
      EnvReferenceMode refMode = EnvReferenceMode::NoEnvReference) {
    return new Finalizer(
        env, finalizeCallback, finalizeData, finalizeHint, refMode);
  }

  static void destroy(Finalizer *finalizer) {
    delete finalizer;
  }

 protected:
  NodeApiEnvironment *env_;
  napi_finalize finalizeCallback_;
  void *finalizeData_;
  void *finalizeHint_;
  bool finalizeRan_ = false;
  bool hasEnvReference_ = false;
};

// Wrapper around v8impl::Persistent that implements reference counting.
struct RefBase : protected Finalizer, RefTracker {
 protected:
  RefBase(
      NodeApiEnvironment *env,
      uint32_t initialRefCount,
      bool deleteSelf,
      napi_finalize finalizeCallback,
      void *finalizeData,
      void *finalizeHint)
      : Finalizer(env, finalizeCallback, finalizeData, finalizeHint),
        refCount_(initialRefCount),
        deleteSelf_(deleteSelf) {
    // TODO: it must be called from NodeApiEnvironment
    link(finalizeCallback == nullptr ? &env->refList_ : &env->finalizingRefList_);
  }

 public:
  static RefBase *create(
      NodeApiEnvironment *env,
      uint32_t initialRefCount,
      bool deleteSelf,
      napi_finalize finalizeCallback,
      void *finalizeData,
      void *finalizeHint) noexcept {
    return new RefBase(
        env,
        initialRefCount,
        deleteSelf,
        finalizeCallback,
        finalizeData,
        finalizeHint);
  }

  virtual ~RefBase() {
    unlink();
  }

  void *data() noexcept {
    return finalizeData_;
  }

  // Destroy is called in 2 ways. Either from the finalizer or
  // from one of Unwrap or napi_delete_reference.
  //
  // When it is called from Unwrap or napi_delete_reference we only
  // want to do the delete if the finalizer has already run or
  // cannot have been queued to run (ie the reference count is > 0),
  // otherwise we may crash when the finalizer does run.
  // If the finalizer may have been queued and has not already run
  // delay the delete until the finalizer runs by not doing the delete
  // and setting _delete_self to true so that the finalizer will
  // delete it when it runs.
  //
  // The second way this is called is from
  // the finalizer and _delete_self is set. In this case we
  // know we need to do the deletion so just do it.
  static void destroy(RefBase *reference) noexcept {
    if ((reference->refCount() != 0) || (reference->deleteSelf_) ||
        (reference->finalizeRan_)) {
      delete reference;
    } else {
      // defer until finalizer runs as
      // it may already be queued
      reference->deleteSelf_ = true;
    }
  }

  uint32_t ref() noexcept {
    return ++refCount_;
  }

  uint32_t unref() noexcept {
    if (refCount_ == 0) {
      return 0;
    }
    return --refCount_;
  }

  uint32_t refCount() noexcept {
    return refCount_;
  }

 protected:
  void finalize(FinalizeReason reason) noexcept override {
    // In addition to being called during environment teardown, this method is
    // also the entry point for the garbage collector. During environment
    // teardown we have to remove the garbage collector's reference to this
    // method so that, if, as part of the user's callback, JS gets executed,
    // resulting in a garbage collection pass, this method is not re-entered as
    // part of that pass, because that'll cause a double free (as seen in
    // https://github.com/nodejs/node/issues/37236).
    //
    // Since this class does not have access to the V8 persistent reference,
    // this method is overridden in the `Reference` class below. Therein the
    // weak callback is removed, ensuring that the garbage collector does not
    // re-enter this method, and the method chains up to continue the process of
    // environment-teardown-induced finalization.

    // During environment teardown we have to convert a strong reference to
    // a weak reference to force the deferring behavior if the user's finalizer
    // happens to delete this reference so that the code in this function that
    // follows the call to the user's finalizer may safely access variables from
    // this instance.
    if (reason == FinalizeReason::EnvTeardown && refCount() > 0) {
      refCount_ = 0;
    }

    if (finalizeCallback_ != nullptr) {
      // This ensures that we never call the finalizer twice.
      napi_finalize fini = finalizeCallback_;
      finalizeCallback_ = nullptr;
      // TODO: [vmoroz] Implement
      //_env->CallFinalizer(fini, _finalize_data, _finalize_hint);
    }

    // this is safe because if a request to delete the reference
    // is made in the finalize_callback it will defer deletion
    // to this block and set _delete_self to true
    if (deleteSelf_ || reason == FinalizeReason::EnvTeardown) {
      destroy(this);
    } else {
      finalizeRan_ = true;
    }
  }

 private:
  uint32_t refCount_;
  bool deleteSelf_;
};

struct Reference final : RefBase {
  static Reference *create(
      NodeApiEnvironment *env,
      vm::PinnedHermesValue value,
      uint32_t initialRefCount,
      bool deleteSelf,
      napi_finalize finalizeCallback = nullptr,
      void *finalizeData = nullptr,
      void *finalizeHint = nullptr) noexcept {
    return new Reference(
        env,
        value,
        initialRefCount,
        deleteSelf,
        finalizeCallback,
        finalizeData,
        finalizeHint);
  }

  uint32_t ref() noexcept {
    uint32_t refCount = RefBase::ref();
    if (refCount == 1) {
      // TODO:
      // env_.removeWeakRef(weakRef_);
      // weakRef_ = nullptr;
    }
    return refCount;
  }

  uint32_t unref() noexcept {
    uint32_t oldRefCount = refCount();
    uint32_t refCount = RefBase::unref();
    if (oldRefCount == 1 && refCount == 0) {
      // TODO:
      // weakRef_ = env->addWeakRef(this, finalizeCallback, value_);
    }
    return refCount;
  }

  vm::PinnedHermesValue &get() noexcept {
    return value_;
  }

 protected:
  template <typename... TArgs>
  Reference(
      NodeApiEnvironment *env,
      vm::PinnedHermesValue value,
      TArgs &&...args) noexcept
      : RefBase(env, std::forward<TArgs>(args)...), value_(value) {
    if (refCount() == 0) {
      // TODO:
      // weakRef_ = env->addWeakRef(this, finalizeCallback, value_);
    }
  }

  void finalize(FinalizeReason reason) noexcept override {
    // During env teardown, `~napi_env()` alone is responsible for finalizing.
    // Thus, we don't want any stray gc passes to trigger a second call to
    // `finalize()`, so let's reset the persistent here if nothing is
    // keeping it alive.
    // TODO:
    // if (isEnvTeardown && weakRef_) {
    // env_->removeWeakRef(weakRef_);
    // weakRef_ = nullptr;
    //}

    // Chain up to perform the rest of the finalization.
    RefBase::finalize(reason);
  }

 private:
  // TODO: Allow running finalizers when the GC in a good state - add a second
  // GC pass.
  static void finalizeCallback(void *data) noexcept {
    // Reference *reference = static_cast<Reference *>(data);
    // reference->finalize();
  }

 private:
  vm::PinnedHermesValue value_;
};

/*static*/ vm::CallResult<vm::HermesValue>
HFContext::func(void *context, vm::Runtime *runtime, vm::NativeArgs hvArgs) {
  HFContext *hfc = reinterpret_cast<HFContext *>(context);
  NodeApiEnvironment &env = hfc->env_;
  assert(runtime == &env.runtime_);
  auto &stats = env.runtime_.getRuntimeStats();
  const vm::instrumentation::RAIITimer timer{
      "Host Function", stats, stats.hostFunction};

  CallbackInfo callbackInfo{*hfc, hvArgs};
  auto result = hfc->hostCallback_(
      reinterpret_cast<napi_env>(&env),
      reinterpret_cast<napi_callback_info>(&callbackInfo));
  return env.phv(result);
  // TODO: handle errors
  // TODO: Add call in module
}

struct DataFinalizer {
  using Callback = void(void *data);
  void *data;
  Callback *callback;
};

struct NapiHostObjectProxy final : vm::HostObjectProxy {
  NapiHostObjectProxy(
      NodeApiEnvironment &env,
      void *data,
      napi_finalize finalizeCallback,
      void *finalizeHint) noexcept
      : env_(env),
        data_(data),
        finalizeCallback_(finalizeCallback),
        finalizeHint_(finalizeHint) {}

  ~NapiHostObjectProxy() override {
    if (finalizeCallback_) {
      finalizeCallback_(
          reinterpret_cast<napi_env>(&env_), data_, finalizeHint_);
    }
  }

  void *data() noexcept {
    return data_;
  }

  vm::CallResult<vm::HermesValue> get(vm::SymbolID /*id*/) override {
    return env_.runtime_.setThrownValue(vm::HermesValue::encodeNullValue());
    // TODO: create the error object
    //    hvFromValue(
    // rt_.global()
    //     .getPropertyAsFunction(rt_, "Error")
    //     .call(
    //         rt_,
    //         "Exception in HostObject::get: for prop '" +
    //             rt_.runtime_.getIdentifierTable().convertSymbolToUTF8(
    //                 id) +
    //             "': <unknown exception>")));
  }

  vm::CallResult<bool> set(vm::SymbolID /*id*/, vm::HermesValue /*value*/)
      override {
    return env_.runtime_.setThrownValue(vm::HermesValue::encodeNullValue());
    // TODO: create the error object
    //    hvFromValue(
    // rt_.global()
    //     .getPropertyAsFunction(rt_, "Error")
    //     .call(
    //         rt_,
    //         "Exception in HostObject::get: for prop '" +
    //             rt_.runtime_.getIdentifierTable().convertSymbolToUTF8(
    //                 id) +
    //             "': <unknown exception>")));
  }

  vm::CallResult<vm::Handle<vm::JSArray>> getHostPropertyNames() override {
    return env_.runtime_.setThrownValue(vm::HermesValue::encodeNullValue());
    // TODO: create the error object
    //    hvFromValue(
    // rt_.global()
    //     .getPropertyAsFunction(rt_, "Error")
    //     .call(
    //         rt_,
    //         "Exception in HostObject::get: for prop '" +
    //             rt_.runtime_.getIdentifierTable().convertSymbolToUTF8(
    //                 id) +
    //             "': <unknown exception>")));
  }

 private:
  NodeApiEnvironment &env_;
  void *data_;
  napi_finalize finalizeCallback_;
  void *finalizeHint_;
};

//=============================================================================
// NodeApiEnvironment implementation
//=============================================================================

namespace {
// Max size of the runtime's register stack.
// The runtime register stack needs to be small enough to be allocated on the
// native thread stack in Android (1MiB) and on MacOS's thread stack (512 KiB)
// Calculated by: (thread stack size - size of runtime -
// 8 memory pages for other stuff in the thread)
static constexpr unsigned kMaxNumRegisters =
    (512 * 1024 - sizeof(vm::Runtime) - 4096 * 8) /
    sizeof(vm::PinnedHermesValue);
} // namespace

NodeApiEnvironment::NodeApiEnvironment(
    const vm::RuntimeConfig &runtimeConfig) noexcept
    :
// TODO: pass parameters
#ifdef HERMESJSI_ON_STACK
      stackRuntime_(runtimeConfig),
      runtime_(stackRuntime_.getRuntime()),
#else
      rt_(vm::Runtime::create(runtimeConfig.rebuild()
                                  .withRegisterStack(nullptr)
                                  .withMaxNumRegisters(kMaxNumRegisters)
                                  .build())),
      runtime_(*rt_),
#endif
      vmExperimentFlags_(runtimeConfig.getVMExperimentFlags()),
      crashMgr_(runtimeConfig.getCrashMgr()) {
  compileFlags_.optimize = false;
#ifdef HERMES_ENABLE_DEBUGGER
  compileFlags_.debug = true;
#endif

  switch (runtimeConfig.getCompilationMode()) {
    case vm::SmartCompilation:
      compileFlags_.lazy = true;
      // (Leaves thresholds at default values)
      break;
    case vm::ForceEagerCompilation:
      compileFlags_.lazy = false;
      break;
    case vm::ForceLazyCompilation:
      compileFlags_.lazy = true;
      compileFlags_.preemptiveFileCompilationThreshold = 0;
      compileFlags_.preemptiveFunctionCompilationThreshold = 0;
      break;
  }

  compileFlags_.enableGenerator = runtimeConfig.getEnableGenerator();
  compileFlags_.emitAsyncBreakCheck = defaultEmitAsyncBreakCheck_ =
      runtimeConfig.getAsyncBreakCheckInEval();

#ifndef HERMESJSI_ON_STACK
  // Register the memory for the runtime if it isn't stored on the stack.
  crashMgr_->registerMemory(&runtime_, sizeof(vm::Runtime));
#endif
  runtime_.addCustomRootsFunction([this](vm::GC *, vm::RootAcceptor &acceptor) {
    m_stackValues.for_each([&](const vm::PinnedHermesValue &phv) {
      acceptor.accept(const_cast<vm::PinnedHermesValue &>(phv));
    });
    // for (auto it = hermesValues_->begin(); it != hermesValues_->end();) {
    //   if (it->get() == 0) {
    //     it = hermesValues_->erase(it);
    //   } else {
    //     acceptor.accept(const_cast<vm::PinnedHermesValue &>(it->phv));
    //     ++it;
    //   }
    // }
  });
  // runtime_.addCustomWeakRootsFunction(
  //     [this](vm::GC *, vm::WeakRefAcceptor &acceptor) {
  //       for (auto it = weakHermesValues_->begin();
  //            it != weakHermesValues_->end();) {
  //         if (it->get() == 0) {
  //           it = weakHermesValues_->erase(it);
  //         } else {
  //           acceptor.accept(it->wr);
  //           ++it;
  //         }
  //       }
  //     });
  // runtime_.addCustomSnapshotFunction(
  //     [this](vm::HeapSnapshot &snap) {
  //       snap.beginNode();
  //       snap.endNode(
  //           vm::HeapSnapshot::NodeType::Native,
  //           "ManagedValues",
  //           vm::GCBase::IDTracker::reserved(
  //               vm::GCBase::IDTracker::ReservedObjectID::
  //                   JSIHermesValueList),
  //           hermesValues_->size() * sizeof(HermesPointerValue),
  //           0);
  //       snap.beginNode();
  //       snap.endNode(
  //           vm::HeapSnapshot::NodeType::Native,
  //           "ManagedValues",
  //           vm::GCBase::IDTracker::reserved(
  //               vm::GCBase::IDTracker::ReservedObjectID::
  //                   JSIWeakHermesValueList),
  //           weakHermesValues_->size() * sizeof(WeakRefPointerValue),
  //           0);
  //     },
  //     [](vm::HeapSnapshot &snap) {
  //       snap.addNamedEdge(
  //           vm::HeapSnapshot::EdgeType::Internal,
  //           "hermesValues",
  //           vm::GCBase::IDTracker::reserved(
  //               vm::GCBase::IDTracker::ReservedObjectID::
  //                   JSIHermesValueList));
  //       snap.addNamedEdge(
  //           vm::HeapSnapshot::EdgeType::Internal,
  //           "weakHermesValues",
  //           vm::GCBase::IDTracker::reserved(
  //               vm::GCBase::IDTracker::ReservedObjectID::
  //                   JSIWeakHermesValueList));
  //     });

  vm::GCScope gcScope(&runtime_);
  auto setPredefined = [this](
                           NapiPredefined key, vm::HermesValue value) noexcept {
    predefinedValues_[static_cast<size_t>(key)] = value;
  };
  setPredefined(
      NapiPredefined::UndefinedValue, vm::HermesValue::encodeUndefinedValue());
  setPredefined(NapiPredefined::NullValue, vm::HermesValue::encodeNullValue());
  setPredefined(
      NapiPredefined::TrueValue, vm::HermesValue::encodeBoolValue(true));
  setPredefined(
      NapiPredefined::FalseValue, vm::HermesValue::encodeBoolValue(false));
  setPredefined(
      NapiPredefined::WrapSymbol,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().createNotUniquedLazySymbol(
              "napi.wrap.12556f9a-9053-4cb0-a70d-b6e54f8b9875")));
  setPredefined(
      NapiPredefined::WeakFinalizerSymbol,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().createNotUniquedLazySymbol(
              "napi.weakFinalizer.735e14c9-354f-489b-9f27-02acbc090975")));
}

NodeApiEnvironment::~NodeApiEnvironment() {
  // First we must finalize those references that have `napi_finalizer`
  // callbacks. The reason is that addons might store other references which
  // they delete during their `napi_finalizer` callbacks. If we deleted such
  // references here first, they would be doubly deleted when the
  // `napi_finalizer` deleted them subsequently.
  RefTracker::finalizeAll(&finalizingRefList_);
  RefTracker::finalizeAll(&refList_);
}

template <typename F>
napi_status NodeApiEnvironment::handleExceptions(const F &f) noexcept {
  RETURN_STATUS_IF_FALSE(lastException_.isEmpty(), napi_pending_exception);
  ClearLastError();
  vm::GCScope gcScope(&runtime_);
#ifdef HERMESVM_EXCEPTION_ON_OOM
  try {
    return f();
  } catch (const vm::JSOutOfMemoryError &ex) {
    return SetLastError(napi_generic_failure);
  }
#else // HERMESVM_EXCEPTION_ON_OOM
  return f();
#endif
}

napi_status NodeApiEnvironment::Ref() noexcept {
  m_refs++;
  return napi_status::napi_ok;
}

napi_status NodeApiEnvironment::Unref() noexcept {
  if (--m_refs == 0) {
    delete this;
  }
  return napi_status::napi_ok;
}

napi_status NodeApiEnvironment::SetLastError(
    napi_status error_code,
    uint32_t engine_error_code,
    void *engine_reserved) noexcept {
  lastError_.error_code = error_code;
  lastError_.engine_error_code = engine_error_code;
  lastError_.engine_reserved = engine_reserved;
  return error_code;
}

napi_status NodeApiEnvironment::ClearLastError() noexcept {
  lastError_.error_code = napi_ok;
  lastError_.engine_error_code = 0;
  lastError_.engine_reserved = nullptr;
  return napi_ok;
}

const vm::PinnedHermesValue &NodeApiEnvironment::getPredefined(
    NapiPredefined predefinedKey) noexcept {
  return predefinedValues_[static_cast<size_t>(predefinedKey)];
}

template <WrapType wrapType>
napi_status NodeApiEnvironment::wrapObject(
    napi_value object,
    void *nativeObject,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_ref *result) noexcept {
#if 0
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    auto objHandle = toObjectHandle(object);

    auto wrapKey = getPredefined(NapiPredefined::WrapSymbol).getSymbol();

    if (wrapType == WrapType::Retrievable) {
      // If we've already wrapped this object, we error out.
      vm::NamedPropertyDescriptor desc;
      if (vm::JSObject::getOwnNamedDescriptor(
              objHandle, &runtime_, wrapKey, desc)) {
        return SetLastError(napi_invalid_arg);
      }
    } else if (wrapType == WrapType::Anonymous) {
      // If no finalize callback is provided, we error out.
      CHECK_ARG(finalizeCallback);
    }

    Reference *reference{};
    if (result != nullptr) {
      // The returned reference should be deleted via napi_delete_reference()
      // ONLY in response to the finalize callback invocation. (If it is deleted
      // before then, then the finalize callback will never be invoked.)
      // Therefore a finalize callback is required when returning a reference.
      CHECK_ARG(finalizeCallback);
      reference = Reference::New(
          this,
          phv(object),
          0,
          false,
          finalizeCallback,
          nativeObject,
          finalizeHint);
      *result = reinterpret_cast<napi_ref>(reference);
    } else {
      // Create a self-deleting reference.
      reference = Reference::New(
          this,
          phv(object),
          0,
          true,
          finalizeCallback,
          nativeObject,
          finalizeCallback == nullptr ? nullptr : finalizeHint);
    }

    if (wrapType == WrapType::Retrievable) {
      // TODO:
      // vm::JSObject::putNamed_RJS(objHandle, &runtime_, wrapKey, )
      //     CHECK(obj->SetPrivate(
      //                  context,
      //                  NAPI_PRIVATE_KEY(context, wrapper),
      //                  v8::External::New(env->isolate, reference))
      //               .FromJust());
    }
  });
#endif
  return napi_ok;
}

napi_status NodeApiEnvironment::unwrapObject(
    napi_value object,
    UnwrapAction action,
    void **result) noexcept {
#if 0
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    if (action == UnwrapAction::KeepWrap) {
      CHECK_ARG(result);
    }

    auto objHandle = toObjectHandle(object);
    auto valRes = getPrivate(objHandle, NapiPredefined::WrapSymbol);
    CHECK_STATUS(valRes.getStatus());
    RETURN_STATUS_IF_FALSE(
        vm::vmisa<vm::HostObject>(valRes->getHermesValue()), napi_invalid_arg);

    if (result) {
      auto proxy =
          vm::vmcast<vm::HostObject>(valRes->getHermesValue())->getProxy();
      *result = static_cast<NapiHostObjectProxy *>(proxy)->data();
    }

    if (action == UnwrapAction::RemoveWrap) {
      ASSIGN_CHECKED(
          bool res, deletePrivate(objHandle, NapiPredefined::WrapSymbol));
      RETURN_STATUS_IF_FALSE(res, napi_generic_failure);
      // TODO: Reference::Delete(reference);
    }

    return ClearLastError();
  });
#endif
  return napi_ok;
}

vm::CallResult<vm::PseudoHandle<>> NodeApiEnvironment::getPrivate(
    vm::Handle<vm::JSObject> obj,
    NapiPredefined key) noexcept {
  vm::SymbolID name = getPredefined(key).getSymbol();
  return vm::JSObject::getNamed_RJS(
      obj, &runtime_, name, vm::PropOpFlags().plusThrowOnError());
}

vm::CallResult<bool> NodeApiEnvironment::setPrivate(
    vm::Handle<vm::JSObject> obj,
    vm::SymbolID propID,
    vm::HermesValue value) noexcept {
  // TODO:
  return false;
}

vm::CallResult<bool> NodeApiEnvironment::deletePrivate(
    vm::Handle<vm::JSObject> obj,
    NapiPredefined key) noexcept {
  vm::SymbolID name = getPredefined(key).getSymbol();
  return vm::JSObject::deleteNamed(
      obj, &runtime_, name, vm::PropOpFlags().plusThrowOnError());
}

#if 0
vm::CallResult<bool> NodeApiEnvironment::addWeakFinalizer(
    vm::Handle<vm::JSObject> object,
    IWeakFinalizer *weakFinalizer) noexcept {
  auto privRes = getPrivate(object, NapiPredefined::WeakFinalizerSymbol);
  if (privRes.getStatus() == vm::ExecutionStatus::EXCEPTION) {
    auto objRes = vm::HostObject::createWithoutPrototype(
        &runtime_, std::make_unique<NapiWeakFinalizer>(weakFinalizer));
    CHECK_STATUS(objRes.getStatus());
  } else {
    privRes->
  }
}

vm::CallResult<bool> NodeApiEnvironment::removeWeakFinalizer(
    vm::Handle<vm::JSObject> object,
    IWeakFinalizer *weakFinalizer) noexcept {}
#endif

vm::CallResult<vm::HermesValue> NodeApiEnvironment::stringHVFromAscii(
    const char *str,
    size_t length) noexcept {
  return vm::StringPrimitive::createEfficient(
      &runtime_, llvh::makeArrayRef(str, length));
}

vm::CallResult<vm::HermesValue> NodeApiEnvironment::stringHVFromLatin1(
    const char *str,
    size_t length) noexcept {
  if (isAllASCII(str, str + length)) {
    return stringHVFromAscii(str, length);
  }

  // Latin1 has the same codes as Unicode. We just need to expand char to
  // char16_t.
  std::u16string out(length, u' ');
  for (auto i = 0; i < length; ++i) {
    out[i] = str[i];
  }
  return vm::StringPrimitive::createEfficient(&runtime_, std::move(out));
}

static void convertUtf8ToUtf16(
    const uint8_t *utf8,
    size_t length,
    std::u16string &out) noexcept {
  // length is the number of input bytes
  out.resize(length);
  const llvh::UTF8 *sourceStart = (const llvh::UTF8 *)utf8;
  const llvh::UTF8 *sourceEnd = sourceStart + length;
  llvh::UTF16 *targetStart = (llvh::UTF16 *)&out[0];
  llvh::UTF16 *targetEnd = targetStart + out.size();
  llvh::ConversionResult cRes;
  cRes = ConvertUTF8toUTF16(
      &sourceStart,
      sourceEnd,
      &targetStart,
      targetEnd,
      llvh::lenientConversion);
  (void)cRes;
  assert(
      cRes != llvh::ConversionResult::targetExhausted &&
      "not enough space allocated for UTF16 conversion");
  out.resize((char16_t *)targetStart - &out[0]);
}

vm::CallResult<vm::HermesValue> NodeApiEnvironment::stringHVFromUtf8(
    const uint8_t *utf8,
    size_t length) noexcept {
  if (isAllASCII(utf8, utf8 + length)) {
    return stringHVFromAscii((const char *)utf8, length);
  }
  std::u16string out;
  convertUtf8ToUtf16(utf8, length, out);
  return vm::StringPrimitive::createEfficient(&runtime_, std::move(out));
}

vm::CallResult<vm::HermesValue> NodeApiEnvironment::stringHVFromUtf8(
    const char *utf8) noexcept {
  size_t length = std::char_traits<char>::length(utf8);
  return stringHVFromUtf8(reinterpret_cast<const uint8_t *>(utf8), length);
}

napi_value NodeApiEnvironment::addStackValue(vm::HermesValue value) noexcept {
  m_stackValues.emplace_back(value);
  return reinterpret_cast<napi_value>(&m_stackValues.back());
}

napi_status NodeApiEnvironment::checkStatus(
    vm::ExecutionStatus status) noexcept {
  if (LLVM_LIKELY(status != vm::ExecutionStatus::EXCEPTION)) {
    return napi_ok;
  }

  lastException_ = runtime_.getThrownValue();
  runtime_.clearThrownValue();
  return napi_pending_exception;
}

// Warning: Keep in-sync with napi_status enum
static const char *error_messages[] = {
    nullptr,
    "Invalid argument",
    "An object was expected",
    "A string was expected",
    "A string or symbol was expected",
    "A function was expected",
    "A number was expected",
    "A boolean was expected",
    "An array was expected",
    "Unknown failure",
    "An exception is pending",
    "The async work item was cancelled",
    "napi_escape_handle already called on scope",
    "Invalid handle scope usage",
    "Invalid callback scope usage",
    "Thread-safe function queue is full",
    "Thread-safe function handle is closing",
    "A bigint was expected",
    "A date was expected",
    "An arraybuffer was expected",
    "A detachable arraybuffer was expected",
    "Main thread would deadlock",
};

napi_status NodeApiEnvironment::GetLastErrorInfo(
    const napi_extended_error_info **result) noexcept {
  // CHECK_ARG(env, result);

  // // The value of the constant below must be updated to reference the last
  // // message in the `napi_status` enum each time a new error message is
  // added.
  // // We don't have a napi_status_last as this would result in an ABI
  // // change each time a message was added.
  // const int last_status = napi_would_deadlock;

  // static_assert(
  //     NAPI_ARRAYSIZE(error_messages) == last_status + 1,
  //     "Count of error messages must match count of error values");
  // CHECK_LE(env->last_error.error_code, last_status);

  // // Wait until someone requests the last error information to fetch the
  // error
  // // message string
  // env->last_error.error_message =
  //     error_messages[env->last_error.error_code];

  // *result = &(env->last_error);
  return napi_ok;
}

napi_status NodeApiEnvironment::newFunction(
    vm::SymbolID name,
    napi_callback callback,
    void *callbackData,
    napi_value *result) noexcept {
  auto context = std::make_unique<HFContext>(*this, callback, callbackData);
  auto funcRes = vm::FinalizableNativeFunction::createWithoutPrototype(
      &runtime_,
      context.get(),
      &HFContext::func,
      &HFContext::finalize,
      name,
      /*paramCount:*/ 0);
  CHECK_STATUS(funcRes.getStatus());
  context.release();
  *result = addStackValue(*funcRes);
  return ClearLastError();
}

napi_status NodeApiEnvironment::createFunction(
    const char *utf8Name,
    size_t length,
    napi_callback callback,
    void *callbackData,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    napi_value nameValue{};
    STATUS_CALL(CreateStringUtf8(utf8Name, length, &nameValue));
    auto nameRes = vm::stringToSymbolID(
        &runtime_, vm::createPseudoHandle(phv(nameValue).getString()));
    CHECK_STATUS(nameRes.getStatus());
    STATUS_CALL(newFunction(nameRes->get(), callback, callbackData, result));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::defineClass(
    const char *utf8Name,
    size_t length,
    napi_callback constructor,
    void *callbackData,
    size_t propertyCount,
    const napi_property_descriptor *properties,
    napi_value *result) noexcept {
  // return handleExceptions([&] {
  //   CHECK_ARG(result);
  //   CHECK_ARG(constructor);
  //   if (propertyCount > 0) {
  //     CHECK_ARG(properties);
  //   }

  // v8::Isolate *isolate = env->isolate;

  // v8::EscapableHandleScope scope(isolate);
  // v8::Local<v8::FunctionTemplate> tpl;
  // STATUS_CALL(v8impl::FunctionCallbackWrapper::NewTemplate(
  //     env, constructor, callback_data, &tpl));

  // v8::Local<v8::String> name_string;
  // CHECK_NEW_FROM_UTF8_LEN(env, name_string, utf8name, length);
  // tpl->SetClassName(name_string);

  // size_t static_property_count = 0;
  // for (size_t i = 0; i < property_count; i++) {
  //   const napi_property_descriptor *p = properties + i;

  //   if ((p->attributes & napi_static) != 0) {
  //     // Static properties are handled separately below.
  //     static_property_count++;
  //     continue;
  //   }

  //   v8::Local<v8::Name> property_name;
  //   STATUS_CALL(v8impl::V8NameFromPropertyDescriptor(env, p,
  //   &property_name));

  //   v8::PropertyAttribute attributes =
  //       v8impl::V8PropertyAttributesFromDescriptor(p);

  //   // This code is similar to that in napi_define_properties(); the
  //   // difference is it applies to a template instead of an object,
  //   // and preferred PropertyAttribute for lack of PropertyDescriptor
  //   // support on ObjectTemplate.
  //   if (p->getter != nullptr || p->setter != nullptr) {
  //     v8::Local<v8::FunctionTemplate> getter_tpl;
  //     v8::Local<v8::FunctionTemplate> setter_tpl;
  //     if (p->getter != nullptr) {
  //       STATUS_CALL(v8impl::FunctionCallbackWrapper::NewTemplate(
  //           env, p->getter, p->data, &getter_tpl));
  //     }
  //     if (p->setter != nullptr) {
  //       STATUS_CALL(v8impl::FunctionCallbackWrapper::NewTemplate(
  //           env, p->setter, p->data, &setter_tpl));
  //     }

  //     tpl->PrototypeTemplate()->SetAccessorProperty(
  //         property_name,
  //         getter_tpl,
  //         setter_tpl,
  //         attributes,
  //         v8::AccessControl::DEFAULT);
  //   } else if (p->method != nullptr) {
  //     v8::Local<v8::FunctionTemplate> t;
  //     STATUS_CALL(v8impl::FunctionCallbackWrapper::NewTemplate(
  //         env, p->method, p->data, &t, v8::Signature::New(isolate, tpl)));

  //     tpl->PrototypeTemplate()->Set(property_name, t, attributes);
  //   } else {
  //     v8::Local<v8::Value> value =
  //     v8impl::V8LocalValueFromJsValue(p->value);
  //     tpl->PrototypeTemplate()->Set(property_name, value, attributes);
  //   }
  // }

  // v8::Local<v8::Context> context = env->context();
  // *result = v8impl::JsValueFromV8LocalValue(
  //     scope.Escape(tpl->GetFunction(context).ToLocalChecked()));

  // if (static_property_count > 0) {
  //   std::vector<napi_property_descriptor> static_descriptors;
  //   static_descriptors.reserve(static_property_count);

  //   for (size_t i = 0; i < property_count; i++) {
  //     const napi_property_descriptor *p = properties + i;
  //     if ((p->attributes & napi_static) != 0) {
  //       static_descriptors.push_back(*p);
  //     }
  //   }

  //   STATUS_CALL(napi_define_properties(
  //       env, *result, static_descriptors.size(),
  //       static_descriptors.data()));
  // }

  // return GET_RETURN_STATUS(env);
  //  });

  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);
  // CHECK_ARG(env, constructor);

  // if (property_count > 0) {
  //   CHECK_ARG(env, properties);
  // }

  // v8::Isolate *isolate = env->isolate;

  // v8::EscapableHandleScope scope(isolate);
  // v8::Local<v8::FunctionTemplate> tpl;
  // STATUS_CALL(v8impl::FunctionCallbackWrapper::NewTemplate(
  //     env, constructor, callback_data, &tpl));

  // v8::Local<v8::String> name_string;
  // CHECK_NEW_FROM_UTF8_LEN(env, name_string, utf8name, length);
  // tpl->SetClassName(name_string);

  // size_t static_property_count = 0;
  // for (size_t i = 0; i < property_count; i++) {
  //   const napi_property_descriptor *p = properties + i;

  //   if ((p->attributes & napi_static) != 0) {
  //     // Static properties are handled separately below.
  //     static_property_count++;
  //     continue;
  //   }

  //   v8::Local<v8::Name> property_name;
  //   STATUS_CALL(v8impl::V8NameFromPropertyDescriptor(env, p,
  //   &property_name));

  //   v8::PropertyAttribute attributes =
  //       v8impl::V8PropertyAttributesFromDescriptor(p);

  //   // This code is similar to that in napi_define_properties(); the
  //   // difference is it applies to a template instead of an object,
  //   // and preferred PropertyAttribute for lack of PropertyDescriptor
  //   // support on ObjectTemplate.
  //   if (p->getter != nullptr || p->setter != nullptr) {
  //     v8::Local<v8::FunctionTemplate> getter_tpl;
  //     v8::Local<v8::FunctionTemplate> setter_tpl;
  //     if (p->getter != nullptr) {
  //       STATUS_CALL(v8impl::FunctionCallbackWrapper::NewTemplate(
  //           env, p->getter, p->data, &getter_tpl));
  //     }
  //     if (p->setter != nullptr) {
  //       STATUS_CALL(v8impl::FunctionCallbackWrapper::NewTemplate(
  //           env, p->setter, p->data, &setter_tpl));
  //     }

  //     tpl->PrototypeTemplate()->SetAccessorProperty(
  //         property_name,
  //         getter_tpl,
  //         setter_tpl,
  //         attributes,
  //         v8::AccessControl::DEFAULT);
  //   } else if (p->method != nullptr) {
  //     v8::Local<v8::FunctionTemplate> t;
  //     STATUS_CALL(v8impl::FunctionCallbackWrapper::NewTemplate(
  //         env, p->method, p->data, &t, v8::Signature::New(isolate, tpl)));

  //     tpl->PrototypeTemplate()->Set(property_name, t, attributes);
  //   } else {
  //     v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(p->value);
  //     tpl->PrototypeTemplate()->Set(property_name, value, attributes);
  //   }
  // }

  // v8::Local<v8::Context> context = env->context();
  // *result = v8impl::JsValueFromV8LocalValue(
  //     scope.Escape(tpl->GetFunction(context).ToLocalChecked()));

  // if (static_property_count > 0) {
  //   std::vector<napi_property_descriptor> static_descriptors;
  //   static_descriptors.reserve(static_property_count);

  //   for (size_t i = 0; i < property_count; i++) {
  //     const napi_property_descriptor *p = properties + i;
  //     if ((p->attributes & napi_static) != 0) {
  //       static_descriptors.push_back(*p);
  //     }
  //   }

  //   STATUS_CALL(napi_define_properties(
  //       env, *result, static_descriptors.size(), static_descriptors.data()));
  // }

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::getPropertyNames(
    napi_value object,
    napi_value *result) noexcept {
  return getAllPropertyNames(
      object,
      napi_key_include_prototypes,
      static_cast<napi_key_filter>(napi_key_enumerable | napi_key_skip_symbols),
      napi_key_numbers_to_strings,
      result);
}

napi_status NodeApiEnvironment::getAllPropertyNames(
    napi_value object,
    napi_key_collection_mode keyMode,
    napi_key_filter keyFilter,
    napi_key_conversion keyConversion,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    CHECK_OBJECT_ARG(object);
    RETURN_STATUS_IF_FALSE(
        keyMode == napi_key_include_prototypes || keyMode == napi_key_own_only,
        napi_invalid_arg);
    RETURN_STATUS_IF_FALSE(
        keyConversion == napi_key_keep_numbers ||
            keyConversion == napi_key_numbers_to_strings,
        napi_invalid_arg);

    auto objHandle = toObjectHandle(object);
    struct JSObjectAccessor : vm::JSObject {
      auto getClazz() {
        return clazz_;
      }
      auto getFlags() {
        return flags_;
      }
    };
    auto objAccessor = static_cast<JSObjectAccessor *>(objHandle.get());
    auto objVT = reinterpret_cast<const vm::ObjectVTable *>(
        static_cast<vm::GCCell *>(objHandle.get())->getVT());

    if (LLVM_UNLIKELY(
            objAccessor->getFlags().lazyObject ||
            objAccessor->getFlags().proxyObject)) {
      if (objAccessor->getFlags().proxyObject) {
        auto okFlags = vm::OwnKeysFlags();
        okFlags.setIncludeNonSymbols((keyFilter & napi_key_skip_strings) == 0);
        okFlags.setIncludeSymbols((keyFilter & napi_key_skip_symbols) == 0);
        okFlags.setIncludeNonEnumerable((keyFilter & napi_key_enumerable) == 0);
        auto proxyRes =
            vm::JSProxy::ownPropertyKeys(objHandle, &runtime_, okFlags);
        CHECK_STATUS(proxyRes.getStatus());
        *result = addStackValue(proxyRes->getHermesValue());
        return ClearLastError();
      }
      assert(
          objAccessor->getFlags().lazyObject &&
          "descriptor flags are impossible");
      vm::JSObject::initializeLazyObject(&runtime_, objHandle);
    }

    auto range = objVT->getOwnIndexedRange(objHandle.get(), &runtime_);

    // Estimate the capacity of the output array.  This estimate is only
    // reasonable for the non-symbol case.
    uint32_t capacity = (keyFilter & napi_key_skip_strings) == 0
        ? (objAccessor->getClazz().get(&runtime_)->getNumProperties() +
           range.second - range.first)
        : 0;

    ASSIGN_CHECKED(auto array, vm::JSArray::create(&runtime_, capacity, 0));

    // Optional array of SymbolIDs reported via host object API
    llvh::Optional<vm::Handle<vm::JSArray>> hostObjectSymbols;
    size_t hostObjectSymbolCount = 0;

    // If current object is a host object we need to deduplicate its properties
    llvh::SmallSet<vm::SymbolID::RawType, 16> dedupSet;

    // Output index.
    uint32_t index = 0;

    // Avoid allocating a new handle per element.
    vm::MutableHandle<> tmpHandle{&runtime_};

    // Number of indexed properties.
    uint32_t numIndexed = 0;

    // Regular properties with names that are array indexes are stashed here, if
    // encountered.
    llvh::SmallVector<uint32_t, 8> indexNames{};

    // Iterate the named properties excluding those which use Symbols.
    if ((keyFilter & napi_key_skip_strings) == 0) {
      // Get host object property names
      if (LLVM_UNLIKELY(objAccessor->getFlags().hostObject)) {
        assert(
            range.first == range.second &&
            "Host objects cannot own indexed range");
        ASSIGN_CHECKED(
            auto hostSymbols,
            vm::vmcast<vm::HostObject>(objHandle.get())
                ->getHostPropertyNames());
        if ((hostObjectSymbolCount = (*hostSymbols)->getEndIndex()) != 0) {
          hostObjectSymbols = std::move(hostSymbols);
          capacity += hostObjectSymbolCount;
        }
      }

      // Iterate the indexed properties.
      vm::GCScopeMarkerRAII marker{&runtime_};
      for (auto i = range.first; i != range.second; ++i) {
        auto propFlags =
            objVT->getOwnIndexedPropertyFlags(objHandle.get(), &runtime_, i);
        if (!propFlags)
          continue;

        // If specified, check whether it is enumerable.
        if ((keyFilter & napi_key_enumerable) && !propFlags->enumerable) {
          continue;
        }
        // If specified, check whether it is writable.
        if ((keyFilter & napi_key_writable) && !propFlags->writable) {
          continue;
        }
        // If specified, check whether it is configurable.
        if ((keyFilter & napi_key_configurable) && !propFlags->configurable) {
          continue;
        }

        tmpHandle = vm::HermesValue::encodeDoubleValue(i);
        vm::JSArray::setElementAt(array, &runtime_, index++, tmpHandle);
        marker.flush();
      }

      numIndexed = index;

      vm::HiddenClass::forEachProperty(
          runtime_.makeHandle(objAccessor->getClazz()),
          &runtime_,
          [this,
           keyFilter,
           array,
           hostObjectSymbolCount,
           &index,
           &indexNames,
           &tmpHandle,
           &dedupSet](vm::SymbolID id, vm::NamedPropertyDescriptor desc) {
            if (!isPropertyNamePrimitive(id)) {
              return;
            }

            // If specified, check whether it is enumerable.
            if ((keyFilter & napi_key_enumerable) && !desc.flags.enumerable) {
              return;
            }
            // If specified, check whether it is writable.
            if ((keyFilter & napi_key_writable) && !desc.flags.writable) {
              return;
            }
            // If specified, check whether it is configurable.
            if ((keyFilter & napi_key_configurable) &&
                !desc.flags.configurable) {
              return;
            }

            // Host properties might overlap with the ones recognized by the
            // hidden class. If we're dealing with a host object then keep track
            // of hidden class properties for the deduplication purposes.
            if (LLVM_UNLIKELY(hostObjectSymbolCount > 0)) {
              dedupSet.insert(id.unsafeGetRaw());
            }

            // Check if this property is an integer index. If it is, we stash it
            // away to deal with it later. This check should be fast since most
            // property names don't start with a digit.
            auto propNameAsIndex = vm::toArrayIndex(
                runtime_.getIdentifierTable().getStringView(&runtime_, id));
            if (LLVM_UNLIKELY(propNameAsIndex)) {
              indexNames.push_back(*propNameAsIndex);
              return;
            }

            tmpHandle = vm::HermesValue::encodeStringValue(
                runtime_.getStringPrimFromSymbolID(id));
            vm::JSArray::setElementAt(array, &runtime_, index++, tmpHandle);
          });
      // Iterate over HostObject properties and append them to the array. Do not
      // append duplicates.
      if (LLVM_UNLIKELY(hostObjectSymbols)) {
        for (size_t i = 0; i < hostObjectSymbolCount; ++i) {
          assert(
              (*hostObjectSymbols)->at(&runtime_, i).isSymbol() &&
              "Host object needs to return array of SymbolIDs");
          marker.flush();

          vm::SymbolID id = (*hostObjectSymbols)->at(&runtime_, i).getSymbol();
          if (dedupSet.count(id.unsafeGetRaw()) == 0) {
            dedupSet.insert(id.unsafeGetRaw());
            assert(
                !vm::InternalProperty::isInternal(id) &&
                "host object returned reserved symbol");
            auto propNameAsIndex = vm::toArrayIndex(
                runtime_.getIdentifierTable().getStringView(&runtime_, id));
            if (LLVM_UNLIKELY(propNameAsIndex)) {
              indexNames.push_back(*propNameAsIndex);
              continue;
            }
            tmpHandle = vm::HermesValue::encodeStringValue(
                runtime_.getStringPrimFromSymbolID(id));
            vm::JSArray::setElementAt(array, &runtime_, index++, tmpHandle);
          }
        }
      }
    }

    // Now iterate the named properties again, including only Symbols.
    // We could iterate only once, if we chose to ignore (and disallow)
    // own properties on HostObjects, as we do with Proxies.
    if ((keyFilter & napi_key_skip_symbols) == 0) {
      vm::MutableHandle<vm::SymbolID> idHandle{&runtime_};
      vm::HiddenClass::forEachProperty(
          runtime_.makeHandle(objAccessor->getClazz()),
          &runtime_,
          [this, keyFilter, array, &index, &idHandle](
              vm::SymbolID id, vm::NamedPropertyDescriptor desc) {
            if (!vm::isSymbolPrimitive(id)) {
              return;
            }
            // If specified, check whether it is enumerable.
            if ((keyFilter & napi_key_enumerable) && !desc.flags.enumerable) {
              return;
            }
            // If specified, check whether it is writable.
            if ((keyFilter & napi_key_writable) && !desc.flags.writable) {
              return;
            }
            // If specified, check whether it is configurable.
            if ((keyFilter & napi_key_configurable) &&
                !desc.flags.configurable) {
              return;
            }
            idHandle = id;
            vm::JSArray::setElementAt(array, &runtime_, index++, idHandle);
          });
    }

    // The end (exclusive) of the named properties.
    uint32_t endNamed = index;

    // Properly set the length of the array.
    auto cr = vm::JSArray::setLengthProperty(
        array, &runtime_, endNamed + indexNames.size(), vm::PropOpFlags{});
    (void)cr;
    assert(
        cr != vm::ExecutionStatus::EXCEPTION && *cr &&
        "JSArray::setLength() failed");

    // If we have no index-like names, we are done.
    if (LLVM_LIKELY(indexNames.empty())) {
      *result = addStackValue(array.getHermesValue());
      return ClearLastError();
    }

    // In the unlikely event that we encountered index-like names, we need to
    // sort them and merge them with the real indexed properties. Note that it
    // is guaranteed that there are no clashes.
    std::sort(indexNames.begin(), indexNames.end());

    // Also make space for the new elements by shifting all the named properties
    // to the right. First, resize the array.
    vm::JSArray::setStorageEndIndex(
        array, &runtime_, endNamed + indexNames.size());

    // Shift the non-index property names. The region [numIndexed..endNamed) is
    // moved to [numIndexed+indexNames.size()..array->size()).
    // TODO: optimize this by implementing memcpy-like functionality in
    // ArrayImpl.
    for (uint32_t last = endNamed, toLast = array->getEndIndex();
         last != numIndexed;) {
      --last;
      --toLast;
      tmpHandle = array->at(&runtime_, last);
      vm::JSArray::setElementAt(array, &runtime_, toLast, tmpHandle);
    }

    // Now we need to merge the indexes in indexNames and the array
    // [0..numIndexed). We start from the end and copy the larger element from
    // either array.
    // 1+ the destination position to copy into.
    for (uint32_t toLast = numIndexed + indexNames.size(),
                  indexNamesLast = indexNames.size();
         toLast != 0;) {
      if (numIndexed) {
        uint32_t a = (uint32_t)array->at(&runtime_, numIndexed - 1).getNumber();
        uint32_t b;

        if (indexNamesLast && (b = indexNames[indexNamesLast - 1]) > a) {
          tmpHandle = vm::HermesValue::encodeDoubleValue(b);
          --indexNamesLast;
        } else {
          tmpHandle = vm::HermesValue::encodeDoubleValue(a);
          --numIndexed;
        }
      } else {
        assert(indexNamesLast && "prematurely ran out of source values");
        tmpHandle =
            vm::HermesValue::encodeDoubleValue(indexNames[indexNamesLast - 1]);
        --indexNamesLast;
      }

      --toLast;
      vm::JSArray::setElementAt(array, &runtime_, toLast, tmpHandle);
    }

    *result = addStackValue(array.getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::setProperty(
    napi_value object,
    napi_value key,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(value);

    auto objHandle = toObjectHandle(object);
    CHECK_STATUS(objHandle
                     ->putComputed_RJS(
                         objHandle,
                         &runtime_,
                         toHandle(key),
                         toHandle(value),
                         vm::PropOpFlags().plusThrowOnError())
                     .getStatus());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::hasProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(result);
    auto objHandle = toObjectHandle(object);
    auto res = objHandle->hasComputed(objHandle, &runtime_, stringHandle(key));
    CHECK_STATUS(res.getStatus());
    *result = *res;
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::getProperty(
    napi_value object,
    napi_value key,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(result);
    auto objHandle = toObjectHandle(object);
    auto res =
        objHandle->getComputed_RJS(objHandle, &runtime_, stringHandle(key));
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(res->get());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::deleteProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(result);
    auto objHandle = toObjectHandle(object);
    auto res = vm::JSObject::deleteComputed(
        objHandle,
        &runtime_,
        stringHandle(key),
        vm::PropOpFlags().plusThrowOnError());
    CHECK_STATUS(res.getStatus());
    *result = *res;
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::hasOwnProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(result);
    auto objHandle = toObjectHandle(object);
    auto res = objHandle->hasComputed(objHandle, &runtime_, stringHandle(key));
    CHECK_STATUS(res.getStatus());
    *result = *res;
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::setNamedProperty(
    napi_value object,
    const char *utf8Name,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(utf8Name);
    CHECK_ARG(value);

    auto objHandle = toObjectHandle(object);
    ASSIGN_CHECKED(const auto &name, stringHVFromUtf8(utf8Name));
    CHECK_STATUS(objHandle
                     ->putComputed_RJS(
                         objHandle,
                         &runtime_,
                         toHandle(name),
                         toHandle(value),
                         vm::PropOpFlags().plusThrowOnError())
                     .getStatus());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::hasNamedProperty(
    napi_value object,
    const char *utf8Name,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(utf8Name);
    CHECK_ARG(result);

    auto objHandle = toObjectHandle(object);
    ASSIGN_CHECKED(const auto &name, stringHVFromUtf8(utf8Name));

    ASSIGN_CHECKED(
        *result, objHandle->hasComputed(objHandle, &runtime_, toHandle(name)));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::getNamedProperty(
    napi_value object,
    const char *utf8Name,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(utf8Name);
    CHECK_ARG(result);

    auto objHandle = toObjectHandle(object);
    ASSIGN_CHECKED(const auto &name, stringHVFromUtf8(utf8Name));

    ASSIGN_CHECKED(
        const auto &res,
        objHandle->getComputed_RJS(objHandle, &runtime_, toHandle(name)));
    *result = addStackValue(res.get());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::setElement(
    napi_value arr,
    uint32_t index,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(arr);
    CHECK_ARG(value);

    // TODO:
    // if (LLVM_UNLIKELY(index >= size(arr))) {
    //   throw makeJSError(
    //       *this,
    //       "setValueAtIndex: index ",
    //       i,
    //       " is out of bounds [0, ",
    //       size(arr),
    //       ")");
    // }

    auto h = arrayHandle(arr);
    h->setElementAt(h, &runtime_, index, toHandle(value));

    return ClearLastError();
  });
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, value);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // auto set_maybe = obj->Set(context, index, val);

  // RETURN_STATUS_IF_FALSE(env, set_maybe.FromMaybe(false),
  // napi_generic_failure);

  // return GET_RETURN_STATUS(env);
  // return napi_ok;
}

napi_status NodeApiEnvironment::hasElement(
    napi_value object,
    uint32_t index,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Maybe<bool> has_maybe = obj->Has(context, index);

  // CHECK_MAYBE_NOTHING(env, has_maybe, napi_generic_failure);

  // *result = has_maybe.FromMaybe(false);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::getElement(
    napi_value object,
    uint32_t index,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // auto get_maybe = obj->Get(context, index);

  // CHECK_MAYBE_EMPTY(env, get_maybe, napi_generic_failure);

  // *result = v8impl::JsValueFromV8LocalValue(get_maybe.ToLocalChecked());
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::deleteElement(
    napi_value object,
    uint32_t index,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);
  // v8::Maybe<bool> delete_maybe = obj->Delete(context, index);
  // CHECK_MAYBE_NOTHING(env, delete_maybe, napi_generic_failure);

  // if (result != nullptr)
  //   *result = delete_maybe.FromMaybe(false);

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::symbolIDFromPropertyDescriptor(
    const napi_property_descriptor *p,
    vm::MutableHandle<vm::SymbolID> *result) noexcept {
  if (p->utf8name != nullptr) {
    napi_value nameValue{};
    STATUS_CALL(CreateStringUtf8(p->utf8name, NAPI_AUTO_LENGTH, &nameValue));
    ASSIGN_CHECKED(
        *result,
        vm::stringToSymbolID(
            &runtime_, vm::createPseudoHandle(phv(nameValue).getString())));
  } else {
    auto namePHV = phv(p->name);
    if (namePHV.isString()) {
      ASSIGN_CHECKED(
          *result,
          vm::stringToSymbolID(
              &runtime_, vm::createPseudoHandle(namePHV.getString())));
    } else if (namePHV.isSymbol()) {
      *result = namePHV.getSymbol();
    } else {
      return SetLastError(napi_name_expected);
    }
  }

  return napi_ok;
}

napi_status NodeApiEnvironment::defineProperties(
    napi_value object,
    size_t propertyCount,
    const napi_property_descriptor *properties) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    if (propertyCount > 0) {
      CHECK_ARG(properties);
    }

    for (size_t i = 0; i < propertyCount; ++i) {
      const napi_property_descriptor *p = &properties[i];
      vm::MutableHandle<vm::SymbolID> name{&runtime_};
      STATUS_CALL(symbolIDFromPropertyDescriptor(p, &name));

      auto dpFlags = vm::DefinePropertyFlags::getDefaultNewPropertyFlags();
      if ((p->attributes & napi_writable) == 0) {
        dpFlags.writable = 0;
      }
      if ((p->attributes & napi_enumerable) == 0) {
        dpFlags.enumerable = 0;
      }
      if ((p->attributes & napi_configurable) == 0) {
        dpFlags.configurable = 0;
      }

      if (p->getter != nullptr || p->setter != nullptr) {
        napi_value localGetter{};
        napi_value localSetter{};

        if (p->getter != nullptr) {
          auto cr = vm::stringToSymbolID(
              &runtime_, vm::StringPrimitive::createNoThrow(&runtime_, "get"));
          CHECK_STATUS(cr.getStatus());
          STATUS_CALL(newFunction(cr->get(), p->getter, p->data, &localGetter));
        }
        if (p->setter != nullptr) {
          auto cr = vm::stringToSymbolID(
              &runtime_, vm::StringPrimitive::createNoThrow(&runtime_, "set"));
          CHECK_STATUS(cr.getStatus());
          STATUS_CALL(newFunction(cr->get(), p->getter, p->data, &localSetter));
        }

        auto propRes = vm::PropertyAccessor::create(
            &runtime_,
            vm::Handle<vm::Callable>::vmcast(&phv(localGetter)),
            vm::Handle<vm::Callable>::vmcast(&phv(localSetter)));
        CHECK_STATUS(propRes.getStatus());
        CHECK_STATUS(vm::JSObject::defineOwnProperty(
                         toObjectHandle(object),
                         &runtime_,
                         name.get(),
                         dpFlags,
                         toHandle(*propRes),
                         vm::PropOpFlags().plusThrowOnError())
                         .getStatus());
      } else if (p->method != nullptr) {
        napi_value method{};
        STATUS_CALL(newFunction(name.get(), p->getter, p->data, &method));
        CHECK_STATUS(vm::JSObject::defineOwnProperty(
                         toObjectHandle(object),
                         &runtime_,
                         name.get(),
                         dpFlags,
                         toHandle(method),
                         vm::PropOpFlags().plusThrowOnError())
                         .getStatus());
      } else {
        CHECK_STATUS(vm::JSObject::defineOwnProperty(
                         toObjectHandle(object),
                         &runtime_,
                         name.get(),
                         dpFlags,
                         toHandle(p->value),
                         vm::PropOpFlags().plusThrowOnError())
                         .getStatus());
      }
    }

    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::objectFreeze(napi_value object) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);

    CHECK_STATUS(vm::JSObject::freeze(toObjectHandle(object), &runtime_));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::objectSeal(napi_value object) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);

    CHECK_STATUS(vm::JSObject::seal(toObjectHandle(object), &runtime_));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::isArray(
    napi_value value,
    bool *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_OBJECT_ARG(value);
  CHECK_ARG(result);

  *result = vm::vmisa<vm::JSArray>(phv(value));
  return ClearLastError();
}

napi_status NodeApiEnvironment::getArrayLength(
    napi_value value,
    uint32_t *result) noexcept {
  return handleExceptions([&] {
    auto res = vm::JSObject::getNamed_RJS(
        toArrayHandle(value),
        &runtime_,
        vm::Predefined::getSymbolID(vm::Predefined::length));
    CHECK_STATUS(res.getStatus());
    if (!(*res)->isNumber()) {
      return SetLastError(napi_number_expected);
    }
    *result = static_cast<uint32_t>((*res)->getDouble());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::strictEquals(
    napi_value lhs,
    napi_value rhs,
    bool *result) noexcept {
  const vm::PinnedHermesValue &lhsHV = phv(lhs);
  const vm::PinnedHermesValue &rhsHV = phv(rhs);
  auto lhsTag = lhsHV.getTag();
  if (lhsTag != rhsHV.getTag()) {
    *result = false;
  } else if (lhsTag == vm::StrTag) {
    *result = lhsHV.getString()->equals(rhsHV.getString());
  } else if (lhsTag == vm::SymbolTag) {
    *result = lhsHV.getSymbol() == rhsHV.getSymbol();
  } else {
    *result = lhsHV.getRaw() == rhsHV.getRaw();
  }
  return napi_ok;
}

napi_status NodeApiEnvironment::getPrototype(
    napi_value object,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(result);
    auto res = vm::JSObject::getPrototypeOf(
        vm::PseudoHandle<vm::JSObject>(toObjectHandle(object)), &runtime_);
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(res->getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::createObject(napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    *result = addStackValue(vm::JSObject::create(&runtime_).getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::createArray(napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    auto res = vm::JSArray::create(&runtime_, /*capacity:*/ 16, /*length:*/ 0);
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(res->getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::createArray(
    size_t length,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    auto res = vm::JSArray::create(&runtime_, length, length);
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(res->getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateStringLatin1(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(str);
    CHECK_ARG(result);
    RETURN_STATUS_IF_FALSE(
        (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);
    if (length == NAPI_AUTO_LENGTH) {
      length = std::char_traits<char>::length(str);
    }
    auto res = stringHVFromLatin1(str, length);
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(*res);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateStringUtf8(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(str);
    CHECK_ARG(result);
    RETURN_STATUS_IF_FALSE(
        (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);
    if (length == NAPI_AUTO_LENGTH) {
      length = std::char_traits<char>::length(str);
    }
    auto res = stringHVFromUtf8(reinterpret_cast<const uint8_t *>(str), length);
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(*res);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateStringUtf16(
    const char16_t *str,
    size_t length,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(str);
    CHECK_ARG(result);
    RETURN_STATUS_IF_FALSE(
        (length == NAPI_AUTO_LENGTH) || length <= INT_MAX, napi_invalid_arg);
    if (length == NAPI_AUTO_LENGTH) {
      length = std::char_traits<char16_t>::length(str);
    }
    auto res = vm::StringPrimitive::createEfficient(
        &runtime_, llvh::makeArrayRef(str, length));
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(*res);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateNumber(
    double value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    *result = addStackValue(vm::HermesValue::encodeUntrustedDoubleValue(value));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateNumber(
    int32_t value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    *result = addStackValue(vm::HermesValue::encodeNumberValue(value));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateNumber(
    uint32_t value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    *result = addStackValue(vm::HermesValue::encodeNumberValue(value));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateNumber(
    int64_t value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    *result = addStackValue(vm::HermesValue::encodeNumberValue(value));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::GetBoolean(
    bool value,
    napi_value *result) noexcept {
  CHECK_ARG(result);
  *result = addStackValue(runtime_.getBoolValue(value).getHermesValue());
  return ClearLastError();
}

napi_status NodeApiEnvironment::createSymbol(
    napi_value description,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    vm::MutableHandle<vm::StringPrimitive> descString{&runtime_};
    if (description) {
      CHECK_STRING_ARG(description);
      descString = phv(description).getString();
    } else {
      // If description is undefined, the descString will eventually be "".
      descString = runtime_.getPredefinedString(vm::Predefined::emptyString);
    }

    auto symbolRes = runtime_.getIdentifierTable().createNotUniquedSymbol(
        &runtime_, descString);
    CHECK_STATUS(symbolRes.getStatus());
    *result = addStackValue(vm::HermesValue::encodeSymbolValue(*symbolRes));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateError(
    napi_value code,
    napi_value msg,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_STRING_ARG(msg);
    CHECK_ARG(result);

    auto err = vm::JSError::create(
        &runtime_, vm::Handle<vm::JSObject>::vmcast(&runtime_.ErrorPrototype));

    vm::PinnedHermesValue err_phv{err.getHermesValue()};
    CHECK_STATUS(vm::JSError::setMessage(
        vm::Handle<vm::JSError>::vmcast(&err_phv),
        &runtime_,
        stringHandle(msg)));
    // STATUS_CALL(set_error_code(env, error_obj, code, nullptr));

    *result = addStackValue(err_phv);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateTypeError(
    napi_value code,
    napi_value msg,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_STRING_ARG(msg);
    CHECK_ARG(result);

    auto err = vm::JSError::create(
        &runtime_,
        vm::Handle<vm::JSObject>::vmcast(&runtime_.TypeErrorPrototype));

    vm::PinnedHermesValue err_phv{err.getHermesValue()};
    CHECK_STATUS(vm::JSError::setMessage(
        vm::Handle<vm::JSError>::vmcast(&err_phv),
        &runtime_,
        stringHandle(msg)));
    // STATUS_CALL(set_error_code(env, error_obj, code, nullptr));

    *result = addStackValue(err_phv);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateRangeError(
    napi_value code,
    napi_value msg,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_STRING_ARG(msg);
    CHECK_ARG(result);

    auto err = vm::JSError::create(
        &runtime_,
        vm::Handle<vm::JSObject>::vmcast(&runtime_.RangeErrorPrototype));

    vm::PinnedHermesValue err_phv{err.getHermesValue()};
    CHECK_STATUS(vm::JSError::setMessage(
        vm::Handle<vm::JSError>::vmcast(&err_phv),
        &runtime_,
        stringHandle(msg)));
    // STATUS_CALL(set_error_code(env, error_obj, code, nullptr));

    *result = addStackValue(err_phv);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::typeOf(
    napi_value value,
    napi_valuetype *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(value);
  CHECK_ARG(result);

  const vm::PinnedHermesValue &hv = phv(value);

  if (hv.isNumber()) {
    *result = napi_number;
    // BigInt is not supported by Hermes yet.
    //} else if (hv.IsBigInt()) {
    //   *result = napi_bigint;
  } else if (hv.isString()) {
    *result = napi_string;
  } else if (hv.isObject()) {
    if (vm::vmisa<vm::Callable>(hv)) {
      *result = napi_function;
    } else if (vm::vmisa<vm::HostObject>(hv)) {
      *result = napi_external;
    } else {
      *result = napi_object;
    }
  } else if (hv.isBool()) {
    *result = napi_boolean;
  } else if (hv.isUndefined() || hv.isEmpty()) {
    *result = napi_undefined;
  } else if (hv.isSymbol()) {
    *result = napi_symbol;
  } else if (hv.isNull()) {
    *result = napi_null;
  } else {
    // Should not get here unless Hermes has added some new kind of value.
    return SetLastError(napi_invalid_arg);
  }

  return ClearLastError();
}

napi_status NodeApiEnvironment::getUndefined(napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(result);
  *result = addStackValue(runtime_.getUndefinedValue().getHermesValue());
  return ClearLastError();
}

napi_status NodeApiEnvironment::getNull(napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(result);
  *result = addStackValue(runtime_.getNullValue().getHermesValue());
  return ClearLastError();
}

napi_status NodeApiEnvironment::getCallbackInfo(
    CallbackInfo *callbackInfo,
    size_t *argCount,
    napi_value *args,
    napi_value *thisArg,
    void **data) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(callbackInfo);

  if (args != nullptr) {
    CHECK_ARG(argCount);
    callbackInfo->Args(args, argCount);
  }

  if (argCount != nullptr) {
    *argCount = callbackInfo->ArgCount();
  }

  if (thisArg != nullptr) {
    *thisArg = callbackInfo->This();
  }

  if (data != nullptr) {
    *data = callbackInfo->Data();
  }

  return ClearLastError();
}

napi_status NodeApiEnvironment::getNewTarget(
    CallbackInfo *callbackInfo,
    napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(callbackInfo);
  CHECK_ARG(result);

  *result = callbackInfo->GetNewTarget();

  return ClearLastError();
}

napi_status NodeApiEnvironment::callFunction(
    napi_value object,
    napi_value func,
    size_t argCount,
    const napi_value *args,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    if (argCount > 0) {
      CHECK_ARG(args);
    }
    vm::Handle<vm::Callable> handle =
        vm::Handle<vm::Callable>::vmcast(&phv(func));
    if (argCount > std::numeric_limits<uint32_t>::max() ||
        !runtime_.checkAvailableStack((uint32_t)argCount)) {
      LOG_EXCEPTION_CAUSE(
          "NodeApiEnvironment::CallFunction: Unable to call function: stack overflow");
      // TODO: implement
      // throw jsi::JSINativeException(
      //    "NodeApiEnvironment::CallFunction: Unable to call function: stack
      //    overflow");
      return SetLastError(napi_generic_failure);
    }

    auto &stats = runtime_.getRuntimeStats();
    const vm::instrumentation::RAIITimer timer{
        "Incoming Function", stats, stats.incomingFunction};
    vm::ScopedNativeCallFrame newFrame{
        &runtime_,
        static_cast<uint32_t>(argCount),
        handle.getHermesValue(),
        vm::HermesValue::encodeUndefinedValue(),
        phv(object)};
    if (LLVM_UNLIKELY(newFrame.overflowed())) {
      CHECK_STATUS(runtime_.raiseStackOverflow(
          vm::StackRuntime::StackOverflowKind::NativeStack));
    }

    for (uint32_t i = 0; i < argCount; ++i) {
      newFrame->getArgRef(i) = phv(args[i]);
    }
    auto callRes = vm::Callable::call(handle, &runtime_);
    CHECK_STATUS(callRes.getStatus());

    *result = addStackValue(callRes->get());
    return ClearLastError();
  });

  // v8::Local<v8::Value> v8recv = v8impl::V8LocalValueFromJsValue(recv);

  // v8::Local<v8::Function> v8func;
  // CHECK_TO_FUNCTION(env, v8func, func);

  // auto maybe = v8func->Call(
  //     context,
  //     v8recv,
  //     argc,
  //     reinterpret_cast<v8::Local<v8::Value> *>(const_cast<napi_value
  //     *>(argv)));

  // if (try_catch.HasCaught()) {
  //   return napi_set_last_error(env, napi_pending_exception);
  // } else {
  //   if (result != nullptr) {
  //     CHECK_MAYBE_EMPTY(env, maybe, napi_generic_failure);
  //     *result = v8impl::JsValueFromV8LocalValue(maybe.ToLocalChecked());
  //   }
  //   return napi_clear_last_error(env);
  // }
}

napi_status NodeApiEnvironment::getGlobal(napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(result);
  *result = addStackValue(runtime_.getGlobal().getHermesValue());
  return ClearLastError();
}

napi_status NodeApiEnvironment::Throw(napi_value error) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, error);

  // v8::Isolate *isolate = env->isolate;

  // isolate->ThrowException(v8impl::V8LocalValueFromJsValue(error));
  // // any VM calls after this point and before returning
  // // to the javascript invoker will fail
  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::ThrowError(
    const char *code,
    const char *msg) noexcept {
  // NAPI_PREAMBLE(env);

  // v8::Isolate *isolate = env->isolate;
  // v8::Local<v8::String> str;
  // CHECK_NEW_FROM_UTF8(env, str, msg);

  // v8::Local<v8::Value> error_obj = v8::Exception::Error(str);
  // STATUS_CALL(set_error_code(env, error_obj, nullptr, code));

  // isolate->ThrowException(error_obj);
  // // any VM calls after this point and before returning
  // // to the javascript invoker will fail
  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::ThrowTypeError(
    const char *code,
    const char *msg) noexcept {
  // NAPI_PREAMBLE(env);

  // v8::Isolate *isolate = env->isolate;
  // v8::Local<v8::String> str;
  // CHECK_NEW_FROM_UTF8(env, str, msg);

  // v8::Local<v8::Value> error_obj = v8::Exception::TypeError(str);
  // STATUS_CALL(set_error_code(env, error_obj, nullptr, code));

  // isolate->ThrowException(error_obj);
  // // any VM calls after this point and before returning
  // // to the javascript invoker will fail
  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::ThrowRangeError(
    const char *code,
    const char *msg) noexcept {
  // NAPI_PREAMBLE(env);

  // v8::Isolate *isolate = env->isolate;
  // v8::Local<v8::String> str;
  // CHECK_NEW_FROM_UTF8(env, str, msg);

  // v8::Local<v8::Value> error_obj = v8::Exception::RangeError(str);
  // STATUS_CALL(set_error_code(env, error_obj, nullptr, code));

  // isolate->ThrowException(error_obj);
  // // any VM calls after this point and before returning
  // // to the javascript invoker will fail
  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::IsError(
    napi_value value,
    bool *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // // throw JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // *result = val->IsNativeError();

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetNumberValue(
    napi_value value,
    double *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_NUMBER_ARG(value);
  CHECK_ARG(result);

  *result = phv(value).getNumberAs<double>();
  return ClearLastError();
}

napi_status NodeApiEnvironment::GetNumberValue(
    napi_value value,
    int32_t *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_NUMBER_ARG(value);
  CHECK_ARG(result);

  *result = phv(value).getNumberAs<int32_t>();
  return ClearLastError();
}

napi_status NodeApiEnvironment::GetNumberValue(
    napi_value value,
    uint32_t *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_NUMBER_ARG(value);
  CHECK_ARG(result);

  *result = phv(value).getNumberAs<uint32_t>();
  return ClearLastError();
}

napi_status NodeApiEnvironment::GetNumberValue(
    napi_value value,
    int64_t *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_NUMBER_ARG(value);
  CHECK_ARG(result);

  *result = phv(value).getNumberAs<int64_t>();
  return ClearLastError();
}

napi_status NodeApiEnvironment::GetBoolValue(
    napi_value value,
    bool *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_BOOL_ARG(value);
  CHECK_ARG(result);

  *result = phv(value).getBool();
  return ClearLastError();
}

// Copies a JavaScript string into a LATIN-1 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status NodeApiEnvironment::GetValueStringLatin1(
    napi_value value,
    char *buf,
    size_t bufsize,
    size_t *result) noexcept {
  return handleExceptions([&] {
    CHECK_STRING_ARG(value);
    vm::Handle<vm::StringPrimitive> handle(
        &runtime_, stringHandle(value)->getString());
    auto view = vm::StringPrimitive::createStringView(&runtime_, handle);

    if (!buf) {
      CHECK_ARG(result);
      *result = view.length();
    } else if (bufsize != 0) {
      auto copied = std::min(bufsize - 1, view.length());
      for (auto cur = view.begin(), end = view.begin() + copied; cur < end;
           ++cur) {
        *buf++ = static_cast<char>(*cur);
      }
      *buf = '\0';
      if (result != nullptr) {
        *result = copied;
      }
    } else if (result != nullptr) {
      *result = 0;
    }

    return ClearLastError();
  });
}

static size_t utf8Length(llvh::ArrayRef<char16_t> input) {
  size_t length{0};
  for (auto cur = input.begin(), end = input.end(); cur < end; ++cur) {
    char16_t c = cur[0];
    // ASCII fast-path.
    if (LLVM_LIKELY(c <= 0x7F)) {
      ++length;
      continue;
    }

    char32_t c32;
    if (isLowSurrogate(c)) {
      // Unpaired low surrogate.
      c32 = UNICODE_REPLACEMENT_CHARACTER;
    } else if (isHighSurrogate(c)) {
      // Leading high surrogate. See if the next character is a low surrogate.
      if (cur + 1 == end || !isLowSurrogate(cur[1])) {
        // Trailing or unpaired high surrogate.
        c32 = UNICODE_REPLACEMENT_CHARACTER;
      } else {
        // Decode surrogate pair and increment, because we consumed two chars.
        c32 = decodeSurrogatePair(c, cur[1]);
        ++cur;
      }
    } else {
      // Not a surrogate.
      c32 = c;
    }

    if (c32 <= 0x7FF) {
      length += 2;
    } else if (c32 <= 0xFFFF) {
      length += 3;
    } else if (c32 <= 0x1FFFFF) {
      length += 4;
    } else if (c32 <= 0x3FFFFFF) {
      length += 5;
    } else {
      length += 6;
    }
  }

  return length;
}

static char *convertASCIIToUTF8(
    llvh::ArrayRef<char> input,
    char *buf,
    size_t maxCharacters) {
  char *curBuf = buf;
  char *endBuf = buf + maxCharacters;
  for (auto cur = input.begin(), end = input.end();
       cur < end && curBuf < endBuf;
       ++cur, ++curBuf) {
    *curBuf = *cur;
  }

  return curBuf;
}

static char *convertUTF16ToUTF8WithReplacements(
    llvh::ArrayRef<char16_t> input,
    char *buf,
    size_t maxCharacters) {
  char *curBuf = buf;
  char *endBuf = buf + maxCharacters;
  for (auto cur = input.begin(), end = input.end();
       cur < end && curBuf < endBuf;
       ++cur) {
    char16_t c = cur[0];
    // ASCII fast-path.
    if (LLVM_LIKELY(c <= 0x7F)) {
      *curBuf++ = static_cast<char>(c);
      continue;
    }

    char32_t c32;
    if (isLowSurrogate(c)) {
      // Unpaired low surrogate.
      c32 = UNICODE_REPLACEMENT_CHARACTER;
    } else if (isHighSurrogate(c)) {
      // Leading high surrogate. See if the next character is a low surrogate.
      if (cur + 1 == end || !isLowSurrogate(cur[1])) {
        // Trailing or unpaired high surrogate.
        c32 = UNICODE_REPLACEMENT_CHARACTER;
      } else {
        // Decode surrogate pair and increment, because we consumed two chars.
        c32 = decodeSurrogatePair(c, cur[1]);
        ++cur;
      }
    } else {
      // Not a surrogate.
      c32 = c;
    }

    char buff[UTF8CodepointMaxBytes];
    char *ptr = buff;
    encodeUTF8(ptr, c32);
    ptrdiff_t u8length = ptr - buff;
    if (curBuf + u8length <= endBuf) {
      for (auto u8ptr = buff; u8ptr < ptr; ++u8ptr) {
        *curBuf++ = *u8ptr;
      }
    } else {
      break;
    }
  }

  return curBuf;
}

// Copies a JavaScript string into a UTF-8 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status NodeApiEnvironment::GetValueStringUtf8(
    napi_value value,
    char *buf,
    size_t bufsize,
    size_t *result) noexcept {
  return handleExceptions([&] {
    CHECK_STRING_ARG(value);
    vm::Handle<vm::StringPrimitive> handle(
        &runtime_, stringHandle(value)->getString());
    auto view = vm::StringPrimitive::createStringView(&runtime_, handle);

    if (!buf) {
      CHECK_ARG(result);
      *result = view.isASCII() || view.length() == 0
          ? view.length()
          : utf8Length(vm::UTF16Ref(view.castToChar16Ptr(), view.length()));
    } else if (bufsize != 0) {
      char *end = view.length() > 0 ? view.isASCII()
              ? convertASCIIToUTF8(
                    vm::ASCIIRef(view.castToCharPtr(), view.length()),
                    buf,
                    bufsize - 1)
              : convertUTF16ToUTF8WithReplacements(
                    vm::UTF16Ref(view.castToChar16Ptr(), view.length()),
                    buf,
                    bufsize - 1)
                                    : buf;
      *end = '\0';
      if (result != nullptr) {
        *result = static_cast<size_t>(end - buf);
      }
    } else if (result != nullptr) {
      *result = 0;
    }

    return ClearLastError();
  });
}

// Copies a JavaScript string into a UTF-16 string buffer. The result is the
// number of 2-byte code units (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in 2-byte
// code units) via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status NodeApiEnvironment::GetValueStringUtf16(
    napi_value value,
    char16_t *buf,
    size_t bufsize,
    size_t *result) noexcept {
  return handleExceptions([&] {
    CHECK_STRING_ARG(value);
    vm::Handle<vm::StringPrimitive> handle(
        &runtime_, stringHandle(value)->getString());
    auto view = vm::StringPrimitive::createStringView(&runtime_, handle);

    if (!buf) {
      CHECK_ARG(result);
      *result = view.length();
    } else if (bufsize != 0) {
      auto copied = std::min(bufsize - 1, view.length());
      std::copy(view.begin(), view.begin() + copied, buf);
      buf[copied] = '\0';
      if (result != nullptr) {
        *result = copied;
      }
    } else if (result != nullptr) {
      *result = 0;
    }

    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::coerceToBool(
    napi_value value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    CHECK_ARG(result);
    bool res = vm::toBoolean(phv(value));
    STATUS_CALL(GetBoolean(res, result));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::coerceToNumber(
    napi_value value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    CHECK_ARG(result);
    auto res = vm::toNumber_RJS(&runtime_, toHandle(value));
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(*res);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::coerceToObject(
    napi_value value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    CHECK_ARG(result);
    auto res = vm::toObject(&runtime_, toHandle(value));
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(*res);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::coerceToString(
    napi_value value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    CHECK_ARG(result);
    auto res = vm::toString_RJS(&runtime_, toHandle(value));
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(vm::HermesValue::encodeStringValue(res->get()));
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::wrap(
    napi_value object,
    void *nativeObject,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_ref *result) noexcept {
  return wrapObject<WrapType::Retrievable>(
      object, nativeObject, finalizeCallback, finalizeHint, result);
}

napi_status NodeApiEnvironment::unwrap(
    napi_value object,
    void **result) noexcept {
  return unwrapObject(object, UnwrapAction::KeepWrap, result);
}

napi_status NodeApiEnvironment::removeWrap(
    napi_value object,
    void **result) noexcept {
  return unwrapObject(object, UnwrapAction::RemoveWrap, result);
}

napi_status NodeApiEnvironment::createExternal(
    void *data,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    auto objRes = vm::HostObject::createWithoutPrototype(
        &runtime_,
        std::make_unique<NapiHostObjectProxy>(
            *this, data, finalizeCallback, finalizeHint));
    CHECK_STATUS(objRes.getStatus());
    *result = addStackValue(*objRes);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::typeTagObject(
    napi_value object,
    const napi_type_tag *typeTag) noexcept {
  // NAPI_PREAMBLE(env);
  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;
  // CHECK_TO_OBJECT_WITH_PREAMBLE(env, context, obj, object);
  // CHECK_ARG_WITH_PREAMBLE(env, type_tag);

  // auto key = NAPI_PRIVATE_KEY(context, type_tag);
  // auto maybe_has = obj->HasPrivate(context, key);
  // CHECK_MAYBE_NOTHING_WITH_PREAMBLE(env, maybe_has, napi_generic_failure);
  // RETURN_STATUS_IF_FALSE_WITH_PREAMBLE(
  //     env, !maybe_has.FromJust(), napi_invalid_arg);

  // auto tag = v8::BigInt::NewFromWords(
  //     context, 0, 2, reinterpret_cast<const uint64_t *>(type_tag));
  // CHECK_MAYBE_EMPTY_WITH_PREAMBLE(env, tag, napi_generic_failure);

  // auto maybe_set = obj->SetPrivate(context, key, tag.ToLocalChecked());
  // CHECK_MAYBE_NOTHING_WITH_PREAMBLE(env, maybe_set, napi_generic_failure);
  // RETURN_STATUS_IF_FALSE_WITH_PREAMBLE(
  //     env, maybe_set.FromJust(), napi_generic_failure);

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::checkObjectTypeTag(
    napi_value object,
    const napi_type_tag *typeTag,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);
  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;
  // CHECK_TO_OBJECT_WITH_PREAMBLE(env, context, obj, object);
  // CHECK_ARG_WITH_PREAMBLE(env, type_tag);
  // CHECK_ARG_WITH_PREAMBLE(env, result);

  // auto maybe_value =
  //     obj->GetPrivate(context, NAPI_PRIVATE_KEY(context, type_tag));
  // CHECK_MAYBE_EMPTY_WITH_PREAMBLE(env, maybe_value, napi_generic_failure);
  // v8::Local<v8::Value> val = maybe_value.ToLocalChecked();

  // // We consider the type check to have failed unless we reach the line
  // below
  // // where we set whether the type check succeeded or not based on the
  // // comparison of the two type tags.
  // *result = false;
  // if (val->IsBigInt()) {
  //   int sign;
  //   int size = 2;
  //   napi_type_tag tag;
  //   val.As<v8::BigInt>()->ToWordsArray(
  //       &sign, &size, reinterpret_cast<uint64_t *>(&tag));
  //   if (size == 2 && sign == 0)
  //     *result = (tag.lower == type_tag->lower && tag.upper ==
  //     type_tag->upper);
  // }

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::getValueExternal(
    napi_value value,
    void **result) noexcept {
  return handleExceptions([&] {
    CHECK_EXTERNAL_ARG(value);
    CHECK_ARG(result);
    auto proxy = vm::vmcast<vm::HostObject>(phv(value))->getProxy();
    *result = static_cast<NapiHostObjectProxy *>(proxy)->data();
    return ClearLastError();
  });
}

// Set initial_refcount to 0 for a weak reference, >0 for a strong reference.
napi_status NodeApiEnvironment::createReference(
    napi_value value,
    uint32_t initialRefCount,
    napi_ref *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_OBJECT_ARG(value);
  CHECK_ARG(result);

  Reference *reference = Reference::create(
      this, phv(value), initialRefCount, /*deleteSelf:*/ false);

  *result = reinterpret_cast<napi_ref>(reference);
  return ClearLastError();
}

// Deletes a reference. The referenced value is released, and may be GC'd unless
// there are other references to it.
napi_status NodeApiEnvironment::deleteReference(napi_ref ref) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(ref);

  Reference::destroy(reinterpret_cast<Reference *>(ref));

  return ClearLastError();
}

// Increments the reference count, optionally returning the resulting count.
// After this call the reference will be a strong reference because its
// refcount is >0, and the referenced object is effectively "pinned".
// Calling this when the refcount is 0 and the object is unavailable
// results in an error.
napi_status NodeApiEnvironment::referenceRef(
    napi_ref ref,
    uint32_t *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(ref);

  Reference *reference = reinterpret_cast<Reference *>(ref);
  uint32_t count = reference->ref();

  if (result != nullptr) {
    *result = count;
  }

  return ClearLastError();
}

// Decrements the reference count, optionally returning the resulting count. If
// the result is 0 the reference is now weak and the object may be GC'd at any
// time if there are no other references. Calling this when the refcount is
// already 0 results in an error.
napi_status NodeApiEnvironment::referenceUnref(
    napi_ref ref,
    uint32_t *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(ref);

  Reference *reference = reinterpret_cast<Reference *>(ref);

  if (reference->refCount() == 0) {
    return SetLastError(napi_generic_failure);
  }

  uint32_t count = reference->unref();

  if (result != nullptr) {
    *result = count;
  }

  return ClearLastError();
}

// Attempts to get a referenced value. If the reference is weak, the value might
// no longer be available, in that case the call is still successful but the
// result is nullptr.
napi_status NodeApiEnvironment::getReferenceValue(
    napi_ref ref,
    napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(ref);
  CHECK_ARG(result);

  Reference *reference = reinterpret_cast<Reference *>(ref);
  *result = addStackValue(reference->get());

  return ClearLastError();
}

napi_status NodeApiEnvironment::OpenHandleScope(
    napi_handle_scope *result) noexcept {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because Hermes calls here cannot
  // throw JS exceptions.
  CHECK_ARG(result);

  Marker stackMarker = m_stackValues.create_marker();
  m_stackMarkers.emplace_back(std::move(stackMarker));
  *result = reinterpret_cast<napi_handle_scope>(&m_stackMarkers.back());
  return ClearLastError();
}

napi_status NodeApiEnvironment::CloseHandleScope(
    napi_handle_scope scope) noexcept {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because Hermes calls here cannot
  // throw JS exceptions.
  CHECK_ARG(scope);
  if (m_stackMarkers.empty()) {
    return napi_handle_scope_mismatch;
  }

  Marker &lastMarker = m_stackMarkers.back();
  if (reinterpret_cast<Marker *>(scope) != &lastMarker) {
    return napi_handle_scope_mismatch;
  }

  if (!m_stackValues.pop_marker(lastMarker)) {
    return napi_invalid_arg;
  }

  m_stackMarkers.pop_back();
  return ClearLastError();
}

napi_status NodeApiEnvironment::OpenEscapableHandleScope(
    napi_escapable_handle_scope *result) noexcept {
  CHECK_ARG(result);

  if (m_stackMarkers.empty()) {
    return napi_invalid_arg;
  }

  m_stackValues.emplace_back(); // value to escape to parent scope
  m_stackValues.emplace_back(
      vm::HermesValue::encodeNativeUInt32(kEscapeableSentinelNativeValue));

  return OpenHandleScope(reinterpret_cast<napi_handle_scope *>(result));
}

napi_status NodeApiEnvironment::CloseEscapableHandleScope(
    napi_escapable_handle_scope scope) noexcept {
  auto status = CloseHandleScope(reinterpret_cast<napi_handle_scope>(scope));

  if (status == napi_status::napi_ok) {
    auto &sentinelValue = m_stackValues.back();
    if (sentinelValue.isNativeValue()) {
      auto nativeValue = sentinelValue.getNativeUInt32();
      if (nativeValue == kEscapeableSentinelNativeValue ||
          nativeValue == kUsedEscapeableSentinelNativeValue) {
        m_stackValues.pop_back();
      } else {
        status = napi_handle_scope_mismatch;
      }
    }
  }

  return status;
}

napi_status NodeApiEnvironment::EscapeHandle(
    napi_escapable_handle_scope scope,
    napi_value escapee,
    napi_value *result) noexcept {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because Hermes calls here cannot
  // throw JS exceptions.
  CHECK_ARG(scope);
  CHECK_ARG(escapee);
  CHECK_ARG(result);

  Marker *marker = reinterpret_cast<Marker *>(scope);
  bool isValidMarker{false};
  m_stackMarkers.for_each(
      [&](const Marker &m) { isValidMarker |= &m == marker; });
  if (!isValidMarker) {
    return napi_invalid_arg;
  }

  Marker sentinelMarker = m_stackValues.get_previous_marker(*marker);
  if (!sentinelMarker.IsValid()) {
    return napi_invalid_arg;
  }
  Marker escapedValueMarker = m_stackValues.get_previous_marker(sentinelMarker);
  if (!escapedValueMarker.IsValid()) {
    return napi_invalid_arg;
  }

  vm::PinnedHermesValue *sentinelTag = m_stackValues.at(sentinelMarker);
  if (!sentinelTag || !sentinelTag->isNativeValue()) {
    return napi_invalid_arg;
  }
  if (sentinelTag->getNativeUInt32() != kUsedEscapeableSentinelNativeValue) {
    return SetLastError(napi_escape_called_twice);
  }
  if (sentinelTag->getNativeUInt32() != kEscapeableSentinelNativeValue) {
    return napi_invalid_arg;
  }

  vm::PinnedHermesValue *escapedValue = m_stackValues.at(escapedValueMarker);
  *escapedValue = *reinterpret_cast<vm::PinnedHermesValue *>(escapee);

  return ClearLastError();
}

napi_status NodeApiEnvironment::NewInstance(
    napi_value constructor,
    size_t argc,
    const napi_value *argv,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, constructor);
  // if (argc > 0) {
  //   CHECK_ARG(env, argv);
  // }
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();

  // v8::Local<v8::Function> ctor;
  // CHECK_TO_FUNCTION(env, ctor, constructor);

  // auto maybe = ctor->NewInstance(
  //     context,
  //     argc,
  //     reinterpret_cast<v8::Local<v8::Value> *>(const_cast<napi_value
  //     *>(argv)));

  // CHECK_MAYBE_EMPTY(env, maybe, napi_generic_failure);

  // *result = v8impl::JsValueFromV8LocalValue(maybe.ToLocalChecked());
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

vm::PinnedHermesValue &NodeApiEnvironment::phv(napi_value value) noexcept {
  return *reinterpret_cast<vm::PinnedHermesValue *>(value);
}

vm::Handle<vm::JSObject> NodeApiEnvironment::toObjectHandle(
    napi_value value) noexcept {
  return vm::Handle<vm::JSObject>::vmcast(&phv(value));
}

vm::Handle<vm::JSArray> NodeApiEnvironment::toArrayHandle(
    napi_value value) noexcept {
  return vm::Handle<vm::JSArray>::vmcast(&phv(value));
}

vm::Handle<vm::HermesValue> NodeApiEnvironment::stringHandle(
    napi_value value) noexcept {
  return vm::Handle<vm::HermesValue>::vmcast(&phv(value));
}

vm::Handle<vm::JSArray> NodeApiEnvironment::arrayHandle(
    napi_value value) noexcept {
  return vm::Handle<vm::JSArray>::vmcast(&phv(value));
}

vm::Handle<vm::HermesValue> NodeApiEnvironment::toHandle(
    const vm::HermesValue &value) noexcept {
  return runtime_.makeHandle(value);
}

vm::Handle<> NodeApiEnvironment::toHandle(napi_value value) noexcept {
  auto &hv = phv(value);
  if (hv.isUndefined()) {
    return vm::Runtime::getUndefinedValue();
  } else if (hv.isNull()) {
    return vm::Runtime::getNullValue();
  } else if (hv.isBool()) {
    return vm::Runtime::getBoolValue(hv.getBool());
  } else if (hv.isNumber()) {
    return runtime_.makeHandle(
        vm::HermesValue::encodeUntrustedDoubleValue(hv.getNumber()));
  } else if (hv.isSymbol() || hv.isString() || hv.isObject()) {
    return vm::Handle<vm::HermesValue>(&hv);
  } else {
    llvm_unreachable("unknown value kind");
  }
}

napi_status NodeApiEnvironment::InstanceOf(
    napi_value object,
    napi_value constructor,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_FUNCTION_ARG(constructor);
    CHECK_ARG(result);
    auto res = vm::instanceOfOperator_RJS(
        &runtime_,
        runtime_.makeHandle(phv(object)),
        runtime_.makeHandle(phv(constructor)));
    CHECK_STATUS(res.getStatus());
    *result = *res;
    return ClearLastError();
  });

  // v8::Local<v8::Object> ctor;
  // v8::Local<v8::Context> context = env->context();

  // CHECK_TO_OBJECT(env, context, ctor, constructor);

  // if (!ctor->IsFunction()) {
  //   napi_throw_type_error(
  //       env, "ERR_NAPI_CONS_FUNCTION", "Constructor must be a function");

  //   return napi_set_last_error(env, napi_function_expected);
  // }

  // napi_status status = napi_generic_failure;

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(object);
  // auto maybe_result = val->InstanceOf(context, ctor);
  // CHECK_MAYBE_NOTHING(env, maybe_result, status);
  // *result = maybe_result.FromJust();
  // return GET_RETURN_STATUS(env);
  // return napi_ok;
}

// Methods to support catching exceptions
napi_status NodeApiEnvironment::IsExceptionPending(bool *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(result);

  *result = !lastException_.isEmpty();
  return ClearLastError();
}

napi_status NodeApiEnvironment::GetAndClearLastException(
    napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(result);

  if (lastException_.isEmpty()) {
    return getUndefined(result);
  } else {
    *result = addStackValue(lastException_);
    lastException_ = EmptyHermesValue;
  }

  return ClearLastError();
}

napi_status NodeApiEnvironment::isArrayBuffer(
    napi_value value,
    bool *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_OBJECT_ARG(value);
  CHECK_ARG(result);

  *result = vm::vmisa<vm::JSArrayBuffer>(phv(value));
  return ClearLastError();
}

napi_status NodeApiEnvironment::CreateArrayBuffer(
    size_t byte_length,
    void **data,
    napi_value *result) noexcept {
  // CHECK_ARG(this, result);
  // return HandleExceptions([&] {
  //   vm::GCScope gcScope(&runtime_);
  //   vm::JSArrayBuffer::create(&runtime_, ) auto res =
  //       vm::JSArray::create(&runtime_, length, length);
  //   CHECK_STATUS(res.getStatus());
  //   *result = AddStackValue(res->getHermesValue());
  // });

  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Isolate *isolate = env->isolate;
  // v8::Local<v8::ArrayBuffer> buffer =
  //     v8::ArrayBuffer::New(isolate, byte_length);

  // // Optionally return a pointer to the buffer's data, to avoid another
  // call to
  // // retrieve it.
  // if (data != nullptr) {
  //   *data = buffer->GetBackingStore()->Data();
  // }

  // *result = v8impl::JsValueFromV8LocalValue(buffer);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateExternalArrayBuffer(
    void *external_data,
    size_t byte_length,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) noexcept {
  // // The API contract here is that the cleanup function runs on the JS
  // thread,
  // // and is able to use napi_env. Implementing that properly is hard, so
  // use the
  // // `Buffer` variant for easier implementation.
  // napi_value buffer;
  // STATUS_CALL(napi_create_external_buffer(
  //     env, byte_length, external_data, finalize_cb, finalize_hint,
  //     &buffer));
  // return napi_get_typedarray_info(
  //     env, buffer, nullptr, nullptr, nullptr, result, nullptr);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetArrayBufferInfo(
    napi_value arraybuffer,
    void **data,
    size_t *byte_length) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, arraybuffer);

  // v8::Local<v8::Value> value =
  // v8impl::V8LocalValueFromJsValue(arraybuffer); RETURN_STATUS_IF_FALSE(env,
  // value->IsArrayBuffer(), napi_invalid_arg);

  // std::shared_ptr<v8::BackingStore> backing_store =
  //     value.As<v8::ArrayBuffer>()->GetBackingStore();

  // if (data != nullptr) {
  //   *data = backing_store->Data();
  // }

  // if (byte_length != nullptr) {
  //   *byte_length = backing_store->ByteLength();
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::IsTypedArray(
    napi_value value,
    bool *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // *result = val->IsTypedArray();

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateTypedArray(
    napi_typedarray_type type,
    size_t length,
    napi_value arraybuffer,
    size_t byte_offset,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, arraybuffer);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> value =
  // v8impl::V8LocalValueFromJsValue(arraybuffer); RETURN_STATUS_IF_FALSE(env,
  // value->IsArrayBuffer(), napi_invalid_arg);

  // v8::Local<v8::ArrayBuffer> buffer = value.As<v8::ArrayBuffer>();
  // v8::Local<v8::TypedArray> typedArray;

  // switch (type) {
  //   case napi_int8_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Int8Array, 1, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_uint8_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Uint8Array, 1, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_uint8_clamped_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Uint8ClampedArray, 1, buffer, byte_offset, length,
  //         typedArray);
  //     break;
  //   case napi_int16_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Int16Array, 2, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_uint16_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Uint16Array, 2, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_int32_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Int32Array, 4, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_uint32_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Uint32Array, 4, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_float32_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Float32Array, 4, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_float64_array:
  //     CREATE_TYPED_ARRAY(
  //         env, Float64Array, 8, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_bigint64_array:
  //     CREATE_TYPED_ARRAY(
  //         env, BigInt64Array, 8, buffer, byte_offset, length, typedArray);
  //     break;
  //   case napi_biguint64_array:
  //     CREATE_TYPED_ARRAY(
  //         env, BigUint64Array, 8, buffer, byte_offset, length, typedArray);
  //     break;
  //   default:
  //     return napi_set_last_error(env, napi_invalid_arg);
  // }

  // *result = v8impl::JsValueFromV8LocalValue(typedArray);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetTypedArrayInfo(
    napi_value typedarray,
    napi_typedarray_type *type,
    size_t *length,
    void **data,
    napi_value *arraybuffer,
    size_t *byte_offset) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, typedarray);

  // v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(typedarray);
  // RETURN_STATUS_IF_FALSE(env, value->IsTypedArray(), napi_invalid_arg);

  // v8::Local<v8::TypedArray> array = value.As<v8::TypedArray>();

  // if (type != nullptr) {
  //   if (value->IsInt8Array()) {
  //     *type = napi_int8_array;
  //   } else if (value->IsUint8Array()) {
  //     *type = napi_uint8_array;
  //   } else if (value->IsUint8ClampedArray()) {
  //     *type = napi_uint8_clamped_array;
  //   } else if (value->IsInt16Array()) {
  //     *type = napi_int16_array;
  //   } else if (value->IsUint16Array()) {
  //     *type = napi_uint16_array;
  //   } else if (value->IsInt32Array()) {
  //     *type = napi_int32_array;
  //   } else if (value->IsUint32Array()) {
  //     *type = napi_uint32_array;
  //   } else if (value->IsFloat32Array()) {
  //     *type = napi_float32_array;
  //   } else if (value->IsFloat64Array()) {
  //     *type = napi_float64_array;
  //   } else if (value->IsBigInt64Array()) {
  //     *type = napi_bigint64_array;
  //   } else if (value->IsBigUint64Array()) {
  //     *type = napi_biguint64_array;
  //   }
  // }

  // if (length != nullptr) {
  //   *length = array->Length();
  // }

  // v8::Local<v8::ArrayBuffer> buffer;
  // if (data != nullptr || arraybuffer != nullptr) {
  //   // Calling Buffer() may have the side effect of allocating the buffer,
  //   // so only do this when it’s needed.
  //   buffer = array->Buffer();
  // }

  // if (data != nullptr) {
  //   *data = static_cast<uint8_t *>(buffer->GetBackingStore()->Data()) +
  //       array->ByteOffset();
  // }

  // if (arraybuffer != nullptr) {
  //   *arraybuffer = v8impl::JsValueFromV8LocalValue(buffer);
  // }

  // if (byte_offset != nullptr) {
  //   *byte_offset = array->ByteOffset();
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateDataView(
    size_t byte_length,
    napi_value arraybuffer,
    size_t byte_offset,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, arraybuffer);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> value =
  // v8impl::V8LocalValueFromJsValue(arraybuffer); RETURN_STATUS_IF_FALSE(env,
  // value->IsArrayBuffer(), napi_invalid_arg);

  // v8::Local<v8::ArrayBuffer> buffer = value.As<v8::ArrayBuffer>();
  // if (byte_length + byte_offset > buffer->ByteLength()) {
  //   napi_throw_range_error(
  //       env,
  //       "ERR_NAPI_INVALID_DATAVIEW_ARGS",
  //       "byte_offset + byte_length should be less than or "
  //       "equal to the size in bytes of the array passed in");
  //   return napi_set_last_error(env, napi_pending_exception);
  // }
  // v8::Local<v8::DataView> DataView =
  //     v8::DataView::New(buffer, byte_offset, byte_length);

  // *result = v8impl::JsValueFromV8LocalValue(DataView);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::IsDataView(
    napi_value value,
    bool *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // *result = val->IsDataView();

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetDataViewInfo(
    napi_value dataview,
    size_t *byte_length,
    void **data,
    napi_value *arraybuffer,
    size_t *byte_offset) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, dataview);

  // v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(dataview);
  // RETURN_STATUS_IF_FALSE(env, value->IsDataView(), napi_invalid_arg);

  // v8::Local<v8::DataView> array = value.As<v8::DataView>();

  // if (byte_length != nullptr) {
  //   *byte_length = array->ByteLength();
  // }

  // v8::Local<v8::ArrayBuffer> buffer;
  // if (data != nullptr || arraybuffer != nullptr) {
  //   // Calling Buffer() may have the side effect of allocating the buffer,
  //   // so only do this when it’s needed.
  //   buffer = array->Buffer();
  // }

  // if (data != nullptr) {
  //   *data = static_cast<uint8_t *>(buffer->GetBackingStore()->Data()) +
  //       array->ByteOffset();
  // }

  // if (arraybuffer != nullptr) {
  //   *arraybuffer = v8impl::JsValueFromV8LocalValue(buffer);
  // }

  // if (byte_offset != nullptr) {
  //   *byte_offset = array->ByteOffset();
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetVersion(uint32_t *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);
  // *result = NAPI_VERSION;
  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreatePromise(
    napi_deferred *deferred,
    napi_value *promise) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, deferred);
  // CHECK_ARG(env, promise);

  // auto maybe = v8::Promise::Resolver::New(env->context());
  // CHECK_MAYBE_EMPTY(env, maybe, napi_generic_failure);

  // auto v8_resolver = maybe.ToLocalChecked();
  // auto v8_deferred = new v8impl::Persistent<v8::Value>();
  // v8_deferred->Reset(env->isolate, v8_resolver);

  // *deferred = v8impl::JsDeferredFromNodePersistent(v8_deferred);
  // *promise = v8impl::JsValueFromV8LocalValue(v8_resolver->GetPromise());
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::ResolveDeferred(
    napi_deferred deferred,
    napi_value resolution) noexcept {
  // return v8impl::ConcludeDeferred(env, deferred, resolution, true);
  return napi_ok;
}

napi_status NodeApiEnvironment::RejectDeferred(
    napi_deferred deferred,
    napi_value resolution) noexcept {
  // return v8impl::ConcludeDeferred(env, deferred, resolution, false);
  return napi_ok;
}

napi_status NodeApiEnvironment::IsPromise(
    napi_value value,
    bool *is_promise) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, is_promise);

  // *is_promise = v8impl::V8LocalValueFromJsValue(value)->IsPromise();

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateDate(
    double time,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::MaybeLocal<v8::Value> maybe_date = v8::Date::New(env->context(),
  // time); CHECK_MAYBE_EMPTY(env, maybe_date, napi_generic_failure);

  // *result = v8impl::JsValueFromV8LocalValue(maybe_date.ToLocalChecked());

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::IsDate(
    napi_value value,
    bool *is_date) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, is_date);

  // *is_date = v8impl::V8LocalValueFromJsValue(value)->IsDate();

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetDateValue(
    napi_value value,
    double *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // RETURN_STATUS_IF_FALSE(env, val->IsDate(), napi_date_expected);

  // v8::Local<v8::Date> date = val.As<v8::Date>();
  // *result = date->ValueOf();

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::addFinalizer(
    napi_value object,
    void *nativeObject,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_ref *result) noexcept {
  return wrapObject<WrapType::Anonymous>(
      object, nativeObject, finalizeCallback, finalizeHint, result);
}

napi_status NodeApiEnvironment::AdjustExternalMemory(
    int64_t change_in_bytes,
    int64_t *adjusted_value) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, adjusted_value);

  // *adjusted_value =
  //     env->isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::SetInstanceData(
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint) noexcept {
  // CHECK_ENV(env);

  // v8impl::RefBase *old_data =
  //     static_cast<v8impl::RefBase *>(env->instance_data);
  // if (old_data != nullptr) {
  //   // Our contract so far has been to not finalize any old data there may
  //   be.
  //   // So we simply delete it.
  //   v8impl::RefBase::Delete(old_data);
  // }

  // env->instance_data =
  //     v8impl::RefBase::New(env, 0, true, finalize_cb, data, finalize_hint);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetInstanceData(void **data) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, data);

  // v8impl::RefBase *idata = static_cast<v8impl::RefBase
  // *>(env->instance_data);

  // *data = (idata == nullptr ? nullptr : idata->Data());

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::DetachArrayBuffer(
    napi_value arraybuffer) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, arraybuffer);

  // v8::Local<v8::Value> value =
  // v8impl::V8LocalValueFromJsValue(arraybuffer); RETURN_STATUS_IF_FALSE(
  //     env, value->IsArrayBuffer(), napi_arraybuffer_expected);

  // v8::Local<v8::ArrayBuffer> it = value.As<v8::ArrayBuffer>();
  // RETURN_STATUS_IF_FALSE(
  //     env, it->IsDetachable(), napi_detachable_arraybuffer_expected);

  // it->Detach();

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::IsDetachedArrayBuffer(
    napi_value arraybuffer,
    bool *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, arraybuffer);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> value =
  // v8impl::V8LocalValueFromJsValue(arraybuffer);

  // *result = value->IsArrayBuffer() &&
  //     value.As<v8::ArrayBuffer>()->GetBackingStore()->Data() == nullptr;

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::runScript(
    napi_value source,
    const char *sourceURL,
    napi_value *result) noexcept {
  size_t sourceSize{};
  STATUS_CALL(GetValueStringUtf8(source, nullptr, 0, &sourceSize));
  auto buffer = std::make_unique<std::vector<uint8_t>>();
  buffer->assign(sourceSize + 1, '\0');
  STATUS_CALL(GetValueStringUtf8(
      source,
      reinterpret_cast<char *>(buffer->data()),
      sourceSize + 1,
      nullptr));
  STATUS_CALL(runScriptWithSourceMap(
      makeHermesBuffer(
          reinterpret_cast<napi_env>(this),
          reinterpret_cast<napi_ext_buffer>(buffer.release()),
          [](napi_env /*env*/,
             napi_ext_buffer buffer,
             const uint8_t **buffer_start,
             size_t *buffer_length) {
            auto buf = reinterpret_cast<std::vector<uint8_t> *>(buffer);
            *buffer_start = buf->data();
            *buffer_length = buf->size() - 1;
          },
          [](napi_env /*env*/, napi_ext_buffer buffer) {
            std::unique_ptr<std::vector<uint8_t>> buf(
                reinterpret_cast<std::vector<uint8_t> *>(buffer));
          }),
      nullptr,
      sourceURL,
      result));
  return ClearLastError();
}

napi_status NodeApiEnvironment::runSerializedScript(
    const uint8_t *buffer,
    size_t bufferLength,
    napi_value /*source*/,
    const char *sourceURL,
    napi_value *result) noexcept {
  auto bufferCopy = std::make_unique<std::vector<uint8_t>>();
  bufferCopy->assign(bufferLength, '\0');
  std::copy(buffer, buffer + bufferLength, bufferCopy->data());
  STATUS_CALL(runScriptWithSourceMap(
      makeHermesBuffer(
          reinterpret_cast<napi_env>(this),
          reinterpret_cast<napi_ext_buffer>(bufferCopy.release()),
          [](napi_env /*env*/,
             napi_ext_buffer buffer,
             const uint8_t **buffer_start,
             size_t *buffer_length) {
            auto buf = reinterpret_cast<std::vector<uint8_t> *>(buffer);
            *buffer_start = buf->data();
            *buffer_length = buf->size();
          },
          [](napi_env /*env*/, napi_ext_buffer buffer) {
            std::unique_ptr<std::vector<uint8_t>> buf(
                reinterpret_cast<std::vector<uint8_t> *>(buffer));
          }),
      nullptr,
      sourceURL,
      result));
  return ClearLastError();
}

napi_status NodeApiEnvironment::serializeScript(
    napi_value source,
    const char *sourceURL,
    napi_ext_buffer_callback bufferCallback,
    void *bufferHint) noexcept {
  size_t sourceSize{};
  STATUS_CALL(GetValueStringUtf8(source, nullptr, 0, &sourceSize));
  auto buffer = std::make_unique<std::vector<uint8_t>>();
  buffer->assign(sourceSize + 1, '\0');
  STATUS_CALL(GetValueStringUtf8(
      source,
      reinterpret_cast<char *>(buffer->data()),
      sourceSize + 1,
      nullptr));
  napi_ext_prepared_script preparedScript{};
  STATUS_CALL(prepareScriptWithSourceMap(
      makeHermesBuffer(
          reinterpret_cast<napi_env>(this),
          reinterpret_cast<napi_ext_buffer>(buffer.release()),
          [](napi_env /*env*/,
             napi_ext_buffer buffer,
             const uint8_t **buffer_start,
             size_t *buffer_length) {
            auto buf = reinterpret_cast<std::vector<uint8_t> *>(buffer);
            *buffer_start = buf->data();
            *buffer_length = buf->size() - 1;
          },
          [](napi_env /*env*/, napi_ext_buffer buffer) {
            std::unique_ptr<std::vector<uint8_t>> buf(
                reinterpret_cast<std::vector<uint8_t> *>(buffer));
          }),
      nullptr,
      sourceURL,
      &preparedScript));
  STATUS_CALL(
      serializePreparedScript(preparedScript, bufferCallback, bufferHint));
  return ClearLastError();

  return ClearLastError();
}

napi_status NodeApiEnvironment::runScriptWithSourceMap(
    std::unique_ptr<HermesBuffer> script,
    std::unique_ptr<HermesBuffer> sourceMap,
    const char *sourceURL,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    napi_ext_prepared_script preparedScript{nullptr};
    STATUS_CALL(prepareScriptWithSourceMap(
        std::move(script), std::move(sourceMap), sourceURL, &preparedScript));
    STATUS_CALL(runPreparedScript(preparedScript, result));
    return ClearLastError();
  });
}

/*static*/ bool NodeApiEnvironment::isHermesBytecode(
    const uint8_t *data,
    size_t len) noexcept {
  return hbc::BCProviderFromBuffer::isBytecodeStream(
      llvh::ArrayRef<uint8_t>(data, len));
}

napi_status NodeApiEnvironment::prepareScriptWithSourceMap(
    std::unique_ptr<HermesBuffer> buffer,
    std::unique_ptr<HermesBuffer> sourceMapBuf,
    const char *sourceURL,
    napi_ext_prepared_script *preparedScript) noexcept {
  std::pair<std::unique_ptr<hbc::BCProvider>, std::string> bcErr{};
  vm::RuntimeModuleFlags runtimeFlags{};
  runtimeFlags.persistent = true;

  bool isBytecode = isHermesBytecode(buffer->data(), buffer->size());
#ifdef HERMESVM_PLATFORM_LOGGING
  hermesLog(
      "HermesVM", "Prepare JS on %s.", isBytecode ? "bytecode" : "source");
#endif
  // Save the first few bytes of the buffer so that we can later append them
  // to any error message.
  uint8_t bufPrefix[16];
  const size_t bufSize = buffer->size();
  memcpy(bufPrefix, buffer->data(), std::min(sizeof(bufPrefix), bufSize));

  // Construct the BC provider either from buffer or source.
  if (isBytecode) {
    if (sourceMapBuf) {
      return SetLastError(
          napi_generic_failure,
          0,
          "Source map cannot be specified with bytecode");
    }
    bcErr = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
        std::move(buffer));
  } else {
#if defined(HERMESVM_LEAN)
    bcErr.second = "prepareJavaScript source compilation not supported";
#else
    std::unique_ptr<SourceMap> sourceMap{};
    if (sourceMapBuf) {
      // Convert the buffer into a form the parser needs.
      llvh::MemoryBufferRef mbref(
          llvh::StringRef(
              (const char *)sourceMapBuf->data(), sourceMapBuf->size()),
          "");
      SimpleDiagHandler diag;
      SourceErrorManager sm;
      diag.installInto(sm);
      sourceMap = SourceMapParser::parse(mbref, sm);
      if (!sourceMap) {
        auto errorStr = diag.getErrorString();
        LOG_EXCEPTION_CAUSE("Error parsing source map: %s", errorStr.c_str());
        return SetLastError(napi_generic_failure);
        // TODO: throw std::runtime_error("Error parsing source map:" +
        // errorStr);
      }
    }
    bcErr = hbc::BCProviderFromSrc::createBCProviderFromSrc(
        std::move(buffer),
        std::string(sourceURL ? sourceURL : ""),
        std::move(sourceMap),
        compileFlags_);
#endif
  }
  if (!bcErr.first) {
    std::string storage;
    llvh::raw_string_ostream os(storage);
    os << " Buffer size " << bufSize << " starts with: ";
    for (size_t i = 0; i < sizeof(bufPrefix) && i < bufSize; ++i)
      os << llvh::format_hex_no_prefix(bufPrefix[i], 2);
    LOG_EXCEPTION_CAUSE(
        "Compiling JS failed: %s, %s", bcErr.second.c_str(), os.str().c_str());
    // throw jsi::JSINativeException(
    //    "Compiling JS failed: " + std::move(bcErr.second) + os.str());
    return SetLastError(napi_generic_failure);
  }
  *preparedScript =
      reinterpret_cast<napi_ext_prepared_script>(new HermesPreparedJavaScript(
          std::move(bcErr.first),
          runtimeFlags,
          sourceURL ? sourceURL : "",
          isBytecode));
  return ClearLastError();
}

napi_status NodeApiEnvironment::runPreparedScript(
    napi_ext_prepared_script preparedScript,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(preparedScript);
    CHECK_ARG(result);
    auto &stats = runtime_.getRuntimeStats();
    const vm::instrumentation::RAIITimer timer{
        "Evaluate JS", stats, stats.evaluateJS};
    const auto *hermesPrep =
        reinterpret_cast<HermesPreparedJavaScript *>(preparedScript);
    auto res = runtime_.runBytecode(
        hermesPrep->bytecodeProvider(),
        hermesPrep->runtimeFlags(),
        hermesPrep->sourceURL(),
        vm::Runtime::makeNullHandle<vm::Environment>());
    CHECK_STATUS(res.getStatus());
    *result = addStackValue(*res);
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::deletePreparedScript(
    napi_ext_prepared_script preparedScript) noexcept {
  CHECK_ARG(preparedScript);
  delete reinterpret_cast<HermesPreparedJavaScript *>(preparedScript);
  return ClearLastError();
}

napi_status NodeApiEnvironment::serializePreparedScript(
    napi_ext_prepared_script preparedScript,
    napi_ext_buffer_callback bufferCallback,
    void *bufferHint) noexcept {
  CHECK_ARG(preparedScript);
  CHECK_ARG(bufferCallback);

  auto *hermesPreparedScript =
      reinterpret_cast<HermesPreparedJavaScript *>(preparedScript);

  if (hermesPreparedScript->isBytecode()) {
    auto bytecodeProvider = std::static_pointer_cast<hbc::BCProviderFromBuffer>(
        hermesPreparedScript->bytecodeProvider());
    auto bufferRef = bytecodeProvider->getRawBuffer();
    bufferCallback(
        reinterpret_cast<napi_env>(this),
        bufferRef.data(),
        bufferRef.size(),
        bufferHint);
  } else {
    auto bytecodeProvider = std::static_pointer_cast<hbc::BCProviderFromSrc>(
        hermesPreparedScript->bytecodeProvider());
    auto *bcModule = bytecodeProvider->getBytecodeModule();

    // Serialize/deserialize can't handle lazy compilation as of now. Do a
    // check to make sure there is no lazy BytecodeFunction in module_.
    for (uint32_t i = 0; i < bcModule->getNumFunctions(); i++) {
      if (bytecodeProvider->isFunctionLazy(i)) {
        hermes_fatal("Cannot serialize lazy functions");
      }
    }

    // Serialize the bytecode. Call BytecodeSerializer to do the heavy
    // lifting. Write to a SmallVector first, so we can know the total bytes
    // and write it first and make life easier for Deserializer. This is going
    // to be slower than writing to Serializer directly but it's OK to slow
    // down serialization if it speeds up Deserializer.
    auto bytecodeGenOpts = BytecodeGenerationOptions::defaults();
    llvh::SmallVector<char, 0> bytecodeVector;
    llvh::raw_svector_ostream OS(bytecodeVector);
    hbc::BytecodeSerializer BS{OS, bytecodeGenOpts};
    BS.serialize(*bcModule, bytecodeProvider->getSourceHash());
    bufferCallback(
        reinterpret_cast<napi_env>(this),
        reinterpret_cast<uint8_t *>(bytecodeVector.data()),
        bytecodeVector.size(),
        bufferHint);
  }

  return ClearLastError();
}

} // namespace napi
} // namespace hermes

//=============================================================================
// NAPI implementation
//=============================================================================

napi_status napi_get_last_error_info(
    napi_env env,
    const napi_extended_error_info **result) {
  return CHECKED_ENV(env)->GetLastErrorInfo(result);
}

napi_status napi_create_function(
    napi_env env,
    const char *utf8name,
    size_t length,
    napi_callback cb,
    void *callback_data,
    napi_value *result) {
  return CHECKED_ENV(env)->createFunction(
      utf8name, length, cb, callback_data, result);
}

napi_status napi_define_class(
    napi_env env,
    const char *utf8name,
    size_t length,
    napi_callback constructor,
    void *callback_data,
    size_t property_count,
    const napi_property_descriptor *properties,
    napi_value *result) {
  return CHECKED_ENV(env)->defineClass(
      utf8name,
      length,
      constructor,
      callback_data,
      property_count,
      properties,
      result);
}

napi_status
napi_get_property_names(napi_env env, napi_value object, napi_value *result) {
  return CHECKED_ENV(env)->getPropertyNames(object, result);
}

napi_status napi_get_all_property_names(
    napi_env env,
    napi_value object,
    napi_key_collection_mode key_mode,
    napi_key_filter key_filter,
    napi_key_conversion key_conversion,
    napi_value *result) {
  return CHECKED_ENV(env)->getAllPropertyNames(
      object, key_mode, key_filter, key_conversion, result);
}

napi_status napi_set_property(
    napi_env env,
    napi_value object,
    napi_value key,
    napi_value value) {
  return CHECKED_ENV(env)->setProperty(object, key, value);
}

napi_status napi_has_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->hasProperty(object, key, result);
}

napi_status napi_get_property(
    napi_env env,
    napi_value object,
    napi_value key,
    napi_value *result) {
  return CHECKED_ENV(env)->getProperty(object, key, result);
}

napi_status napi_delete_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->deleteProperty(object, key, result);
}

napi_status napi_has_own_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->hasOwnProperty(object, key, result);
}

napi_status napi_set_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    napi_value value) {
  return CHECKED_ENV(env)->setNamedProperty(object, utf8name, value);
}

napi_status napi_has_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    bool *result) {
  return CHECKED_ENV(env)->hasNamedProperty(object, utf8name, result);
}

napi_status napi_get_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    napi_value *result) {
  return CHECKED_ENV(env)->getNamedProperty(object, utf8name, result);
}

napi_status napi_set_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    napi_value value) {
  return CHECKED_ENV(env)->setElement(object, index, value);
}

napi_status napi_has_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    bool *result) {
  return CHECKED_ENV(env)->hasElement(object, index, result);
}

napi_status napi_get_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    napi_value *result) {
  return CHECKED_ENV(env)->getElement(object, index, result);
}

napi_status napi_delete_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    bool *result) {
  return CHECKED_ENV(env)->deleteElement(object, index, result);
}

napi_status napi_define_properties(
    napi_env env,
    napi_value object,
    size_t property_count,
    const napi_property_descriptor *properties) {
  return CHECKED_ENV(env)->defineProperties(object, property_count, properties);
}

napi_status napi_object_freeze(napi_env env, napi_value object) {
  return CHECKED_ENV(env)->objectFreeze(object);
}

napi_status napi_object_seal(napi_env env, napi_value object) {
  return CHECKED_ENV(env)->objectSeal(object);
}

napi_status napi_is_array(napi_env env, napi_value value, bool *result) {
  return CHECKED_ENV(env)->isArray(value, result);
}

napi_status
napi_get_array_length(napi_env env, napi_value value, uint32_t *result) {
  return CHECKED_ENV(env)->getArrayLength(value, result);
}

napi_status
napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs, bool *result) {
  return CHECKED_ENV(env)->strictEquals(lhs, rhs, result);
}

napi_status
napi_get_prototype(napi_env env, napi_value object, napi_value *result) {
  return CHECKED_ENV(env)->getPrototype(object, result);
}

napi_status napi_create_object(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->createObject(result);
}

napi_status napi_create_array(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->createArray(result);
}

napi_status
napi_create_array_with_length(napi_env env, size_t length, napi_value *result) {
  return CHECKED_ENV(env)->createArray(length, result);
}

napi_status napi_create_string_latin1(
    napi_env env,
    const char *str,
    size_t length,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateStringLatin1(str, length, result);
}

napi_status napi_create_string_utf8(
    napi_env env,
    const char *str,
    size_t length,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateStringUtf8(str, length, result);
}

napi_status napi_create_string_utf16(
    napi_env env,
    const char16_t *str,
    size_t length,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateStringUtf16(str, length, result);
}

napi_status napi_create_double(napi_env env, double value, napi_value *result) {
  return CHECKED_ENV(env)->CreateNumber(value, result);
}

napi_status napi_create_int32(napi_env env, int32_t value, napi_value *result) {
  return CHECKED_ENV(env)->CreateNumber(value, result);
}

napi_status
napi_create_uint32(napi_env env, uint32_t value, napi_value *result) {
  return CHECKED_ENV(env)->CreateNumber(value, result);
}

napi_status napi_create_int64(napi_env env, int64_t value, napi_value *result) {
  return CHECKED_ENV(env)->CreateNumber(value, result);
}

napi_status
napi_create_bigint_int64(napi_env env, int64_t value, napi_value *result) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->SetLastError(napi_generic_failure);
}

napi_status
napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value *result) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->SetLastError(napi_generic_failure);
}

napi_status napi_create_bigint_words(
    napi_env env,
    int sign_bit,
    size_t word_count,
    const uint64_t *words,
    napi_value *result) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->SetLastError(napi_generic_failure);
}

napi_status napi_get_boolean(napi_env env, bool value, napi_value *result) {
  return CHECKED_ENV(env)->GetBoolean(value, result);
}

napi_status
napi_create_symbol(napi_env env, napi_value description, napi_value *result) {
  return CHECKED_ENV(env)->createSymbol(description, result);
}

napi_status napi_create_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateError(code, msg, result);
}

napi_status napi_create_type_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateTypeError(code, msg, result);
}

napi_status napi_create_range_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateRangeError(code, msg, result);
}

napi_status
napi_typeof(napi_env env, napi_value value, napi_valuetype *result) {
  return CHECKED_ENV(env)->typeOf(value, result);
}

napi_status napi_get_undefined(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->getUndefined(result);
}

napi_status napi_get_null(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->getNull(result);
}

napi_status napi_get_cb_info(
    napi_env env,
    napi_callback_info cbinfo,
    size_t *argc,
    napi_value *argv,
    napi_value *this_arg,
    void **data) {
  return CHECKED_ENV(env)->getCallbackInfo(
      reinterpret_cast<hermes::napi::CallbackInfo *>(cbinfo),
      argc,
      argv,
      this_arg,
      data);
}

napi_status napi_get_new_target(
    napi_env env,
    napi_callback_info cbinfo,
    napi_value *result) {
  return CHECKED_ENV(env)->getNewTarget(
      reinterpret_cast<hermes::napi::CallbackInfo *>(cbinfo), result);
}

napi_status napi_call_function(
    napi_env env,
    napi_value recv,
    napi_value func,
    size_t argc,
    const napi_value *argv,
    napi_value *result) {
  return CHECKED_ENV(env)->callFunction(recv, func, argc, argv, result);
}

napi_status napi_get_global(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->getGlobal(result);
}

napi_status napi_throw(napi_env env, napi_value error) {
  return CHECKED_ENV(env)->Throw(error);
}

napi_status napi_throw_error(napi_env env, const char *code, const char *msg) {
  return CHECKED_ENV(env)->ThrowError(code, msg);
}

napi_status
napi_throw_type_error(napi_env env, const char *code, const char *msg) {
  return CHECKED_ENV(env)->ThrowTypeError(code, msg);
}

napi_status
napi_throw_range_error(napi_env env, const char *code, const char *msg) {
  return CHECKED_ENV(env)->ThrowRangeError(code, msg);
}

napi_status napi_is_error(napi_env env, napi_value value, bool *result) {
  return CHECKED_ENV(env)->IsError(value, result);
}

napi_status
napi_get_value_double(napi_env env, napi_value value, double *result) {
  return CHECKED_ENV(env)->GetNumberValue(value, result);
}

napi_status
napi_get_value_int32(napi_env env, napi_value value, int32_t *result) {
  return CHECKED_ENV(env)->GetNumberValue(value, result);
}

napi_status
napi_get_value_uint32(napi_env env, napi_value value, uint32_t *result) {
  return CHECKED_ENV(env)->GetNumberValue(value, result);
}

napi_status
napi_get_value_int64(napi_env env, napi_value value, int64_t *result) {
  return CHECKED_ENV(env)->GetNumberValue(value, result);
}

napi_status napi_get_value_bigint_int64(
    napi_env env,
    napi_value value,
    int64_t *result,
    bool *lossless) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->SetLastError(napi_generic_failure);
}

napi_status napi_get_value_bigint_uint64(
    napi_env env,
    napi_value value,
    uint64_t *result,
    bool *lossless) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->SetLastError(napi_generic_failure);
}

napi_status napi_get_value_bigint_words(
    napi_env env,
    napi_value value,
    int *sign_bit,
    size_t *word_count,
    uint64_t *words) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->SetLastError(napi_generic_failure);
}

napi_status napi_get_value_bool(napi_env env, napi_value value, bool *result) {
  return CHECKED_ENV(env)->GetBoolValue(value, result);
}

// Copies a JavaScript string into a LATIN-1 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status napi_get_value_string_latin1(
    napi_env env,
    napi_value value,
    char *buf,
    size_t bufsize,
    size_t *result) {
  return CHECKED_ENV(env)->GetValueStringLatin1(value, buf, bufsize, result);
}

// Copies a JavaScript string into a UTF-8 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status napi_get_value_string_utf8(
    napi_env env,
    napi_value value,
    char *buf,
    size_t bufsize,
    size_t *result) {
  return CHECKED_ENV(env)->GetValueStringUtf8(value, buf, bufsize, result);
}

// Copies a JavaScript string into a UTF-16 string buffer. The result is the
// number of 2-byte code units (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in 2-byte
// code units) via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status napi_get_value_string_utf16(
    napi_env env,
    napi_value value,
    char16_t *buf,
    size_t bufsize,
    size_t *result) {
  return CHECKED_ENV(env)->GetValueStringUtf16(value, buf, bufsize, result);
}

napi_status
napi_coerce_to_bool(napi_env env, napi_value value, napi_value *result) {
  return CHECKED_ENV(env)->coerceToBool(value, result);
}

#define GEN_COERCE_FUNCTION(UpperCaseName, MixedCaseName, LowerCaseName) \
  napi_status napi_coerce_to_##LowerCaseName(                            \
      napi_env env, napi_value value, napi_value *result) {              \
    return CHECKED_ENV(env)->coerceTo##MixedCaseName(value, result);     \
  }

GEN_COERCE_FUNCTION(NUMBER, Number, number)
GEN_COERCE_FUNCTION(OBJECT, Object, object)
GEN_COERCE_FUNCTION(STRING, String, string)

#undef GEN_COERCE_FUNCTION

napi_status napi_wrap(
    napi_env env,
    napi_value js_object,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) {
  return CHECKED_ENV(env)->wrap(
      js_object, native_object, finalize_cb, finalize_hint, result);
}

napi_status napi_unwrap(napi_env env, napi_value obj, void **result) {
  return CHECKED_ENV(env)->unwrap(obj, result);
}

napi_status napi_remove_wrap(napi_env env, napi_value obj, void **result) {
  return CHECKED_ENV(env)->removeWrap(obj, result);
}

napi_status napi_create_external(
    napi_env env,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  return CHECKED_ENV(env)->createExternal(
      data, finalize_cb, finalize_hint, result);
}

NAPI_EXTERN napi_status napi_type_tag_object(
    napi_env env,
    napi_value object,
    const napi_type_tag *type_tag) {
  return CHECKED_ENV(env)->typeTagObject(object, type_tag);
}

NAPI_EXTERN napi_status napi_check_object_type_tag(
    napi_env env,
    napi_value object,
    const napi_type_tag *type_tag,
    bool *result) {
  return CHECKED_ENV(env)->checkObjectTypeTag(object, type_tag, result);
}

napi_status
napi_get_value_external(napi_env env, napi_value value, void **result) {
  return CHECKED_ENV(env)->getValueExternal(value, result);
}

// Set initial_refcount to 0 for a weak reference, >0 for a strong reference.
napi_status napi_create_reference(
    napi_env env,
    napi_value value,
    uint32_t initial_refcount,
    napi_ref *result) {
  return CHECKED_ENV(env)->createReference(value, initial_refcount, result);
}

// Deletes a reference. The referenced value is released, and may be GC'd unless
// there are other references to it.
napi_status napi_delete_reference(napi_env env, napi_ref ref) {
  return CHECKED_ENV(env)->deleteReference(ref);
}

// Increments the reference count, optionally returning the resulting count.
// After this call the reference will be a strong reference because its
// refcount is >0, and the referenced object is effectively "pinned".
// Calling this when the refcount is 0 and the object is unavailable
// results in an error.
napi_status napi_reference_ref(napi_env env, napi_ref ref, uint32_t *result) {
  return CHECKED_ENV(env)->referenceRef(ref, result);
}

// Decrements the reference count, optionally returning the resulting count. If
// the result is 0 the reference is now weak and the object may be GC'd at any
// time if there are no other references. Calling this when the refcount is
// already 0 results in an error.
napi_status napi_reference_unref(napi_env env, napi_ref ref, uint32_t *result) {
  return CHECKED_ENV(env)->referenceUnref(ref, result);
}

// Attempts to get a referenced value. If the reference is weak, the value might
// no longer be available, in that case the call is still successful but the
// result is NULL.
napi_status
napi_get_reference_value(napi_env env, napi_ref ref, napi_value *result) {
  return CHECKED_ENV(env)->getReferenceValue(ref, result);
}

napi_status napi_open_handle_scope(napi_env env, napi_handle_scope *result) {
  return CHECKED_ENV(env)->OpenHandleScope(result);
}

napi_status napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
  return CHECKED_ENV(env)->CloseHandleScope(scope);
}

napi_status napi_open_escapable_handle_scope(
    napi_env env,
    napi_escapable_handle_scope *result) {
  return CHECKED_ENV(env)->OpenEscapableHandleScope(result);
}

napi_status napi_close_escapable_handle_scope(
    napi_env env,
    napi_escapable_handle_scope scope) {
  return CHECKED_ENV(env)->CloseEscapableHandleScope(scope);
}

napi_status napi_escape_handle(
    napi_env env,
    napi_escapable_handle_scope scope,
    napi_value escapee,
    napi_value *result) {
  return CHECKED_ENV(env)->EscapeHandle(scope, escapee, result);
}

napi_status napi_new_instance(
    napi_env env,
    napi_value constructor,
    size_t argc,
    const napi_value *argv,
    napi_value *result) {
  return CHECKED_ENV(env)->NewInstance(constructor, argc, argv, result);
}

napi_status napi_instanceof(
    napi_env env,
    napi_value object,
    napi_value constructor,
    bool *result) {
  return CHECKED_ENV(env)->InstanceOf(object, constructor, result);
}

// Methods to support catching exceptions
napi_status napi_is_exception_pending(napi_env env, bool *result) {
  return CHECKED_ENV(env)->IsExceptionPending(result);
}

napi_status napi_get_and_clear_last_exception(
    napi_env env,
    napi_value *result) {
  return CHECKED_ENV(env)->GetAndClearLastException(result);
}

napi_status napi_is_arraybuffer(napi_env env, napi_value value, bool *result) {
  return CHECKED_ENV(env)->isArrayBuffer(value, result);
}

napi_status napi_create_arraybuffer(
    napi_env env,
    size_t byte_length,
    void **data,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateArrayBuffer(byte_length, data, result);
}

napi_status napi_create_external_arraybuffer(
    napi_env env,
    void *external_data,
    size_t byte_length,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateExternalArrayBuffer(
      external_data, byte_length, finalize_cb, finalize_hint, result);
}

napi_status napi_get_arraybuffer_info(
    napi_env env,
    napi_value arraybuffer,
    void **data,
    size_t *byte_length) {
  return CHECKED_ENV(env)->GetArrayBufferInfo(arraybuffer, data, byte_length);
}

napi_status napi_is_typedarray(napi_env env, napi_value value, bool *result) {
  return CHECKED_ENV(env)->IsTypedArray(value, result);
}

napi_status napi_create_typedarray(
    napi_env env,
    napi_typedarray_type type,
    size_t length,
    napi_value arraybuffer,
    size_t byte_offset,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateTypedArray(
      type, length, arraybuffer, byte_offset, result);
}

napi_status napi_get_typedarray_info(
    napi_env env,
    napi_value typedarray,
    napi_typedarray_type *type,
    size_t *length,
    void **data,
    napi_value *arraybuffer,
    size_t *byte_offset) {
  return CHECKED_ENV(env)->GetTypedArrayInfo(
      typedarray, type, length, data, arraybuffer, byte_offset);
}

napi_status napi_create_dataview(
    napi_env env,
    size_t byte_length,
    napi_value arraybuffer,
    size_t byte_offset,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateDataView(
      byte_length, arraybuffer, byte_offset, result);
}

napi_status napi_is_dataview(napi_env env, napi_value value, bool *result) {
  return CHECKED_ENV(env)->IsDataView(value, result);
}

napi_status napi_get_dataview_info(
    napi_env env,
    napi_value dataview,
    size_t *byte_length,
    void **data,
    napi_value *arraybuffer,
    size_t *byte_offset) {
  return CHECKED_ENV(env)->GetDataViewInfo(
      dataview, byte_length, data, arraybuffer, byte_offset);
}

napi_status napi_get_version(napi_env env, uint32_t *result) {
  return CHECKED_ENV(env)->GetVersion(result);
}

napi_status napi_create_promise(
    napi_env env,
    napi_deferred *deferred,
    napi_value *promise) {
  return CHECKED_ENV(env)->CreatePromise(deferred, promise);
}

napi_status napi_resolve_deferred(
    napi_env env,
    napi_deferred deferred,
    napi_value resolution) {
  return CHECKED_ENV(env)->ResolveDeferred(deferred, resolution);
}

napi_status napi_reject_deferred(
    napi_env env,
    napi_deferred deferred,
    napi_value resolution) {
  return CHECKED_ENV(env)->RejectDeferred(deferred, resolution);
}

napi_status napi_is_promise(napi_env env, napi_value value, bool *is_promise) {
  return CHECKED_ENV(env)->IsPromise(value, is_promise);
}

napi_status napi_create_date(napi_env env, double time, napi_value *result) {
  return CHECKED_ENV(env)->CreateDate(time, result);
}

napi_status napi_is_date(napi_env env, napi_value value, bool *is_date) {
  return CHECKED_ENV(env)->IsDate(value, is_date);
}

napi_status
napi_get_date_value(napi_env env, napi_value value, double *result) {
  return CHECKED_ENV(env)->GetDateValue(value, result);
}

napi_status
napi_run_script(napi_env env, napi_value script, napi_value *result) {
  return CHECKED_ENV(env)->runScript(script, nullptr, result);
}

napi_status napi_add_finalizer(
    napi_env env,
    napi_value js_object,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) {
  return CHECKED_ENV(env)->addFinalizer(
      js_object, native_object, finalize_cb, finalize_hint, result);
}

napi_status napi_adjust_external_memory(
    napi_env env,
    int64_t change_in_bytes,
    int64_t *adjusted_value) {
  return CHECKED_ENV(env)->AdjustExternalMemory(
      change_in_bytes, adjusted_value);
}

napi_status napi_set_instance_data(
    napi_env env,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint) {
  return CHECKED_ENV(env)->SetInstanceData(data, finalize_cb, finalize_hint);
}

napi_status napi_get_instance_data(napi_env env, void **data) {
  return CHECKED_ENV(env)->GetInstanceData(data);
}

napi_status napi_detach_arraybuffer(napi_env env, napi_value arraybuffer) {
  return CHECKED_ENV(env)->DetachArrayBuffer(arraybuffer);
}

napi_status napi_is_detached_arraybuffer(
    napi_env env,
    napi_value arraybuffer,
    bool *result) {
  return CHECKED_ENV(env)->IsDetachedArrayBuffer(arraybuffer, result);
}

//=============================================================================
// Node-API extensions to host JS engine and to implement JSI
//=============================================================================

napi_status napi_create_hermes_env(napi_env *env) {
  if (!env) {
    return napi_status::napi_invalid_arg;
  }
  *env = reinterpret_cast<napi_env>(new hermes::napi::NodeApiEnvironment());
  return napi_status::napi_ok;
}

napi_status napi_ext_env_ref(napi_env env) {
  return CHECKED_ENV(env)->Ref();
}

napi_status napi_ext_env_unref(napi_env env) {
  return CHECKED_ENV(env)->Unref();
}

// Runs script with the provided source_url origin.
napi_status __cdecl napi_ext_run_script(
    napi_env env,
    napi_value source,
    const char *source_url,
    napi_value *result) {
  return CHECKED_ENV(env)->runScript(source, source_url, result);
}

// Runs serialized script with the provided source_url origin.
napi_status __cdecl napi_ext_run_serialized_script(
    napi_env env,
    const uint8_t *buffer,
    size_t buffer_length,
    napi_value source,
    const char *source_url,
    napi_value *result) {
  return CHECKED_ENV(env)->runSerializedScript(
      buffer, buffer_length, source, source_url, result);
}

// Creates a serialized script.
napi_status __cdecl napi_ext_serialize_script(
    napi_env env,
    napi_value source,
    const char *source_url,
    napi_ext_buffer_callback buffer_cb,
    void *buffer_hint) {
  return CHECKED_ENV(env)->serializeScript(
      source, source_url, buffer_cb, buffer_hint);
}

// Run the script with the source map that can be used for the script debugging.
napi_status __cdecl napi_ext_run_script_with_source_map(
    napi_env env,
    napi_ext_buffer script,
    napi_ext_get_buffer_range get_script_range,
    napi_ext_delete_buffer delete_script,
    napi_ext_buffer source_map,
    napi_ext_get_buffer_range get_source_map_range,
    napi_ext_delete_buffer delete_source_map,
    const char *source_url,
    napi_value *result) {
  return CHECKED_ENV(env)->runScriptWithSourceMap(
      hermes::napi::makeHermesBuffer(
          env, script, get_script_range, delete_script),
      hermes::napi::makeHermesBuffer(
          env, source_map, get_source_map_range, delete_source_map),
      source_url,
      result);
}

// Prepare the script for running.
napi_status __cdecl napi_ext_prepare_script_with_source_map(
    napi_env env,
    napi_ext_buffer script,
    napi_ext_get_buffer_range get_script_range,
    napi_ext_delete_buffer delete_script,
    napi_ext_buffer source_map,
    napi_ext_get_buffer_range get_source_map_range,
    napi_ext_delete_buffer delete_source_map,
    const char *source_url,
    napi_ext_prepared_script *prepared_script) {
  return CHECKED_ENV(env)->prepareScriptWithSourceMap(
      hermes::napi::makeHermesBuffer(
          env, script, get_script_range, delete_script),
      hermes::napi::makeHermesBuffer(
          env, source_map, get_source_map_range, delete_source_map),
      source_url,
      prepared_script);
}

// Run the prepared script.
napi_status __cdecl napi_ext_run_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script,
    napi_value *result) {
  return CHECKED_ENV(env)->runPreparedScript(prepared_script, result);
}

// Delete the prepared script.
napi_status __cdecl napi_ext_delete_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script) {
  return CHECKED_ENV(env)->deletePreparedScript(prepared_script);
}

// Serialize the prepared script.
napi_status __cdecl napi_ext_serialize_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script,
    napi_ext_buffer_callback buffer_cb,
    void *buffer_hint) {
  return CHECKED_ENV(env)->serializePreparedScript(
      prepared_script, buffer_cb, buffer_hint);
}
