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

#include "hermes/VM/StringPrimitive.h"

#include "llvh/Support/ConvertUTF.h"

#include "hermes_napi.h"

#define RETURN_STATUS_IF_FALSE(env, condition, status) \
  do {                                                 \
    if (!(condition)) {                                \
      return env->SetLastError((status));              \
    }                                                  \
  } while (0)

#define CHECK_ENV(env)         \
  do {                         \
    if ((env) == nullptr) {    \
      return napi_invalid_arg; \
    }                          \
  } while (0)

#define CHECKED_ENV(env)                \
  ((env) == nullptr) ? napi_invalid_arg \
                     : reinterpret_cast<NodeApiEnvironment *>(env)

#define CHECK_ARG(env, arg) \
  RETURN_STATUS_IF_FALSE((env), ((arg) != nullptr), napi_invalid_arg)

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

struct RefTracker {
  RefTracker() {}
  virtual ~RefTracker() {}
  virtual void Finalize(bool isEnvTeardown) {}

  using RefList = RefTracker;

  void Link(RefList *list) {
    prev_ = list;
    next_ = list->next_;
    if (next_ != nullptr) {
      next_->prev_ = this;
    }
    list->next_ = this;
  }

  void Unlink() {
    if (prev_ != nullptr) {
      prev_->next_ = next_;
    }
    if (next_ != nullptr) {
      next_->prev_ = prev_;
    }
    prev_ = nullptr;
    next_ = nullptr;
  }

  static void FinalizeAll(RefList *list) {
    while (list->next_ != nullptr) {
      list->next_->Finalize(true);
    }
  }

 private:
  RefList *next_ = nullptr;
  RefList *prev_ = nullptr;
};

struct NodeApiEnvironment {
  explicit NodeApiEnvironment(
      const hermes::vm::RuntimeConfig &runtimeConfig = {}) noexcept;
  virtual ~NodeApiEnvironment();

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
  RefTracker::RefList reflist;
  RefTracker::RefList finalizing_reflist;
  napi_extended_error_info last_error;
  // int open_callback_scopes = 0;
  void *instance_data{nullptr};

  napi_status SetLastError(
      napi_status error_code,
      uint32_t engine_error_code = 0,
      void *engine_reserved = nullptr) noexcept;
  napi_status ClearLastError() noexcept;

  napi_status GetLastErrorInfo(
      const napi_extended_error_info **result) noexcept;

  napi_status CreateFunction(
      const char *utf8name,
      size_t length,
      napi_callback cb,
      void *callback_data,
      napi_value *result) noexcept;

  napi_status DefineClass(
      const char *utf8name,
      size_t length,
      napi_callback constructor,
      void *callback_data,
      size_t property_count,
      const napi_property_descriptor *properties,
      napi_value *result) noexcept;

  napi_status GetPropertyNames(napi_value object, napi_value *result) noexcept;

  napi_status GetAllPropertyNames(
      napi_value object,
      napi_key_collection_mode key_mode,
      napi_key_filter key_filter,
      napi_key_conversion key_conversion,
      napi_value *result) noexcept;

  napi_status
  SetProperty(napi_value object, napi_value key, napi_value value) noexcept;

  napi_status
  HasProperty(napi_value object, napi_value key, bool *result) noexcept;

  napi_status
  GetProperty(napi_value object, napi_value key, napi_value *result) noexcept;

  napi_status
  DeleteProperty(napi_value object, napi_value key, bool *result) noexcept;

  napi_status
  HasOwnProperty(napi_value object, napi_value key, bool *result) noexcept;

  napi_status SetNamedProperty(
      napi_value object,
      const char *utf8name,
      napi_value value) noexcept;

  napi_status HasNamedProperty(
      napi_value object,
      const char *utf8name,
      bool *result) noexcept;

  napi_status GetNamedProperty(
      napi_value object,
      const char *utf8name,
      napi_value *result) noexcept;

  napi_status
  SetElement(napi_value object, uint32_t index, napi_value value) noexcept;

  napi_status
  HasElement(napi_value object, uint32_t index, bool *result) noexcept;

  napi_status
  GetElement(napi_value object, uint32_t index, napi_value *result) noexcept;

  napi_status
  DeleteElement(napi_value object, uint32_t index, bool *result) noexcept;

  napi_status DefineProperties(
      napi_value object,
      size_t property_count,
      const napi_property_descriptor *properties) noexcept;

  napi_status ObjectFreeze(napi_value object) noexcept;

  napi_status ObjectSeal(napi_value object) noexcept;

  napi_status IsArray(napi_value value, bool *result) noexcept;

  napi_status GetArrayLength(napi_value value, uint32_t *result) noexcept;

  napi_status
  StrictEquals(napi_value lhs, napi_value rhs, bool *result) noexcept;

  napi_status GetPrototype(napi_value object, napi_value *result) noexcept;

  napi_status CreateObject(napi_value *result) noexcept;

  napi_status CreateArray(napi_value *result) noexcept;

  napi_status CreateArray(size_t length, napi_value *result) noexcept;

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

  napi_status CreateBigInt(int64_t value, napi_value *result) noexcept;

  napi_status CreateBigInt(uint64_t value, napi_value *result) noexcept;

  napi_status CreateBigInt(
      int sign_bit,
      size_t word_count,
      const uint64_t *words,
      napi_value *result) noexcept;

  napi_status GetBoolean(bool value, napi_value *result) noexcept;

  napi_status CreateSymbol(napi_value description, napi_value *result) noexcept;

  napi_status
  CreateError(napi_value code, napi_value msg, napi_value *result) noexcept;

  napi_status
  CreateTypeError(napi_value code, napi_value msg, napi_value *result) noexcept;

  napi_status CreateRangeError(
      napi_value code,
      napi_value msg,
      napi_value *result) noexcept;

  napi_status TypeOf(napi_value value, napi_valuetype *result) noexcept;

  napi_status GetUndefined(napi_value *result) noexcept;

  napi_status GetNull(napi_value *result) noexcept;

  napi_status GetCallbackInfo(
      napi_callback_info cbinfo,
      size_t *argc,
      napi_value *argv,
      napi_value *this_arg,
      void **data) noexcept;

  napi_status GetNewTarget(
      napi_callback_info cbinfo,
      napi_value *result) noexcept;

  napi_status CallFunction(
      napi_value recv,
      napi_value func,
      size_t argc,
      const napi_value *argv,
      napi_value *result) noexcept;

  napi_status GetGlobal(napi_value *result) noexcept;

  napi_status Throw(napi_value error) noexcept;

  napi_status ThrowError(const char *code, const char *msg) noexcept;

  napi_status ThrowTypeError(const char *code, const char *msg) noexcept;

  napi_status ThrowRangeError(const char *code, const char *msg) noexcept;

  napi_status IsError(napi_value value, bool *result) noexcept;

  napi_status GetNumberValue(napi_value value, double *result) noexcept;

  napi_status GetNumberValue(napi_value value, int32_t *result) noexcept;

  napi_status GetNumberValue(napi_value value, uint32_t *result) noexcept;

  napi_status GetNumberValue(napi_value value, int64_t *result) noexcept;

  napi_status
  GetBigIntValue(napi_value value, int64_t *result, bool *lossless) noexcept;

  napi_status
  GetBigIntValue(napi_value value, uint64_t *result, bool *lossless) noexcept;

  napi_status GetBigIntValue(
      napi_value value,
      int *sign_bit,
      size_t *word_count,
      uint64_t *words) noexcept;

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

  napi_status CoerceToBool(napi_value value, napi_value *result) noexcept;
  napi_status CoerceToNumber(napi_value value, napi_value *result) noexcept;
  napi_status CoerceToObject(napi_value value, napi_value *result) noexcept;
  napi_status CoerceToString(napi_value value, napi_value *result) noexcept;

  napi_status Wrap(
      napi_value js_object,
      void *native_object,
      napi_finalize finalize_cb,
      void *finalize_hint,
      napi_ref *result) noexcept;

  napi_status Unwrap(napi_value obj, void **result) noexcept;

  napi_status RemoveWrap(napi_value obj, void **result) noexcept;

  napi_status CreateExternal(
      void *data,
      napi_finalize finalize_cb,
      void *finalize_hint,
      napi_value *result) noexcept;

  napi_status TypeTagObject(
      napi_value object,
      const napi_type_tag *type_tag) noexcept;

  napi_status CheckObjectTypeTag(
      napi_value object,
      const napi_type_tag *type_tag,
      bool *result) noexcept;

  napi_status GetValueExternal(napi_value value, void **result) noexcept;

  napi_status CreateReference(
      napi_value value,
      uint32_t initial_refcount,
      napi_ref *result) noexcept;

  napi_status DeleteReference(napi_ref ref) noexcept;

  napi_status ReferenceRef(napi_ref ref, uint32_t *result) noexcept;

  napi_status ReferenceUnref(napi_ref ref, uint32_t *result) noexcept;

  napi_status GetReferenceValue(napi_ref ref, napi_value *result) noexcept;

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

  napi_status IsArrayBuffer(napi_value value, bool *result) noexcept;

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

  napi_status RunScript(napi_value script, napi_value *result) noexcept;

  napi_status AddFinalizer(
      napi_value js_object,
      void *native_object,
      napi_finalize finalize_cb,
      void *finalize_hint,
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

  // Utility
  hermes::vm::HermesValue StringHVFromAscii(const char *str, size_t length);
  hermes::vm::HermesValue StringHVFromLatin1(const char *str, size_t length);
  hermes::vm::HermesValue StringHVFromUtf8(const uint8_t *utf8, size_t length);
  napi_value AddStackValue(hermes::vm::HermesValue value) noexcept;

  template <typename F>
  napi_status HandleExceptions(const F &f) {
    try {
#ifdef HERMESVM_EXCEPTION_ON_OOM
      try {
        f();
      } catch (const ::hermes::vm::JSOutOfMemoryError &ex) {
        // We surface this as a JSINativeException -- the out of memory
        // exception is not part of the spec.
        throw ::facebook::jsi::JSINativeException(ex.what());
      }
#else // HERMESVM_EXCEPTION_ON_OOM
      f();
#endif
      return ClearLastError();
    } catch (...) {
      return napi_generic_failure;
    }
  }

  void CheckStatus(hermes::vm::ExecutionStatus status) {
    if (LLVM_LIKELY(status != hermes::vm::ExecutionStatus::EXCEPTION)) {
      return;
    }

    // TODO: [vmoroz] implement
    // jsi::Value exception = valueFromHermesValue(runtime_.getThrownValue());
    // runtime_.clearThrownValue();
    // // Here, we increment the depth to detect recursion in error handling.
    // vm::ScopedNativeDepthTracker depthTracker{&runtime_};
    // if (LLVM_LIKELY(!depthTracker.overflowed())) {
    //   auto ex = jsi::JSError(*this, std::move(exception));
    //   LOG_EXCEPTION_CAUSE("JSI rethrowing JS exception: %s", ex.what());
    //   throw ex;
    // }

    // (void)runtime_.raiseStackOverflow(
    //     vm::Runtime::StackOverflowKind::NativeStack);
    // exception = valueFromHermesValue(runtime_.getThrownValue());
    // runtime_.clearThrownValue();
    // // Here, we give us a little more room so we can call into JS to
    // // populate the JSError members.
    // vm::ScopedNativeDepthReducer reducer(&runtime_);
    // throw jsi::JSError(*this, std::move(exception));
  }

 private:
#ifdef HERMESJSI_ON_STACK
  StackRuntime stackRuntime_;
#else
  std::shared_ptr<::hermes::vm::Runtime> rt_;
#endif
  ::hermes::vm::Runtime &runtime_;
#ifdef HERMES_ENABLE_DEBUGGER
  friend class debugger::Debugger;
  std::unique_ptr<debugger::Debugger> debugger_;
#endif
  ::hermes::vm::experiments::VMExperimentFlags vmExperimentFlags_{0};
  std::shared_ptr<hermes::vm::CrashManager> crashMgr_;

  /// Compilation flags used by prepareJavaScript().
  hermes::hbc::CompileFlags compileFlags_{};
  /// The default setting of "emit async break check" in this runtime.
  bool defaultEmitAsyncBreakCheck_{false};

  std::atomic<int> m_refs{1};

 public:
  NonMovableObjStack<hermes::vm::PinnedHermesValue> m_stackValues;
  NonMovableObjStack<Marker> m_stackMarkers;
  static constexpr uint32_t kEscapeableSentinelNativeValue = 0x35456789;
  static constexpr uint32_t kUsedEscapeableSentinelNativeValue =
      kEscapeableSentinelNativeValue + 1;
};

// Reference counter implementation.
struct ExtRefCounter : protected RefTracker {
  ~ExtRefCounter() override {
    Unlink();
  }

  void Ref() {
    ++ref_count_;
  }

  void Unref() {
    if (--ref_count_ == 0) {
      Finalize(false);
    }
  }

  virtual hermes::vm::PinnedHermesValue *Get(NodeApiEnvironment *env) = 0;

 protected:
  ExtRefCounter(NodeApiEnvironment *env) {
    Link(&env->reflist);
  }

  void Finalize(bool is_env_teardown) override {
    delete this;
  }

 private:
  uint32_t ref_count_{1};
};

// Wrapper around v8impl::Persistent that implements reference counting.
struct ExtReference : protected ExtRefCounter {
  static ExtReference *New(
      NodeApiEnvironment *env,
      hermes::vm::PinnedHermesValue &value) {
    return new ExtReference(env, value);
  }

  hermes::vm::PinnedHermesValue *Get(NodeApiEnvironment * /*env*/) override {
    return &persistent_;
  }

 protected:
  ExtReference(NodeApiEnvironment *env, hermes::vm::PinnedHermesValue value)
      : ExtRefCounter(env), persistent_(value) {}

 private:
  hermes::vm::PinnedHermesValue persistent_;
};

// Associates data with ExtReference.
struct ExtReferenceWithData : protected ExtReference {
  static ExtReferenceWithData *New(
      NodeApiEnvironment *env,
      hermes::vm::PinnedHermesValue value,
      void *native_object,
      napi_finalize finalize_cb,
      void *finalize_hint) {
    return new ExtReferenceWithData(
        env, value, native_object, finalize_cb, finalize_hint);
  }

 protected:
  ExtReferenceWithData(
      NodeApiEnvironment *env,
      hermes::vm::PinnedHermesValue value,
      void *native_object,
      napi_finalize finalize_cb,
      void *finalize_hint)
      : ExtReference(env, value),
        env_{env},
        native_object_{native_object},
        finalize_cb_{finalize_cb},
        finalize_hint_{finalize_hint} {}

  void Finalize(bool is_env_teardown) override {
    if (finalize_cb_) {
      finalize_cb_(
          reinterpret_cast<napi_env>(env_), native_object_, finalize_hint_);
      finalize_cb_ = nullptr;
    }
    ExtReference::Finalize(is_env_teardown);
  }

 private:
  NodeApiEnvironment *env_{nullptr};
  void *native_object_{nullptr};
  napi_finalize finalize_cb_{nullptr};
  void *finalize_hint_{nullptr};
};

// Wrapper around v8impl::Persistent that implements reference counting.
// TODO:
// struct ExtWeakReference : protected ExtRefCounter {
//   static ExtWeakReference *New(napi_env env,
//   hermes::vm::WeakRef<hermes::vm::HermesValue> value) {
//     return new ExtWeakReference(env, value);
//   }

//   ~ExtWeakReference() override {
//     napi_delete_reference(env_, weak_ref_);
//   }

//   v8::Local<v8::Value> Get(napi_env env) override {
//     napi_value result{};
//     napi_get_reference_value(env, weak_ref_, &result);
//     return result ? v8impl::V8LocalValueFromJsValue(result) :
//     v8::Local<v8::Value>();
//   }

//  protected:
//   ExtWeakReference(napi_env env, v8::Local<v8::Value> value) :
//   ExtRefCounter(env), env_{env} {
//     napi_create_reference(env, v8impl::JsValueFromV8LocalValue(value), 0,
//     &weak_ref_);
//   }

//  private:
//   napi_env env_{nullptr};
//   napi_ref weak_ref_{nullptr};
// };

// Adapter for napi_finalize callbacks.
class Finalizer {
 public:
  // Some Finalizers are run during shutdown when the napi_env is destroyed,
  // and some need to keep an explicit reference to the napi_env because they
  // are run independently.
  enum EnvReferenceMode { kNoEnvReference, kKeepEnvReference };

 protected:
  Finalizer(
      NodeApiEnvironment *env,
      napi_finalize finalize_callback,
      void *finalize_data,
      void *finalize_hint,
      EnvReferenceMode refmode = kNoEnvReference)
      : _env(env),
        _finalize_callback(finalize_callback),
        _finalize_data(finalize_data),
        _finalize_hint(finalize_hint),
        _has_env_reference(refmode == kKeepEnvReference) {
    if (_has_env_reference)
      _env->Ref();
  }

  ~Finalizer() {
    if (_has_env_reference)
      _env->Unref();
  }

 public:
  static Finalizer *New(
      NodeApiEnvironment *env,
      napi_finalize finalize_callback = nullptr,
      void *finalize_data = nullptr,
      void *finalize_hint = nullptr,
      EnvReferenceMode refmode = kNoEnvReference) {
    return new Finalizer(
        env, finalize_callback, finalize_data, finalize_hint, refmode);
  }

  static void Delete(Finalizer *finalizer) {
    delete finalizer;
  }

 protected:
  NodeApiEnvironment *_env;
  napi_finalize _finalize_callback;
  void *_finalize_data;
  void *_finalize_hint;
  bool _finalize_ran = false;
  bool _has_env_reference = false;
};

// Wrapper around v8impl::Persistent that implements reference counting.
class RefBase : protected Finalizer, RefTracker {
 protected:
  RefBase(
      NodeApiEnvironment *env,
      uint32_t initial_refcount,
      bool delete_self,
      napi_finalize finalize_callback,
      void *finalize_data,
      void *finalize_hint)
      : Finalizer(env, finalize_callback, finalize_data, finalize_hint),
        _refcount(initial_refcount),
        _delete_self(delete_self) {
    Link(
        finalize_callback == nullptr ? &env->reflist
                                     : &env->finalizing_reflist);
  }

 public:
  static RefBase *New(
      NodeApiEnvironment *env,
      uint32_t initial_refcount,
      bool delete_self,
      napi_finalize finalize_callback,
      void *finalize_data,
      void *finalize_hint) {
    return new RefBase(
        env,
        initial_refcount,
        delete_self,
        finalize_callback,
        finalize_data,
        finalize_hint);
  }

  virtual ~RefBase() {
    Unlink();
  }

  inline void *Data() {
    return _finalize_data;
  }

  // Delete is called in 2 ways. Either from the finalizer or
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
  static inline void Delete(RefBase *reference) {
    if ((reference->RefCount() != 0) || (reference->_delete_self) ||
        (reference->_finalize_ran)) {
      delete reference;
    } else {
      // defer until finalizer runs as
      // it may alread be queued
      reference->_delete_self = true;
    }
  }

  inline uint32_t Ref() {
    return ++_refcount;
  }

  inline uint32_t Unref() {
    if (_refcount == 0) {
      return 0;
    }
    return --_refcount;
  }

  inline uint32_t RefCount() {
    return _refcount;
  }

 protected:
  inline void Finalize(bool is_env_teardown = false) override {
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
    if (is_env_teardown && RefCount() > 0)
      _refcount = 0;

    if (_finalize_callback != nullptr) {
      // This ensures that we never call the finalizer twice.
      napi_finalize fini = _finalize_callback;
      _finalize_callback = nullptr;
      // TODO: [vmoroz] Implement
      //_env->CallFinalizer(fini, _finalize_data, _finalize_hint);
    }

    // this is safe because if a request to delete the reference
    // is made in the finalize_callback it will defer deletion
    // to this block and set _delete_self to true
    if (_delete_self || is_env_teardown) {
      Delete(this);
    } else {
      _finalize_ran = true;
    }
  }

 private:
  uint32_t _refcount;
  bool _delete_self;
};

class Reference : public RefBase {
 protected:
  template <typename... Args>
  Reference(
      NodeApiEnvironment *env,
      hermes::vm::PinnedHermesValue value,
      Args &&...args)
      : RefBase(env, std::forward<Args>(args)...), _persistent(value) {
    if (RefCount() == 0) {
      // TODO: [vmoroz] Implement
      // _persistent.SetWeak(
      //     this, FinalizeCallback, v8::WeakCallbackType::kParameter);
    }
  }

 public:
  static inline Reference *New(
      NodeApiEnvironment *env,
      hermes::vm::PinnedHermesValue value,
      uint32_t initial_refcount,
      bool delete_self,
      napi_finalize finalize_callback = nullptr,
      void *finalize_data = nullptr,
      void *finalize_hint = nullptr) {
    return new Reference(
        env,
        value,
        initial_refcount,
        delete_self,
        finalize_callback,
        finalize_data,
        finalize_hint);
  }

  inline uint32_t Ref() {
    uint32_t refcount = RefBase::Ref();
    // TODO: [vmoroz] Implement
    // if (refcount == 1) {
    //   _persistent.ClearWeak();
    // }
    return refcount;
  }

  inline uint32_t Unref() {
    uint32_t old_refcount = RefCount();
    uint32_t refcount = RefBase::Unref();
    if (old_refcount == 1 && refcount == 0) {
      // TODO: [vmoroz] Implement
      // _persistent.SetWeak(
      //     this, FinalizeCallback, v8::WeakCallbackType::kParameter);
    }
    return refcount;
  }

  hermes::vm::PinnedHermesValue *Get() {
    // TODO: [vmoroz] Implement
    // if (_persistent.IsEmpty()) {
    //   return v8::Local<v8::Value>();
    // } else {
    //   return v8::Local<v8::Value>::New(_env->isolate, _persistent);
    // }
    return nullptr;
  }

 protected:
  inline void Finalize(bool is_env_teardown = false) override {
    // During env teardown, `~napi_env()` alone is responsible for finalizing.
    // Thus, we don't want any stray gc passes to trigger a second call to
    // `Finalize()`, so let's reset the persistent here if nothing is
    // keeping it alive.
    // TODO: [vmoroz] Implement
    // if (is_env_teardown && _persistent.IsWeak()) {
    //   _persistent.ClearWeak();
    // }

    // Chain up to perform the rest of the finalization.
    RefBase::Finalize(is_env_teardown);
  }

 private:
  // The N-API finalizer callback may make calls into the engine. V8's heap is
  // not in a consistent state during the weak callback, and therefore it does
  // not support calls back into it. However, it provides a mechanism for adding
  // a finalizer which may make calls back into the engine by allowing us to
  // attach such a second-pass finalizer from the first pass finalizer. Thus,
  // we do that here to ensure that the N-API finalizer callback is free to call
  // into the engine.
  // TODO: [vmoroz] Implement
  // static void FinalizeCallback(const v8::WeakCallbackInfo<Reference> &data) {
  //   Reference *reference = data.GetParameter();

  //   // The reference must be reset during the first pass.
  //   // TODO: [vmoroz] Implement
  //   // reference->_persistent.Reset();

  //   // TODO: [vmoroz] Implement
  //   // data.SetSecondPassCallback(SecondPassCallback);
  // }

  // TODO: [vmoroz] Implement
  // static void SecondPassCallback(const v8::WeakCallbackInfo<Reference>& data)
  // {
  //   data.GetParameter()->Finalize();
  // }

  hermes::vm::PinnedHermesValue _persistent;
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
    (512 * 1024 - sizeof(::hermes::vm::Runtime) - 4096 * 8) /
    sizeof(::hermes::vm::PinnedHermesValue);
} // namespace

NodeApiEnvironment::NodeApiEnvironment(
    const hermes::vm::RuntimeConfig &runtimeConfig) noexcept
    :
// TODO: pass parameters
#ifdef HERMESJSI_ON_STACK
      stackRuntime_(runtimeConfig),
      runtime_(stackRuntime_.getRuntime()),
#else
      rt_(hermes::vm::Runtime::create(runtimeConfig.rebuild()
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
    case hermes::vm::SmartCompilation:
      compileFlags_.lazy = true;
      // (Leaves thresholds at default values)
      break;
    case hermes::vm::ForceEagerCompilation:
      compileFlags_.lazy = false;
      break;
    case hermes::vm::ForceLazyCompilation:
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
  crashMgr_->registerMemory(&runtime_, sizeof(hermes::vm::Runtime));
#endif
  runtime_.addCustomRootsFunction(
      [this](hermes::vm::GC *, hermes::vm::RootAcceptor &acceptor) {
        m_stackValues.for_each([&](const hermes::vm::PinnedHermesValue &phv) {
          acceptor.accept(const_cast<hermes::vm::PinnedHermesValue &>(phv));
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
}

NodeApiEnvironment::~NodeApiEnvironment() {
  // First we must finalize those references that have `napi_finalizer`
  // callbacks. The reason is that addons might store other references which
  // they delete during their `napi_finalizer` callbacks. If we deleted such
  // references here first, they would be doubly deleted when the
  // `napi_finalizer` deleted them subsequently.
  RefTracker::FinalizeAll(&finalizing_reflist);
  RefTracker::FinalizeAll(&reflist);
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
  last_error.error_code = error_code;
  last_error.engine_error_code = engine_error_code;
  last_error.engine_reserved = engine_reserved;
  return error_code;
}

napi_status NodeApiEnvironment::ClearLastError() noexcept {
  last_error.error_code = napi_ok;
  last_error.engine_error_code = 0;
  last_error.engine_reserved = nullptr;
  return napi_ok;
}

hermes::vm::HermesValue NodeApiEnvironment::StringHVFromAscii(
    const char *str,
    size_t length) {
  auto strRes = hermes::vm::StringPrimitive::createEfficient(
      &runtime_, llvh::makeArrayRef(str, length));
  CheckStatus(strRes.getStatus());
  return *strRes;
}

hermes::vm::HermesValue NodeApiEnvironment::StringHVFromLatin1(
    const char *str,
    size_t length) {
  if (::hermes::isAllASCII(str, str + length)) {
    return StringHVFromAscii(str, length);
  }

  // Latin1 has the same codes as Unicode. We just need to expand char to
  // char16_t.
  std::u16string out(length, u' ');
  for (auto i = 0; i < length; ++i) {
    out[i] = str[i];
  }
  auto strRes =
      hermes::vm::StringPrimitive::createEfficient(&runtime_, std::move(out));
  CheckStatus(strRes.getStatus());
  return *strRes;
}

static void
ConvertUtf8ToUtf16(const uint8_t *utf8, size_t length, std::u16string &out) {
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

hermes::vm::HermesValue NodeApiEnvironment::StringHVFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  if (::hermes::isAllASCII(utf8, utf8 + length)) {
    return StringHVFromAscii((const char *)utf8, length);
  }
  std::u16string out;
  ConvertUtf8ToUtf16(utf8, length, out);
  auto strRes =
      hermes::vm::StringPrimitive::createEfficient(&runtime_, std::move(out));
  CheckStatus(strRes.getStatus());
  return *strRes;
}

napi_value NodeApiEnvironment::AddStackValue(
    hermes::vm::HermesValue value) noexcept {
  m_stackValues.emplace_back(value);
  return reinterpret_cast<napi_value>(&m_stackValues.back());
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

napi_status NodeApiEnvironment::CreateFunction(
    const char *utf8name,
    size_t length,
    napi_callback cb,
    void *callback_data,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);
  // CHECK_ARG(env, cb);

  // v8::Local<v8::Function> return_value;
  // v8::EscapableHandleScope scope(env->isolate);
  // v8::Local<v8::Function> fn;
  // STATUS_CALL(v8impl::FunctionCallbackWrapper::NewFunction(
  //     env, cb, callback_data, &fn));
  // return_value = scope.Escape(fn);

  // if (utf8name != nullptr) {
  //   v8::Local<v8::String> name_string;
  //   CHECK_NEW_FROM_UTF8_LEN(env, name_string, utf8name, length);
  //   return_value->SetName(name_string);
  // }

  // *result = v8impl::JsValueFromV8LocalValue(return_value);

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::DefineClass(
    const char *utf8name,
    size_t length,
    napi_callback constructor,
    void *callback_data,
    size_t property_count,
    const napi_property_descriptor *properties,
    napi_value *result) noexcept {
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

napi_status NodeApiEnvironment::GetPropertyNames(
    napi_value object,
    napi_value *result) noexcept {
  return GetAllPropertyNames(
      object,
      napi_key_include_prototypes,
      static_cast<napi_key_filter>(napi_key_enumerable | napi_key_skip_symbols),
      napi_key_numbers_to_strings,
      result);
}

napi_status NodeApiEnvironment::GetAllPropertyNames(
    napi_value object,
    napi_key_collection_mode key_mode,
    napi_key_filter key_filter,
    napi_key_conversion key_conversion,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;
  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::PropertyFilter filter = v8::PropertyFilter::ALL_PROPERTIES;
  // if (key_filter & napi_key_writable) {
  //   filter = static_cast<v8::PropertyFilter>(
  //       filter | v8::PropertyFilter::ONLY_WRITABLE);
  // }
  // if (key_filter & napi_key_enumerable) {
  //   filter = static_cast<v8::PropertyFilter>(
  //       filter | v8::PropertyFilter::ONLY_ENUMERABLE);
  // }
  // if (key_filter & napi_key_configurable) {
  //   filter = static_cast<v8::PropertyFilter>(
  //       filter | v8::PropertyFilter::ONLY_WRITABLE);
  // }
  // if (key_filter & napi_key_skip_strings) {
  //   filter = static_cast<v8::PropertyFilter>(
  //       filter | v8::PropertyFilter::SKIP_STRINGS);
  // }
  // if (key_filter & napi_key_skip_symbols) {
  //   filter = static_cast<v8::PropertyFilter>(
  //       filter | v8::PropertyFilter::SKIP_SYMBOLS);
  // }
  // v8::KeyCollectionMode collection_mode;
  // v8::KeyConversionMode conversion_mode;

  // switch (key_mode) {
  //   case napi_key_include_prototypes:
  //     collection_mode = v8::KeyCollectionMode::kIncludePrototypes;
  //     break;
  //   case napi_key_own_only:
  //     collection_mode = v8::KeyCollectionMode::kOwnOnly;
  //     break;
  //   default:
  //     return napi_set_last_error(env, napi_invalid_arg);
  // }

  // switch (key_conversion) {
  //   case napi_key_keep_numbers:
  //     conversion_mode = v8::KeyConversionMode::kKeepNumbers;
  //     break;
  //   case napi_key_numbers_to_strings:
  //     conversion_mode = v8::KeyConversionMode::kConvertToString;
  //     break;
  //   default:
  //     return napi_set_last_error(env, napi_invalid_arg);
  // }

  // v8::MaybeLocal<v8::Array> maybe_all_propertynames = obj->GetPropertyNames(
  //     context,
  //     collection_mode,
  //     filter,
  //     v8::IndexFilter::kIncludeIndices,
  //     conversion_mode);

  // CHECK_MAYBE_EMPTY_WITH_PREAMBLE(
  //     env, maybe_all_propertynames, napi_generic_failure);

  // *result =
  //     v8impl::JsValueFromV8LocalValue(maybe_all_propertynames.ToLocalChecked());
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::SetProperty(
    napi_value object,
    napi_value key,
    napi_value value) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, key);
  // CHECK_ARG(env, value);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Local<v8::Value> k = v8impl::V8LocalValueFromJsValue(key);
  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // v8::Maybe<bool> set_maybe = obj->Set(context, k, val);

  // RETURN_STATUS_IF_FALSE(env, set_maybe.FromMaybe(false),
  // napi_generic_failure); return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::HasProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);
  // CHECK_ARG(env, key);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Local<v8::Value> k = v8impl::V8LocalValueFromJsValue(key);
  // v8::Maybe<bool> has_maybe = obj->Has(context, k);

  // CHECK_MAYBE_NOTHING(env, has_maybe, napi_generic_failure);

  // *result = has_maybe.FromMaybe(false);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetProperty(
    napi_value object,
    napi_value key,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, key);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Value> k = v8impl::V8LocalValueFromJsValue(key);
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // auto get_maybe = obj->Get(context, k);

  // CHECK_MAYBE_EMPTY(env, get_maybe, napi_generic_failure);

  // v8::Local<v8::Value> val = get_maybe.ToLocalChecked();
  // *result = v8impl::JsValueFromV8LocalValue(val);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::DeleteProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, key);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Value> k = v8impl::V8LocalValueFromJsValue(key);
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);
  // v8::Maybe<bool> delete_maybe = obj->Delete(context, k);
  // CHECK_MAYBE_NOTHING(env, delete_maybe, napi_generic_failure);

  // if (result != nullptr)
  //   *result = delete_maybe.FromMaybe(false);

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::HasOwnProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, key);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);
  // v8::Local<v8::Value> k = v8impl::V8LocalValueFromJsValue(key);
  // RETURN_STATUS_IF_FALSE(env, k->IsName(), napi_name_expected);
  // v8::Maybe<bool> has_maybe = obj->HasOwnProperty(context, k.As<v8::Name>());
  // CHECK_MAYBE_NOTHING(env, has_maybe, napi_generic_failure);
  // *result = has_maybe.FromMaybe(false);

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::SetNamedProperty(
    napi_value object,
    const char *utf8name,
    napi_value value) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, value);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Local<v8::Name> key;
  // CHECK_NEW_FROM_UTF8(env, key, utf8name);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // v8::Maybe<bool> set_maybe = obj->Set(context, key, val);

  // RETURN_STATUS_IF_FALSE(env, set_maybe.FromMaybe(false),
  // napi_generic_failure); return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::HasNamedProperty(
    napi_value object,
    const char *utf8name,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Local<v8::Name> key;
  // CHECK_NEW_FROM_UTF8(env, key, utf8name);

  // v8::Maybe<bool> has_maybe = obj->Has(context, key);

  // CHECK_MAYBE_NOTHING(env, has_maybe, napi_generic_failure);

  // *result = has_maybe.FromMaybe(false);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetNamedProperty(
    napi_value object,
    const char *utf8name,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();

  // v8::Local<v8::Name> key;
  // CHECK_NEW_FROM_UTF8(env, key, utf8name);

  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // auto get_maybe = obj->Get(context, key);

  // CHECK_MAYBE_EMPTY(env, get_maybe, napi_generic_failure);

  // v8::Local<v8::Value> val = get_maybe.ToLocalChecked();
  // *result = v8impl::JsValueFromV8LocalValue(val);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::SetElement(
    napi_value object,
    uint32_t index,
    napi_value value) noexcept {
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
  return napi_ok;
}

napi_status NodeApiEnvironment::HasElement(
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

napi_status NodeApiEnvironment::GetElement(
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

napi_status NodeApiEnvironment::DeleteElement(
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

napi_status NodeApiEnvironment::DefineProperties(
    napi_value object,
    size_t property_count,
    const napi_property_descriptor *properties) noexcept {
  // NAPI_PREAMBLE(env);
  // if (property_count > 0) {
  //   CHECK_ARG(env, properties);
  // }

  // v8::Local<v8::Context> context = env->context();

  // v8::Local<v8::Object> obj;
  // CHECK_TO_OBJECT(env, context, obj, object);

  // for (size_t i = 0; i < property_count; i++) {
  //   const napi_property_descriptor *p = &properties[i];

  //   v8::Local<v8::Name> property_name;
  //   STATUS_CALL(v8impl::V8NameFromPropertyDescriptor(env, p,
  //   &property_name));

  //   if (p->getter != nullptr || p->setter != nullptr) {
  //     v8::Local<v8::Function> local_getter;
  //     v8::Local<v8::Function> local_setter;

  //     if (p->getter != nullptr) {
  //       STATUS_CALL(v8impl::FunctionCallbackWrapper::NewFunction(
  //           env, p->getter, p->data, &local_getter));
  //     }
  //     if (p->setter != nullptr) {
  //       STATUS_CALL(v8impl::FunctionCallbackWrapper::NewFunction(
  //           env, p->setter, p->data, &local_setter));
  //     }

  //     v8::PropertyDescriptor descriptor(local_getter, local_setter);
  //     descriptor.set_enumerable((p->attributes & napi_enumerable) != 0);
  //     descriptor.set_configurable((p->attributes & napi_configurable) != 0);

  //     auto define_maybe =
  //         obj->DefineProperty(context, property_name, descriptor);

  //     if (!define_maybe.FromMaybe(false)) {
  //       return napi_set_last_error(env, napi_invalid_arg);
  //     }
  //   } else if (p->method != nullptr) {
  //     v8::Local<v8::Function> method;
  //     STATUS_CALL(v8impl::FunctionCallbackWrapper::NewFunction(
  //         env, p->method, p->data, &method));
  //     v8::PropertyDescriptor descriptor(
  //         method, (p->attributes & napi_writable) != 0);
  //     descriptor.set_enumerable((p->attributes & napi_enumerable) != 0);
  //     descriptor.set_configurable((p->attributes & napi_configurable) != 0);

  //     auto define_maybe =
  //         obj->DefineProperty(context, property_name, descriptor);

  //     if (!define_maybe.FromMaybe(false)) {
  //       return napi_set_last_error(env, napi_generic_failure);
  //     }
  //   } else {
  //     v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(p->value);

  //     v8::PropertyDescriptor descriptor(
  //         value, (p->attributes & napi_writable) != 0);
  //     descriptor.set_enumerable((p->attributes & napi_enumerable) != 0);
  //     descriptor.set_configurable((p->attributes & napi_configurable) != 0);

  //     auto define_maybe =
  //         obj->DefineProperty(context, property_name, descriptor);

  //     if (!define_maybe.FromMaybe(false)) {
  //       return napi_set_last_error(env, napi_invalid_arg);
  //     }
  //   }
  // }

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::ObjectFreeze(napi_value object) noexcept {
  // NAPI_PREAMBLE(env);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Maybe<bool> set_frozen =
  //     obj->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen);

  // RETURN_STATUS_IF_FALSE_WITH_PREAMBLE(
  //     env, set_frozen.FromMaybe(false), napi_generic_failure);

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::ObjectSeal(napi_value object) noexcept {
  // NAPI_PREAMBLE(env);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::Object> obj;

  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Maybe<bool> set_sealed =
  //     obj->SetIntegrityLevel(context, v8::IntegrityLevel::kSealed);

  // RETURN_STATUS_IF_FALSE_WITH_PREAMBLE(
  //     env, set_sealed.FromMaybe(false), napi_generic_failure);

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::IsArray(
    napi_value value,
    bool *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // *result = val->IsArray();
  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetArrayLength(
    napi_value value,
    uint32_t *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // RETURN_STATUS_IF_FALSE(env, val->IsArray(), napi_array_expected);

  // v8::Local<v8::Array> arr = val.As<v8::Array>();
  // *result = arr->Length();

  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::StrictEquals(
    napi_value lhs,
    napi_value rhs,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, lhs);
  // CHECK_ARG(env, rhs);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> a = v8impl::V8LocalValueFromJsValue(lhs);
  // v8::Local<v8::Value> b = v8impl::V8LocalValueFromJsValue(rhs);

  // *result = a->StrictEquals(b);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetPrototype(
    napi_value object,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();

  // v8::Local<v8::Object> obj;
  // CHECK_TO_OBJECT(env, context, obj, object);

  // v8::Local<v8::Value> val = obj->GetPrototype();
  // *result = v8impl::JsValueFromV8LocalValue(val);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateObject(napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);

  // *result = v8impl::JsValueFromV8LocalValue(v8::Object::New(env->isolate));

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateArray(napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);

  // *result = v8impl::JsValueFromV8LocalValue(v8::Array::New(env->isolate));

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateArray(
    size_t length,
    napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);

  // *result =
  //     v8impl::JsValueFromV8LocalValue(v8::Array::New(env->isolate, length));

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateStringLatin1(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  CHECK_ARG(this, result);
  RETURN_STATUS_IF_FALSE(
      this,
      (length == NAPI_AUTO_LENGTH) || length <= INT_MAX,
      napi_invalid_arg);
  return HandleExceptions([&] {
    hermes::vm::GCScope gcScope(&runtime_);
    *result = AddStackValue(StringHVFromLatin1(str, length));
  });
}

napi_status NodeApiEnvironment::CreateStringUtf8(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  // TODO: validate stack scope
  CHECK_ARG(this, result);
  RETURN_STATUS_IF_FALSE(
      this,
      (length == NAPI_AUTO_LENGTH) || length <= INT_MAX,
      napi_invalid_arg);

  return HandleExceptions([&] {
    hermes::vm::GCScope gcScope(&runtime_);
    *result = AddStackValue(
        StringHVFromUtf8(reinterpret_cast<const uint8_t *>(str), length));
  });
}

napi_status NodeApiEnvironment::CreateStringUtf16(
    const char16_t *str,
    size_t length,
    napi_value *result) noexcept {
  CHECK_ARG(this, result);
  RETURN_STATUS_IF_FALSE(
      this,
      (length == NAPI_AUTO_LENGTH) || length <= INT_MAX,
      napi_invalid_arg);

  return HandleExceptions([&] {
    hermes::vm::GCScope gcScope(&runtime_);
    auto strRes = hermes::vm::StringPrimitive::createEfficient(
        &runtime_, llvh::makeArrayRef(str, length));
    CheckStatus(strRes.getStatus());
    *result = AddStackValue(*strRes);
  });
}

napi_status NodeApiEnvironment::CreateNumber(
    double value,
    napi_value *result) noexcept {
  CHECK_ARG(this, result);
  return HandleExceptions([&] {
    hermes::vm::GCScope gcScope(&runtime_);
    *result = AddStackValue(runtime_.makeHandle(
        hermes::vm::HermesValue::encodeUntrustedDoubleValue(value)).getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateNumber(
    int32_t value,
    napi_value *result) noexcept {
  CHECK_ARG(this, result);
  return HandleExceptions([&] {
    hermes::vm::GCScope gcScope(&runtime_);
    *result = AddStackValue(runtime_.makeHandle(
        hermes::vm::HermesValue::encodeNumberValue(value)).getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateNumber(
    uint32_t value,
    napi_value *result) noexcept {
  CHECK_ARG(this, result);
  return HandleExceptions([&] {
    hermes::vm::GCScope gcScope(&runtime_);
    *result = AddStackValue(runtime_.makeHandle(
        hermes::vm::HermesValue::encodeNumberValue(value)).getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateNumber(
    int64_t value,
    napi_value *result) noexcept {
  CHECK_ARG(this, result);
  return HandleExceptions([&] {
    hermes::vm::GCScope gcScope(&runtime_);
    *result = AddStackValue(runtime_.makeHandle(
        hermes::vm::HermesValue::encodeNumberValue(value)).getHermesValue());
    return ClearLastError();
  });
}

napi_status NodeApiEnvironment::CreateBigInt(
    int64_t value,
    napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);

  // *result =
  //     v8impl::JsValueFromV8LocalValue(v8::BigInt::New(env->isolate, value));

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateBigInt(
    uint64_t value,
    napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);

  // *result = v8impl::JsValueFromV8LocalValue(
  //     v8::BigInt::NewFromUnsigned(env->isolate, value));

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateBigInt(
    int sign_bit,
    size_t word_count,
    const uint64_t *words,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, words);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();

  // RETURN_STATUS_IF_FALSE(env, word_count <= INT_MAX, napi_invalid_arg);

  // v8::MaybeLocal<v8::BigInt> b =
  //     v8::BigInt::NewFromWords(context, sign_bit, word_count, words);

  // CHECK_MAYBE_EMPTY_WITH_PREAMBLE(env, b, napi_generic_failure);

  // *result = v8impl::JsValueFromV8LocalValue(b.ToLocalChecked());
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetBoolean(
    bool value,
    napi_value *result) noexcept {
  CHECK_ARG(this, result);
  *result = AddStackValue(runtime_.getBoolValue(value).getHermesValue());
  return ClearLastError();
}

napi_status NodeApiEnvironment::CreateSymbol(
    napi_value description,
    napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);

  // v8::Isolate *isolate = env->isolate;

  // if (description == nullptr) {
  //   *result = v8impl::JsValueFromV8LocalValue(v8::Symbol::New(isolate));
  // } else {
  //   v8::Local<v8::Value> desc = v8impl::V8LocalValueFromJsValue(description);
  //   RETURN_STATUS_IF_FALSE(env, desc->IsString(), napi_string_expected);

  //   *result = v8impl::JsValueFromV8LocalValue(
  //       v8::Symbol::New(isolate, desc.As<v8::String>()));
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

// static inline napi_status set_error_code(
//     napi_env env,
//     v8::Local<v8::Value> error,
//     napi_value code,
//     const char *code_cstring) {
//   if ((code != nullptr) || (code_cstring != nullptr)) {
//     v8::Local<v8::Context> context = env->context();
//     v8::Local<v8::Object> err_object = error.As<v8::Object>();

//     v8::Local<v8::Value> code_value = v8impl::V8LocalValueFromJsValue(code);
//     if (code != nullptr) {
//       code_value = v8impl::V8LocalValueFromJsValue(code);
//       RETURN_STATUS_IF_FALSE(env, code_value->IsString(),
//       napi_string_expected);
//     } else {
//       CHECK_NEW_FROM_UTF8(env, code_value, code_cstring);
//     }

//     v8::Local<v8::Name> code_key;
//     CHECK_NEW_FROM_UTF8(env, code_key, "code");

//     v8::Maybe<bool> set_maybe = err_object->Set(context, code_key,
//     code_value); RETURN_STATUS_IF_FALSE(
//         env, set_maybe.FromMaybe(false), napi_generic_failure);
//   }
//   return napi_ok;
// }

napi_status NodeApiEnvironment::CreateError(
    napi_value code,
    napi_value msg,
    napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, msg);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> message_value = v8impl::V8LocalValueFromJsValue(msg);
  // RETURN_STATUS_IF_FALSE(env, message_value->IsString(),
  // napi_string_expected);

  // v8::Local<v8::Value> error_obj =
  //     v8::Exception::Error(message_value.As<v8::String>());
  // STATUS_CALL(set_error_code(env, error_obj, code, nullptr));

  // *result = v8impl::JsValueFromV8LocalValue(error_obj);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateTypeError(
    napi_value code,
    napi_value msg,
    napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, msg);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> message_value = v8impl::V8LocalValueFromJsValue(msg);
  // RETURN_STATUS_IF_FALSE(env, message_value->IsString(),
  // napi_string_expected);

  // v8::Local<v8::Value> error_obj =
  //     v8::Exception::TypeError(message_value.As<v8::String>());
  // STATUS_CALL(set_error_code(env, error_obj, code, nullptr));

  // *result = v8impl::JsValueFromV8LocalValue(error_obj);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateRangeError(
    napi_value code,
    napi_value msg,
    napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, msg);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> message_value = v8impl::V8LocalValueFromJsValue(msg);
  // RETURN_STATUS_IF_FALSE(env, message_value->IsString(),
  // napi_string_expected);

  // v8::Local<v8::Value> error_obj =
  //     v8::Exception::RangeError(message_value.As<v8::String>());
  // STATUS_CALL(set_error_code(env, error_obj, code, nullptr));

  // *result = v8impl::JsValueFromV8LocalValue(error_obj);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::TypeOf(
    napi_value value,
    napi_valuetype *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> v = v8impl::V8LocalValueFromJsValue(value);

  // if (v->IsNumber()) {
  //   *result = napi_number;
  // } else if (v->IsBigInt()) {
  //   *result = napi_bigint;
  // } else if (v->IsString()) {
  //   *result = napi_string;
  // } else if (v->IsFunction()) {
  //   // This test has to come before IsObject because IsFunction
  //   // implies IsObject
  //   *result = napi_function;
  // } else if (v->IsExternal()) {
  //   // This test has to come before IsObject because IsExternal
  //   // implies IsObject
  //   *result = napi_external;
  // } else if (v->IsObject()) {
  //   *result = napi_object;
  // } else if (v->IsBoolean()) {
  //   *result = napi_boolean;
  // } else if (v->IsUndefined()) {
  //   *result = napi_undefined;
  // } else if (v->IsSymbol()) {
  //   *result = napi_symbol;
  // } else if (v->IsNull()) {
  //   *result = napi_null;
  // } else {
  //   // Should not get here unless V8 has added some new kind of value.
  //   return napi_set_last_error(env, napi_invalid_arg);
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetUndefined(napi_value *result) noexcept {
  CHECK_ARG(this, result);
  *result = AddStackValue(runtime_.getUndefinedValue().getHermesValue());
  return ClearLastError();
}

napi_status NodeApiEnvironment::GetNull(napi_value *result) noexcept {
  CHECK_ARG(this, result);
  *result = AddStackValue(runtime_.getNullValue().getHermesValue());
  return ClearLastError();
}

// Gets all callback info in a single call. (Ugly, but faster.)
napi_status NodeApiEnvironment::GetCallbackInfo(
    napi_callback_info cbinfo, // [in] Opaque callback-info handle
    size_t *argc, // [in-out] Specifies the size of the provided argv array
                  // and receives the actual count of args.
    napi_value *argv, // [out] Array of values
    napi_value *this_arg, // [out] Receives the JS 'this' arg for the call
    void **data) noexcept { // [out] Receives the data pointer for the callback.
  // CHECK_ENV(env);
  // CHECK_ARG(env, cbinfo);

  // v8impl::CallbackWrapper *info =
  //     reinterpret_cast<v8impl::CallbackWrapper *>(cbinfo);

  // if (argv != nullptr) {
  //   CHECK_ARG(env, argc);
  //   info->Args(argv, *argc);
  // }
  // if (argc != nullptr) {
  //   *argc = info->ArgsLength();
  // }
  // if (this_arg != nullptr) {
  //   *this_arg = info->This();
  // }
  // if (data != nullptr) {
  //   *data = info->Data();
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetNewTarget(
    napi_callback_info cbinfo,
    napi_value *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, cbinfo);
  // CHECK_ARG(env, result);

  // v8impl::CallbackWrapper *info =
  //     reinterpret_cast<v8impl::CallbackWrapper *>(cbinfo);

  // *result = info->GetNewTarget();
  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CallFunction(
    napi_value recv,
    napi_value func,
    size_t argc,
    const napi_value *argv,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, recv);
  // if (argc > 0) {
  //   CHECK_ARG(env, argv);
  // }

  // v8::Local<v8::Context> context = env->context();

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
  return napi_ok;
}

napi_status NodeApiEnvironment::GetGlobal(napi_value *result) noexcept {
  // TODO: validate stack scope
  CHECK_ARG(this, result);
  *result = AddStackValue(runtime_.getGlobal().getHermesValue());
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
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // RETURN_STATUS_IF_FALSE(env, val->IsNumber(), napi_number_expected);

  // *result = val.As<v8::Number>()->Value();

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetNumberValue(
    napi_value value,
    int32_t *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // if (val->IsInt32()) {
  //   *result = val.As<v8::Int32>()->Value();
  // } else {
  //   RETURN_STATUS_IF_FALSE(env, val->IsNumber(), napi_number_expected);

  //   // Empty context: https://github.com/nodejs/node/issues/14379
  //   v8::Local<v8::Context> context;
  //   *result = val->Int32Value(context).FromJust();
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetNumberValue(
    napi_value value,
    uint32_t *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // if (val->IsUint32()) {
  //   *result = val.As<v8::Uint32>()->Value();
  // } else {
  //   RETURN_STATUS_IF_FALSE(env, val->IsNumber(), napi_number_expected);

  //   // Empty context: https://github.com/nodejs/node/issues/14379
  //   v8::Local<v8::Context> context;
  //   *result = val->Uint32Value(context).FromJust();
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetNumberValue(
    napi_value value,
    int64_t *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // // This is still a fast path very likely to be taken.
  // if (val->IsInt32()) {
  //   *result = val.As<v8::Int32>()->Value();
  //   return napi_clear_last_error(env);
  // }

  // RETURN_STATUS_IF_FALSE(env, val->IsNumber(), napi_number_expected);

  // // v8::Value::IntegerValue() converts NaN, +Inf, and -Inf to INT64_MIN,
  // // inconsistent with v8::Value::Int32Value() which converts those values to
  // 0.
  // // Special-case all non-finite values to match that behavior.
  // double doubleValue = val.As<v8::Number>()->Value();
  // if (std::isfinite(doubleValue)) {
  //   // Empty context: https://github.com/nodejs/node/issues/14379
  //   v8::Local<v8::Context> context;
  //   *result = val->IntegerValue(context).FromJust();
  // } else {
  //   *result = 0;
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetBigIntValue(
    napi_value value,
    int64_t *result,
    bool *lossless) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);
  // CHECK_ARG(env, lossless);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // RETURN_STATUS_IF_FALSE(env, val->IsBigInt(), napi_bigint_expected);

  // *result = val.As<v8::BigInt>()->Int64Value(lossless);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetBigIntValue(
    napi_value value,
    uint64_t *result,
    bool *lossless) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);
  // CHECK_ARG(env, lossless);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // RETURN_STATUS_IF_FALSE(env, val->IsBigInt(), napi_bigint_expected);

  // *result = val.As<v8::BigInt>()->Uint64Value(lossless);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetBigIntValue(
    napi_value value,
    int *sign_bit,
    size_t *word_count,
    uint64_t *words) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, word_count);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);

  // RETURN_STATUS_IF_FALSE(env, val->IsBigInt(), napi_bigint_expected);

  // v8::Local<v8::BigInt> big = val.As<v8::BigInt>();

  // int word_count_int = *word_count;

  // if (sign_bit == nullptr && words == nullptr) {
  //   word_count_int = big->WordCount();
  // } else {
  //   CHECK_ARG(env, sign_bit);
  //   CHECK_ARG(env, words);
  //   big->ToWordsArray(sign_bit, &word_count_int, words);
  // }

  // *word_count = word_count_int;

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetBoolValue(
    napi_value value,
    bool *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // RETURN_STATUS_IF_FALSE(env, val->IsBoolean(), napi_boolean_expected);

  // *result = val.As<v8::Boolean>()->Value();

  // return napi_clear_last_error(env);
  return napi_ok;
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
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // RETURN_STATUS_IF_FALSE(env, val->IsString(), napi_string_expected);

  // if (!buf) {
  //   CHECK_ARG(env, result);
  //   *result = val.As<v8::String>()->Length();
  // } else if (bufsize != 0) {
  //   int copied = val.As<v8::String>()->WriteOneByte(
  //       env->isolate,
  //       reinterpret_cast<uint8_t *>(buf),
  //       0,
  //       bufsize - 1,
  //       v8::String::NO_NULL_TERMINATION);

  //   buf[copied] = '\0';
  //   if (result != nullptr) {
  //     *result = copied;
  //   }
  // } else if (result != nullptr) {
  //   *result = 0;
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
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
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // RETURN_STATUS_IF_FALSE(env, val->IsString(), napi_string_expected);

  // if (!buf) {
  //   CHECK_ARG(env, result);
  //   *result = val.As<v8::String>()->Utf8Length(env->isolate);
  // } else if (bufsize != 0) {
  //   int copied = val.As<v8::String>()->WriteUtf8(
  //       env->isolate,
  //       buf,
  //       bufsize - 1,
  //       nullptr,
  //       v8::String::REPLACE_INVALID_UTF8 | v8::String::NO_NULL_TERMINATION);

  //   buf[copied] = '\0';
  //   if (result != nullptr) {
  //     *result = copied;
  //   }
  // } else if (result != nullptr) {
  //   *result = 0;
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
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
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // RETURN_STATUS_IF_FALSE(env, val->IsString(), napi_string_expected);

  // if (!buf) {
  //   CHECK_ARG(env, result);
  //   // V8 assumes UTF-16 length is the same as the number of characters.
  //   *result = val.As<v8::String>()->Length();
  // } else if (bufsize != 0) {
  //   int copied = val.As<v8::String>()->Write(
  //       env->isolate,
  //       reinterpret_cast<uint16_t *>(buf),
  //       0,
  //       bufsize - 1,
  //       v8::String::NO_NULL_TERMINATION);

  //   buf[copied] = '\0';
  //   if (result != nullptr) {
  //     *result = copied;
  //   }
  // } else if (result != nullptr) {
  //   *result = 0;
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CoerceToBool(
    napi_value value,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Isolate *isolate = env->isolate;
  // v8::Local<v8::Boolean> b =
  //     v8impl::V8LocalValueFromJsValue(value)->ToBoolean(isolate);
  // *result = v8impl::JsValueFromV8LocalValue(b);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CoerceToNumber(
    napi_value value,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::MixedCaseName> str;

  // CHECK_TO_##UpperCaseName(env, context, str, value);

  // *result = v8impl::JsValueFromV8LocalValue(str);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CoerceToObject(
    napi_value value,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::MixedCaseName> str;

  // CHECK_TO_##UpperCaseName(env, context, str, value);

  // *result = v8impl::JsValueFromV8LocalValue(str);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CoerceToString(
    napi_value value,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Context> context = env->context();
  // v8::Local<v8::MixedCaseName> str;

  // CHECK_TO_##UpperCaseName(env, context, str, value);

  // *result = v8impl::JsValueFromV8LocalValue(str);
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::Wrap(
    napi_value js_object,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) noexcept {
  // return v8impl::Wrap<v8impl::retrievable>(
  //     env, js_object, native_object, finalize_cb, finalize_hint, result);
  return napi_ok;
}

napi_status NodeApiEnvironment::Unwrap(napi_value obj, void **result) noexcept {
  //  return v8impl::Unwrap(env, obj, result, v8impl::KeepWrap);
  return napi_ok;
}

napi_status NodeApiEnvironment::RemoveWrap(
    napi_value obj,
    void **result) noexcept {
  //  return v8impl::Unwrap(env, obj, result, v8impl::RemoveWrap);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateExternal(
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Isolate *isolate = env->isolate;

  // v8::Local<v8::Value> external_value = v8::External::New(isolate, data);

  // // The Reference object will delete itself after invoking the finalizer
  // // callback.
  // v8impl::Reference::New(
  //     env, external_value, 0, true, finalize_cb, data, finalize_hint);

  // *result = v8impl::JsValueFromV8LocalValue(external_value);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::TypeTagObject(
    napi_value object,
    const napi_type_tag *type_tag) noexcept {
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

napi_status NodeApiEnvironment::CheckObjectTypeTag(
    napi_value object,
    const napi_type_tag *type_tag,
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

  // // We consider the type check to have failed unless we reach the line below
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

napi_status NodeApiEnvironment::GetValueExternal(
    napi_value value,
    void **result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // RETURN_STATUS_IF_FALSE(env, val->IsExternal(), napi_invalid_arg);

  // v8::Local<v8::External> external_value = val.As<v8::External>();
  // *result = external_value->Value();

  // return napi_clear_last_error(env);
  return napi_ok;
}

// Set initial_refcount to 0 for a weak reference, >0 for a strong reference.
napi_status NodeApiEnvironment::CreateReference(
    napi_value value,
    uint32_t initial_refcount,
    napi_ref *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> v8_value = v8impl::V8LocalValueFromJsValue(value);

  // if (!(v8_value->IsObject() || v8_value->IsFunction())) {
  //   return napi_set_last_error(env, napi_object_expected);
  // }

  // v8impl::Reference *reference =
  //     v8impl::Reference::New(env, v8_value, initial_refcount, false);

  // *result = reinterpret_cast<napi_ref>(reference);
  // return napi_clear_last_error(env);
  return napi_ok;
}

// Deletes a reference. The referenced value is released, and may be GC'd unless
// there are other references to it.
napi_status NodeApiEnvironment::DeleteReference(napi_ref ref) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, ref);

  // v8impl::Reference::Delete(reinterpret_cast<v8impl::Reference *>(ref));

  // return napi_clear_last_error(env);
  return napi_ok;
}

// Increments the reference count, optionally returning the resulting count.
// After this call the reference will be a strong reference because its
// refcount is >0, and the referenced object is effectively "pinned".
// Calling this when the refcount is 0 and the object is unavailable
// results in an error.
napi_status NodeApiEnvironment::ReferenceRef(
    napi_ref ref,
    uint32_t *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, ref);

  // v8impl::Reference *reference = reinterpret_cast<v8impl::Reference *>(ref);
  // uint32_t count = reference->Ref();

  // if (result != nullptr) {
  //   *result = count;
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

// Decrements the reference count, optionally returning the resulting count. If
// the result is 0 the reference is now weak and the object may be GC'd at any
// time if there are no other references. Calling this when the refcount is
// already 0 results in an error.
napi_status NodeApiEnvironment::ReferenceUnref(
    napi_ref ref,
    uint32_t *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, ref);

  // v8impl::Reference *reference = reinterpret_cast<v8impl::Reference *>(ref);

  // if (reference->RefCount() == 0) {
  //   return napi_set_last_error(env, napi_generic_failure);
  // }

  // uint32_t count = reference->Unref();

  // if (result != nullptr) {
  //   *result = count;
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

// Attempts to get a referenced value. If the reference is weak, the value might
// no longer be available, in that case the call is still successful but the
// result is NULL.
napi_status NodeApiEnvironment::GetReferenceValue(
    napi_ref ref,
    napi_value *result) noexcept {
  // // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot
  // throw
  // // JS exceptions.
  // CHECK_ENV(env);
  // CHECK_ARG(env, ref);
  // CHECK_ARG(env, result);

  // v8impl::Reference *reference = reinterpret_cast<v8impl::Reference *>(ref);
  // *result = v8impl::JsValueFromV8LocalValue(reference->Get());

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::OpenHandleScope(
    napi_handle_scope *result) noexcept {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because Hermes calls here cannot
  // throw JS exceptions.
  CHECK_ARG(this, result);

  Marker stackMarker = m_stackValues.create_marker();
  m_stackMarkers.emplace_back(std::move(stackMarker));
  *result = reinterpret_cast<napi_handle_scope>(&m_stackMarkers.back());
  return ClearLastError();
}

napi_status NodeApiEnvironment::CloseHandleScope(
    napi_handle_scope scope) noexcept {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because Hermes calls here cannot
  // throw JS exceptions.
  CHECK_ARG(this, scope);
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
  CHECK_ARG(this, result);

  if (m_stackMarkers.empty()) {
    return napi_invalid_arg;
  }

  m_stackValues.emplace_back(); // value to escape to parent scope
  m_stackValues.emplace_back(hermes::vm::HermesValue::encodeNativeUInt32(
      kEscapeableSentinelNativeValue));

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
  CHECK_ARG(this, scope);
  CHECK_ARG(this, escapee);
  CHECK_ARG(this, result);

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

  hermes::vm::PinnedHermesValue *sentinelTag = m_stackValues.at(sentinelMarker);
  if (!sentinelTag || !sentinelTag->isNativeValue()) {
    return napi_invalid_arg;
  }
  if (sentinelTag->getNativeUInt32() != kUsedEscapeableSentinelNativeValue) {
    return SetLastError(napi_escape_called_twice);
  }
  if (sentinelTag->getNativeUInt32() != kEscapeableSentinelNativeValue) {
    return napi_invalid_arg;
  }

  hermes::vm::PinnedHermesValue *escapedValue =
      m_stackValues.at(escapedValueMarker);
  *escapedValue = *reinterpret_cast<hermes::vm::PinnedHermesValue *>(escapee);

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

napi_status NodeApiEnvironment::InstanceOf(
    napi_value object,
    napi_value constructor,
    bool *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, object);
  // CHECK_ARG(env, result);

  // *result = false;

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
  return napi_ok;
}

// Methods to support catching exceptions
napi_status NodeApiEnvironment::IsExceptionPending(bool *result) noexcept {
  // // NAPI_PREAMBLE is not used here: this function must execute when there is
  // a
  // // pending exception.
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);

  // *result = !env->last_exception.IsEmpty();
  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::GetAndClearLastException(
    napi_value *result) noexcept {
  // NAPI_PREAMBLE is not used here: this function must execute when there is a
  // pending exception.
  // CHECK_ENV(env);
  // CHECK_ARG(env, result);

  // if (env->last_exception.IsEmpty()) {
  //   return napi_get_undefined(env, result);
  // } else {
  //   *result = v8impl::JsValueFromV8LocalValue(
  //       v8::Local<v8::Value>::New(env->isolate, env->last_exception));
  //   env->last_exception.Reset();
  // }

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::IsArrayBuffer(
    napi_value value,
    bool *result) noexcept {
  // CHECK_ENV(env);
  // CHECK_ARG(env, value);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
  // *result = val->IsArrayBuffer();

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::CreateArrayBuffer(
    size_t byte_length,
    void **data,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, result);

  // v8::Isolate *isolate = env->isolate;
  // v8::Local<v8::ArrayBuffer> buffer =
  //     v8::ArrayBuffer::New(isolate, byte_length);

  // // Optionally return a pointer to the buffer's data, to avoid another call
  // to
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
  // // and is able to use napi_env. Implementing that properly is hard, so use
  // the
  // // `Buffer` variant for easier implementation.
  // napi_value buffer;
  // STATUS_CALL(napi_create_external_buffer(
  //     env, byte_length, external_data, finalize_cb, finalize_hint, &buffer));
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

  // v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(arraybuffer);
  // RETURN_STATUS_IF_FALSE(env, value->IsArrayBuffer(), napi_invalid_arg);

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

  // v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(arraybuffer);
  // RETURN_STATUS_IF_FALSE(env, value->IsArrayBuffer(), napi_invalid_arg);

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

  // v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(arraybuffer);
  // RETURN_STATUS_IF_FALSE(env, value->IsArrayBuffer(), napi_invalid_arg);

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

  // v8::MaybeLocal<v8::Value> maybe_date = v8::Date::New(env->context(), time);
  // CHECK_MAYBE_EMPTY(env, maybe_date, napi_generic_failure);

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

napi_status NodeApiEnvironment::RunScript(
    napi_value script,
    napi_value *result) noexcept {
  // NAPI_PREAMBLE(env);
  // CHECK_ARG(env, script);
  // CHECK_ARG(env, result);

  // v8::Local<v8::Value> v8_script = v8impl::V8LocalValueFromJsValue(script);

  // if (!v8_script->IsString()) {
  //   return napi_set_last_error(env, napi_string_expected);
  // }

  // v8::Local<v8::Context> context = env->context();

  // auto maybe_script =
  //     v8::Script::Compile(context, v8::Local<v8::String>::Cast(v8_script));
  // CHECK_MAYBE_EMPTY(env, maybe_script, napi_generic_failure);

  // auto script_result = maybe_script.ToLocalChecked()->Run(context);
  // CHECK_MAYBE_EMPTY(env, script_result, napi_generic_failure);

  // *result = v8impl::JsValueFromV8LocalValue(script_result.ToLocalChecked());
  // return GET_RETURN_STATUS(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::AddFinalizer(
    napi_value js_object,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) noexcept {
  // return v8impl::Wrap<v8impl::anonymous>(
  //     env, js_object, native_object, finalize_cb, finalize_hint, result);
  return napi_ok;
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

  // v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(arraybuffer);
  // RETURN_STATUS_IF_FALSE(
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

  // v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(arraybuffer);

  // *result = value->IsArrayBuffer() &&
  //     value.As<v8::ArrayBuffer>()->GetBackingStore()->Data() == nullptr;

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status napi_create_hermes_env(napi_env *env) {
  if (!env) {
    return napi_status::napi_invalid_arg;
  }
  *env = reinterpret_cast<napi_env>(new NodeApiEnvironment());
  return napi_status::napi_ok;
}

napi_status napi_ext_env_ref(napi_env env) {
  return CHECKED_ENV(env)->Ref();
}

napi_status napi_ext_env_unref(napi_env env) {
  return CHECKED_ENV(env)->Unref();
}

#if 0
#define CHECK_MAYBE_NOTHING(env, maybe, status) \
  RETURN_STATUS_IF_FALSE((env), !((maybe).IsNothing()), (status))

#define CHECK_MAYBE_NOTHING_WITH_PREAMBLE(env, maybe, status) \
  RETURN_STATUS_IF_FALSE_WITH_PREAMBLE((env), !((maybe).IsNothing()), (status))

#define CHECK_TO_NUMBER(env, context, result, src) \
  CHECK_TO_TYPE((env), Number, (context), (result), (src), napi_number_expected)

// n-api defines NAPI_AUTO_LENGHTH as the indicator that a string
// is null terminated. For V8 the equivalent is -1. The assert
// validates that our cast of NAPI_AUTO_LENGTH results in -1 as
// needed by V8.
#define CHECK_NEW_FROM_UTF8_LEN(env, result, str, len)                         \
  do {                                                                         \
    static_assert(                                                             \
        static_cast<int>(NAPI_AUTO_LENGTH) == -1,                              \
        "Casting NAPI_AUTO_LENGTH to int must result in -1");                  \
    RETURN_STATUS_IF_FALSE(                                                    \
        (env), (len == NAPI_AUTO_LENGTH) || len <= INT_MAX, napi_invalid_arg); \
    RETURN_STATUS_IF_FALSE((env), (str) != nullptr, napi_invalid_arg);         \
    auto str_maybe = v8::String::NewFromUtf8(                                  \
        (env)->isolate,                                                        \
        (str),                                                                 \
        v8::NewStringType::kInternalized,                                      \
        static_cast<int>(len));                                                \
    CHECK_MAYBE_EMPTY((env), str_maybe, napi_generic_failure);                 \
    (result) = str_maybe.ToLocalChecked();                                     \
  } while (0)

#define CHECK_NEW_FROM_UTF8(env, result, str) \
  CHECK_NEW_FROM_UTF8_LEN((env), (result), (str), NAPI_AUTO_LENGTH)

#define CREATE_TYPED_ARRAY(                                                   \
    env, type, size_of_element, buffer, byte_offset, length, out)             \
  do {                                                                        \
    if ((size_of_element) > 1) {                                              \
      THROW_RANGE_ERROR_IF_FALSE(                                             \
          (env),                                                              \
          (byte_offset) % (size_of_element) == 0,                             \
          "ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT",                            \
          "start offset of " #type                                            \
          " should be a multiple of " #size_of_element);                      \
    }                                                                         \
    THROW_RANGE_ERROR_IF_FALSE(                                               \
        (env),                                                                \
        (length) * (size_of_element) + (byte_offset) <= buffer->ByteLength(), \
        "ERR_NAPI_INVALID_TYPEDARRAY_LENGTH",                                 \
        "Invalid typed array length");                                        \
    (out) = v8::type::New((buffer), (byte_offset), (length));                 \
  } while (0)

namespace v8impl {

namespace {

inline static napi_status
V8NameFromPropertyDescriptor(napi_env env,
                             const napi_property_descriptor* p,
                             v8::Local<v8::Name>* result) {
  if (p->utf8name != nullptr) {
    CHECK_NEW_FROM_UTF8(env, *result, p->utf8name);
  } else {
    v8::Local<v8::Value> property_value =
      v8impl::V8LocalValueFromJsValue(p->name);

    RETURN_STATUS_IF_FALSE(env, property_value->IsName(), napi_name_expected);
    *result = property_value.As<v8::Name>();
  }

  return napi_ok;
}

// convert from n-api property attributes to v8::PropertyAttribute
inline static v8::PropertyAttribute V8PropertyAttributesFromDescriptor(
    const napi_property_descriptor* descriptor) {
  unsigned int attribute_flags = v8::PropertyAttribute::None;

  // The napi_writable attribute is ignored for accessor descriptors, but
  // V8 would throw `TypeError`s on assignment with nonexistence of a setter.
  if ((descriptor->getter == nullptr && descriptor->setter == nullptr) &&
    (descriptor->attributes & napi_writable) == 0) {
    attribute_flags |= v8::PropertyAttribute::ReadOnly;
  }

  if ((descriptor->attributes & napi_enumerable) == 0) {
    attribute_flags |= v8::PropertyAttribute::DontEnum;
  }
  if ((descriptor->attributes & napi_configurable) == 0) {
    attribute_flags |= v8::PropertyAttribute::DontDelete;
  }

  return static_cast<v8::PropertyAttribute>(attribute_flags);
}

inline static napi_deferred
JsDeferredFromNodePersistent(v8impl::Persistent<v8::Value>* local) {
  return reinterpret_cast<napi_deferred>(local);
}

inline static v8impl::Persistent<v8::Value>*
NodePersistentFromJsDeferred(napi_deferred local) {
  return reinterpret_cast<v8impl::Persistent<v8::Value>*>(local);
}

class HandleScopeWrapper {
 public:
  explicit HandleScopeWrapper(v8::Isolate* isolate) : scope(isolate) {}

 private:
  v8::HandleScope scope;
};

// In node v0.10 version of v8, there is no EscapableHandleScope and the
// node v0.10 port use HandleScope::Close(Local<T> v) to mimic the behavior
// of a EscapableHandleScope::Escape(Local<T> v), but it is not the same
// semantics. This is an example of where the api abstraction fail to work
// across different versions.
class EscapableHandleScopeWrapper {
 public:
  explicit EscapableHandleScopeWrapper(v8::Isolate* isolate)
      : scope(isolate), escape_called_(false) {}
  bool escape_called() const {
    return escape_called_;
  }
  template <typename T>
  v8::Local<T> Escape(v8::Local<T> handle) {
    escape_called_ = true;
    return scope.Escape(handle);
  }

 private:
  v8::EscapableHandleScope scope;
  bool escape_called_;
};

inline static napi_handle_scope
JsHandleScopeFromV8HandleScope(HandleScopeWrapper* s) {
  return reinterpret_cast<napi_handle_scope>(s);
}

inline static HandleScopeWrapper*
V8HandleScopeFromJsHandleScope(napi_handle_scope s) {
  return reinterpret_cast<HandleScopeWrapper*>(s);
}

inline static napi_escapable_handle_scope
JsEscapableHandleScopeFromV8EscapableHandleScope(
    EscapableHandleScopeWrapper* s) {
  return reinterpret_cast<napi_escapable_handle_scope>(s);
}

inline static EscapableHandleScopeWrapper*
V8EscapableHandleScopeFromJsEscapableHandleScope(
    napi_escapable_handle_scope s) {
  return reinterpret_cast<EscapableHandleScopeWrapper*>(s);
}

inline static napi_status ConcludeDeferred(napi_env env,
                                           napi_deferred deferred,
                                           napi_value result,
                                           bool is_resolved) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  v8::Local<v8::Context> context = env->context();
  v8impl::Persistent<v8::Value>* deferred_ref =
      NodePersistentFromJsDeferred(deferred);
  v8::Local<v8::Value> v8_deferred =
      v8::Local<v8::Value>::New(env->isolate, *deferred_ref);

  auto v8_resolver = v8::Local<v8::Promise::Resolver>::Cast(v8_deferred);

  v8::Maybe<bool> success = is_resolved ?
      v8_resolver->Resolve(context, v8impl::V8LocalValueFromJsValue(result)) :
      v8_resolver->Reject(context, v8impl::V8LocalValueFromJsValue(result));

  delete deferred_ref;

  RETURN_STATUS_IF_FALSE(env, success.FromMaybe(false), napi_generic_failure);

  return GET_RETURN_STATUS(env);
}

// Wrapper around v8impl::Persistent that implements reference counting.
class RefBase : protected Finalizer, RefTracker {
 protected:
  RefBase(napi_env env,
          uint32_t initial_refcount,
          bool delete_self,
          napi_finalize finalize_callback,
          void* finalize_data,
          void* finalize_hint)
       : Finalizer(env, finalize_callback, finalize_data, finalize_hint),
        _refcount(initial_refcount),
        _delete_self(delete_self) {
    Link(finalize_callback == nullptr
        ? &env->reflist
        : &env->finalizing_reflist);
  }

 public:
  static RefBase* New(napi_env env,
                      uint32_t initial_refcount,
                      bool delete_self,
                      napi_finalize finalize_callback,
                      void* finalize_data,
                      void* finalize_hint) {
    return new RefBase(env,
                       initial_refcount,
                       delete_self,
                       finalize_callback,
                       finalize_data,
                       finalize_hint);
  }

  virtual ~RefBase() { Unlink(); }

  inline void* Data() {
    return _finalize_data;
  }

  // Delete is called in 2 ways. Either from the finalizer or
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
  static inline void Delete(RefBase* reference) {
    if ((reference->RefCount() != 0) ||
        (reference->_delete_self) ||
        (reference->_finalize_ran)) {
      delete reference;
    } else {
      // defer until finalizer runs as
      // it may alread be queued
      reference->_delete_self = true;
    }
  }

  inline uint32_t Ref() {
    return ++_refcount;
  }

  inline uint32_t Unref() {
    if (_refcount == 0) {
        return 0;
    }
    return --_refcount;
  }

  inline uint32_t RefCount() {
    return _refcount;
  }

 protected:
  inline void Finalize(bool is_env_teardown = false) override {
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
    if (is_env_teardown && RefCount() > 0) _refcount = 0;

    if (_finalize_callback != nullptr) {
      // This ensures that we never call the finalizer twice.
      napi_finalize fini = _finalize_callback;
      _finalize_callback = nullptr;
      _env->CallFinalizer(fini, _finalize_data, _finalize_hint);
    }

    // this is safe because if a request to delete the reference
    // is made in the finalize_callback it will defer deletion
    // to this block and set _delete_self to true
    if (_delete_self || is_env_teardown) {
      Delete(this);
    } else {
      _finalize_ran = true;
    }
  }

 private:
  uint32_t _refcount;
  bool _delete_self;
};

class Reference : public RefBase {
 protected:
  template <typename... Args>
  Reference(napi_env env,
            v8::Local<v8::Value> value,
            Args&&... args)
      : RefBase(env, std::forward<Args>(args)...),
            _persistent(env->isolate, value) {
    if (RefCount() == 0) {
      _persistent.SetWeak(
          this, FinalizeCallback, v8::WeakCallbackType::kParameter);
    }
  }

 public:
  static inline Reference* New(napi_env env,
                             v8::Local<v8::Value> value,
                             uint32_t initial_refcount,
                             bool delete_self,
                             napi_finalize finalize_callback = nullptr,
                             void* finalize_data = nullptr,
                             void* finalize_hint = nullptr) {
    return new Reference(env,
                         value,
                         initial_refcount,
                         delete_self,
                         finalize_callback,
                         finalize_data,
                         finalize_hint);
  }

  inline uint32_t Ref() {
    uint32_t refcount = RefBase::Ref();
    if (refcount == 1) {
      _persistent.ClearWeak();
    }
    return refcount;
  }

  inline uint32_t Unref() {
    uint32_t old_refcount = RefCount();
    uint32_t refcount = RefBase::Unref();
    if (old_refcount == 1 && refcount == 0) {
      _persistent.SetWeak(
          this, FinalizeCallback, v8::WeakCallbackType::kParameter);
    }
    return refcount;
  }

  inline v8::Local<v8::Value> Get() {
    if (_persistent.IsEmpty()) {
      return v8::Local<v8::Value>();
    } else {
      return v8::Local<v8::Value>::New(_env->isolate, _persistent);
    }
  }

 protected:
  inline void Finalize(bool is_env_teardown = false) override {
    // During env teardown, `~napi_env()` alone is responsible for finalizing.
    // Thus, we don't want any stray gc passes to trigger a second call to
    // `Finalize()`, so let's reset the persistent here if nothing is
    // keeping it alive.
    if (is_env_teardown && _persistent.IsWeak()) {
      _persistent.ClearWeak();
    }

    // Chain up to perform the rest of the finalization.
    RefBase::Finalize(is_env_teardown);
  }

 private:
  // The N-API finalizer callback may make calls into the engine. V8's heap is
  // not in a consistent state during the weak callback, and therefore it does
  // not support calls back into it. However, it provides a mechanism for adding
  // a finalizer which may make calls back into the engine by allowing us to
  // attach such a second-pass finalizer from the first pass finalizer. Thus,
  // we do that here to ensure that the N-API finalizer callback is free to call
  // into the engine.
  static void FinalizeCallback(const v8::WeakCallbackInfo<Reference>& data) {
    Reference* reference = data.GetParameter();

    // The reference must be reset during the first pass.
    reference->_persistent.Reset();

    data.SetSecondPassCallback(SecondPassCallback);
  }

  static void SecondPassCallback(const v8::WeakCallbackInfo<Reference>& data) {
    data.GetParameter()->Finalize();
  }

  v8impl::Persistent<v8::Value> _persistent;
};

enum UnwrapAction {
  KeepWrap,
  RemoveWrap
};

inline static napi_status Unwrap(napi_env env,
                                 napi_value js_object,
                                 void** result,
                                 UnwrapAction action) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, js_object);
  if (action == KeepWrap) {
    CHECK_ARG(env, result);
  }

  v8::Local<v8::Context> context = env->context();

  v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(js_object);
  RETURN_STATUS_IF_FALSE(env, value->IsObject(), napi_invalid_arg);
  v8::Local<v8::Object> obj = value.As<v8::Object>();

  auto val = obj->GetPrivate(context, NAPI_PRIVATE_KEY(context, wrapper))
      .ToLocalChecked();
  RETURN_STATUS_IF_FALSE(env, val->IsExternal(), napi_invalid_arg);
  Reference* reference =
      static_cast<v8impl::Reference*>(val.As<v8::External>()->Value());

  if (result) {
    *result = reference->Data();
  }

  if (action == RemoveWrap) {
    CHECK(obj->DeletePrivate(context, NAPI_PRIVATE_KEY(context, wrapper))
        .FromJust());
    Reference::Delete(reference);
  }

  return GET_RETURN_STATUS(env);
}

//=== Function napi_callback wrapper =================================

// Use this data structure to associate callback data with each N-API function
// exposed to JavaScript. The structure is stored in a v8::External which gets
// passed into our callback wrapper. This reduces the performance impact of
// calling through N-API.
// Ref: benchmark/misc/function_call
// Discussion (incl. perf. data): https://github.com/nodejs/node/pull/21072
class CallbackBundle {
 public:
  // Creates an object to be made available to the static function callback
  // wrapper, used to retrieve the native callback function and data pointer.
  static inline v8::Local<v8::Value>
  New(napi_env env, napi_callback cb, void* data) {
    CallbackBundle* bundle = new CallbackBundle();
    bundle->cb = cb;
    bundle->cb_data = data;
    bundle->env = env;

    v8::Local<v8::Value> cbdata = v8::External::New(env->isolate, bundle);
    Reference::New(env, cbdata, 0, true, Delete, bundle, nullptr);
    return cbdata;
  }
  napi_env       env;      // Necessary to invoke C++ NAPI callback
  void*          cb_data;  // The user provided callback data
  napi_callback  cb;
 private:
  static void Delete(napi_env env, void* data, void* hint) {
    CallbackBundle* bundle = static_cast<CallbackBundle*>(data);
    delete bundle;
  }
};

// Base class extended by classes that wrap V8 function and property callback
// info.
class CallbackWrapper {
 public:
  inline CallbackWrapper(napi_value this_arg, size_t args_length, void* data)
      : _this(this_arg), _args_length(args_length), _data(data) {}

  virtual napi_value GetNewTarget() = 0;
  virtual void Args(napi_value* buffer, size_t bufferlength) = 0;
  virtual void SetReturnValue(napi_value value) = 0;

  napi_value This() { return _this; }

  size_t ArgsLength() { return _args_length; }

  void* Data() { return _data; }

 protected:
  const napi_value _this;
  const size_t _args_length;
  void* _data;
};

class CallbackWrapperBase : public CallbackWrapper {
 public:
  inline CallbackWrapperBase(const v8::FunctionCallbackInfo<v8::Value>& cbinfo,
                             const size_t args_length)
      : CallbackWrapper(JsValueFromV8LocalValue(cbinfo.This()),
                        args_length,
                        nullptr),
        _cbinfo(cbinfo) {
    _bundle = reinterpret_cast<CallbackBundle*>(
        v8::Local<v8::External>::Cast(cbinfo.Data())->Value());
    _data = _bundle->cb_data;
  }

 protected:
  inline void InvokeCallback() {
    napi_callback_info cbinfo_wrapper = reinterpret_cast<napi_callback_info>(
        static_cast<CallbackWrapper*>(this));

    // All other pointers we need are stored in `_bundle`
    napi_env env = _bundle->env;
    napi_callback cb = _bundle->cb;

    napi_value result;
    env->CallIntoModule([&](napi_env env) {
      result = cb(env, cbinfo_wrapper);
    });

    if (result != nullptr) {
      this->SetReturnValue(result);
    }
  }

  const v8::FunctionCallbackInfo<v8::Value>& _cbinfo;
  CallbackBundle* _bundle;
};

class FunctionCallbackWrapper
    : public CallbackWrapperBase {
 public:
  static void Invoke(const v8::FunctionCallbackInfo<v8::Value>& info) {
    FunctionCallbackWrapper cbwrapper(info);
    cbwrapper.InvokeCallback();
  }

  static inline napi_status NewFunction(napi_env env,
                                        napi_callback cb,
                                        void* cb_data,
                                        v8::Local<v8::Function>* result) {
    v8::Local<v8::Value> cbdata = v8impl::CallbackBundle::New(env, cb, cb_data);
    RETURN_STATUS_IF_FALSE(env, !cbdata.IsEmpty(), napi_generic_failure);

    v8::MaybeLocal<v8::Function> maybe_function =
        v8::Function::New(env->context(), Invoke, cbdata);
    CHECK_MAYBE_EMPTY(env, maybe_function, napi_generic_failure);

    *result = maybe_function.ToLocalChecked();
    return napi_clear_last_error(env);
  }

  static inline napi_status NewTemplate(napi_env env,
                    napi_callback cb,
                    void* cb_data,
                    v8::Local<v8::FunctionTemplate>* result,
                    v8::Local<v8::Signature> sig = v8::Local<v8::Signature>()) {
    v8::Local<v8::Value> cbdata = v8impl::CallbackBundle::New(env, cb, cb_data);
    RETURN_STATUS_IF_FALSE(env, !cbdata.IsEmpty(), napi_generic_failure);

    *result = v8::FunctionTemplate::New(env->isolate, Invoke, cbdata, sig);
    return napi_clear_last_error(env);
  }

  explicit FunctionCallbackWrapper(
      const v8::FunctionCallbackInfo<v8::Value>& cbinfo)
      : CallbackWrapperBase(cbinfo, cbinfo.Length()) {}

  napi_value GetNewTarget() override {
    if (_cbinfo.IsConstructCall()) {
      return v8impl::JsValueFromV8LocalValue(_cbinfo.NewTarget());
    } else {
      return nullptr;
    }
  }

  /*virtual*/
  void Args(napi_value* buffer, size_t buffer_length) override {
    size_t i = 0;
    size_t min = std::min(buffer_length, _args_length);

    for (; i < min; i += 1) {
      buffer[i] = v8impl::JsValueFromV8LocalValue(_cbinfo[i]);
    }

    if (i < buffer_length) {
      napi_value undefined =
          v8impl::JsValueFromV8LocalValue(v8::Undefined(_cbinfo.GetIsolate()));
      for (; i < buffer_length; i += 1) {
        buffer[i] = undefined;
      }
    }
  }

  /*virtual*/
  void SetReturnValue(napi_value value) override {
    v8::Local<v8::Value> val = v8impl::V8LocalValueFromJsValue(value);
    _cbinfo.GetReturnValue().Set(val);
  }
};

enum WrapType {
  retrievable,
  anonymous
};

template <WrapType wrap_type>
inline napi_status Wrap(napi_env env,
                        napi_value js_object,
                        void* native_object,
                        napi_finalize finalize_cb,
                        void* finalize_hint,
                        napi_ref* result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, js_object);

  v8::Local<v8::Context> context = env->context();

  v8::Local<v8::Value> value = v8impl::V8LocalValueFromJsValue(js_object);
  RETURN_STATUS_IF_FALSE(env, value->IsObject(), napi_invalid_arg);
  v8::Local<v8::Object> obj = value.As<v8::Object>();

  if (wrap_type == retrievable) {
    // If we've already wrapped this object, we error out.
    RETURN_STATUS_IF_FALSE(env,
        !obj->HasPrivate(context, NAPI_PRIVATE_KEY(context, wrapper))
            .FromJust(),
        napi_invalid_arg);
  } else if (wrap_type == anonymous) {
    // If no finalize callback is provided, we error out.
    CHECK_ARG(env, finalize_cb);
  }

  v8impl::Reference* reference = nullptr;
  if (result != nullptr) {
    // The returned reference should be deleted via napi_delete_reference()
    // ONLY in response to the finalize callback invocation. (If it is deleted
    // before then, then the finalize callback will never be invoked.)
    // Therefore a finalize callback is required when returning a reference.
    CHECK_ARG(env, finalize_cb);
    reference = v8impl::Reference::New(
        env, obj, 0, false, finalize_cb, native_object, finalize_hint);
    *result = reinterpret_cast<napi_ref>(reference);
  } else {
    // Create a self-deleting reference.
    reference = v8impl::Reference::New(env, obj, 0, true, finalize_cb,
        native_object, finalize_cb == nullptr ? nullptr : finalize_hint);
  }

  if (wrap_type == retrievable) {
    CHECK(obj->SetPrivate(context, NAPI_PRIVATE_KEY(context, wrapper),
          v8::External::New(env->isolate, reference)).FromJust());
  }

  return GET_RETURN_STATUS(env);
}

}  // end of anonymous namespace

}  // end of namespace v8impl
#endif

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
  return CHECKED_ENV(env)->CreateFunction(
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
  return CHECKED_ENV(env)->DefineClass(
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
  return CHECKED_ENV(env)->GetPropertyNames(object, result);
}

napi_status napi_get_all_property_names(
    napi_env env,
    napi_value object,
    napi_key_collection_mode key_mode,
    napi_key_filter key_filter,
    napi_key_conversion key_conversion,
    napi_value *result) {
  return CHECKED_ENV(env)->GetAllPropertyNames(
      object, key_mode, key_filter, key_conversion, result);
}

napi_status napi_set_property(
    napi_env env,
    napi_value object,
    napi_value key,
    napi_value value) {
  return CHECKED_ENV(env)->SetProperty(object, key, value);
}

napi_status napi_has_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->HasProperty(object, key, result);
}

napi_status napi_get_property(
    napi_env env,
    napi_value object,
    napi_value key,
    napi_value *result) {
  return CHECKED_ENV(env)->GetProperty(object, key, result);
}

napi_status napi_delete_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->DeleteProperty(object, key, result);
}

napi_status napi_has_own_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->HasOwnProperty(object, key, result);
}

napi_status napi_set_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    napi_value value) {
  return CHECKED_ENV(env)->SetNamedProperty(object, utf8name, value);
}

napi_status napi_has_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    bool *result) {
  return CHECKED_ENV(env)->HasNamedProperty(object, utf8name, result);
}

napi_status napi_get_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    napi_value *result) {
  return CHECKED_ENV(env)->GetNamedProperty(object, utf8name, result);
}

napi_status napi_set_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    napi_value value) {
  return CHECKED_ENV(env)->SetElement(object, index, value);
}

napi_status napi_has_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    bool *result) {
  return CHECKED_ENV(env)->HasElement(object, index, result);
}

napi_status napi_get_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    napi_value *result) {
  return CHECKED_ENV(env)->GetElement(object, index, result);
}

napi_status napi_delete_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    bool *result) {
  return CHECKED_ENV(env)->DeleteElement(object, index, result);
}

napi_status napi_define_properties(
    napi_env env,
    napi_value object,
    size_t property_count,
    const napi_property_descriptor *properties) {
  return CHECKED_ENV(env)->DefineProperties(object, property_count, properties);
}

napi_status napi_object_freeze(napi_env env, napi_value object) {
  return CHECKED_ENV(env)->ObjectFreeze(object);
}

napi_status napi_object_seal(napi_env env, napi_value object) {
  return CHECKED_ENV(env)->ObjectSeal(object);
}

napi_status napi_is_array(napi_env env, napi_value value, bool *result) {
  return CHECKED_ENV(env)->IsArray(value, result);
}

napi_status
napi_get_array_length(napi_env env, napi_value value, uint32_t *result) {
  return CHECKED_ENV(env)->GetArrayLength(value, result);
}

napi_status
napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs, bool *result) {
  return CHECKED_ENV(env)->StrictEquals(lhs, rhs, result);
}

napi_status
napi_get_prototype(napi_env env, napi_value object, napi_value *result) {
  return CHECKED_ENV(env)->GetPrototype(object, result);
}

napi_status napi_create_object(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->CreateObject(result);
}

napi_status napi_create_array(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->CreateArray(result);
}

napi_status
napi_create_array_with_length(napi_env env, size_t length, napi_value *result) {
  return CHECKED_ENV(env)->CreateArray(length, result);
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
  return CHECKED_ENV(env)->CreateBigInt(value, result);
}

napi_status
napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value *result) {
  return CHECKED_ENV(env)->CreateBigInt(value, result);
}

napi_status napi_create_bigint_words(
    napi_env env,
    int sign_bit,
    size_t word_count,
    const uint64_t *words,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateBigInt(sign_bit, word_count, words, result);
}

napi_status napi_get_boolean(napi_env env, bool value, napi_value *result) {
  return CHECKED_ENV(env)->GetBoolean(value, result);
}

napi_status
napi_create_symbol(napi_env env, napi_value description, napi_value *result) {
  return CHECKED_ENV(env)->CreateSymbol(description, result);
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
  return CHECKED_ENV(env)->TypeOf(value, result);
}

napi_status napi_get_undefined(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->GetUndefined(result);
}

napi_status napi_get_null(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->GetNull(result);
}

// Gets all callback info in a single call. (Ugly, but faster.)
napi_status napi_get_cb_info(
    napi_env env, // [in] NAPI environment handle
    napi_callback_info cbinfo, // [in] Opaque callback-info handle
    size_t *argc, // [in-out] Specifies the size of the provided argv array
                  // and receives the actual count of args.
    napi_value *argv, // [out] Array of values
    napi_value *this_arg, // [out] Receives the JS 'this' arg for the call
    void **data) { // [out] Receives the data pointer for the callback.
  return CHECKED_ENV(env)->GetCallbackInfo(cbinfo, argc, argv, this_arg, data);
}

napi_status napi_get_new_target(
    napi_env env,
    napi_callback_info cbinfo,
    napi_value *result) {
  return CHECKED_ENV(env)->GetNewTarget(cbinfo, result);
}

napi_status napi_call_function(
    napi_env env,
    napi_value recv,
    napi_value func,
    size_t argc,
    const napi_value *argv,
    napi_value *result) {
  return CHECKED_ENV(env)->CallFunction(recv, func, argc, argv, result);
}

napi_status napi_get_global(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->GetGlobal(result);
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
  return CHECKED_ENV(env)->GetBigIntValue(value, result, lossless);
}

napi_status napi_get_value_bigint_uint64(
    napi_env env,
    napi_value value,
    uint64_t *result,
    bool *lossless) {
  return CHECKED_ENV(env)->GetBigIntValue(value, result, lossless);
}

napi_status napi_get_value_bigint_words(
    napi_env env,
    napi_value value,
    int *sign_bit,
    size_t *word_count,
    uint64_t *words) {
  return CHECKED_ENV(env)->GetBigIntValue(value, sign_bit, word_count, words);
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
  return CHECKED_ENV(env)->CoerceToBool(value, result);
}

#define GEN_COERCE_FUNCTION(UpperCaseName, MixedCaseName, LowerCaseName) \
  napi_status napi_coerce_to_##LowerCaseName(                            \
      napi_env env, napi_value value, napi_value *result) {              \
    return CHECKED_ENV(env)->CoerceTo##MixedCaseName(value, result);     \
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
  return CHECKED_ENV(env)->Wrap(
      js_object, native_object, finalize_cb, finalize_hint, result);
}

napi_status napi_unwrap(napi_env env, napi_value obj, void **result) {
  return CHECKED_ENV(env)->Unwrap(obj, result);
}

napi_status napi_remove_wrap(napi_env env, napi_value obj, void **result) {
  return CHECKED_ENV(env)->RemoveWrap(obj, result);
}

napi_status napi_create_external(
    napi_env env,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  return CHECKED_ENV(env)->CreateExternal(
      data, finalize_cb, finalize_hint, result);
}

NAPI_EXTERN napi_status napi_type_tag_object(
    napi_env env,
    napi_value object,
    const napi_type_tag *type_tag) {
  return CHECKED_ENV(env)->TypeTagObject(object, type_tag);
}

NAPI_EXTERN napi_status napi_check_object_type_tag(
    napi_env env,
    napi_value object,
    const napi_type_tag *type_tag,
    bool *result) {
  return CHECKED_ENV(env)->CheckObjectTypeTag(object, type_tag, result);
}

napi_status
napi_get_value_external(napi_env env, napi_value value, void **result) {
  return CHECKED_ENV(env)->GetValueExternal(value, result);
}

// Set initial_refcount to 0 for a weak reference, >0 for a strong reference.
napi_status napi_create_reference(
    napi_env env,
    napi_value value,
    uint32_t initial_refcount,
    napi_ref *result) {
  return CHECKED_ENV(env)->CreateReference(value, initial_refcount, result);
}

// Deletes a reference. The referenced value is released, and may be GC'd unless
// there are other references to it.
napi_status napi_delete_reference(napi_env env, napi_ref ref) {
  return CHECKED_ENV(env)->DeleteReference(ref);
}

// Increments the reference count, optionally returning the resulting count.
// After this call the reference will be a strong reference because its
// refcount is >0, and the referenced object is effectively "pinned".
// Calling this when the refcount is 0 and the object is unavailable
// results in an error.
napi_status napi_reference_ref(napi_env env, napi_ref ref, uint32_t *result) {
  return CHECKED_ENV(env)->ReferenceRef(ref, result);
}

// Decrements the reference count, optionally returning the resulting count. If
// the result is 0 the reference is now weak and the object may be GC'd at any
// time if there are no other references. Calling this when the refcount is
// already 0 results in an error.
napi_status napi_reference_unref(napi_env env, napi_ref ref, uint32_t *result) {
  return CHECKED_ENV(env)->ReferenceUnref(ref, result);
}

// Attempts to get a referenced value. If the reference is weak, the value might
// no longer be available, in that case the call is still successful but the
// result is NULL.
napi_status
napi_get_reference_value(napi_env env, napi_ref ref, napi_value *result) {
  return CHECKED_ENV(env)->GetReferenceValue(ref, result);
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
  return CHECKED_ENV(env)->IsArrayBuffer(value, result);
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
  return CHECKED_ENV(env)->RunScript(script, result);
}

napi_status napi_add_finalizer(
    napi_env env,
    napi_value js_object,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) {
  return CHECKED_ENV(env)->AddFinalizer(
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
