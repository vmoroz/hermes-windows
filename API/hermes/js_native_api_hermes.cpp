// TODO: change the stack references to use collection with chunks of equal 2^n
// size.
// TODO: Implement unit tests
// TODO: Sync with latest code
// TODO: Reorder the code
// TODO: Better native error handling
// TODO: review all object functions

// TODO: adjustExternalMemory
// TODO: Unique strings
// TODO: Fix references for Unwrap
// TODO: use extended message for errors
// TODO: see if finalizers can return error or exception

#define NAPI_EXPERIMENTAL

#include "hermes_napi.h"

#include "hermes.h"
#include "hermes/BCGen/HBC/BytecodeDataProvider.h"
#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/BCGen/HBC/BytecodeProviderFromSrc.h"
#include "hermes/SourceMap/SourceMapParser.h"
#include "hermes/Support/SimpleDiagHandler.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSDataView.h"
#include "hermes/VM/JSDate.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/PrimitiveBox.h"
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"

#include "llvh/ADT/SmallSet.h"
#include "llvh/Support/Compiler.h"
#include "llvh/Support/ConvertUTF.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

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

// TODO: review and rename macros
#define CHECK_NAPI(...)                       \
  do {                                        \
    if (napi_status status = (__VA_ARGS__)) { \
      return status;                          \
    }                                         \
  } while (false)

#define RETURN_STATUS_IF_FALSE(condition, status) \
  do {                                            \
    if (!(condition)) {                           \
      return env.setLastError((status));          \
    }                                             \
  } while (false)

#define CRASH_IF_FALSE(condition)  \
  do {                             \
    if (!(condition)) {            \
      assert(false && #condition); \
      std::terminate();            \
    }                              \
  } while (false)

#define CHECKED_ENV(env) \
  ((env) == nullptr)     \
      ? napi_invalid_arg \
      : reinterpret_cast<hermes::napi::NodeApiEnvironment *>(env)

#define CHECK_ARG(arg) \
  RETURN_STATUS_IF_FALSE(((arg) != nullptr), napi_invalid_arg)

#define CHECK_TYPED_ARG(arg, isCheck, status) \
  CHECK_ARG(arg);                             \
  RETURN_STATUS_IF_FALSE(phv(arg)->isCheck(), status)

#define CHECK_OBJECT_ARG(arg) \
  CHECK_TYPED_ARG((arg), isObject, napi_object_expected)

#define CHECK_EXTERNAL_ARG(arg) \
  CHECK_ARG(arg);               \
  RETURN_STATUS_IF_FALSE(getExternalValue(*phv(arg)), napi_invalid_arg)

#define CHECK_FUNCTION_ARG(arg)                    \
  do {                                             \
    CHECK_OBJECT_ARG(arg);                         \
    if (!vm::vmisa<vm::Callable>(*phv(arg))) {     \
      return setLastError(napi_function_expected); \
    }                                              \
  } while (false)

#define CHECK_STRING_ARG(arg) \
  CHECK_TYPED_ARG((arg), isString, napi_string_expected)

#define CHECK_NUMBER_ARG(arg) \
  CHECK_TYPED_ARG((arg), isNumber, napi_number_expected)

#define CHECK_BOOL_ARG(arg) \
  CHECK_TYPED_ARG((arg), isBool, napi_boolean_expected)

#define CHECK_STATUS(hermesStatus) \
  CHECK_NAPI(checkStatus(hermesStatus, napi_generic_failure))

#define CHECK_HERMES_STATUS(hermesStatus, status) \
  CHECK_NAPI(checkStatus(hermesStatus, status))

#define CONCAT_IMPL(left, right) left##right
#define CONCAT(left, right) CONCAT_IMPL(left, right)
#define TEMP_VARNAME(tempSuffix) CONCAT(temp_, tempSuffix)

#define ASSIGN_ELSE_RETURN_STATUS_IMPL(var, expr, status, tempSuffix) \
  auto TEMP_VARNAME(tempSuffix) = (expr);                             \
  CHECK_HERMES_STATUS(TEMP_VARNAME(tempSuffix).getStatus(), status);  \
  var = std::move(*TEMP_VARNAME(tempSuffix));

#define ASSIGN_ELSE_RETURN_FAILURE(var, expr) \
  ASSIGN_ELSE_RETURN_STATUS_IMPL(var, expr, napi_generic_failure, __COUNTER__)

#define ASSIGN_ELSE_RETURN_STATUS(var, expr, status) \
  ASSIGN_ELSE_RETURN_STATUS_IMPL(var, expr, status, __COUNTER__)

#define ASSIGN_ELSE_RETURN_HERMES_EXCEPTION_IMPL(var, expr, tempSuffix) \
  auto TEMP_VARNAME(tempSuffix) = (expr);                               \
  if (TEMP_VARNAME(tempSuffix).getStatus() ==                           \
      vm::ExecutionStatus::EXCEPTION) {                                 \
    return vm::ExecutionStatus::EXCEPTION;                              \
  }                                                                     \
  var = std::move(*TEMP_VARNAME(tempSuffix));

#define ASSIGN_ELSE_RETURN_HERMES_EXCEPTION(var, expr) \
  ASSIGN_ELSE_RETURN_HERMES_EXCEPTION_IMPL(var, expr, __COUNTER__)

#define THROW_RANGE_ERROR_IF_FALSE(condition, error, ...)           \
  do {                                                              \
    if (!(condition)) {                                             \
      StringBuilder message(__VA_ARGS__);                           \
      env.throwRangeError((error), message.c_str());                \
      return setLastError(napi_pending_exception, message.c_str()); \
    }                                                               \
  } while (0)

#define CHECK_TO_OBJECT(var, value) \
  CHECK_ARG(value);                 \
  ASSIGN_ELSE_RETURN_STATUS(        \
      var, convertToObject(makeHandle(value)), napi_object_expected)

namespace hermes {
namespace napi {

// Forward declarations
struct Finalizer;
struct HermesBuffer;
struct InstanceData;
struct HFContext;
struct NodeApiEnvironment;
struct Reference;
struct OrderedSet;

const napi_status napi_not_implemented = napi_generic_failure;

template <class T, std::size_t N>
constexpr std::size_t size(const T (&array)[N]) noexcept {
  return N;
}

template <class TEnum>
bool isInEnumRange(TEnum value, TEnum lowerBound, TEnum upperBound) noexcept {
  return lowerBound <= value && value <= upperBound;
}

struct Marker {
  size_t chunkIndex{0};
  size_t itemIndex{0};

  bool isValid() const {
    return chunkIndex < std::numeric_limits<size_t>::max();
  }

  static const Marker invalidMarker;
};

/*static*/ const Marker Marker::invalidMarker{
    std::numeric_limits<size_t>::max(),
    0};

template <typename T>
struct NonMovableObjStack {
  NonMovableObjStack() {
    // There is always at least one chunk in the storage
    std::vector<T> firstChunk;
    firstChunk.reserve(ChunkSize);
    storage_.push_back(std::move(firstChunk));
  }

  bool empty() const {
    return storage_[0].empty();
  }

  template <typename... TArgs>
  void emplaceBack(TArgs &&...args) {
    auto &storageChunk = storage_.back();
    if (storageChunk.size() == storageChunk.capacity()) {
      std::vector<T> newChunk;
      newChunk.reserve(std::min(storageChunk.capacity() * 2, MaxChunkSize));
      storage_.push_back(std::move(newChunk));
      storageChunk = storage_.back();
    }
    storageChunk.emplace_back(std::forward<TArgs>(args)...);
  }

  T &back() {
    return storage_.back().back();
  }

  bool popBack() {
    auto &storageChunk = storage_.back();
    if (storageChunk.empty()) {
      return false;
    }

    storageChunk.pop_back();
    if (storageChunk.empty() && storage_.size() > 1) {
      storage_.pop_back();
    }

    return true;
  }

  bool popMarker(const Marker &marker) {
    if (marker.chunkIndex > storage_.size()) {
      return false; // Invalid ChunkIndex
    } else if (marker.chunkIndex == storage_.size()) {
      // ChunkIndex is valid only if ItemIndex is 0.
      // In that case we have nothing to remove.
      return marker.itemIndex == 0;
    }

    auto &markerChunk = storage_[marker.chunkIndex];
    if (marker.itemIndex > markerChunk.size()) {
      return false; // Invalid ItemIndex
    }

    if (marker.chunkIndex < storage_.size() - 1) {
      // Delete the whole chunks
      storage_.erase(storage_.begin() + marker.chunkIndex + 1, storage_.end());
    }

    if (marker.chunkIndex > 0 && marker.itemIndex == 0) {
      // Delete the last chunk
      storage_.erase(storage_.begin() + marker.chunkIndex, storage_.end());
    } else if (marker.itemIndex < markerChunk.size()) {
      // Delete items in the marker chunk
      markerChunk.erase(
          markerChunk.begin() + marker.itemIndex, markerChunk.end());
    }

    return true;
  }

  // New marker points to a location where to insert a new element.
  // Thus, it always points to an invalid location after the last element.
  Marker createMarker() {
    auto &lastChunk = storage_.back();
    if (lastChunk.size() < lastChunk.capacity()) {
      return {storage_.size() - 1, lastChunk.size()};
    } else {
      return {storage_.size(), 0};
    }
  }

  Marker getPreviousMarker(const Marker &marker) {
    if (marker.itemIndex > 0) {
      return {marker.chunkIndex, marker.itemIndex - 1};
    } else if (marker.chunkIndex > 0) {
      auto prevChunkIndex = marker.chunkIndex - 1;
      if (storage_[prevChunkIndex].size() > 0) {
        return {prevChunkIndex, storage_[prevChunkIndex].size() - 1};
      }
    }
    return Marker::invalidMarker;
  }

  T *at(const Marker &marker) {
    if (marker.chunkIndex >= storage_.size()) {
      return nullptr;
    }
    auto &chunk = storage_[marker.chunkIndex];
    if (marker.itemIndex >= chunk.size()) {
      return nullptr;
    }
    return &chunk[marker.itemIndex];
  }

  template <typename F>
  void forEach(const F &f) noexcept {
    for (const auto &storageChunk : storage_) {
      for (const auto &item : storageChunk) {
        f(item);
      }
    }
  }

 private:
  static const size_t ChunkSize = 16;
  static const size_t MaxChunkSize = 4096;
  std::vector<std::vector<T>>
      storage_; // There is always at least one chunk in the storage
};

template <typename T>
struct LinkedList {
  LinkedList() noexcept {
    // The list is circular:
    // head.next_ points to the first item
    // head.prev_ points to the last item
    head_.next_ = &head_;
    head_.prev_ = &head_;
  }

  struct Item {
    void linkNext(T *item) noexcept {
      if (item->isLinked()) {
        item->unlink();
      }
      item->prev_ = this;
      item->next_ = next_;
      item->next_->prev_ = item;
      next_ = item;
    }

    void unlink() noexcept {
      prev_->next_ = next_;
      next_->prev_ = prev_;
      prev_ = nullptr;
      next_ = nullptr;
    }

    bool isLinked() const noexcept {
      return prev_ != nullptr;
    }

    friend LinkedList;

   private:
    Item *next_{};
    Item *prev_{};
  };

  void pushFront(T *item) noexcept {
    head_.linkNext(item);
  }

  void pushBack(T *item) noexcept {
    head_.prev_->linkNext(item);
  }

  T *begin() noexcept {
    return static_cast<T *>(head_.next_);
  }

  // The end() returns a pointer to an invalid object.
  T *end() noexcept {
    return static_cast<T *>(&head_);
  }

  bool isEmpty() noexcept {
    return head_.next_ == head_.prev_;
  }

  template <typename TLambda>
  void forEach(TLambda lambda) noexcept {
    for (auto item = begin(); item != end();) {
      // lambda can delete the item - get the next one before calling it.
      auto nextItem = static_cast<T *>(item->next_);
      lambda(item);
      item = nextItem;
    }
  }

 private:
  Item head_;
};

napi_value napiValue(const vm::PinnedHermesValue *hv) noexcept {
  return reinterpret_cast<napi_value>(const_cast<vm::PinnedHermesValue *>(hv));
}

namespace {

const vm::PinnedHermesValue *phv(napi_value value) noexcept {
  return reinterpret_cast<vm::PinnedHermesValue *>(value);
}

const vm::PinnedHermesValue *phv(const vm::PinnedHermesValue *value) noexcept {
  return value;
}

Reference *asReference(napi_ext_ref ref) noexcept {
  return reinterpret_cast<Reference *>(ref);
}

Reference *asReference(napi_ref ref) noexcept {
  return reinterpret_cast<Reference *>(ref);
}

Reference *asReference(void *ref) noexcept {
  return reinterpret_cast<Reference *>(ref);
}

} // namespace

struct CallbackInfo final {
  CallbackInfo(HFContext &context, vm::NativeArgs &hvArgs) noexcept
      : context_(context), hvArgs_(hvArgs) {}

  void args(napi_value *args, size_t *argCount) noexcept {
    *args = napiValue(&*hvArgs_.begin());
    *argCount = hvArgs_.getArgCount();
  }

  size_t argCount() noexcept {
    return hvArgs_.getArgCount();
  }

  napi_value thisArg() noexcept {
    return napiValue(&hvArgs_.getThisArg());
  }

  void *data() noexcept;

  napi_value getNewTarget() noexcept {
    return napiValue(&hvArgs_.getNewTarget());
  }

 private:
  HFContext &context_;
  vm::NativeArgs &hvArgs_;
};

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

void *CallbackInfo::data() noexcept {
  return context_.data_;
}

enum class UnwrapAction { KeepWrap, RemoveWrap };

enum class NapiPredefined {
  UndefinedValue,
  NullValue,
  TrueValue,
  FalseValue,
  ExternalValueSymbol,
  CodeSymbol,
  TypeTagSymbol,
  PredefinedCount // a special value that must be last in the enum
};

enum class IfNotFound {
  ThenCreate,
  ThenReturnNull,
};

struct ExternalValue : vm::DecoratedObject::Decoration {
  ExternalValue(NodeApiEnvironment &env) noexcept : env_(env) {}
  ExternalValue(NodeApiEnvironment &env, void *nativeData) noexcept
      : env_(env), nativeData_(nativeData) {}

  ExternalValue(const ExternalValue &other) = delete;
  ExternalValue &operator=(const ExternalValue &other) = delete;

  ~ExternalValue() override;

  size_t getMallocSize() const override {
    return sizeof(*this);
  }

  void addFinalizer(Finalizer *finalizer) noexcept;

  void *nativeData() noexcept {
    return nativeData_;
  }

  void setNativeData(void *value) noexcept {
    nativeData_ = value;
  }

 private:
  NodeApiEnvironment &env_;
  void *nativeData_{};
  LinkedList<Finalizer> finalizers_;
};

struct NodeApiEnvironment final {
  explicit NodeApiEnvironment(
      const vm::RuntimeConfig &runtimeConfig = {}) noexcept;
  ~NodeApiEnvironment();

 public: // Function accessed from the C-base NAPI
  template <typename... TArgs>
  napi_status setLastError(napi_status errorCode, TArgs &&...args) noexcept;
  napi_status clearLastError() noexcept;
  napi_status getLastErrorInfo(
      const napi_extended_error_info **result) noexcept;

  napi_status getUndefined(napi_value *result) noexcept;
  napi_status getNull(napi_value *result) noexcept;
  napi_status getGlobal(napi_value *result) noexcept;
  napi_status getBoolean(bool value, napi_value *result) noexcept;

  napi_status createObject(napi_value *result) noexcept;
  napi_status createArray(napi_value *result) noexcept;
  napi_status createArray(size_t length, napi_value *result) noexcept;
  template <class T, std::enable_if_t<std::is_arithmetic_v<T>, bool> = true>
  napi_status createNumber(T value, napi_value *result) noexcept;
  napi_status createStringLatin1(
      const char *str,
      size_t length,
      napi_value *result) noexcept;
  napi_status
  createStringUtf8(const char *str, size_t length, napi_value *result) noexcept;
  napi_status createStringUtf16(
      const char16_t *str,
      size_t length,
      napi_value *result) noexcept;
  napi_status createSymbol(napi_value description, napi_value *result) noexcept;
  napi_status createFunction(
      const char *utf8Name,
      size_t length,
      napi_callback callback,
      void *callbackData,
      napi_value *result) noexcept;
  napi_status
  createError(napi_value code, napi_value msg, napi_value *result) noexcept;
  napi_status
  createTypeError(napi_value code, napi_value msg, napi_value *result) noexcept;
  napi_status createRangeError(
      napi_value code,
      napi_value msg,
      napi_value *result) noexcept;

  napi_status typeOf(napi_value value, napi_valuetype *result) noexcept;
  napi_status getNumberValue(napi_value value, double *result) noexcept;
  napi_status getNumberValue(napi_value value, int32_t *result) noexcept;
  napi_status getNumberValue(napi_value value, uint32_t *result) noexcept;
  napi_status getNumberValue(napi_value value, int64_t *result) noexcept;
  napi_status getBoolValue(napi_value value, bool *result) noexcept;
  napi_status getValueStringLatin1(
      napi_value value,
      char *buf,
      size_t bufsize,
      size_t *result) noexcept;
  napi_status getValueStringUtf8(
      napi_value value,
      char *buf,
      size_t bufsize,
      size_t *result) noexcept;
  napi_status getValueStringUtf16(
      napi_value value,
      char16_t *buf,
      size_t bufsize,
      size_t *result) noexcept;

  napi_status coerceToBool(napi_value value, napi_value *result) noexcept;
  napi_status coerceToNumber(napi_value value, napi_value *result) noexcept;
  napi_status coerceToObject(napi_value value, napi_value *result) noexcept;
  napi_status coerceToString(napi_value value, napi_value *result) noexcept;

  napi_status getPrototype(napi_value object, napi_value *result) noexcept;
  napi_status getPropertyNames(napi_value object, napi_value *result) noexcept;
  napi_status getForInPropertyNames(
      vm::Handle<vm::JSObject> object,
      napi_key_conversion keyConversion,
      napi_value *result) noexcept;
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
  setElement(napi_value object, uint32_t index, napi_value value) noexcept;
  napi_status
  hasElement(napi_value object, uint32_t index, bool *result) noexcept;
  napi_status
  getElement(napi_value object, uint32_t index, napi_value *result) noexcept;
  napi_status
  deleteElement(napi_value object, uint32_t index, bool *result) noexcept;
  napi_status defineProperties(
      napi_value object,
      size_t propertyCount,
      const napi_property_descriptor *properties) noexcept;

  napi_status isArray(napi_value value, bool *result) noexcept;
  napi_status getArrayLength(napi_value value, uint32_t *result) noexcept;

  napi_status
  strictEquals(napi_value lhs, napi_value rhs, bool *result) noexcept;

  napi_status callFunction(
      napi_value object,
      napi_value func,
      size_t argCount,
      const napi_value *args,
      napi_value *result) noexcept;
  napi_status newInstance(
      napi_value constructor,
      size_t argc,
      const napi_value *argv,
      napi_value *result) noexcept;
  napi_status
  instanceOf(napi_value object, napi_value constructor, bool *result) noexcept;

  napi_status getCallbackInfo(
      CallbackInfo *callbackInfo,
      size_t *argCount,
      napi_value *args,
      napi_value *thisArg,
      void **data) noexcept;
  napi_status getNewTarget(
      CallbackInfo *callbackInfo,
      napi_value *result) noexcept;
  napi_status defineClass(
      const char *utf8Name,
      size_t length,
      napi_callback constructor,
      void *callbackData,
      size_t propertyCount,
      const napi_property_descriptor *properties,
      napi_value *result) noexcept;

  napi_status wrapObject(
      napi_value object,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_ref *result) noexcept;
  napi_status
  unwrapObject(napi_value object, UnwrapAction action, void **result) noexcept;
  napi_status createExternal(
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_value *result) noexcept;
  napi_status getValueExternal(napi_value value, void **result) noexcept;

  napi_status createReference(
      napi_value value,
      uint32_t initialRefCount,
      napi_ref *result) noexcept;
  napi_status deleteReference(napi_ref ref) noexcept;
  napi_status incReference(napi_ref ref, uint32_t *result) noexcept;
  napi_status decReference(napi_ref ref, uint32_t *result) noexcept;
  napi_status getReferenceValue(napi_ref ref, napi_value *result) noexcept;
  napi_status openHandleScope(napi_handle_scope *result) noexcept;
  napi_status closeHandleScope(napi_handle_scope scope) noexcept;
  napi_status openEscapableHandleScope(
      napi_escapable_handle_scope *result) noexcept;
  napi_status closeEscapableHandleScope(
      napi_escapable_handle_scope scope) noexcept;
  napi_status escapeHandle(
      napi_escapable_handle_scope scope,
      napi_value escapee,
      napi_value *result) noexcept;

  napi_status throwError(napi_value error) noexcept;
  napi_status throwError(
      const char *code,
      const char *message,
      const vm::PinnedHermesValue &prototype) noexcept;
  napi_status throwError(const char *code, const char *message) noexcept;
  napi_status throwTypeError(const char *code, const char *message) noexcept;
  napi_status throwRangeError(const char *code, const char *message) noexcept;
  napi_status isError(napi_value value, bool *result) noexcept;

  napi_status isExceptionPending(bool *result) noexcept;
  napi_status getAndClearLastException(napi_value *result) noexcept;

  napi_status isArrayBuffer(napi_value value, bool *result) noexcept;
  napi_status createArrayBuffer(
      size_t byteLength,
      void **data,
      napi_value *result) noexcept;
  napi_status createExternalArrayBuffer(
      void *externalData,
      size_t byteLength,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_value *result) noexcept;
  napi_status getArrayBufferInfo(
      napi_value arrayBuffer,
      void **data,
      size_t *byteLength) noexcept;
  napi_status isTypedArray(napi_value value, bool *result) noexcept;
  template <typename TItem, vm::CellKind CellKind>
  napi_status createTypedArray(
      size_t length,
      vm::JSArrayBuffer *buffer,
      size_t byteOffset,
      vm::JSTypedArrayBase **result) noexcept;
  napi_status createTypedArray(
      napi_typedarray_type type,
      size_t length,
      napi_value arrayBuffer,
      size_t byteOffset,
      napi_value *result) noexcept;
  napi_status getTypedArrayInfo(
      napi_value typedArray,
      napi_typedarray_type *type,
      size_t *length,
      void **data,
      napi_value *arrayBuffer,
      size_t *byteOffset) noexcept;
  napi_status createDataView(
      size_t byteLength,
      napi_value arrayBuffer,
      size_t byteOffset,
      napi_value *result) noexcept;
  napi_status isDataView(napi_value value, bool *result) noexcept;
  napi_status getDataViewInfo(
      napi_value dataView,
      size_t *byteLength,
      void **data,
      napi_value *arrayBuffer,
      size_t *byteOffset) noexcept;

  napi_status getVersion(uint32_t *result) noexcept;

  napi_status createPromise(
      napi_value *promise,
      napi_value *resolveFunction,
      napi_value *rejectFunction) noexcept;
  napi_status createPromise(
      napi_deferred *deferred,
      napi_value *result) noexcept;
  napi_status resolveDeferred(
      napi_deferred deferred,
      napi_value resolution) noexcept;
  napi_status rejectDeferred(
      napi_deferred deferred,
      napi_value resolution) noexcept;
  napi_status concludeDeferred(
      napi_deferred deferred,
      const char *propertyName,
      napi_value result) noexcept;
  napi_status isPromise(napi_value value, bool *result) noexcept;

  napi_status adjustExternalMemory(
      int64_t change_in_bytes,
      int64_t *adjusted_value) noexcept;

  napi_status createDate(double dateTime, napi_value *result) noexcept;
  napi_status isDate(napi_value value, bool *result) noexcept;
  napi_status getDateValue(napi_value value, double *result) noexcept;

  napi_status addFinalizer(
      napi_value object,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_ref *result) noexcept;

  napi_status setInstanceData(
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint) noexcept;
  napi_status getInstanceData(void **nativeData) noexcept;

  napi_status detachArrayBuffer(napi_value arrayBuffer) noexcept;
  napi_status isDetachedArrayBuffer(
      napi_value arrayBuffer,
      bool *result) noexcept;

  napi_status typeTagObject(
      napi_value object,
      const napi_type_tag *typeTag) noexcept;
  napi_status checkObjectTypeTag(
      napi_value object,
      const napi_type_tag *typeTag,
      bool *result) noexcept;
  napi_status objectFreeze(napi_value object) noexcept;
  napi_status objectSeal(napi_value object) noexcept;

 public: // Extension methods
  napi_status incRefCount() noexcept;
  napi_status decRefCount() noexcept;
  napi_status collectGarbage() noexcept;

  napi_status createStrongReference(
      napi_value value,
      napi_ext_ref *result) noexcept;
  napi_status createStrongReferenceWithData(
      napi_value value,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_ext_ref *result) noexcept;
  napi_status createWeakReference(
      napi_value value,
      napi_ext_ref *result) noexcept;
  napi_status incReference(napi_ext_ref ref) noexcept;
  napi_status decReference(napi_ext_ref ref) noexcept;
  napi_status getReferenceValue(napi_ext_ref ref, napi_value *result) noexcept;

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

 public:
  template <class TObject>
  napi_status convertToObject(
      TObject object,
      vm::MutableHandle<vm::JSObject> *result) noexcept;

  template <class TKey, class TValue>
  napi_status putComputed(
      vm::Handle<vm::JSObject> objHandle,
      TKey key,
      TValue value,
      bool *result) noexcept;

  template <class TKey>
  napi_status hasComputed(
      vm::Handle<vm::JSObject> objHandle,
      TKey key,
      bool *result) noexcept;

  template <class TKey>
  napi_status getComputed(
      vm::Handle<vm::JSObject> objHandle,
      TKey key,
      napi_value *result) noexcept;

  template <class TKey>
  napi_status deleteComputed(
      vm::Handle<vm::JSObject> objHandle,
      TKey key,
      bool *result) noexcept;

  template <class TKey>
  napi_status getOwnComputedDescriptor(
      vm::Handle<vm::JSObject> objHandle,
      TKey key,
      vm::MutableHandle<vm::SymbolID> &tmpSymbolStorage,
      vm::ComputedPropertyDescriptor &desc,
      bool *result) noexcept;

  template <class T>
  vm::MutableHandle<T> makeMutableHandle() noexcept;

  vm::CallResult<vm::Handle<vm::JSObject>> convertToObject(
      vm::Handle<> value) noexcept;

  napi_status convertKeyStorageToArray(
      vm::Handle<vm::BigStorage> keyStorage,
      uint32_t startIndex,
      uint32_t length,
      napi_key_conversion keyConversion,
      napi_value *result) noexcept;

  vm::Handle<> makeHandle(const vm::PinnedHermesValue *value) noexcept;
  vm::Handle<> makeHandle(napi_value value) noexcept;
  vm::Handle<> makeHandle(vm::HermesValue value) noexcept;
  vm::Handle<> makeHandle(vm::Handle<> value) noexcept;
  vm::Handle<> makeHandle(uint32_t value) noexcept;

  template <class T>
  vm::CallResult<vm::Handle<T>> makeHandle(
      vm::CallResult<vm::PseudoHandle<T>> &&callResult) noexcept;

  template <class T>
  vm::CallResult<vm::MutableHandle<T>> makeMutableHandle(
      vm::CallResult<vm::PseudoHandle<T>> &&callResult) noexcept;

  template <typename F>
  napi_status handleExceptions(const F &f) noexcept;

  vm::Handle<> toHandle(napi_value value) noexcept;

  napi_status genericFailure(const char *message) noexcept;

  vm::WeakRoot<vm::JSObject> createWeakRoot(vm::JSObject *object) noexcept;

  const vm::PinnedHermesValue &lockWeakObject(
      vm::WeakRoot<vm::JSObject> &weakRoot) noexcept;

  template <typename TLambda>
  void callIntoModule(TLambda &&call) noexcept;

  napi_status callFinalizer(
      napi_finalize finalizeCallback,
      void *nativeData,
      void *finalizeHint) noexcept;

  static vm::JSObject *getObject(const vm::HermesValue &value) noexcept;
  static vm::Handle<vm::JSObject> toObjectHandle(napi_value value) noexcept;
  static vm::Handle<vm::JSObject> toObjectHandle(
      const vm::PinnedHermesValue *value) noexcept;
  static vm::Handle<vm::JSArray> toArrayHandle(napi_value value) noexcept;
  static vm::Handle<vm::HermesValue> stringHandle(napi_value value) noexcept;
  vm::Handle<vm::HermesValue> toHandle(const vm::HermesValue &value) noexcept;
  void addToFinalizerQueue(Finalizer *finalizer) noexcept;
  void addGCRoot(Reference *reference) noexcept;
  void addFinalizingGCRoot(Reference *reference) noexcept;

  void pushOrderedSet(OrderedSet &set) noexcept;
  void popOrderedSet() noexcept;

  vm::CallResult<vm::Handle<>> convertIndexToString(double value) noexcept;
  vm::ExecutionStatus convertKeyNumbersToStrings(
      vm::Handle<vm::JSArray> array) noexcept;

  template <class T, class TResult>
  napi_status setResult(T &&value, TResult *result) noexcept;
  template <class T, class TResult>
  napi_status setOptionalResult(T &&value, TResult *result) noexcept;
  template <class T>
  napi_status setOptionalResult(T &&value, std::nullptr_t) noexcept;
  template <class T>
  napi_status setResultUnsafe(T &&value, T *result) noexcept;
  napi_status setResultUnsafe(
      vm::HermesValue value,
      napi_value *result) noexcept;
  template <class T>
  napi_status setResultUnsafe(
      vm::Handle<T> &&handle,
      napi_value *result) noexcept;
  template <class T>
  napi_status setResultUnsafe(
      vm::PseudoHandle<T> &&handle,
      napi_value *result) noexcept;
  template <class T>
  napi_status setResultUnsafe(
      vm::Handle<T> &&handle,
      vm::MutableHandle<T> *result) noexcept;
  template <class T, class TResult>
  napi_status setResultUnsafe(
      vm::CallResult<T> &&value,
      TResult *result) noexcept;
  template <class T, class TResult>
  napi_status setResultUnsafe(
      vm::CallResult<T> &&,
      napi_status onException,
      TResult *result) noexcept;

  napi_status createSymbolID(
      const char *str,
      size_t length,
      vm::MutableHandle<vm::SymbolID> *result) noexcept;

  napi_status createSymbolID(
      napi_value str,
      vm::MutableHandle<vm::SymbolID> *result) noexcept;

  napi_status runReferenceFinalizers() noexcept;

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
  napi_status stringHVFromUtf8(
      const uint8_t *utf8,
      size_t length,
      vm::Handle<> *result) noexcept;
  napi_status stringHVFromUtf8(const char *utf8, vm::Handle<> *result) noexcept;

  napi_value addStackValue(vm::HermesValue value) noexcept;
  napi_value toNapiValue(const vm::PinnedHermesValue &value) noexcept;
  napi_status checkStatus(
      vm::ExecutionStatus hermesStatus,
      napi_status status) noexcept;

  napi_status newFunction(
      vm::SymbolID name,
      napi_callback callback,
      void *callbackData,
      napi_value *result) noexcept;

  static bool isHermesBytecode(const uint8_t *data, size_t len) noexcept;

  napi_status symbolIDFromPropertyDescriptor(
      const napi_property_descriptor *p,
      vm::MutableHandle<vm::SymbolID> *result) noexcept;

  const vm::PinnedHermesValue &getPredefined(
      NapiPredefined predefinedKey) noexcept;

  vm::CallResult<bool> hasPrivate(
      vm::Handle<vm::JSObject> objHandle,
      NapiPredefined key) noexcept;

  vm::CallResult<vm::PseudoHandle<>> getPrivate(
      vm::Handle<vm::JSObject> objHandle,
      NapiPredefined key) noexcept;

  vm::CallResult<bool> setPrivate(
      vm::Handle<vm::JSObject> objHandle,
      NapiPredefined key,
      const vm::HermesValue &value) noexcept;

  vm::CallResult<bool> deletePrivate(
      vm::Handle<vm::JSObject> objHandle,
      NapiPredefined key) noexcept;

  napi_status addObjectFinalizer(
      const vm::PinnedHermesValue *value,
      Finalizer *finalizer) noexcept;

  vm::PseudoHandle<vm::DecoratedObject> createExternal(
      void *nativeData,
      ExternalValue **externalValue) noexcept;

  ExternalValue *getExternalValue(const vm::HermesValue &value) noexcept;

  napi_status getExternalValue(
      vm::Handle<vm::JSObject> objHandle,
      IfNotFound ifNotFound,
      ExternalValue **result) noexcept;

  vm::Runtime &runtime() noexcept;

  struct RuntimeAccessor : vm::Runtime {
    using vm::Runtime::falseValue_;
    using vm::Runtime::global_;
    using vm::Runtime::nullValue_;
    using vm::Runtime::trueValue_;
    using vm::Runtime::undefinedValue_;
  };
  RuntimeAccessor &runtimeAccessor() noexcept {
    return static_cast<RuntimeAccessor &>(runtime_);
  }

 private:
#ifdef HERMESJSI_ON_STACK
  StackRuntime stackRuntime_;
#else
  std::shared_ptr<vm::Runtime> rt_;
#endif
  vm::Runtime &runtime_;
// TODO: fix #ifdef HERMES_ENABLE_DEBUGGER
#if 0
  friend class debugger::Debugger;
  std::unique_ptr<debugger::Debugger> debugger_;
#endif
  vm::experiments::VMExperimentFlags vmExperimentFlags_{0};
  std::shared_ptr<vm::CrashManager> crashMgr_;

  /// Compilation flags used by prepareJavaScript().
  hbc::CompileFlags compileFlags_{};
  /// The default setting of "emit async break check" in this runtime.
  bool defaultEmitAsyncBreakCheck_{false};

  std::atomic<int> refCount_{1};

  vm::PinnedHermesValue lastException_{EmptyHermesValue};
  std::array<
      vm::PinnedHermesValue,
      static_cast<size_t>(NapiPredefined::PredefinedCount)>
      predefinedValues_{};

  NonMovableObjStack<vm::PinnedHermesValue> stackValues_;
  NonMovableObjStack<Marker> stackMarkers_;
  static constexpr uint32_t kEscapeableSentinelNativeValue = 0x35456789;
  static constexpr uint32_t kUsedEscapeableSentinelNativeValue =
      kEscapeableSentinelNativeValue + 1;
  static constexpr uint32_t kExternalValueTag = 0x00353637;
  static constexpr int32_t kExternalTagSlot = 0;
  static constexpr vm::HermesValue EmptyHermesValue{
      vm::HermesValue::encodeEmptyValue()};

  int openHandleScopes_{};
  int openCallbackScopes_{};

  // We store references in two different lists, depending on whether they
  // have `napi_finalizer` callbacks, because we must first finalize the
  // ones that have such a callback. See `~NodeApiEnvironment()` above for
  // details.
  LinkedList<Reference> gcRoots_{};
  LinkedList<Reference> finalizingGCRoots_{};
  LinkedList<Finalizer> finalizerQueue_{};
  bool isRunningFinalizers_{};

  std::string lastErrorMessage_;
  napi_extended_error_info lastError_{"", 0, 0, napi_ok};
  int openCallbackScopeCount_{};
  InstanceData *instanceData_{};

  llvh::SmallVector<OrderedSet *, 16> orderedSets_;

  NodeApiEnvironment &env{*this};
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

// Different types of references:
// 1. Strong reference - it can wrap up object of any type
//   a. Ref count maintains the reference lifetime. When it reaches zero it is
//   removed.
// 2. Weak reference - it can only wrap up objects
//   a. Ref count maintains the lifetime of the reference. When it reaches zero
//   it is removed.
// 3. Combined reference -
//   a. Ref count only for strong references. Zero converts it to a weak ref.
//   Removal is explicit if external code holds a reference.

// TODO: can we apply shared_ptr-like concept with two ref counts?

// A base class for References that wrap native data and must be finalized.
struct Finalizer : LinkedList<Finalizer>::Item {
  virtual void finalize(NodeApiEnvironment &env) noexcept = 0;

 protected:
  Finalizer() = default;

  ~Finalizer() noexcept {
    unlink();
  }
};

// A base class for all references.
struct Reference : LinkedList<Reference>::Item {
  enum class ReasonToDelete {
    ZeroRefCount,
    FinalizerCall,
    ExternalCall,
    EnvironmentShutdown,
  };

  static napi_status deleteReference(
      NodeApiEnvironment &env,
      Reference *reference,
      ReasonToDelete reason) noexcept {
    if (reference && reference->startDeleting(env, reason)) {
      delete reference;
    }
    return env.clearLastError();
  }

  virtual napi_status incRefCount(
      NodeApiEnvironment &env,
      uint32_t & /*result*/) noexcept {
    return env.genericFailure("This reference does not support ref count.");
  }

  virtual napi_status decRefCount(
      NodeApiEnvironment &env,
      uint32_t & /*result*/) noexcept {
    return env.genericFailure("This reference does not support ref count.");
  }

  virtual const vm::PinnedHermesValue &value(NodeApiEnvironment &env) noexcept {
    return env.getPredefined(NapiPredefined::UndefinedValue);
  }

  virtual void *nativeData() noexcept {
    return nullptr;
  }

  virtual void *finalizeHint() noexcept {
    return nullptr;
  }

  virtual vm::PinnedHermesValue *getGCRoot(
      NodeApiEnvironment & /*env*/) noexcept {
    return nullptr;
  }

  virtual vm::WeakRoot<vm::JSObject> *getGCWeakRoot(
      NodeApiEnvironment & /*env*/) noexcept {
    return nullptr;
  }

  static void getGCRoots(
      NodeApiEnvironment &env,
      LinkedList<Reference> &list,
      vm::RootAcceptor &acceptor) noexcept {
    list.forEach([&](Reference *ref) {
      if (vm::PinnedHermesValue *value = ref->getGCRoot(env)) {
        acceptor.accept(*value);
      }
    });
  }

  static void getGCWeakRoots(
      NodeApiEnvironment &env,
      LinkedList<Reference> &list,
      vm::WeakRootAcceptor &acceptor) noexcept {
    list.forEach([&](Reference *ref) {
      if (vm::WeakRoot<vm::JSObject> *weakRoot = ref->getGCWeakRoot(env)) {
        acceptor.acceptWeak(*weakRoot);
      }
    });
  }

  virtual napi_status callFinalizeCallback(NodeApiEnvironment &env) noexcept {
    return napi_ok;
  }

  virtual void finalize(NodeApiEnvironment &env) noexcept {}

  template <typename TItem>
  static void finalizeAll(
      NodeApiEnvironment &env,
      LinkedList<TItem> &list) noexcept {
    for (auto item = list.begin(); item != list.end(); item = list.begin()) {
      item->finalize(env);
    }
  }

  static void deleteAll(
      NodeApiEnvironment &env,
      LinkedList<Reference> &list,
      ReasonToDelete reason) noexcept {
    for (auto ref = list.begin(); ref != list.end(); ref = list.begin()) {
      deleteReference(env, ref, reason);
    }
  }

 protected:
  // Make protected to avoid using operator delete directly.
  // Use the deleteReference method instead.
  virtual ~Reference() noexcept {
    unlink();
  }

  virtual bool startDeleting(
      NodeApiEnvironment &env,
      ReasonToDelete /*reason*/) noexcept {
    return true;
  }
};

// A reference with a ref count that can be changed from any thread.
// The reference deletion is done as a part of GC root detection to avoid
// deletion in a random thread.
struct AtomicRefCountReference : Reference {
  napi_status incRefCount(NodeApiEnvironment &env, uint32_t &result) noexcept
      override {
    result = refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    CRASH_IF_FALSE(result > 1 && "The ref count cannot bounce from zero.");
    CRASH_IF_FALSE(result < MaxRefCount && "The ref count is too big.");
    return napi_ok;
  }

  napi_status decRefCount(NodeApiEnvironment &env, uint32_t &result) noexcept
      override {
    result = refCount_.fetch_sub(1, std::memory_order_release) - 1;
    if (result == 0) {
      std::atomic_thread_fence(std::memory_order_acquire);
    } else if (result > MaxRefCount) {
      // Decrement of an unsigned value below zero is getting to a very big
      // number.
      CRASH_IF_FALSE(
          result < MaxRefCount && "The ref count must not be negative.");
    }
    return napi_ok;
  }

 protected:
  uint32_t refCount() const noexcept {
    return refCount_;
  }

  bool startDeleting(NodeApiEnvironment &env, ReasonToDelete reason) noexcept
      override {
    return reason != ReasonToDelete::ExternalCall;
  }

 private:
  std::atomic<uint32_t> refCount_{1};

  static constexpr uint32_t MaxRefCount =
      std::numeric_limits<uint32_t>::max() / 2;
};

// Atomic ref counting for vm::PinnedHermesValue.
struct StrongReference : AtomicRefCountReference {
  static napi_status create(
      NodeApiEnvironment &env,
      const vm::PinnedHermesValue *value,
      StrongReference **result) noexcept {
    CHECK_ARG(value);
    CHECK_ARG(result);
    *result = new StrongReference(*value);
    env.addGCRoot(*result);
    return env.clearLastError();
  }

  const vm::PinnedHermesValue &value(
      NodeApiEnvironment &env) noexcept override {
    return value_;
  }

  vm::PinnedHermesValue *getGCRoot(NodeApiEnvironment &env) noexcept override {
    if (refCount() > 0) {
      return &value_;
    } else {
      deleteReference(env, this, ReasonToDelete::ZeroRefCount);
      return nullptr;
    }
  }

 protected:
  StrongReference(const vm::PinnedHermesValue &value) noexcept
      : value_(value) {}

 private:
  vm::PinnedHermesValue value_;
};

// Atomic ref counting for a vm::WeakRef<vm::HermesValue>.
struct WeakReference final : AtomicRefCountReference {
  static napi_status create(
      NodeApiEnvironment &env,
      const vm::PinnedHermesValue *value,
      WeakReference **result) noexcept {
    CHECK_OBJECT_ARG(value);
    CHECK_ARG(result);
    *result = new WeakReference(
        env.createWeakRoot(NodeApiEnvironment::getObject(*value)));
    env.addGCRoot(*result);
    return env.clearLastError();
  }

  const vm::PinnedHermesValue &value(
      NodeApiEnvironment &env) noexcept override {
    return env.lockWeakObject(weakRoot_);
  }

  vm::WeakRoot<vm::JSObject> *getGCWeakRoot(
      NodeApiEnvironment &env) noexcept override {
    if (refCount() > 0) {
      return &weakRoot_;
    } else {
      deleteReference(env, this, ReasonToDelete::ZeroRefCount);
      return nullptr;
    }
  }

 protected:
  WeakReference(vm::WeakRoot<vm::JSObject> weakRoot) noexcept
      : weakRoot_(weakRoot) {}

 private:
  vm::WeakRoot<vm::JSObject> weakRoot_;
};

// Keep vm::PinnedHermesValue when ref count > 0 or vm::WeakRoot<vm::JSObject>
// when ref count == 0. The ref count is not atomic and must be changed only
// from the JS thread.
struct ComplexReference : Reference {
  static napi_status create(
      NodeApiEnvironment &env,
      const vm::PinnedHermesValue *value,
      uint32_t initialRefCount,
      ComplexReference **result) noexcept {
    CHECK_OBJECT_ARG(value);
    CHECK_ARG(result);
    *result = new ComplexReference(
        initialRefCount,
        *value,
        initialRefCount == 0
            ? env.createWeakRoot(NodeApiEnvironment::getObject(*value))
            : vm::WeakRoot<vm::JSObject>{});
    env.addGCRoot(*result);
    return env.clearLastError();
  }

  napi_status incRefCount(NodeApiEnvironment &env, uint32_t &result) noexcept
      override {
    if (refCount_ == 0) {
      value_ = env.lockWeakObject(weakRoot_);
    }
    CRASH_IF_FALSE(++refCount_ >= MaxRefCount && "The ref count is too big.");
    result = refCount_;
    return env.clearLastError();
  }

  napi_status decRefCount(NodeApiEnvironment &env, uint32_t &result) noexcept
      override {
    if (refCount_ == 0) {
      // Ignore this error situation to match NAPI for V8 implementation.
      result = 0;
      return napi_ok;
    }
    if (--refCount_ == 0) {
      if (value_.isObject()) {
        weakRoot_ = env.createWeakRoot(NodeApiEnvironment::getObject(value_));
      } else {
        weakRoot_ = vm::WeakRoot<vm::JSObject>{};
      }
    }
    result = refCount_;
    return env.clearLastError();
  }

  const vm::PinnedHermesValue &value(
      NodeApiEnvironment &env) noexcept override {
    if (refCount_ > 0) {
      return value_;
    } else {
      return env.lockWeakObject(weakRoot_);
    }
  }

  vm::PinnedHermesValue *getGCRoot(
      NodeApiEnvironment & /*env*/) noexcept override {
    return (refCount_ > 0) ? &value_ : nullptr;
  }

  vm::WeakRoot<vm::JSObject> *getGCWeakRoot(
      NodeApiEnvironment & /*env*/) noexcept override {
    return (refCount_ == 0 && weakRoot_) ? &weakRoot_ : nullptr;
  }

 protected:
  ComplexReference(
      uint32_t initialRefCount,
      const vm::PinnedHermesValue &value,
      vm::WeakRoot<vm::JSObject> weakRoot) noexcept
      : refCount_(initialRefCount), value_(value), weakRoot_(weakRoot) {}

  uint32_t refCount() const noexcept {
    return refCount_;
  }

 private:
  uint32_t refCount_{0};
  vm::PinnedHermesValue value_;
  vm::WeakRoot<vm::JSObject> weakRoot_;

  static constexpr uint32_t MaxRefCount =
      std::numeric_limits<uint32_t>::max() / 2;
};

// TODO: make sure that in the unwrap we can convert the self deleting complex
// ref to manually deleted one if the ref count is incremented

// Store finalizeHint if it is not null.
template <typename TBaseReference>
struct FinalizeHintHolder : TBaseReference {
  template <typename... TArgs>
  FinalizeHintHolder(void *finalizeHint, TArgs &&...args) noexcept
      : TBaseReference(std::forward<TArgs>(args)...),
        finalizeHint_(finalizeHint) {}

  void *finalizeHint() noexcept override {
    return finalizeHint_;
  }

 private:
  void *finalizeHint_;
};

// Store and call finalizeCallback if it is not null.
template <typename TBaseReference>
struct FinalizeCallbackHolder : TBaseReference {
  template <typename... TArgs>
  FinalizeCallbackHolder(
      napi_finalize finalizeCallback,
      TArgs &&...args) noexcept
      : TBaseReference(std::forward<TArgs>(args)...),
        finalizeCallback_(finalizeCallback) {}

  napi_status callFinalizeCallback(NodeApiEnvironment &env) noexcept override {
    if (finalizeCallback_) {
      auto finalizeCallback = std::exchange(finalizeCallback_, nullptr);
      return env.callFinalizer(finalizeCallback, nativeData(), finalizeHint());
    }
    return napi_ok;
  }

 private:
  napi_finalize finalizeCallback_{};
};

// Store nativeData if it is not null.
template <typename TBaseReference>
struct NativeDataHolder : TBaseReference {
  template <typename... TArgs>
  NativeDataHolder(void *nativeData, TArgs &&...args) noexcept
      : TBaseReference(std::forward<TArgs>(args)...), nativeData_(nativeData) {}

  void *nativeData() noexcept override {
    return nativeData_;
  }

 private:
  void *nativeData_;
};

// Common code for references inherited from Finalizer.
template <typename TBaseReference>
struct FinalizingReference final : TBaseReference {
  template <typename... TArgs>
  FinalizingReference(TArgs &&...args) noexcept
      : TBaseReference(std::forward<TArgs>(args)...) {}

  void finalize(NodeApiEnvironment &env) noexcept override {
    callFinalizeCallback(env);
    Reference::deleteReference(
        env, this, Reference::ReasonToDelete::FinalizerCall);
  }
};

// Create FinalizingReference with the optimized storage.
template <typename TReference>
struct FinalizingReferenceFactory {
  template <typename... TArgs>
  static TReference *create(
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      TArgs &&...args) noexcept {
    int selector = (nativeData ? 0b100 : 0) | (finalizeCallback ? 0b010 : 0) |
        (finalizeHint ? 0b001 : 0);
    switch (selector) {
      default:
      case 0b000:
      case 0b001:
        return new FinalizingReference<TReference>(
            std::forward<TArgs>(args)...);
      case 0b010:
        return new FinalizingReference<FinalizeCallbackHolder<TReference>>(
            finalizeCallback, std::forward<TArgs>(args)...);
      case 0b011:
        return new FinalizingReference<
            FinalizeCallbackHolder<FinalizeHintHolder<TReference>>>(
            finalizeCallback, finalizeHint, std::forward<TArgs>(args)...);
      case 0b100:
      case 0b101:
        return new FinalizingReference<NativeDataHolder<TReference>>(
            nativeData, std::forward<TArgs>(args)...);
      case 0b110:
        return new FinalizingReference<
            NativeDataHolder<FinalizeCallbackHolder<TReference>>>(
            nativeData, finalizeCallback, std::forward<TArgs>(args)...);
      case 0b111:
        return new FinalizingReference<NativeDataHolder<
            FinalizeCallbackHolder<FinalizeHintHolder<TReference>>>>(
            nativeData,
            finalizeCallback,
            finalizeHint,
            std::forward<TArgs>(args)...);
    }
  }
};

// The reference that is never returned to the user code and only used to hold
// the native data and its finalizer callback.
// It is either deleted from the finalizer queue, on environment shutdown, or
// directly when deleting the object wrap.
struct FinalizingAnonymousReference : Reference, Finalizer {
  static napi_status create(
      NodeApiEnvironment &env,
      const vm::PinnedHermesValue *value,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      /*optional*/ FinalizingAnonymousReference **result) noexcept {
    CHECK_OBJECT_ARG(value);
    auto ref = FinalizingReferenceFactory<FinalizingAnonymousReference>::create(
        nativeData, finalizeCallback, finalizeHint);
    env.addObjectFinalizer(value, ref);
    env.addFinalizingGCRoot(ref);
    if (result) {
      *result = ref;
    }
    return env.clearLastError();
  }
};

// Associates data with StrongReference.
struct FinalizingStrongReference : StrongReference, Finalizer {
  static napi_status create(
      NodeApiEnvironment &env,
      const vm::PinnedHermesValue *value,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      FinalizingStrongReference **result) noexcept {
    CHECK_ARG(value);
    CHECK_ARG(*result);
    *result = FinalizingReferenceFactory<FinalizingStrongReference>::create(
        nativeData, finalizeCallback, finalizeHint, *value);
    env.addFinalizingGCRoot(*result);
    return env.clearLastError();
  }

 protected:
  FinalizingStrongReference(const vm::PinnedHermesValue &value) noexcept
      : StrongReference(value) {}

  bool startDeleting(NodeApiEnvironment &env, ReasonToDelete reason) noexcept
      override {
    if (reason == ReasonToDelete::ZeroRefCount) {
      // Let the finalizer to run first.
      env.addToFinalizerQueue(this);
      return false;
    } else if (reason == ReasonToDelete::FinalizerCall) {
      if (refCount() != 0) {
        // On shutdown the finalizer is called when the ref count is not zero
        // yet. Postpone the deletion until all finalizers are finished to run.
        Finalizer::unlink();
        env.addGCRoot(this);
        return false;
      }
    }
    return true;
  }
};

// A reference that can be either strong or weak and that holds a finalizer
// callback.
struct FinalizingComplexReference : ComplexReference, Finalizer {
  static napi_status create(
      NodeApiEnvironment &env,
      uint32_t initialRefCount,
      const vm::PinnedHermesValue *value,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      FinalizingComplexReference **result) noexcept {
    CHECK_OBJECT_ARG(value);
    CHECK_ARG(result);
    *result = FinalizingReferenceFactory<FinalizingComplexReference>::create(
        nativeData,
        finalizeCallback,
        finalizeHint,
        initialRefCount,
        *value,
        initialRefCount == 0
            ? env.createWeakRoot(NodeApiEnvironment::getObject(*value))
            : vm::WeakRoot<vm::JSObject>{});
    if (initialRefCount == 0) {
      env.addObjectFinalizer(value, *result);
    }
    env.addFinalizingGCRoot(*result);
    return env.clearLastError();
  }

  napi_status incRefCount(NodeApiEnvironment &env, uint32_t &result) noexcept
      override {
    CHECK_NAPI(ComplexReference::incRefCount(env, result));
    if (result == 1) {
      LinkedList<Finalizer>::Item::unlink();
    }
    return env.clearLastError();
  }

  napi_status decRefCount(NodeApiEnvironment &env, uint32_t &result) noexcept
      override {
    vm::PinnedHermesValue hv;
    bool shouldConvertToWeakRef = refCount() == 1;
    if (shouldConvertToWeakRef) {
      hv = value(env);
    }
    CHECK_NAPI(ComplexReference::decRefCount(env, result));
    if (shouldConvertToWeakRef && hv.isObject()) {
      return env.addObjectFinalizer(&hv, this);
    }
    return env.clearLastError();
  }

 protected:
  using ComplexReference::ComplexReference;

  bool startDeleting(NodeApiEnvironment &env, ReasonToDelete reason) noexcept
      override {
    if (reason == ReasonToDelete::ExternalCall &&
        LinkedList<Finalizer>::Item::isLinked()) {
      // Let the finalizer to the environment shutdown to delete the reference.
      deleteSelf_ = true;
      return false;
    }
    if (reason == ReasonToDelete::FinalizerCall && !deleteSelf_) {
      // Let the external call or the environment shutdown to delete the
      // reference.
      Finalizer::unlink();
      env.addGCRoot(this);
      return false;
    }
    return true;
  }

 private:
  bool deleteSelf_{false};
};

struct InstanceData : Reference {
  static napi_status create(
      NodeApiEnvironment &env,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      /*optional*/ InstanceData **result) noexcept {
    auto ref = FinalizingReferenceFactory<InstanceData>::create(
        nativeData, finalizeCallback, finalizeHint);
    if (result) {
      *result = ref;
    }
    return env.clearLastError();
  }
};

ExternalValue::~ExternalValue() {
  finalizers_.forEach(
      [&](Finalizer *finalizer) { env_.addToFinalizerQueue(finalizer); });
}

void ExternalValue::addFinalizer(Finalizer *finalizer) noexcept {
  finalizers_.pushBack(finalizer);
}

/*static*/ vm::CallResult<vm::HermesValue>
HFContext::func(void *context, vm::Runtime *runtime, vm::NativeArgs hvArgs) {
  HFContext *hfc = reinterpret_cast<HFContext *>(context);
  NodeApiEnvironment &env = hfc->env_;
  assert(runtime == &env.runtime());
  auto &stats = env.runtime().getRuntimeStats();
  const vm::instrumentation::RAIITimer timer{
      "Host Function", stats, stats.hostFunction};

  CallbackInfo callbackInfo{*hfc, hvArgs};
  auto result = hfc->hostCallback_(
      reinterpret_cast<napi_env>(&env),
      reinterpret_cast<napi_callback_info>(&callbackInfo));
  return *phv(result);
  // TODO: handle errors
  // TODO: Add call in module
}

struct OrderedSet {
  using Compare =
      int32_t(const vm::HermesValue &item1, const vm::HermesValue &item2);

  OrderedSet(NodeApiEnvironment &env, Compare *compare) noexcept
      : env_(env), compare_(compare) {
    env_.pushOrderedSet(*this);
  }

  ~OrderedSet() {
    env_.popOrderedSet();
  }

  bool insert(vm::HermesValue value) noexcept {
    auto it = llvh::lower_bound(
        items_,
        value,
        [this](const vm::HermesValue &item1, const vm::HermesValue &item2) {
          return (*compare_)(item1, item2) < 0;
        });
    if (it == items_.end() || (*compare_)(*it, value) == 0) {
      return false;
    }
    items_.insert(it, value);
    return true;
  }

  static void getGCRoots(
      llvh::iterator_range<OrderedSet **> range,
      vm::RootAcceptor &acceptor) noexcept {
    for (auto set : range) {
      for (vm::PinnedHermesValue &value : set->items_) {
        acceptor.accept(value);
      }
    }
  }

 private:
  NodeApiEnvironment &env_;
  llvh::SmallVector<vm::PinnedHermesValue, 16> items_;
  Compare *compare_{};
};

struct UInt32OrderedSet {
  bool insert(uint32_t value) noexcept {
    auto it = llvh::lower_bound(items_, value);
    if (it == items_.end() || *it == value) {
      return false;
    }
    items_.insert(it, value);
    return true;
  }

 private:
  llvh::SmallVector<uint32_t, 16> items_;
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
    stackValues_.forEach([&](const vm::PinnedHermesValue &value) {
      acceptor.accept(const_cast<vm::PinnedHermesValue &>(value));
    });
    Reference::getGCRoots(*this, gcRoots_, acceptor);
    Reference::getGCRoots(*this, finalizingGCRoots_, acceptor);
    if (!lastException_.isEmpty()) {
      acceptor.accept(lastException_);
    }
    for (auto &value : predefinedValues_) {
      acceptor.accept(value);
    }
    OrderedSet::getGCRoots(orderedSets_, acceptor);
  });
  runtime_.addCustomWeakRootsFunction(
      [this](vm::GC *, vm::WeakRootAcceptor &acceptor) {
        Reference::getGCWeakRoots(*this, gcRoots_, acceptor);
        Reference::getGCWeakRoots(*this, finalizingGCRoots_, acceptor);
      });
  // TODO: implement
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
      NapiPredefined::ExternalValueSymbol,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().createNotUniquedLazySymbol(
              "napi.externalValue.735e14c9-354f-489b-9f27-02acbc090975")));
  setPredefined(
      NapiPredefined::CodeSymbol,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().createNotUniquedLazySymbol("code")));
  setPredefined(
      NapiPredefined::TypeTagSymbol,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().createNotUniquedLazySymbol(
              "napi.typeTag.026ae0ec-b391-49da-a935-0cab733ab615")));
}

NodeApiEnvironment::~NodeApiEnvironment() {
  if (instanceData_) {
    instanceData_->finalize(*this);
    instanceData_ = nullptr;
  }

  // First we must finalize those references that have `napi_finalizer`
  // callbacks. The reason is that addons might store other references which
  // they delete during their `napi_finalizer` callbacks. If we deleted such
  // references here first, they would be doubly deleted when the
  // `napi_finalizer` deleted them subsequently.
  Reference::finalizeAll(*this, finalizerQueue_);
  Reference::finalizeAll(*this, finalizingGCRoots_);
  Reference::deleteAll(
      *this, gcRoots_, Reference::ReasonToDelete::EnvironmentShutdown);
  CRASH_IF_FALSE(finalizerQueue_.isEmpty());
  CRASH_IF_FALSE(finalizingGCRoots_.isEmpty());
  CRASH_IF_FALSE(gcRoots_.isEmpty());
}

napi_status NodeApiEnvironment::getUndefined(napi_value *result) noexcept {
  return setResult(napiValue(&runtimeAccessor().undefinedValue_), result);
}

napi_status NodeApiEnvironment::getNull(napi_value *result) noexcept {
  return setResult(napiValue(&runtimeAccessor().nullValue_), result);
}

napi_status NodeApiEnvironment::getGlobal(napi_value *result) noexcept {
  return setResult(napiValue(&runtimeAccessor().global_), result);
}

napi_status NodeApiEnvironment::getBoolean(
    bool value,
    napi_value *result) noexcept {
  return setResult(
      value ? napiValue(&runtimeAccessor().trueValue_)
            : napiValue(&runtimeAccessor().falseValue_),
      result);
}

napi_status NodeApiEnvironment::createObject(napi_value *result) noexcept {
  return handleExceptions(
      [&] { return setResult(vm::JSObject::create(&runtime_), result); });
}

napi_status NodeApiEnvironment::createArray(napi_value *result) noexcept {
  return handleExceptions([&] {
    return setResult(
        vm::JSArray::create(&runtime_, /*capacity:*/ 0, /*length:*/ 0), result);
  });
}

napi_status NodeApiEnvironment::createArray(
    size_t length,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    return setResult(
        vm::JSArray::create(
            &runtime_, /*capacity:*/ length, /*length:*/ length),
        result);
  });
}

template <class T, std::enable_if_t<std::is_arithmetic_v<T>, bool>>
napi_status NodeApiEnvironment::createNumber(
    T value,
    napi_value *result) noexcept {
  return setResult(
      vm::HermesValue::encodeNumberValue(static_cast<double>(value)), result);
}

napi_status NodeApiEnvironment::createStringLatin1(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(str);
    if (length == NAPI_AUTO_LENGTH) {
      length = std::char_traits<char>::length(str);
    }
    RETURN_STATUS_IF_FALSE(
        length <= std::numeric_limits<int32_t>::max(), napi_invalid_arg);
    ASSIGN_ELSE_RETURN_FAILURE(
        vm::HermesValue res /*=*/, stringHVFromLatin1(str, length));
    return setResult(std::move(res), result);
  });
}

napi_status NodeApiEnvironment::createStringUtf8(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(str);
    if (length == NAPI_AUTO_LENGTH) {
      length = std::char_traits<char>::length(str);
    }
    RETURN_STATUS_IF_FALSE(
        length <= std::numeric_limits<int32_t>::max(), napi_invalid_arg);
    ASSIGN_ELSE_RETURN_FAILURE(
        vm::HermesValue res /*=*/,
        stringHVFromUtf8(reinterpret_cast<const uint8_t *>(str), length));
    return setResult(res, result);
  });
}

napi_status NodeApiEnvironment::createStringUtf16(
    const char16_t *str,
    size_t length,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(str);
    if (length == NAPI_AUTO_LENGTH) {
      length = std::char_traits<char16_t>::length(str);
    }
    RETURN_STATUS_IF_FALSE(
        length <= std::numeric_limits<int32_t>::max(), napi_invalid_arg);
    ASSIGN_ELSE_RETURN_FAILURE(
        vm::HermesValue res /*=*/,
        vm::StringPrimitive::createEfficient(
            &runtime_, llvh::makeArrayRef(str, length)));
    return setResult(res, result);
  });
}

napi_status NodeApiEnvironment::incRefCount() noexcept {
  refCount_++;
  return napi_status::napi_ok;
}

napi_status NodeApiEnvironment::decRefCount() noexcept {
  if (--refCount_ == 0) {
    delete this;
  }
  return napi_status::napi_ok;
}

template <typename F>
napi_status NodeApiEnvironment::handleExceptions(const F &f) noexcept {
  napi_status status{};
  RETURN_STATUS_IF_FALSE(lastException_.isEmpty(), napi_pending_exception);
  clearLastError();
  {
    vm::GCScope gcScope(&runtime_);
#ifdef HERMESVM_EXCEPTION_ON_OOM
    try {
      status = f();
    } catch (const vm::JSOutOfMemoryError &ex) {
      return SetLastError(napi_generic_failure);
    }
#else // HERMESVM_EXCEPTION_ON_OOM
    status = f();
#endif
  }
  if (status == napi_ok) {
    CHECK_NAPI(runReferenceFinalizers());
  }
  return status;
}

napi_status NodeApiEnvironment::createStrongReference(
    napi_value value,
    napi_ext_ref *result) noexcept {
  return StrongReference::create(
      *this, phv(value), reinterpret_cast<StrongReference **>(result));
}

napi_status NodeApiEnvironment::createStrongReferenceWithData(
    napi_value value,
    void *nativeData,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_ext_ref *result) noexcept {
  return FinalizingStrongReference::create(
      *this,
      phv(value),
      nativeData,
      finalizeCallback,
      finalizeHint,
      reinterpret_cast<FinalizingStrongReference **>(result));
}

napi_status NodeApiEnvironment::createWeakReference(
    napi_value value,
    napi_ext_ref *result) noexcept {
  return WeakReference::create(
      *this, phv(value), reinterpret_cast<WeakReference **>(result));
}

napi_status NodeApiEnvironment::incReference(napi_ext_ref ref) noexcept {
  CHECK_ARG(ref);
  uint32_t refCount{};
  return asReference(ref)->incRefCount(*this, /*ref*/ refCount);
}

napi_status NodeApiEnvironment::decReference(napi_ext_ref ref) noexcept {
  CHECK_ARG(ref);
  uint32_t refCount{};
  return asReference(ref)->decRefCount(*this, /*ref*/ refCount);
}

napi_status NodeApiEnvironment::getReferenceValue(
    napi_ext_ref ref,
    napi_value *result) noexcept {
  CHECK_ARG(ref);
  CHECK_ARG(result);
  *result = addStackValue(asReference(ref)->value(*this));
  return clearLastError();
}

vm::Runtime &NodeApiEnvironment::runtime() noexcept {
  return runtime_;
}

struct StringBuilder {
  struct AdoptStringTag {};
  constexpr static AdoptStringTag AdoptString{};

  StringBuilder(AdoptStringTag, std::string &&str) noexcept
      : str_(std::move(str)), stream_(str_) {}

  template <typename... TArgs>
  StringBuilder(TArgs &&...args) noexcept : stream_(str_) {
    append(std::forward<TArgs>(args)...);
  }

  StringBuilder &append() noexcept {
    return *this;
  }

  template <typename TArg0, typename... TArgs>
  StringBuilder &append(TArg0 &&arg0, TArgs &&...args) noexcept {
    stream_ << arg0;
    return append(std::forward<TArgs>(args)...);
  }

  std::string &str() noexcept {
    stream_.flush();
    return str_;
  }

  const char *c_str() noexcept {
    return str().c_str();
  }

  vm::CallResult<vm::Handle<>> makeStringHV(vm::Runtime &runtime) noexcept {
    stream_.flush();
    auto res = vm::StringPrimitive::createEfficient(
        &runtime, llvh::makeArrayRef(str_.data(), str_.size()));
    if (LLVM_UNLIKELY(res.getStatus() == vm::ExecutionStatus::EXCEPTION)) {
      return vm::ExecutionStatus::EXCEPTION;
    }
    return runtime.makeHandle(*res);
  }

 private:
  std::string str_;
  llvh::raw_string_ostream stream_;
};

template <typename... TArgs>
napi_status NodeApiEnvironment::setLastError(
    napi_status errorCode,
    TArgs &&...args) noexcept {
  // Warning: Keep in-sync with napi_status enum
  static constexpr const char *errorMessages[] = {
      "",
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

  // The value of the constant below must be updated to reference the last
  // message in the `napi_status` enum each time a new error message is added.
  // We don't have a napi_status_last as this would result in an ABI
  // change each time a message was added.
  const int lastStatus = napi_would_deadlock;
  static_assert(
      size(errorMessages) == lastStatus + 1,
      "Count of error messages must match count of error values");

  if (errorCode < napi_ok || errorCode >= lastStatus) {
    errorCode = napi_generic_failure;
  }

  lastErrorMessage_.clear();
  StringBuilder sb{StringBuilder::AdoptString, std::move(lastErrorMessage_)};
  sb.append(errorMessages[errorCode]);
  if (sizeof...(args) > 0) {
    sb.append(": ", std::forward<TArgs>(args)...);
  }
  lastErrorMessage_ = std::move(sb.str());
  lastError_ = {lastErrorMessage_.c_str(), 0, 0, errorCode};
  return errorCode;
}

napi_status NodeApiEnvironment::clearLastError() noexcept {
  lastErrorMessage_.clear();
  lastError_ = {"", 0, 0, napi_ok};
  return napi_ok;
}

napi_status NodeApiEnvironment::getLastErrorInfo(
    const napi_extended_error_info **result) noexcept {
  CHECK_ARG(result);
  *result = &lastError_;
  return napi_ok;
}

const vm::PinnedHermesValue &NodeApiEnvironment::getPredefined(
    NapiPredefined predefinedKey) noexcept {
  return predefinedValues_[static_cast<size_t>(predefinedKey)];
}

napi_status NodeApiEnvironment::addFinalizer(
    napi_value object,
    void *nativeData,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_ref *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(finalizeCallback);
    if (result != nullptr) {
      return FinalizingComplexReference::create(
          *this,
          0,
          phv(object),
          nativeData,
          finalizeCallback,
          finalizeHint,
          reinterpret_cast<FinalizingComplexReference **>(result));
    } else {
      return FinalizingAnonymousReference::create(
          *this,
          phv(object),
          nativeData,
          finalizeCallback,
          finalizeHint,
          nullptr);
    }
  });
}

napi_status NodeApiEnvironment::wrapObject(
    napi_value object,
    void *nativeData,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_ref *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);

    if (result != nullptr) {
      // The returned reference should be deleted via napi_delete_reference()
      // ONLY in response to the finalize callback invocation. (If it is deleted
      // before that, then the finalize callback will never be invoked.)
      // Therefore a finalize callback is required when returning a reference.
      CHECK_ARG(finalizeCallback);
    }

    // If we've already wrapped this object, we error out.
    ExternalValue *externalValue{};
    CHECK_NAPI(getExternalValue(
        toObjectHandle(object), IfNotFound::ThenCreate, &externalValue));
    RETURN_STATUS_IF_FALSE(!externalValue->nativeData(), napi_invalid_arg);

    Reference *reference{};
    if (result != nullptr) {
      CHECK_NAPI(FinalizingComplexReference::create(
          *this,
          0,
          phv(object),
          nativeData,
          finalizeCallback,
          finalizeHint,
          reinterpret_cast<FinalizingComplexReference **>(reference)));
      *result = reinterpret_cast<napi_ref>(reference);
    } else {
      // TODO: do not use the anonymous ref here
      CHECK_NAPI(FinalizingAnonymousReference::create(
          *this,
          phv(object),
          nativeData,
          finalizeCallback,
          finalizeHint,
          reinterpret_cast<FinalizingAnonymousReference **>(reference)));
    }

    externalValue->setNativeData(reference);
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::unwrapObject(
    napi_value object,
    UnwrapAction action,
    void **result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    if (action == UnwrapAction::KeepWrap) {
      CHECK_ARG(result);
    }

    auto externalValue = getExternalValue(*phv(object));
    if (!externalValue) {
      CHECK_NAPI(getExternalValue(
          toObjectHandle(object), IfNotFound::ThenReturnNull, &externalValue));
      RETURN_STATUS_IF_FALSE(externalValue, napi_invalid_arg);
    }

    auto reference = asReference(externalValue->nativeData());
    if (result) {
      *result = reference->nativeData();
    }

    if (action == UnwrapAction::RemoveWrap) {
      externalValue->setNativeData(nullptr);
      Reference::deleteReference(
          *this, reference, Reference::ReasonToDelete::ExternalCall);
    }

    return clearLastError();
  });
}

vm::CallResult<bool> NodeApiEnvironment::hasPrivate(
    vm::Handle<vm::JSObject> objHandle,
    NapiPredefined key) noexcept {
  vm::SymbolID name = getPredefined(key).getSymbol();
  return vm::JSObject::hasNamed(objHandle, &runtime_, name);
}

vm::CallResult<vm::PseudoHandle<>> NodeApiEnvironment::getPrivate(
    vm::Handle<vm::JSObject> objHandle,
    NapiPredefined key) noexcept {
  vm::SymbolID name = getPredefined(key).getSymbol();
  return vm::JSObject::getNamed_RJS(
      objHandle, &runtime_, name, vm::PropOpFlags().plusThrowOnError());
}

vm::CallResult<bool> NodeApiEnvironment::setPrivate(
    vm::Handle<vm::JSObject> objHandle,
    NapiPredefined key,
    const vm::HermesValue &value) noexcept {
  vm::SymbolID name = getPredefined(key).getSymbol();
  return vm::JSObject::putNamed_RJS(
      objHandle,
      &runtime_,
      name,
      toHandle(value),
      vm::PropOpFlags().plusThrowOnError());
}

vm::CallResult<bool> NodeApiEnvironment::deletePrivate(
    vm::Handle<vm::JSObject> objHandle,
    NapiPredefined key) noexcept {
  vm::SymbolID name = getPredefined(key).getSymbol();
  return vm::JSObject::deleteNamed(
      objHandle, &runtime_, name, vm::PropOpFlags().plusThrowOnError());
}

napi_status NodeApiEnvironment::genericFailure(
    const char * /*message*/) noexcept {
  // TODO: set result message
  return napi_generic_failure;
}

vm::WeakRoot<vm::JSObject> NodeApiEnvironment::createWeakRoot(
    vm::JSObject *object) noexcept {
  return vm::WeakRoot<vm::JSObject>(object, &runtime_);
}

const vm::PinnedHermesValue &NodeApiEnvironment::lockWeakObject(
    vm::WeakRoot<vm::JSObject> &weakRoot) noexcept {
  if (const auto ptr = weakRoot.get(&runtime_, &runtime_.getHeap())) {
    return *phv(addStackValue(vm::HermesValue::encodeObjectValue(ptr)));
  }
  return getPredefined(NapiPredefined::UndefinedValue);
}

napi_status NodeApiEnvironment::addObjectFinalizer(
    const vm::PinnedHermesValue *value,
    Finalizer *finalizer) noexcept {
  return handleExceptions([&] {
    auto externalValue = getExternalValue(*value);
    if (!externalValue) {
      CHECK_NAPI(getExternalValue(
          toObjectHandle(value), IfNotFound::ThenCreate, &externalValue));
    }

    externalValue->addFinalizer(finalizer);

    return clearLastError();
  });
}

vm::PseudoHandle<vm::DecoratedObject> NodeApiEnvironment::createExternal(
    void *nativeData,
    ExternalValue **externalValue) noexcept {
  auto decoratedObj = vm::DecoratedObject::create(
      &runtime_,
      vm::Handle<vm::JSObject>::vmcast(&runtime_.objectPrototype),
      std::make_unique<ExternalValue>(*this, nativeData),
      /*additionalSlotCount:*/ 1);

  // Add a special tag to differentiate from other decorated objects.
  vm::DecoratedObject::setAdditionalSlotValue(
      decoratedObj.get(),
      &runtime_,
      kExternalTagSlot,
      vm::SmallHermesValue::encodeNumberValue(kExternalValueTag, &runtime_));

  if (externalValue) {
    *externalValue =
        static_cast<ExternalValue *>(decoratedObj->getDecoration());
  }

  return decoratedObj;
}

ExternalValue *NodeApiEnvironment::getExternalValue(
    const vm::HermesValue &value) noexcept {
  if (auto decoratedObj = vm::dyn_vmcast_or_null<vm::DecoratedObject>(value)) {
    auto tag = vm::DecoratedObject::getAdditionalSlotValue(
        decoratedObj, &runtime_, kExternalTagSlot);
    if (tag.isNumber() && tag.getNumber(&runtime_) == kExternalValueTag) {
      return static_cast<ExternalValue *>(decoratedObj->getDecoration());
    }
  }

  return nullptr;
}

napi_status NodeApiEnvironment::getExternalValue(
    vm::Handle<vm::JSObject> objHandle,
    IfNotFound ifNotFound,
    ExternalValue **result) noexcept {
  return handleExceptions([&] {
    ExternalValue *externalValue{};
    auto decoratorRes =
        getPrivate(objHandle, NapiPredefined::ExternalValueSymbol);
    if (decoratorRes.getStatus() == vm::ExecutionStatus::RETURNED) {
      externalValue = getExternalValue(decoratorRes->getHermesValue());
      RETURN_STATUS_IF_FALSE(externalValue, napi_generic_failure);
    } else if (ifNotFound == IfNotFound::ThenCreate) {
      auto decoratedObj = createExternal(nullptr, &externalValue);
      CHECK_STATUS(
          setPrivate(
              objHandle,
              NapiPredefined::ExternalValueSymbol,
              runtime_.makeHandle(std::move(decoratedObj)).getHermesValue())
              .getStatus());
    }

    *result = externalValue;

    return clearLastError();
  });
}

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

napi_status NodeApiEnvironment::stringHVFromUtf8(
    const char *utf8,
    vm::Handle<> *result) noexcept {
  vm::CallResult<vm::HermesValue> res = stringHVFromUtf8(utf8);
  return setResult(makeHandle(*res), result);
}

napi_status NodeApiEnvironment::stringHVFromUtf8(
    const uint8_t *utf8,
    size_t length,
    vm::Handle<> *result) noexcept {
  vm::CallResult<vm::HermesValue> res = stringHVFromUtf8(utf8, length);
  return setResult(makeHandle(*res), result);
}

napi_value NodeApiEnvironment::addStackValue(vm::HermesValue value) noexcept {
  stackValues_.emplaceBack(value);
  return reinterpret_cast<napi_value>(&stackValues_.back());
}

napi_value NodeApiEnvironment::toNapiValue(
    const vm::PinnedHermesValue &value) noexcept {
  return reinterpret_cast<napi_value>(
      const_cast<vm::PinnedHermesValue *>(&value));
}

napi_status NodeApiEnvironment::checkStatus(
    vm::ExecutionStatus hermesStatus,
    napi_status status) noexcept {
  if (LLVM_LIKELY(hermesStatus != vm::ExecutionStatus::EXCEPTION)) {
    return napi_ok;
  }

  lastException_ = runtime_.getThrownValue();
  runtime_.clearThrownValue();
  return status;
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
  return clearLastError();
}

napi_status NodeApiEnvironment::createFunction(
    const char *utf8Name,
    size_t length,
    napi_callback callback,
    void *callbackData,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    napi_value nameValue{};
    CHECK_NAPI(createStringUtf8(utf8Name, length, &nameValue));
    auto nameRes = vm::stringToSymbolID(
        &runtime_, vm::createPseudoHandle(phv(nameValue)->getString()));
    CHECK_STATUS(nameRes.getStatus());
    CHECK_NAPI(newFunction(nameRes->get(), callback, callbackData, result));
    return clearLastError();
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
  return handleExceptions([&] {
    CHECK_ARG(result);
    CHECK_ARG(constructor);
    if (propertyCount > 0) {
      CHECK_ARG(properties);
    }

    vm::MutableHandle<vm::SymbolID> name{&runtime_};
    CHECK_NAPI(createSymbolID(utf8Name, length, &name));
    CHECK_NAPI(newFunction(name.get(), constructor, callbackData, result));

    auto classHandle = runtime_.makeHandle<vm::JSObject>(*phv(*result));
    auto prototypeHandle = runtime_.makeHandle(vm::JSObject::create(&runtime_));
    napi_value prototype = addStackValue(prototypeHandle.getHermesValue());
    vm::PropertyFlags pf;
    pf.clear();
    pf.enumerable = 0;
    pf.writable = 1;
    pf.configurable = 0;
    CHECK_STATUS(vm::JSObject::defineNewOwnProperty(
        classHandle,
        &runtime_,
        vm::Predefined::getSymbolID(vm::Predefined::prototype),
        pf,
        prototypeHandle));
    pf.configurable = 1;
    CHECK_STATUS(vm::JSObject::defineNewOwnProperty(
        prototypeHandle,
        &runtime_,
        vm::Predefined::getSymbolID(vm::Predefined::constructor),
        pf,
        classHandle));

    for (size_t i = 0; i < propertyCount; ++i) {
      const napi_property_descriptor *p = properties + i;
      if ((p->attributes & napi_static) != 0) {
        CHECK_NAPI(defineProperties(*result, 1, p));
      } else {
        CHECK_NAPI(defineProperties(prototype, 1, p));
      }
    }

    return clearLastError();
  });
}

vm::CallResult<vm::Handle<vm::JSObject>> NodeApiEnvironment::convertToObject(
    vm::Handle<> value) noexcept {
  ASSIGN_ELSE_RETURN_HERMES_EXCEPTION(
      vm::HermesValue obj /*=*/, vm::toObject(&runtime_, value));
  return vm::Handle<vm::JSObject>::vmcast(&runtime_, obj);
}

vm::Handle<> NodeApiEnvironment::makeHandle(
    const vm::PinnedHermesValue *value) noexcept {
  return vm::Handle<>(value);
}

vm::Handle<> NodeApiEnvironment::makeHandle(napi_value value) noexcept {
  return makeHandle(phv(value));
}

vm::Handle<> NodeApiEnvironment::makeHandle(vm::HermesValue value) noexcept {
  return vm::Handle<>(&runtime_, value);
}

vm::Handle<> NodeApiEnvironment::makeHandle(vm::Handle<> value) noexcept {
  return value;
}

vm::Handle<> NodeApiEnvironment::makeHandle(uint32_t value) noexcept {
  return makeHandle(vm::HermesValue::encodeDoubleValue(value));
}

template <class T>
vm::CallResult<vm::Handle<T>> NodeApiEnvironment::makeHandle(
    vm::CallResult<vm::PseudoHandle<T>> &&callResult) noexcept {
  if (callResult.getStatus() == vm::ExecutionStatus::EXCEPTION) {
    return vm::ExecutionStatus::EXCEPTION;
  }
  return runtime_.makeHandle(std::move(*callResult));
}

template <class T>
vm::CallResult<vm::MutableHandle<T>> NodeApiEnvironment::makeMutableHandle(
    vm::CallResult<vm::PseudoHandle<T>> &&callResult) noexcept {
  vm::CallResult<vm::Handle<T>> handleResult =
      makeHandle(std::move(callResult));
  if (handleResult.getStatus() == vm::ExecutionStatus::EXCEPTION) {
    return vm::ExecutionStatus::EXCEPTION;
  }
  vm::MutableHandle<T> result{&runtime_};
  result = std::move(*callResult);
  return result;
}

template <class T>
vm::MutableHandle<T> NodeApiEnvironment::makeMutableHandle() noexcept {
  return vm::MutableHandle<T>(&runtime_);
}

napi_status NodeApiEnvironment::getPropertyNames(
    napi_value object,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return getForInPropertyNames(
        objHandle, napi_key_numbers_to_strings, result);
  });
}

napi_status NodeApiEnvironment::getAllPropertyNames(
    napi_value object,
    napi_key_collection_mode keyMode,
    napi_key_filter keyFilter,
    napi_key_conversion keyConversion,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    ASSIGN_ELSE_RETURN_STATUS(
        vm::Handle<vm::JSObject> objHandle /*=*/,
        convertToObject(makeHandle(object)),
        /*else return*/ napi_object_expected);
    RETURN_STATUS_IF_FALSE(
        isInEnumRange(keyMode, napi_key_include_prototypes, napi_key_own_only),
        napi_invalid_arg);
    RETURN_STATUS_IF_FALSE(
        isInEnumRange(
            keyConversion, napi_key_keep_numbers, napi_key_numbers_to_strings),
        napi_invalid_arg);

    // We can use optimized code if object has no parent.
    bool hasParent;
    {
      ASSIGN_ELSE_RETURN_FAILURE(
          vm::PseudoHandle<vm::JSObject> parentObj /*=*/,
          vm::JSObject::getPrototypeOf(objHandle, &runtime_));
      hasParent = parentObj.get();
    }

    // The fast path used for the 'for..in' implementation.
    if (keyFilter == (napi_key_enumerable | napi_key_skip_symbols) &&
        (keyMode == napi_key_include_prototypes || !hasParent)) {
      return getForInPropertyNames(objHandle, keyConversion, result);
    }

    // Flags to request own keys
    vm::OwnKeysFlags ownKeyFlags{};
    ownKeyFlags.setIncludeNonSymbols((keyFilter & napi_key_skip_strings) == 0);
    ownKeyFlags.setIncludeSymbols((keyFilter & napi_key_skip_symbols) == 0);
    ownKeyFlags.plusIncludeNonEnumerable(); // for proper shadow checks

    // Use the simple path for own properties without extra filters.
    if ((keyMode == napi_key_own_only || !hasParent) &&
        (keyFilter & (napi_key_writable | napi_key_configurable)) == 0) {
      ownKeyFlags.setIncludeNonEnumerable(
          (keyFilter & napi_key_enumerable) == 0);
      ASSIGN_ELSE_RETURN_FAILURE(
          vm::Handle<vm::JSArray> ownKeys /*=*/,
          vm::JSObject::getOwnPropertyKeys(objHandle, &runtime_, ownKeyFlags));
      if (keyConversion == napi_key_numbers_to_strings) {
        CHECK_STATUS(convertKeyNumbersToStrings(ownKeys));
      }
      return setResult(ownKeys.getHermesValue(), result);
    }

    // Collect all properties into the keyStorage.
    ASSIGN_ELSE_RETURN_FAILURE(
        vm::MutableHandle<vm::BigStorage> keyStorage /*=*/,
        makeMutableHandle(vm::BigStorage::create(&runtime_, 16)));
    uint32_t size{0};

    // Make sure that we do not include into the result properties that were
    // shadowed by the derived objects.
    bool useShadowTracking =
        keyMode == napi_key_include_prototypes && hasParent;
    // TODO: make the OrderedSet be a template
    UInt32OrderedSet shadowIndexes;
    OrderedSet shadowStrings(
        *this, [](const vm::HermesValue &item1, const vm::HermesValue &item2) {
          return item1.getString()->compare(item2.getString());
        });
    OrderedSet shadowSymbols(
        *this, [](const vm::HermesValue &item1, const vm::HermesValue &item2) {
          vm::SymbolID::RawType rawItem1 = item1.getSymbol().unsafeGetRaw();
          vm::SymbolID::RawType rawItem2 = item2.getSymbol().unsafeGetRaw();
          return rawItem1 < rawItem2 ? -1 : rawItem1 > rawItem2 ? 1 : 0;
        });

    // Keep the mutable variables outside of loop for efficiency
    vm::MutableHandle<vm::JSObject> currentObj(&runtime_, objHandle.get());
    vm::MutableHandle<> prop(&runtime_);
    OptValue<uint32_t> propIndexOpt{};
    vm::MutableHandle<vm::StringPrimitive> propString(&runtime_);

    while (currentObj.get()) {
      vm::GCScope gcScope(&runtime_);

      ASSIGN_ELSE_RETURN_FAILURE(
          vm::Handle<vm::JSArray> props /*=*/,
          vm::JSObject::getOwnPropertyKeys(currentObj, &runtime_, ownKeyFlags));

      auto marker = gcScope.createMarker();
      for (uint32_t i = 0, end = props->getEndIndex(); i < end; ++i) {
        gcScope.flushToMarker(marker);
        prop = props->at(&runtime_, i);

        // Do not add a property if it is overriden in the derived object.
        if (useShadowTracking) {
          if (prop->isString()) {
            propString = vm::Handle<vm::StringPrimitive>::vmcast(prop);
            // See if the property name is an index
            propIndexOpt = vm::toArrayIndex(
                vm::StringPrimitive::createStringView(&runtime_, propString));
          } else if (prop->isNumber()) {
            propIndexOpt = doubleToArrayIndex(prop->getNumber());
            assert(propIndexOpt && "Invalid property index");
          } else if (prop->isSymbol()) {
            if (!shadowSymbols.insert(prop.getHermesValue())) {
              continue;
            }
          }

          if (propIndexOpt) {
            if (!shadowIndexes.insert(propIndexOpt.getValue())) {
              continue;
            }
          } else if (propString) {
            if (!shadowStrings.insert(prop.getHermesValue())) {
              continue;
            }
          }
        }

        // Apply filter for the property descriptor flags
        if ((keyFilter &
             (napi_key_writable | napi_key_enumerable |
              napi_key_configurable)) != 0) {
          vm::MutableHandle<vm::SymbolID> tmpSymbolStorage(&runtime_);
          vm::ComputedPropertyDescriptor desc;
          ASSIGN_ELSE_RETURN_FAILURE(
              bool hasDescriptor /*=*/,
              vm::JSObject::getOwnComputedPrimitiveDescriptor(
                  currentObj,
                  &runtime_,
                  prop,
                  vm::JSObject::IgnoreProxy::No,
                  tmpSymbolStorage,
                  desc));
          if (hasDescriptor) {
            if ((keyFilter & napi_key_writable) != 0 && !desc.flags.writable) {
              continue;
            }
            if ((keyFilter & napi_key_enumerable) != 0 &&
                !desc.flags.enumerable) {
              continue;
            }
            if ((keyFilter & napi_key_configurable) != 0 &&
                !desc.flags.configurable) {
              continue;
            }
          }
        }

        CHECK_STATUS(vm::BigStorage::push_back(keyStorage, &runtime_, prop));
        ++size;
      }

      // Continue to follow the prototype chain.
      ASSIGN_ELSE_RETURN_FAILURE(
          vm::PseudoHandle<vm::JSObject> parentObj /*=*/,
          vm::JSObject::getPrototypeOf(currentObj, &runtime_));
      currentObj = parentObj.get();
    }

    return convertKeyStorageToArray(keyStorage, 0, size, keyConversion, result);
  });
}

napi_status NodeApiEnvironment::getForInPropertyNames(
    vm::Handle<vm::JSObject> object,
    napi_key_conversion keyConversion,
    napi_value *result) noexcept {
  // Hermes optimizes retrieving property names for the 'for..in' implementation
  // by caching its results. This function takes the advantage from using it.
  uint32_t beginIndex;
  uint32_t endIndex;
  ASSIGN_ELSE_RETURN_FAILURE(
      vm::Handle<vm::BigStorage> keyStorage,
      vm::getForInPropertyNames(&runtime_, object, beginIndex, endIndex));
  return convertKeyStorageToArray(
      keyStorage, 0, endIndex - beginIndex, keyConversion, result);
}

napi_status NodeApiEnvironment::convertKeyStorageToArray(
    vm::Handle<vm::BigStorage> keyStorage,
    uint32_t startIndex,
    uint32_t length,
    napi_key_conversion keyConversion,
    napi_value *result) noexcept {
  vm::CallResult<vm::Handle<vm::JSArray>> cr =
      vm::JSArray::create(&runtime_, length, length);
  CHECK_HERMES_STATUS(cr.getStatus(), napi_generic_failure);
  vm::Handle<vm::JSArray> array = *cr;
  if (keyConversion == napi_key_numbers_to_strings) {
    vm::GCScopeMarkerRAII marker{&runtime_};
    for (size_t i = 0; i < length; ++i) {
      vm::Handle<> key = makeHandle(keyStorage->at(startIndex + i));
      if (key->isNumber()) {
        ASSIGN_ELSE_RETURN_FAILURE(
            key /*=*/, convertIndexToString(key->getNumber()));
      }
      vm::JSArray::setElementAt(array, &runtime_, i, key);
      marker.flush();
    }
  } else {
    vm::JSArray::setStorageEndIndex(array, &runtime_, length);
    vm::NoAllocScope noAlloc{&runtime_};
    vm::JSArray *arrPtr = array.get();
    for (uint32_t i = 0; i < length; ++i) {
      vm::JSArray::unsafeSetExistingElementAt(
          arrPtr, &runtime_, i, keyStorage->at(startIndex + i));
    }
  }
  return setResult(array.getHermesValue(), result);
}

vm::ExecutionStatus NodeApiEnvironment::convertKeyNumbersToStrings(
    vm::Handle<vm::JSArray> array) noexcept {
  size_t length = vm::JSArray::getLength(array.get(), &runtime_);
  for (size_t i = 0; i < length; ++i) {
    vm::HermesValue key = array->at(&runtime_, i);
    if (LLVM_UNLIKELY(key.isNumber())) {
      ASSIGN_ELSE_RETURN_HERMES_EXCEPTION(
          vm::Handle<> strKey /*=*/, convertIndexToString(key.getNumber()));
      vm::JSArray::setElementAt(array, &runtime_, i, strKey);
    }
  }
  return vm::ExecutionStatus::RETURNED;
}

vm::CallResult<vm::Handle<>> NodeApiEnvironment::convertIndexToString(
    double value) noexcept {
  OptValue<uint32_t> index = doubleToArrayIndex(value);
  if (LLVM_UNLIKELY(!index)) {
    return runtime_.raiseRangeError("Index property is out of range");
  }
  return StringBuilder(*index).makeStringHV(runtime_);
}

template <class T, class TResult>
napi_status NodeApiEnvironment::setResult(T &&value, TResult *result) noexcept {
  CHECK_ARG(result);
  return setResultUnsafe(std::forward<T>(value), result);
}

template <class T, class TResult>
napi_status NodeApiEnvironment::setOptionalResult(
    T &&value,
    TResult *result) noexcept {
  if (result) {
    return setResultUnsafe(std::forward<T>(value), result);
  }
  return clearLastError();
}

template <class T>
napi_status NodeApiEnvironment::setOptionalResult(
    T && /*value*/,
    std::nullptr_t) noexcept {
  return clearLastError();
}

template <class T>
napi_status NodeApiEnvironment::setResultUnsafe(T &&value, T *result) noexcept {
  *result = std::forward<T>(value);
  return clearLastError();
}

napi_status NodeApiEnvironment::setResultUnsafe(
    vm::HermesValue value,
    napi_value *result) noexcept {
  *result = addStackValue(value);
  return clearLastError();
}

template <class T>
napi_status NodeApiEnvironment::setResultUnsafe(
    vm::Handle<T> &&handle,
    napi_value *result) noexcept {
  return setResultUnsafe(handle.getHermesValue(), result);
}

template <class T>
napi_status NodeApiEnvironment::setResultUnsafe(
    vm::PseudoHandle<T> &&handle,
    napi_value *result) noexcept {
  return setResultUnsafe(handle.getHermesValue(), result);
}

template <class T>
napi_status NodeApiEnvironment::setResultUnsafe(
    vm::Handle<T> &&handle,
    vm::MutableHandle<T> *result) noexcept {
  *result = std::move(handle);
  return clearLastError();
}

template <class T, class TResult>
napi_status NodeApiEnvironment::setResultUnsafe(
    vm::CallResult<T> &&value,
    TResult *result) noexcept {
  return setResultUnsafe(std::move(value), napi_generic_failure, result);
}

template <class T, class TResult>
napi_status NodeApiEnvironment::setResultUnsafe(
    vm::CallResult<T> &&value,
    napi_status onException,
    TResult *result) noexcept {
  CHECK_HERMES_STATUS(value.getStatus(), onException);
  return setResultUnsafe(std::move(*value), result);
}

template <class TObject>
napi_status NodeApiEnvironment::convertToObject(
    TObject object,
    vm::MutableHandle<vm::JSObject> *result) noexcept {
  vm::CallResult<vm::HermesValue> obj =
      vm::toObject(&runtime_, makeHandle(object));
  CHECK_HERMES_STATUS(obj.getStatus(), napi_object_expected);
  return setResult(vm::Handle<vm::JSObject>::vmcast(&runtime_, *obj), result);
}

template <class TKey, class TValue>
napi_status NodeApiEnvironment::putComputed(
    vm::Handle<vm::JSObject> objHandle,
    TKey key,
    TValue value,
    bool *result) noexcept {
  vm::CallResult<bool> res = vm::JSObject::putComputed_RJS(
      objHandle,
      &runtime_,
      makeHandle(key),
      makeHandle(value),
      vm::PropOpFlags().plusThrowOnError());
  return setOptionalResult(std::move(res), result);
}

template <class TKey>
napi_status NodeApiEnvironment::hasComputed(
    vm::Handle<vm::JSObject> objHandle,
    TKey key,
    bool *result) noexcept {
  vm::CallResult<bool> res =
      vm::JSObject::hasComputed(objHandle, &runtime_, makeHandle(key));
  return setResult(std::move(res), result);
}

template <class TKey>
napi_status NodeApiEnvironment::getComputed(
    vm::Handle<vm::JSObject> objHandle,
    TKey key,
    napi_value *result) noexcept {
  vm::CallResult<vm::PseudoHandle<>> res =
      vm::JSObject::getComputed_RJS(objHandle, &runtime_, makeHandle(key));
  return setResult(std::move(res), result);
}

template <class TKey>
napi_status NodeApiEnvironment::deleteComputed(
    vm::Handle<vm::JSObject> objHandle,
    TKey key,
    bool *result) noexcept {
  vm::CallResult<bool> res = vm::JSObject::deleteComputed(
      objHandle,
      &runtime_,
      makeHandle(key),
      vm::PropOpFlags().plusThrowOnError());
  return setOptionalResult(std::move(res), result);
}

template <class TKey>
napi_status NodeApiEnvironment::getOwnComputedDescriptor(
    vm::Handle<vm::JSObject> objHandle,
    TKey key,
    vm::MutableHandle<vm::SymbolID> &tmpSymbolStorage,
    vm::ComputedPropertyDescriptor &desc,
    bool *result) noexcept {
  vm::CallResult<bool> res = vm::JSObject::getOwnComputedDescriptor(
      objHandle, &runtime_, makeHandle(key), tmpSymbolStorage, desc);
  return setResult(std::move(res), result);
}

napi_status NodeApiEnvironment::setProperty(
    napi_value object,
    napi_value key,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(key);
    CHECK_ARG(value);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return putComputed(objHandle, key, value, nullptr);
  });
}

napi_status NodeApiEnvironment::hasProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(key);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return hasComputed(objHandle, key, result);
  });
}

napi_status NodeApiEnvironment::getProperty(
    napi_value object,
    napi_value key,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(key);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return getComputed(objHandle, key, result);
  });
}

napi_status NodeApiEnvironment::deleteProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(key);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return deleteComputed(objHandle, key, result);
  });
}

napi_status NodeApiEnvironment::hasOwnProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(key);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    vm::MutableHandle<vm::SymbolID> tmpSymbolStorage(&runtime_);
    vm::ComputedPropertyDescriptor desc;
    return getOwnComputedDescriptor(
        objHandle, key, tmpSymbolStorage, desc, result);
  });
}

napi_status NodeApiEnvironment::setNamedProperty(
    napi_value object,
    const char *utf8Name,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(utf8Name);
    CHECK_ARG(value);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    vm::Handle<> name(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    CHECK_NAPI(stringHVFromUtf8(utf8Name, &name));
    return putComputed(objHandle, name, value, nullptr);
  });
}

napi_status NodeApiEnvironment::hasNamedProperty(
    napi_value object,
    const char *utf8Name,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(utf8Name);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    vm::Handle<> name(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    CHECK_NAPI(stringHVFromUtf8(utf8Name, &name));
    return hasComputed(objHandle, name, result);
  });
}

napi_status NodeApiEnvironment::getNamedProperty(
    napi_value object,
    const char *utf8Name,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(utf8Name);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    vm::Handle<> name(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    CHECK_NAPI(stringHVFromUtf8(utf8Name, &name));
    return getComputed(objHandle, name, result);
  });
}

napi_status NodeApiEnvironment::symbolIDFromPropertyDescriptor(
    const napi_property_descriptor *p,
    vm::MutableHandle<vm::SymbolID> *result) noexcept {
  if (p->utf8name != nullptr) {
    CHECK_NAPI(createSymbolID(p->utf8name, NAPI_AUTO_LENGTH, result));
  } else {
    RETURN_STATUS_IF_FALSE(p->name, napi_name_expected);
    auto namePHV = *phv(p->name);
    if (namePHV.isString()) {
      CHECK_NAPI(createSymbolID(p->name, result));
    } else if (namePHV.isSymbol()) {
      *result = namePHV.getSymbol();
    } else {
      return setLastError(napi_name_expected);
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

    auto objHandle = toObjectHandle(object);
    for (size_t i = 0; i < propertyCount; ++i) {
      const napi_property_descriptor *p = &properties[i];
      vm::MutableHandle<vm::SymbolID> name{&runtime_};
      CHECK_NAPI(symbolIDFromPropertyDescriptor(p, &name));

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
          CHECK_NAPI(newFunction(
              vm::Predefined::getSymbolID(vm::Predefined::get),
              p->getter,
              p->data,
              &localGetter));
        }
        if (p->setter != nullptr) {
          CHECK_NAPI(newFunction(
              vm::Predefined::getSymbolID(vm::Predefined::set),
              p->getter,
              p->data,
              &localSetter));
        }

        auto propRes = vm::PropertyAccessor::create(
            &runtime_,
            runtime_.makeHandle<vm::Callable>(*phv(localGetter)),
            runtime_.makeHandle<vm::Callable>(*phv(localSetter)));
        CHECK_STATUS(propRes.getStatus());
        CHECK_STATUS(vm::JSObject::defineOwnProperty(
                         objHandle,
                         &runtime_,
                         name.get(),
                         dpFlags,
                         toHandle(*propRes),
                         vm::PropOpFlags().plusThrowOnError())
                         .getStatus());
      } else if (p->method != nullptr) {
        napi_value method{};
        CHECK_NAPI(newFunction(name.get(), p->getter, p->data, &method));
        CHECK_STATUS(vm::JSObject::defineOwnProperty(
                         objHandle,
                         &runtime_,
                         name.get(),
                         dpFlags,
                         toHandle(method),
                         vm::PropOpFlags().plusThrowOnError())
                         .getStatus());
      } else {
        CHECK_STATUS(vm::JSObject::defineOwnProperty(
                         objHandle,
                         &runtime_,
                         name.get(),
                         dpFlags,
                         toHandle(p->value),
                         vm::PropOpFlags().plusThrowOnError())
                         .getStatus());
      }
    }

    return clearLastError();
  });
}

napi_status NodeApiEnvironment::objectFreeze(napi_value object) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);

    CHECK_STATUS(vm::JSObject::freeze(toObjectHandle(object), &runtime_));
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::objectSeal(napi_value object) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);

    CHECK_STATUS(vm::JSObject::seal(toObjectHandle(object), &runtime_));
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::strictEquals(
    napi_value lhs,
    napi_value rhs,
    bool *result) noexcept {
  const vm::PinnedHermesValue &lhsHV = *phv(lhs);
  const vm::PinnedHermesValue &rhsHV = *phv(rhs);
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
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::isArray(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  return setResult(vm::vmisa<vm::JSArray>(*phv(value)), result);
}

napi_status NodeApiEnvironment::getArrayLength(
    napi_value value,
    uint32_t *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    vm::Handle<vm::JSArray> arrHandle =
        vm::Handle<vm::JSArray>::vmcast_or_null(phv(value));
    RETURN_STATUS_IF_FALSE(arrHandle, napi_array_expected);
    ASSIGN_ELSE_RETURN_FAILURE(
        vm::PseudoHandle<> res /*=*/,
        vm::JSObject::getNamed_RJS(
            arrHandle,
            &runtime_,
            vm::Predefined::getSymbolID(vm::Predefined::length)));
    RETURN_STATUS_IF_FALSE(res->isNumber(), napi_number_expected);
    return setResult(static_cast<uint32_t>(res->getDouble()), result);
  });
}

napi_status NodeApiEnvironment::hasElement(
    napi_value object,
    uint32_t index,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return hasComputed(objHandle, index, result);
  });
}

napi_status NodeApiEnvironment::getElement(
    napi_value object,
    uint32_t index,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return getComputed(objHandle, index, result);
  });
}

napi_status NodeApiEnvironment::setElement(
    napi_value object,
    uint32_t index,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(value);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return putComputed(objHandle, index, value, nullptr);
  });
}

napi_status NodeApiEnvironment::deleteElement(
    napi_value object,
    uint32_t index,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    vm::MutableHandle<vm::JSObject> objHandle(&runtime_);
    CHECK_NAPI(convertToObject(object, &objHandle));
    return deleteComputed(objHandle, index, result);
  });
}


napi_status NodeApiEnvironment::createSymbolID(
    const char *str,
    size_t length,
    vm::MutableHandle<vm::SymbolID> *result) noexcept {
  napi_value strValue{};
  CHECK_NAPI(createStringUtf8(str, length, &strValue));
  CHECK_NAPI(createSymbolID(strValue, result));
  return napi_ok;
}

napi_status NodeApiEnvironment::createSymbolID(
    napi_value str,
    vm::MutableHandle<vm::SymbolID> *result) noexcept {
  CHECK_STRING_ARG(str);
  ASSIGN_ELSE_RETURN_FAILURE(
      *result,
      vm::stringToSymbolID(
          &runtime_, vm::createPseudoHandle(phv(str)->getString())));
  return napi_ok;
}


napi_status NodeApiEnvironment::createSymbol(
    napi_value description,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    vm::MutableHandle<vm::StringPrimitive> descString{&runtime_};
    if (description) {
      CHECK_STRING_ARG(description);
      descString = phv(description)->getString();
    } else {
      // If description is undefined, the descString will eventually be "".
      descString = runtime_.getPredefinedString(vm::Predefined::emptyString);
    }

    auto symbolRes = runtime_.getIdentifierTable().createNotUniquedSymbol(
        &runtime_, descString);
    CHECK_STATUS(symbolRes.getStatus());
    *result = addStackValue(vm::HermesValue::encodeSymbolValue(*symbolRes));
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::createError(
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
    // CHECK_NAPI(set_error_code(env, error_obj, code, nullptr));

    *result = addStackValue(err_phv);
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::createTypeError(
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
    // CHECK_NAPI(set_error_code(env, error_obj, code, nullptr));

    *result = addStackValue(err_phv);
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::createRangeError(
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
    // CHECK_NAPI(set_error_code(env, error_obj, code, nullptr));

    *result = addStackValue(err_phv);
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::typeOf(
    napi_value value,
    napi_valuetype *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(value);
  CHECK_ARG(result);

  const vm::PinnedHermesValue &hv = *phv(value);

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
    } else if (getExternalValue(hv)) {
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
    return setLastError(napi_invalid_arg);
  }

  return clearLastError();
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
    callbackInfo->args(args, argCount);
  }

  if (argCount != nullptr) {
    *argCount = callbackInfo->argCount();
  }

  if (thisArg != nullptr) {
    *thisArg = callbackInfo->thisArg();
  }

  if (data != nullptr) {
    *data = callbackInfo->data();
  }

  return clearLastError();
}

napi_status NodeApiEnvironment::getNewTarget(
    CallbackInfo *callbackInfo,
    napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(callbackInfo);
  CHECK_ARG(result);

  *result = callbackInfo->getNewTarget();

  return clearLastError();
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
    auto handle = runtime_.makeHandle<vm::Callable>(*phv(func));
    if (argCount > std::numeric_limits<uint32_t>::max() ||
        !runtime_.checkAvailableStack((uint32_t)argCount)) {
      return setLastError(
          napi_generic_failure,
          "NodeApiEnvironment::callFunction: Unable to call function: stack overflow");
    }

    auto &stats = runtime_.getRuntimeStats();
    const vm::instrumentation::RAIITimer timer{
        "Incoming Function", stats, stats.incomingFunction};
    vm::ScopedNativeCallFrame newFrame{
        &runtime_,
        static_cast<uint32_t>(argCount),
        handle.getHermesValue(),
        vm::HermesValue::encodeUndefinedValue(),
        *phv(object)};
    if (LLVM_UNLIKELY(newFrame.overflowed())) {
      CHECK_STATUS(runtime_.raiseStackOverflow(
          vm::StackRuntime::StackOverflowKind::NativeStack));
    }

    for (uint32_t i = 0; i < argCount; ++i) {
      newFrame->getArgRef(i) = *phv(args[i]);
    }
    auto callRes = vm::Callable::call(handle, &runtime_);
    CHECK_STATUS(callRes.getStatus());

    if (result) {
      RETURN_STATUS_IF_FALSE(!callRes->get().isEmpty(), napi_generic_failure);
      *result = addStackValue(callRes->get());
    }
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::throwError(napi_value error) noexcept {
  CHECK_ARG(error);
  runtime_.setThrownValue(*phv(error));
  // any VM calls after this point and before returning
  // to the javascript invoker will fail
  return clearLastError();
}

napi_status NodeApiEnvironment::throwError(
    const char *code,
    const char *message,
    const vm::PinnedHermesValue &prototype) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(message);
    ASSIGN_ELSE_RETURN_FAILURE(auto messageHV, stringHVFromUtf8(message));
    auto messageHandle = runtime_.makeHandle(messageHV);

    auto errorObj = runtime_.makeHandle(vm::JSError::create(
        &runtime_, runtime_.makeHandle<vm::JSObject>(prototype)));
    CHECK_STATUS(vm::JSError::recordStackTrace(errorObj, &runtime_));
    CHECK_STATUS(vm::JSError::setupStack(errorObj, &runtime_));
    CHECK_STATUS(vm::JSError::setMessage(errorObj, &runtime_, messageHandle));
    if (code) {
      ASSIGN_ELSE_RETURN_FAILURE(auto codeHV, stringHVFromUtf8(code));
      auto codeHandle = runtime_.makeHandle(codeHV);
      CHECK_STATUS(vm::JSObject::putNamed_RJS(
                       errorObj,
                       &runtime_,
                       getPredefined(NapiPredefined::CodeSymbol).getSymbol(),
                       codeHandle)
                       .getStatus());
    }

    runtime_.setThrownValue(errorObj.getHermesValue());

    // any VM calls after this point and before returning
    // to the javascript invoker will fail
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::throwError(
    const char *code,
    const char *message) noexcept {
  return throwError(code, message, runtime_.ErrorPrototype);
}

napi_status NodeApiEnvironment::throwTypeError(
    const char *code,
    const char *message) noexcept {
  return throwError(code, message, runtime_.TypeErrorPrototype);
}

napi_status NodeApiEnvironment::throwRangeError(
    const char *code,
    const char *message) noexcept {
  return throwError(code, message, runtime_.RangeErrorPrototype);
}

napi_status NodeApiEnvironment::isError(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  CHECK_ARG(result);
  *result = vm::vmisa<vm::JSError>(*phv(value));
  return clearLastError();
}

napi_status NodeApiEnvironment::getNumberValue(
    napi_value value,
    double *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_NUMBER_ARG(value);
  CHECK_ARG(result);

  *result = phv(value)->getNumberAs<double>();
  return clearLastError();
}

napi_status NodeApiEnvironment::getNumberValue(
    napi_value value,
    int32_t *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_NUMBER_ARG(value);
  CHECK_ARG(result);

  *result = phv(value)->getNumberAs<int32_t>();
  return clearLastError();
}

napi_status NodeApiEnvironment::getNumberValue(
    napi_value value,
    uint32_t *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_NUMBER_ARG(value);
  CHECK_ARG(result);

  *result = phv(value)->getNumberAs<uint32_t>();
  return clearLastError();
}

napi_status NodeApiEnvironment::getNumberValue(
    napi_value value,
    int64_t *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_NUMBER_ARG(value);
  CHECK_ARG(result);

  *result = phv(value)->getNumberAs<int64_t>();
  return clearLastError();
}

napi_status NodeApiEnvironment::getBoolValue(
    napi_value value,
    bool *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_BOOL_ARG(value);
  CHECK_ARG(result);

  *result = phv(value)->getBool();
  return clearLastError();
}

// Copies a JavaScript string into a LATIN-1 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status NodeApiEnvironment::getValueStringLatin1(
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

    return clearLastError();
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
napi_status NodeApiEnvironment::getValueStringUtf8(
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

    return clearLastError();
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
napi_status NodeApiEnvironment::getValueStringUtf16(
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

    return clearLastError();
  });
}

napi_status NodeApiEnvironment::coerceToBool(
    napi_value value,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    CHECK_ARG(result);
    bool res = vm::toBoolean(*phv(value));
    CHECK_NAPI(getBoolean(res, result));
    return clearLastError();
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
    return clearLastError();
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
    return clearLastError();
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
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::createExternal(
    void *nativeData,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    auto decoratedObj = createExternal(nativeData, nullptr);
    *result = addStackValue(decoratedObj.getHermesValue());
    if (finalizeCallback) {
      CHECK_NAPI(FinalizingAnonymousReference::create(
          *this,
          phv(*result),
          nativeData,
          finalizeCallback,
          finalizeHint,
          nullptr));
    }
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::typeTagObject(
    napi_value object,
    const napi_type_tag *typeTag) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(typeTag);
    ASSIGN_ELSE_RETURN_FAILURE(
        auto obj, vm::toObject(&runtime_, toHandle(object)));
    auto objHandle = runtime_.makeHandle<vm::JSObject>(obj);
    ASSIGN_ELSE_RETURN_FAILURE(
        auto hasTag, hasPrivate(objHandle, NapiPredefined::TypeTagSymbol));
    RETURN_STATUS_IF_FALSE(!hasTag, napi_invalid_arg);

    auto tagBuffer = vm::JSArrayBuffer::create(
        &runtime_, toObjectHandle(&runtime_.arrayBufferPrototype));
    // Create tagBufferHandle for the GC-safety before creating data block.
    auto tagBufferHandle =
        runtime_.makeHandle<vm::JSArrayBuffer>(tagBuffer.get());
    CHECK_STATUS(tagBuffer->createDataBlock(&runtime_, 16));

    auto source = reinterpret_cast<const uint8_t *>(typeTag);
    std::copy(source, source + 16, tagBuffer->getDataBlock());

    CHECK_STATUS(setPrivate(
                     objHandle,
                     NapiPredefined::TypeTagSymbol,
                     tagBuffer.getHermesValue())
                     .getStatus());
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::checkObjectTypeTag(
    napi_value object,
    const napi_type_tag *typeTag,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(object);
    CHECK_ARG(typeTag);
    CHECK_ARG(result);
    ASSIGN_ELSE_RETURN_FAILURE(
        auto obj, vm::toObject(&runtime_, toHandle(object)));
    auto objHandle = runtime_.makeHandle<vm::JSObject>(obj);
    ASSIGN_ELSE_RETURN_FAILURE(
        auto tagHV, getPrivate(objHandle, NapiPredefined::TypeTagSymbol));
    auto tagBuffer =
        vm::vmcast_or_null<vm::JSArrayBuffer>(tagHV.getHermesValue());
    RETURN_STATUS_IF_FALSE(tagBuffer, napi_generic_failure);

    auto objTagBuffer = tagBuffer->getDataBlock();
    auto source = reinterpret_cast<const uint8_t *>(typeTag);
    *result = std::equal(source, source + 16, objTagBuffer, objTagBuffer + 16);
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::getValueExternal(
    napi_value value,
    void **result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    ExternalValue *externalValue = getExternalValue(*phv(value));
    RETURN_STATUS_IF_FALSE(externalValue != nullptr, napi_invalid_arg);
    *result = externalValue->nativeData();
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::createReference(
    napi_value value,
    uint32_t initialRefCount,
    napi_ref *result) noexcept {
  return ComplexReference::create(
      *this,
      phv(value),
      initialRefCount,
      reinterpret_cast<ComplexReference **>(result));
}

napi_status NodeApiEnvironment::deleteReference(napi_ref ref) noexcept {
  CHECK_ARG(ref);
  return Reference::deleteReference(
      *this, asReference(ref), Reference::ReasonToDelete::ExternalCall);
}

napi_status NodeApiEnvironment::incReference(
    napi_ref ref,
    uint32_t *result) noexcept {
  CHECK_ARG(ref);
  uint32_t refCount{};
  CHECK_NAPI(asReference(ref)->incRefCount(*this, refCount));
  if (result != nullptr) {
    *result = refCount;
  }
  return clearLastError();
}

napi_status NodeApiEnvironment::decReference(
    napi_ref ref,
    uint32_t *result) noexcept {
  CHECK_ARG(ref);
  uint32_t refCount{};
  CHECK_NAPI(asReference(ref)->decRefCount(*this, refCount));
  if (result != nullptr) {
    *result = refCount;
  }
  return clearLastError();
}

napi_status NodeApiEnvironment::getReferenceValue(
    napi_ref ref,
    napi_value *result) noexcept {
  CHECK_ARG(ref);
  CHECK_ARG(result);
  *result = addStackValue(asReference(ref)->value(env));
  return clearLastError();
}

napi_status NodeApiEnvironment::openHandleScope(
    napi_handle_scope *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(result);

  Marker stackMarker = stackValues_.createMarker();
  stackMarkers_.emplaceBack(std::move(stackMarker));
  *result = reinterpret_cast<napi_handle_scope>(&stackMarkers_.back());
  return clearLastError();
}

napi_status NodeApiEnvironment::closeHandleScope(
    napi_handle_scope scope) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(scope);
  if (stackMarkers_.empty()) {
    return napi_handle_scope_mismatch;
  }

  Marker &lastMarker = stackMarkers_.back();
  if (reinterpret_cast<Marker *>(scope) != &lastMarker) {
    return napi_handle_scope_mismatch;
  }

  if (!stackValues_.popMarker(lastMarker)) {
    return napi_invalid_arg;
  }

  stackMarkers_.popBack();
  return clearLastError();
}

napi_status NodeApiEnvironment::openEscapableHandleScope(
    napi_escapable_handle_scope *result) noexcept {
  CHECK_ARG(result);

  if (stackMarkers_.empty()) {
    return napi_invalid_arg;
  }

  stackValues_.emplaceBack(); // value to escape to parent scope
  stackValues_.emplaceBack(
      vm::HermesValue::encodeNativeUInt32(kEscapeableSentinelNativeValue));

  return openHandleScope(reinterpret_cast<napi_handle_scope *>(result));
}

napi_status NodeApiEnvironment::closeEscapableHandleScope(
    napi_escapable_handle_scope scope) noexcept {
  auto status = closeHandleScope(reinterpret_cast<napi_handle_scope>(scope));

  if (status == napi_status::napi_ok) {
    auto &sentinelValue = stackValues_.back();
    if (sentinelValue.isNativeValue()) {
      auto nativeValue = sentinelValue.getNativeUInt32();
      if (nativeValue == kEscapeableSentinelNativeValue ||
          nativeValue == kUsedEscapeableSentinelNativeValue) {
        stackValues_.popBack();
      } else {
        status = napi_handle_scope_mismatch;
      }
    }
  }

  return status;
}

napi_status NodeApiEnvironment::escapeHandle(
    napi_escapable_handle_scope scope,
    napi_value escapee,
    napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(scope);
  CHECK_ARG(escapee);
  CHECK_ARG(result);

  Marker *marker = reinterpret_cast<Marker *>(scope);
  bool isValidMarker{false};
  stackMarkers_.forEach(
      [&](const Marker &m) { isValidMarker |= &m == marker; });
  if (!isValidMarker) {
    return napi_invalid_arg;
  }

  Marker sentinelMarker = stackValues_.getPreviousMarker(*marker);
  if (!sentinelMarker.isValid()) {
    return napi_invalid_arg;
  }
  Marker escapedValueMarker = stackValues_.getPreviousMarker(sentinelMarker);
  if (!escapedValueMarker.isValid()) {
    return napi_invalid_arg;
  }

  vm::PinnedHermesValue *sentinelTag = stackValues_.at(sentinelMarker);
  if (!sentinelTag || !sentinelTag->isNativeValue()) {
    return napi_invalid_arg;
  }
  if (sentinelTag->getNativeUInt32() != kUsedEscapeableSentinelNativeValue) {
    return setLastError(napi_escape_called_twice);
  }
  if (sentinelTag->getNativeUInt32() != kEscapeableSentinelNativeValue) {
    return napi_invalid_arg;
  }

  vm::PinnedHermesValue *escapedValue = stackValues_.at(escapedValueMarker);
  *escapedValue = *reinterpret_cast<vm::PinnedHermesValue *>(escapee);

  return clearLastError();
}

napi_status NodeApiEnvironment::newInstance(
    napi_value constructor,
    size_t argc,
    const napi_value *argv,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(constructor);
    CHECK_ARG(result);
    if (argc > 0) {
      CHECK_ARG(argv);
    }

    vm::Callable *callable =
        vm::vmcast_or_null<vm::Callable>(*phv(constructor));
    RETURN_STATUS_IF_FALSE(callable, napi_function_expected);
    auto funcHandle = runtime_.makeHandle<vm::Callable>(callable);

    if (argc > std::numeric_limits<uint32_t>::max() ||
        !runtime_.checkAvailableStack((uint32_t)argc)) {
      return setLastError(
          napi_generic_failure,
          "NodeApiEnvironment::newInstance: Unable to call function: stack overflow");
    }

    auto &stats = runtime_.getRuntimeStats();
    const vm::instrumentation::RAIITimer timer{
        "Incoming Function: Call As Constructor",
        stats,
        stats.incomingFunction};

    // We follow es5 13.2.2 [[Construct]] here. Below F == func.
    // 13.2.2.5:
    //    Let proto be the value of calling the [[Get]] internal property of
    //    F with argument "prototype"
    // 13.2.2.6:
    //    If Type(proto) is Object, set the [[Prototype]] internal property
    //    of obj to proto
    // 13.2.2.7:
    //    If Type(proto) is not Object, set the [[Prototype]] internal property
    //    of obj to the standard built-in Object prototype object as described
    //    in 15.2.4
    //
    // Note that 13.2.2.1-4 are also handled by the call to newObject.
    auto thisRes = vm::Callable::createThisForConstruct(funcHandle, &runtime_);
    // We need to capture this in case the ctor doesn't return an object,
    // we need to return this object.
    auto objHandle = runtime_.makeHandle<vm::JSObject>(std::move(*thisRes));

    // 13.2.2.8:
    //    Let result be the result of calling the [[Call]] internal property of
    //    F, providing obj as the this value and providing the argument list
    //    passed into [[Construct]] as args.
    //
    // For us result == res.

    vm::ScopedNativeCallFrame newFrame{
        &runtime_,
        static_cast<uint32_t>(argc),
        funcHandle.getHermesValue(),
        funcHandle.getHermesValue(),
        objHandle.getHermesValue()};
    if (newFrame.overflowed()) {
      CHECK_STATUS(runtime_.raiseStackOverflow(
          ::hermes::vm::StackRuntime::StackOverflowKind::NativeStack));
    }
    for (uint32_t i = 0; i != argc; ++i) {
      newFrame->getArgRef(i) = *phv(argv[i]);
    }
    // The last parameter indicates that this call should construct an object.
    auto callRes = vm::Callable::call(funcHandle, &runtime_);
    CHECK_STATUS(callRes.getStatus());

    // 13.2.2.9:
    //    If Type(result) is Object then return result
    // 13.2.2.10:
    //    Return obj
    auto resultValue = callRes->get();
    vm::HermesValue resultHValue =
        resultValue.isObject() ? resultValue : objHandle.getHermesValue();
    *result = addStackValue(resultHValue);

    return clearLastError();
  });
}

vm::JSObject *NodeApiEnvironment::getObject(
    const vm::HermesValue &value) noexcept {
  return reinterpret_cast<vm::JSObject *>(value.getObject());
}

vm::Handle<vm::JSObject> NodeApiEnvironment::toObjectHandle(
    napi_value value) noexcept {
  return vm::Handle<vm::JSObject>::vmcast(phv(value));
}

vm::Handle<vm::JSObject> NodeApiEnvironment::toObjectHandle(
    const vm::PinnedHermesValue *value) noexcept {
  return vm::Handle<vm::JSObject>::vmcast(value);
}

vm::Handle<vm::JSArray> NodeApiEnvironment::toArrayHandle(
    napi_value value) noexcept {
  return vm::Handle<vm::JSArray>::vmcast(phv(value));
}

vm::Handle<vm::HermesValue> NodeApiEnvironment::stringHandle(
    napi_value value) noexcept {
  return vm::Handle<vm::HermesValue>::vmcast(phv(value));
}

vm::Handle<vm::HermesValue> NodeApiEnvironment::toHandle(
    const vm::HermesValue &value) noexcept {
  return runtime_.makeHandle(value);
}

vm::Handle<> NodeApiEnvironment::toHandle(napi_value value) noexcept {
  auto &hv = *phv(value);
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

void NodeApiEnvironment::addToFinalizerQueue(Finalizer *finalizer) noexcept {
  finalizerQueue_.pushBack(finalizer);
}

void NodeApiEnvironment::addGCRoot(Reference *reference) noexcept {
  gcRoots_.pushBack(reference);
}

void NodeApiEnvironment::addFinalizingGCRoot(Reference *reference) noexcept {
  finalizingGCRoots_.pushBack(reference);
}

void NodeApiEnvironment::pushOrderedSet(OrderedSet &set) noexcept {
  orderedSets_.push_back(&set);
}

void NodeApiEnvironment::popOrderedSet() noexcept {
  orderedSets_.pop_back();
}

template <typename TLambda>
void NodeApiEnvironment::callIntoModule(TLambda &&call) noexcept {
  int openHandleScopesBefore = openHandleScopes_;
  int openCallbackScopesBefore = openCallbackScopes_;
  clearLastError();
  call(this);
  CRASH_IF_FALSE(openHandleScopesBefore == openHandleScopes_);
  CRASH_IF_FALSE(openCallbackScopesBefore == openCallbackScopes_);
  if (!lastException_.isEmpty()) {
    runtime_.setThrownValue(lastException_);
    lastException_ = EmptyHermesValue;
  }
}

napi_status NodeApiEnvironment::callFinalizer(
    napi_finalize finalizeCallback,
    void *nativeData,
    void *finalizeHint) noexcept {
  return handleExceptions([&] {
    callIntoModule([&](NodeApiEnvironment *env) {
      finalizeCallback(
          reinterpret_cast<napi_env>(env), nativeData, finalizeHint);
    });
    return napi_ok;
  });
}

napi_status NodeApiEnvironment::instanceOf(
    napi_value object,
    napi_value constructor,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_FUNCTION_ARG(constructor);
    CHECK_ARG(result);
    auto res = vm::instanceOfOperator_RJS(
        &runtime_,
        runtime_.makeHandle(*phv(object)),
        runtime_.makeHandle(*phv(constructor)));
    CHECK_STATUS(res.getStatus());
    *result = *res;
    return clearLastError();
  });
}

// Methods to support catching exceptions
napi_status NodeApiEnvironment::isExceptionPending(bool *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(result);

  *result = !lastException_.isEmpty();
  return clearLastError();
}

napi_status NodeApiEnvironment::getAndClearLastException(
    napi_value *result) noexcept {
  // No handleExceptions because Hermes calls cannot throw JS exceptions here.
  CHECK_ARG(result);

  if (lastException_.isEmpty()) {
    return getUndefined(result);
  } else {
    *result = addStackValue(lastException_);
    lastException_ = EmptyHermesValue;
  }

  return clearLastError();
}

napi_status NodeApiEnvironment::isArrayBuffer(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  CHECK_ARG(result);
  *result = vm::vmisa<vm::JSArrayBuffer>(*phv(value));
  return clearLastError();
}

napi_status NodeApiEnvironment::createArrayBuffer(
    size_t byteLength,
    void **data,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);

    auto buffer = vm::JSArrayBuffer::create(
        &runtime_, toObjectHandle(&runtime_.arrayBufferPrototype));
    // Add result to the local values before allocating buffer to ensure
    // GC-safety.
    *result = addStackValue(buffer.getHermesValue());
    CHECK_STATUS(buffer->createDataBlock(&runtime_, byteLength));

    // Optionally return a pointer to the buffer's data, to avoid another call
    // to retrieve it.
    if (data != nullptr) {
      *data = buffer->getDataBlock();
    }
    return clearLastError();
  });
}

struct ExternalBuffer : Buffer {
  ExternalBuffer(
      NodeApiEnvironment &env,
      void *externalData,
      size_t byteLength,
      napi_finalize finalizeCallback,
      void *finalizeHint) noexcept
      : Buffer(reinterpret_cast<uint8_t *>(externalData), byteLength),
        env_(env),
        finalizer_(
            FinalizingReferenceFactory<FinalizingAnonymousReference>::create(
                externalData,
                finalizeCallback,
                finalizeHint)) {}

  ~ExternalBuffer() noexcept override {
    env_.addToFinalizerQueue(finalizer_);
  }

 private:
  NodeApiEnvironment &env_;
  FinalizingAnonymousReference *finalizer_;
};

napi_status NodeApiEnvironment::createExternalArrayBuffer(
    void *externalData,
    size_t byteLength,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);

    auto buffer = vm::JSArrayBuffer::create(
        &runtime_, toObjectHandle(&runtime_.arrayBufferPrototype));
    // Add result to the local values before allocating buffer to ensure
    // GC-safety.
    *result = addStackValue(buffer.getHermesValue());

    auto externalBuffer = std::make_unique<ExternalBuffer>(
        env, externalData, byteLength, finalizeCallback, finalizeHint);
    buffer->setExternalBuffer(&runtime_, std::move(externalBuffer));

    return clearLastError();
  });
}

napi_status NodeApiEnvironment::getArrayBufferInfo(
    napi_value arrayBuffer,
    void **data,
    size_t *byteLength) noexcept {
  CHECK_ARG(arrayBuffer);
  RETURN_STATUS_IF_FALSE(
      vm::vmisa<vm::JSArrayBuffer>(*phv(arrayBuffer)), napi_invalid_arg);

  vm::JSArrayBuffer *buffer = vm::vmcast<vm::JSArrayBuffer>(*phv(arrayBuffer));
  if (data != nullptr) {
    // TODO: make it right for attached data blocks
    *data = buffer->attached() ? buffer->getDataBlock() : nullptr;
  }

  if (byteLength != nullptr) {
    // TODO: make it right for attached data blocks
    *byteLength = buffer->attached() ? buffer->size() : 0;
  }

  return clearLastError();
}

napi_status NodeApiEnvironment::isTypedArray(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  CHECK_ARG(result);
  *result = vm::vmisa<vm::JSTypedArrayBase>(*phv(value));
  return clearLastError();
}

template <vm::CellKind CellKind>
constexpr const char *getTypedArrayName() noexcept {
  static constexpr const char *names[] = {
#define TYPED_ARRAY(name, type) #name "Array",
#include "hermes/VM/TypedArrays.def"
  };
  return names
      [static_cast<int>(CellKind) -
       static_cast<int>(vm::CellKind::TypedArrayBaseKind_first)];
}

template <typename TElement, vm::CellKind CellKind>
napi_status NodeApiEnvironment::createTypedArray(
    size_t length,
    vm::JSArrayBuffer *buffer,
    size_t byteOffset,
    vm::JSTypedArrayBase **result) noexcept {
  constexpr size_t elementSize = sizeof(TElement);
  if (elementSize > 1) {
    THROW_RANGE_ERROR_IF_FALSE(
        byteOffset % elementSize == 0,
        "ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT",
        "start offset of ",
        getTypedArrayName<CellKind>(),
        " should be a multiple of ",
        elementSize);
  }
  THROW_RANGE_ERROR_IF_FALSE(
      length * elementSize + byteOffset <= buffer->size(),
      "ERR_NAPI_INVALID_TYPEDARRAY_LENGTH",
      "Invalid typed array length");
  using TypedArray = vm::JSTypedArray<TElement, CellKind>;
  auto arrayHandle =
      TypedArray::create(&runtime_, TypedArray::getPrototype(&runtime_));
  vm::JSTypedArrayBase::setBuffer(
      &runtime_,
      arrayHandle.get(),
      buffer,
      byteOffset,
      length * elementSize,
      static_cast<uint8_t>(elementSize));
  *result = arrayHandle.get();
  return clearLastError();
}

napi_status NodeApiEnvironment::createTypedArray(
    napi_typedarray_type type,
    size_t length,
    napi_value arrayBuffer,
    size_t byteOffset,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(arrayBuffer);
    CHECK_ARG(result);

    RETURN_STATUS_IF_FALSE(
        vm::vmisa<vm::JSArrayBuffer>(*phv(arrayBuffer)), napi_invalid_arg);

    vm::JSArrayBuffer *buffer =
        vm::vmcast<vm::JSArrayBuffer>(*phv(arrayBuffer));
    vm::JSTypedArrayBase *typedArray{};

    switch (type) {
      case napi_int8_array:
        CHECK_NAPI(createTypedArray<int8_t, vm::CellKind::Int8ArrayKind>(
            length, buffer, byteOffset, &typedArray));
        break;
      case napi_uint8_array:
        CHECK_NAPI(createTypedArray<uint8_t, vm::CellKind::Uint8ArrayKind>(
            length, buffer, byteOffset, &typedArray));
        break;
      case napi_uint8_clamped_array:
        CHECK_NAPI(
            createTypedArray<uint8_t, vm::CellKind::Uint8ClampedArrayKind>(
                length, buffer, byteOffset, &typedArray));
        break;
      case napi_int16_array:
        CHECK_NAPI(createTypedArray<int16_t, vm::CellKind::Int16ArrayKind>(
            length, buffer, byteOffset, &typedArray));
        break;
      case napi_uint16_array:
        CHECK_NAPI(createTypedArray<uint16_t, vm::CellKind::Uint16ArrayKind>(
            length, buffer, byteOffset, &typedArray));
        break;
      case napi_int32_array:
        CHECK_NAPI(createTypedArray<int32_t, vm::CellKind::Int32ArrayKind>(
            length, buffer, byteOffset, &typedArray));
        break;
      case napi_uint32_array:
        CHECK_NAPI(createTypedArray<uint32_t, vm::CellKind::Uint32ArrayKind>(
            length, buffer, byteOffset, &typedArray));
        break;
      case napi_float32_array:
        CHECK_NAPI(createTypedArray<float, vm::CellKind::Float32ArrayKind>(
            length, buffer, byteOffset, &typedArray));
        break;
      case napi_float64_array:
        CHECK_NAPI(createTypedArray<double, vm::CellKind::Float64ArrayKind>(
            length, buffer, byteOffset, &typedArray));
        break;
      case napi_bigint64_array:
        return setLastError(
            napi_not_implemented,
            "BigInt64Array is not implemented in Hermes yet");
      case napi_biguint64_array:
        return setLastError(
            napi_not_implemented,
            "BigUint64Array is not implemented in Hermes yet");
      default:
        return setLastError(
            napi_invalid_arg, "Unsupported TypedArray type ", type);
    }

    *result = addStackValue(vm::HermesValue::encodeObjectValue(typedArray));
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::getTypedArrayInfo(
    napi_value typedArray,
    napi_typedarray_type *type,
    size_t *length,
    void **data,
    napi_value *arrayBuffer,
    size_t *byteOffset) noexcept {
  CHECK_ARG(typedArray);

  vm::JSTypedArrayBase *array =
      vm::vmcast_or_null<vm::JSTypedArrayBase>(*phv(typedArray));
  RETURN_STATUS_IF_FALSE(array, napi_invalid_arg);

  if (type != nullptr) {
    if (vm::vmisa<vm::Int8Array>(array)) {
      *type = napi_int8_array;
    } else if (vm::vmisa<vm::Uint8Array>(array)) {
      *type = napi_uint8_array;
    } else if (vm::vmisa<vm::Uint8ClampedArray>(array)) {
      *type = napi_uint8_clamped_array;
    } else if (vm::vmisa<vm::Int16Array>(array)) {
      *type = napi_int16_array;
    } else if (vm::vmisa<vm::Uint16Array>(array)) {
      *type = napi_uint16_array;
    } else if (vm::vmisa<vm::Int32Array>(array)) {
      *type = napi_int32_array;
    } else if (vm::vmisa<vm::Uint32Array>(array)) {
      *type = napi_uint32_array;
    } else if (vm::vmisa<vm::Float32Array>(array)) {
      *type = napi_float32_array;
    } else if (vm::vmisa<vm::Float64Array>(array)) {
      *type = napi_float64_array;
    } else {
      return setLastError(napi_generic_failure, "Unknown TypedArray type");
    }
  }

  if (length != nullptr) {
    *length = array->getLength();
  }

  if (data != nullptr) {
    *data = array->attached(&runtime_)
        ? array->getBuffer(&runtime_)->getDataBlock() + array->getByteOffset()
        : nullptr;
  }

  if (arrayBuffer != nullptr) {
    *arrayBuffer = array->attached(&runtime_)
        ? addStackValue(
              vm::HermesValue::encodeObjectValue(array->getBuffer(&runtime_)))
        : (napi_value)&getPredefined(NapiPredefined::UndefinedValue);
  }

  if (byteOffset != nullptr) {
    *byteOffset = array->getByteOffset();
  }

  return clearLastError();
}

napi_status NodeApiEnvironment::createDataView(
    size_t byteLength,
    napi_value arrayBuffer,
    size_t byteOffset,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(arrayBuffer);
    CHECK_ARG(result);

    vm::JSArrayBuffer *buffer =
        vm::vmcast_or_null<vm::JSArrayBuffer>(*phv(arrayBuffer));
    RETURN_STATUS_IF_FALSE(buffer, napi_invalid_arg);

    THROW_RANGE_ERROR_IF_FALSE(
        byteLength + byteOffset <= buffer->size(),
        "ERR_NAPI_INVALID_DATAVIEW_ARGS",
        "byte_offset + byte_length should be less than or "
        "equal to the size in bytes of the array passed in");
    auto viewHandle = vm::JSDataView::create(
        &runtime_, toObjectHandle(&runtime_.dataViewPrototype));
    viewHandle->setBuffer(&runtime_, buffer, byteOffset, byteLength);
    *result = addStackValue(viewHandle.getHermesValue());
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::isDataView(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  CHECK_ARG(result);
  *result = vm::vmisa<vm::JSDataView>(*phv(value));
  return clearLastError();
}

napi_status NodeApiEnvironment::getDataViewInfo(
    napi_value dataView,
    size_t *byteLength,
    void **data,
    napi_value *arrayBuffer,
    size_t *byteOffset) noexcept {
  CHECK_ARG(dataView);

  vm::JSDataView *view = vm::vmcast_or_null<vm::JSDataView>(*phv(dataView));
  RETURN_STATUS_IF_FALSE(view, napi_invalid_arg);

  if (byteLength != nullptr) {
    *byteLength = view->byteLength();
  }

  if (data != nullptr) {
    *data = view->attached(&runtime_)
        ? view->getBuffer(&runtime_)->getDataBlock() + view->byteOffset()
        : nullptr;
  }

  if (arrayBuffer != nullptr) {
    *arrayBuffer = view->attached(&runtime_)
        ? addStackValue(view->getBuffer(&runtime_).getHermesValue())
        : (napi_value)&getPredefined(NapiPredefined::UndefinedValue);
  }

  if (byteOffset != nullptr) {
    *byteOffset = view->byteOffset();
  }

  return clearLastError();
}

napi_status NodeApiEnvironment::getVersion(uint32_t *result) noexcept {
  CHECK_ARG(result);
  *result = NAPI_VERSION;
  return clearLastError();
}

napi_status NodeApiEnvironment::createPromise(
    napi_value *promise,
    napi_value *resolveFunction,
    napi_value *rejectFunction) noexcept {
  napi_value global;
  napi_value promiseConstructor;
  CHECK_NAPI(getGlobal(&global));
  CHECK_NAPI(getNamedProperty(global, "Promise", &promiseConstructor));

  // The executor function is to be executed by the constructor during the
  // process of constructing the new Promise object. The executor is custom code
  // that ties an outcome to a promise. We return the resolveFunction and
  // rejectFunction given to the executor. Since the execution is synchronous,
  // we allocate executorData on the callstack.
  struct ExecutorData {
    static vm::CallResult<vm::HermesValue>
    callback(void *context, vm::Runtime *runtime, vm::NativeArgs args) {
      return (reinterpret_cast<ExecutorData *>(context))->callback(args);
    }

    vm::CallResult<vm::HermesValue> callback(const vm::NativeArgs &args) {
      *resolve = env_->addStackValue(args.getArg(0));
      *reject = env_->addStackValue(args.getArg(1));
      return vm::HermesValue();
    }

    NodeApiEnvironment *env_{};
    napi_value *resolve{};
    napi_value *reject{};
  } executorData{this, resolveFunction, rejectFunction};

  napi_value nameValue{};
  CHECK_NAPI(createStringUtf8("Promise", NAPI_AUTO_LENGTH, &nameValue));
  auto nameRes = vm::stringToSymbolID(
      &runtime_, vm::createPseudoHandle(phv(nameValue)->getString()));
  CHECK_STATUS(nameRes.getStatus());

  auto executorFunction = vm::NativeFunction::createWithoutPrototype(
      &runtime_, &executorData, &ExecutorData::callback, nameRes->get(), 2);
  napi_value func = addStackValue(executorFunction.getHermesValue());
  return newInstance(promiseConstructor, 1, &func, promise);
}

napi_status NodeApiEnvironment::createPromise(
    napi_deferred *deferred,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(deferred);
    CHECK_ARG(result);

    napi_value jsPromise{}, jsResolve{}, jsReject{}, jsDeferred{};
    CHECK_NAPI(createPromise(&jsPromise, &jsResolve, &jsReject));
    CHECK_NAPI(createObject(&jsDeferred));
    CHECK_NAPI(setNamedProperty(jsDeferred, "resolve", jsResolve));
    CHECK_NAPI(setNamedProperty(jsDeferred, "reject", jsReject));

    CHECK_NAPI(StrongReference::create(
        *this,
        phv(jsDeferred),
        reinterpret_cast<StrongReference **>(deferred)));
    *result = jsPromise;
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::resolveDeferred(
    napi_deferred deferred,
    napi_value resolution) noexcept {
  return concludeDeferred(deferred, "resolve", resolution);
}

napi_status NodeApiEnvironment::rejectDeferred(
    napi_deferred deferred,
    napi_value resolution) noexcept {
  return concludeDeferred(deferred, "resolve", resolution);
}

napi_status NodeApiEnvironment::concludeDeferred(
    napi_deferred deferred,
    const char *propertyName,
    napi_value result) noexcept {
  CHECK_ARG(deferred);
  CHECK_ARG(result);

  Reference *ref = asReference(deferred);

  const auto &jsDeferred = ref->value(*this);
  napi_value resolver, callResult;
  CHECK_NAPI(
      getNamedProperty(addStackValue(jsDeferred), propertyName, &resolver));
  CHECK_NAPI(callFunction(nullptr, resolver, 1, &result, &callResult));
  Reference::deleteReference(
      *this, ref, Reference::ReasonToDelete::ZeroRefCount);
  return clearLastError();
}

napi_status NodeApiEnvironment::isPromise(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  CHECK_ARG(result);

  napi_value global;
  napi_value promiseConstructor;
  CHECK_NAPI(getGlobal(&global));
  CHECK_NAPI(getNamedProperty(global, "Promise", &promiseConstructor));

  return instanceOf(value, promiseConstructor, result);
}

napi_status NodeApiEnvironment::createDate(
    double dateTime,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    auto dateHandle = vm::JSDate::create(
        &runtime_, dateTime, toObjectHandle(&runtime_.datePrototype));
    *result = addStackValue(dateHandle.getHermesValue());
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::isDate(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  CHECK_ARG(result);
  const vm::PinnedHermesValue *hv = phv(value);
  *result = hv->isObject() && vm::vmisa<vm::JSDate>(*hv);
  return clearLastError();
}

napi_status NodeApiEnvironment::getDateValue(
    napi_value value,
    double *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    CHECK_ARG(result);
    vm::JSDate *date = vm::vmcast_or_null<vm::JSDate>(*phv(value));
    RETURN_STATUS_IF_FALSE(date, napi_date_expected);
    *result = date->getPrimitiveValue();
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::adjustExternalMemory(
    int64_t change_in_bytes,
    int64_t *adjusted_value) noexcept {
  // TODO: implement
  // CHECK_ENV(env);
  // CHECK_ARG(env, adjusted_value);

  // *adjusted_value =
  //     env->isolate->AdjustAmountOfExternalAllocatedMemory(change_in_bytes);

  // return napi_clear_last_error(env);
  return napi_ok;
}

napi_status NodeApiEnvironment::setInstanceData(
    void *nativeData,
    napi_finalize finalizeCallback,
    void *finalizeHint) noexcept {
  if (instanceData_ != nullptr) {
    // Our contract so far has been to not finalize any old data there may be.
    // So we simply delete it.
    delete instanceData_;
    instanceData_ = nullptr;
  }
  return InstanceData::create(
      *this, nativeData, finalizeCallback, finalizeHint, &instanceData_);
}

napi_status NodeApiEnvironment::getInstanceData(void **nativeData) noexcept {
  CHECK_ARG(nativeData);
  *nativeData =
      instanceData_ == nullptr ? nullptr : instanceData_->nativeData();
  return clearLastError();
}

napi_status NodeApiEnvironment::detachArrayBuffer(
    napi_value arrayBuffer) noexcept {
  CHECK_ARG(arrayBuffer);
  auto buffer = vm::vmcast_or_null<vm::JSArrayBuffer>(*phv(arrayBuffer));
  RETURN_STATUS_IF_FALSE(buffer, napi_arraybuffer_expected);
  buffer->detach(&runtime_.getHeap());
  return clearLastError();
}

napi_status NodeApiEnvironment::isDetachedArrayBuffer(
    napi_value arrayBuffer,
    bool *result) noexcept {
  CHECK_ARG(arrayBuffer);
  CHECK_ARG(result);
  auto buffer = vm::vmcast_or_null<vm::JSArrayBuffer>(*phv(arrayBuffer));
  RETURN_STATUS_IF_FALSE(buffer, napi_arraybuffer_expected);
  *result = buffer->attached();
  return clearLastError();
}

napi_status NodeApiEnvironment::runScript(
    napi_value source,
    const char *sourceURL,
    napi_value *result) noexcept {
  size_t sourceSize{};
  CHECK_NAPI(getValueStringUtf8(source, nullptr, 0, &sourceSize));
  auto buffer = std::make_unique<std::vector<uint8_t>>();
  buffer->assign(sourceSize + 1, '\0');
  CHECK_NAPI(getValueStringUtf8(
      source,
      reinterpret_cast<char *>(buffer->data()),
      sourceSize + 1,
      nullptr));
  CHECK_NAPI(runScriptWithSourceMap(
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
  return clearLastError();
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
  CHECK_NAPI(runScriptWithSourceMap(
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
  return clearLastError();
}

napi_status NodeApiEnvironment::serializeScript(
    napi_value source,
    const char *sourceURL,
    napi_ext_buffer_callback bufferCallback,
    void *bufferHint) noexcept {
  size_t sourceSize{};
  CHECK_NAPI(getValueStringUtf8(source, nullptr, 0, &sourceSize));
  auto buffer = std::make_unique<std::vector<uint8_t>>();
  buffer->assign(sourceSize + 1, '\0');
  CHECK_NAPI(getValueStringUtf8(
      source,
      reinterpret_cast<char *>(buffer->data()),
      sourceSize + 1,
      nullptr));
  napi_ext_prepared_script preparedScript{};
  CHECK_NAPI(prepareScriptWithSourceMap(
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
  CHECK_NAPI(
      serializePreparedScript(preparedScript, bufferCallback, bufferHint));
  return clearLastError();
}

napi_status NodeApiEnvironment::runScriptWithSourceMap(
    std::unique_ptr<HermesBuffer> script,
    std::unique_ptr<HermesBuffer> sourceMap,
    const char *sourceURL,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    napi_ext_prepared_script preparedScript{nullptr};
    CHECK_NAPI(prepareScriptWithSourceMap(
        std::move(script), std::move(sourceMap), sourceURL, &preparedScript));
    CHECK_NAPI(runPreparedScript(preparedScript, result));
    return clearLastError();
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
      return setLastError(
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
        return setLastError(napi_generic_failure);
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
    return setLastError(napi_generic_failure);
  }
  *preparedScript =
      reinterpret_cast<napi_ext_prepared_script>(new HermesPreparedJavaScript(
          std::move(bcErr.first),
          runtimeFlags,
          sourceURL ? sourceURL : "",
          isBytecode));
  return clearLastError();
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
    return clearLastError();
  });
}

napi_status NodeApiEnvironment::deletePreparedScript(
    napi_ext_prepared_script preparedScript) noexcept {
  CHECK_ARG(preparedScript);
  delete reinterpret_cast<HermesPreparedJavaScript *>(preparedScript);
  return clearLastError();
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

  return clearLastError();
}

napi_status NodeApiEnvironment::collectGarbage() noexcept {
  runtime_.collect("test");
  CHECK_NAPI(runReferenceFinalizers());
  return clearLastError();
}

napi_status NodeApiEnvironment::runReferenceFinalizers() noexcept {
  if (!isRunningFinalizers_) {
    isRunningFinalizers_ = true;
    Reference::finalizeAll(*this, finalizerQueue_);
    isRunningFinalizers_ = false;
  }
  return napi_ok;
}

} // namespace napi
} // namespace hermes

//=============================================================================
// NAPI implementation
//=============================================================================

napi_status __cdecl napi_get_last_error_info(
    napi_env env,
    const napi_extended_error_info **result) {
  return CHECKED_ENV(env)->getLastErrorInfo(result);
}

//-----------------------------------------------------------------------------
// Getters for defined singletons
//-----------------------------------------------------------------------------

napi_status __cdecl napi_get_undefined(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->getUndefined(result);
}

napi_status __cdecl napi_get_null(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->getNull(result);
}

napi_status __cdecl napi_get_global(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->getGlobal(result);
}

napi_status __cdecl napi_get_boolean(
    napi_env env,
    bool value,
    napi_value *result) {
  return CHECKED_ENV(env)->getBoolean(value, result);
}

//-----------------------------------------------------------------------------
// Methods to create Primitive types/Objects
//-----------------------------------------------------------------------------

napi_status __cdecl napi_create_object(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->createObject(result);
}

napi_status __cdecl napi_create_array(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->createArray(result);
}

napi_status __cdecl napi_create_array_with_length(
    napi_env env,
    size_t length,
    napi_value *result) {
  return CHECKED_ENV(env)->createArray(length, result);
}

napi_status __cdecl napi_create_double(
    napi_env env,
    double value,
    napi_value *result) {
  return CHECKED_ENV(env)->createNumber(value, result);
}

napi_status __cdecl napi_create_int32(
    napi_env env,
    int32_t value,
    napi_value *result) {
  return CHECKED_ENV(env)->createNumber(value, result);
}

napi_status __cdecl napi_create_uint32(
    napi_env env,
    uint32_t value,
    napi_value *result) {
  return CHECKED_ENV(env)->createNumber(value, result);
}

napi_status __cdecl napi_create_int64(
    napi_env env,
    int64_t value,
    napi_value *result) {
  return CHECKED_ENV(env)->createNumber(value, result);
}

napi_status __cdecl napi_create_string_latin1(
    napi_env env,
    const char *str,
    size_t length,
    napi_value *result) {
  return CHECKED_ENV(env)->createStringLatin1(str, length, result);
}

napi_status __cdecl napi_create_string_utf8(
    napi_env env,
    const char *str,
    size_t length,
    napi_value *result) {
  return CHECKED_ENV(env)->createStringUtf8(str, length, result);
}

napi_status __cdecl napi_create_string_utf16(
    napi_env env,
    const char16_t *str,
    size_t length,
    napi_value *result) {
  return CHECKED_ENV(env)->createStringUtf16(str, length, result);
}

napi_status __cdecl napi_create_symbol(
    napi_env env,
    napi_value description,
    napi_value *result) {
  return CHECKED_ENV(env)->createSymbol(description, result);
}

napi_status __cdecl napi_create_function(
    napi_env env,
    const char *utf8name,
    size_t length,
    napi_callback cb,
    void *callback_data,
    napi_value *result) {
  return CHECKED_ENV(env)->createFunction(
      utf8name, length, cb, callback_data, result);
}

napi_status __cdecl napi_create_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  return CHECKED_ENV(env)->createError(code, msg, result);
}

napi_status __cdecl napi_create_type_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  return CHECKED_ENV(env)->createTypeError(code, msg, result);
}

napi_status __cdecl napi_create_range_error(
    napi_env env,
    napi_value code,
    napi_value msg,
    napi_value *result) {
  return CHECKED_ENV(env)->createRangeError(code, msg, result);
}

//-----------------------------------------------------------------------------
// Methods to get the native napi_value from Primitive type
//-----------------------------------------------------------------------------

napi_status __cdecl napi_typeof(
    napi_env env,
    napi_value value,
    napi_valuetype *result) {
  return CHECKED_ENV(env)->typeOf(value, result);
}

napi_status __cdecl napi_get_value_double(
    napi_env env,
    napi_value value,
    double *result) {
  return CHECKED_ENV(env)->getNumberValue(value, result);
}

napi_status __cdecl napi_get_value_int32(
    napi_env env,
    napi_value value,
    int32_t *result) {
  return CHECKED_ENV(env)->getNumberValue(value, result);
}

napi_status __cdecl napi_get_value_uint32(
    napi_env env,
    napi_value value,
    uint32_t *result) {
  return CHECKED_ENV(env)->getNumberValue(value, result);
}

napi_status __cdecl napi_get_value_int64(
    napi_env env,
    napi_value value,
    int64_t *result) {
  return CHECKED_ENV(env)->getNumberValue(value, result);
}

napi_status __cdecl napi_get_value_bool(
    napi_env env,
    napi_value value,
    bool *result) {
  return CHECKED_ENV(env)->getBoolValue(value, result);
}

// Copies a JavaScript string into a LATIN-1 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status __cdecl napi_get_value_string_latin1(
    napi_env env,
    napi_value value,
    char *buf,
    size_t bufsize,
    size_t *result) {
  return CHECKED_ENV(env)->getValueStringLatin1(value, buf, bufsize, result);
}

// Copies a JavaScript string into a UTF-8 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status __cdecl napi_get_value_string_utf8(
    napi_env env,
    napi_value value,
    char *buf,
    size_t bufsize,
    size_t *result) {
  return CHECKED_ENV(env)->getValueStringUtf8(value, buf, bufsize, result);
}

// Copies a JavaScript string into a UTF-16 string buffer. The result is the
// number of 2-byte code units (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in 2-byte
// code units) via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status __cdecl napi_get_value_string_utf16(
    napi_env env,
    napi_value value,
    char16_t *buf,
    size_t bufsize,
    size_t *result) {
  return CHECKED_ENV(env)->getValueStringUtf16(value, buf, bufsize, result);
}

//-----------------------------------------------------------------------------
// Methods to coerce values
// These APIs may execute user scripts
//-----------------------------------------------------------------------------

napi_status __cdecl napi_coerce_to_bool(
    napi_env env,
    napi_value value,
    napi_value *result) {
  return CHECKED_ENV(env)->coerceToBool(value, result);
}

napi_status __cdecl napi_coerce_to_number(
    napi_env env,
    napi_value value,
    napi_value *result) {
  return CHECKED_ENV(env)->coerceToNumber(value, result);
}

napi_status __cdecl napi_coerce_to_object(
    napi_env env,
    napi_value value,
    napi_value *result) {
  return CHECKED_ENV(env)->coerceToObject(value, result);
}

napi_status __cdecl napi_coerce_to_string(
    napi_env env,
    napi_value value,
    napi_value *result) {
  return CHECKED_ENV(env)->coerceToString(value, result);
}

//-----------------------------------------------------------------------------
// Methods to work with Objects
//-----------------------------------------------------------------------------

napi_status __cdecl napi_get_prototype(
    napi_env env,
    napi_value object,
    napi_value *result) {
  return CHECKED_ENV(env)->getPrototype(object, result);
}

napi_status __cdecl napi_get_property_names(
    napi_env env,
    napi_value object,
    napi_value *result) {
  return CHECKED_ENV(env)->getPropertyNames(object, result);
}

napi_status __cdecl napi_set_property(
    napi_env env,
    napi_value object,
    napi_value key,
    napi_value value) {
  return CHECKED_ENV(env)->setProperty(object, key, value);
}

napi_status __cdecl napi_has_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->hasProperty(object, key, result);
}

napi_status __cdecl napi_get_property(
    napi_env env,
    napi_value object,
    napi_value key,
    napi_value *result) {
  return CHECKED_ENV(env)->getProperty(object, key, result);
}

napi_status __cdecl napi_delete_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->deleteProperty(object, key, result);
}

napi_status __cdecl napi_has_own_property(
    napi_env env,
    napi_value object,
    napi_value key,
    bool *result) {
  return CHECKED_ENV(env)->hasOwnProperty(object, key, result);
}

napi_status __cdecl napi_set_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    napi_value value) {
  return CHECKED_ENV(env)->setNamedProperty(object, utf8name, value);
}

napi_status __cdecl napi_has_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    bool *result) {
  return CHECKED_ENV(env)->hasNamedProperty(object, utf8name, result);
}

napi_status __cdecl napi_get_named_property(
    napi_env env,
    napi_value object,
    const char *utf8name,
    napi_value *result) {
  return CHECKED_ENV(env)->getNamedProperty(object, utf8name, result);
}

napi_status __cdecl napi_set_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    napi_value value) {
  return CHECKED_ENV(env)->setElement(object, index, value);
}

napi_status __cdecl napi_has_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    bool *result) {
  return CHECKED_ENV(env)->hasElement(object, index, result);
}

napi_status __cdecl napi_get_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    napi_value *result) {
  return CHECKED_ENV(env)->getElement(object, index, result);
}

napi_status __cdecl napi_delete_element(
    napi_env env,
    napi_value object,
    uint32_t index,
    bool *result) {
  return CHECKED_ENV(env)->deleteElement(object, index, result);
}

napi_status __cdecl napi_define_properties(
    napi_env env,
    napi_value object,
    size_t property_count,
    const napi_property_descriptor *properties) {
  return CHECKED_ENV(env)->defineProperties(object, property_count, properties);
}

//-----------------------------------------------------------------------------
// Methods to work with Arrays
//-----------------------------------------------------------------------------

napi_status __cdecl napi_is_array(
    napi_env env,
    napi_value value,
    bool *result) {
  return CHECKED_ENV(env)->isArray(value, result);
}

napi_status __cdecl napi_get_array_length(
    napi_env env,
    napi_value value,
    uint32_t *result) {
  return CHECKED_ENV(env)->getArrayLength(value, result);
}

//-----------------------------------------------------------------------------
// Methods to compare values
//-----------------------------------------------------------------------------

napi_status __cdecl napi_strict_equals(
    napi_env env,
    napi_value lhs,
    napi_value rhs,
    bool *result) {
  return CHECKED_ENV(env)->strictEquals(lhs, rhs, result);
}

//-----------------------------------------------------------------------------
// Methods to work with Functions
//-----------------------------------------------------------------------------

napi_status __cdecl napi_call_function(
    napi_env env,
    napi_value recv,
    napi_value func,
    size_t argc,
    const napi_value *argv,
    napi_value *result) {
  return CHECKED_ENV(env)->callFunction(recv, func, argc, argv, result);
}

napi_status __cdecl napi_new_instance(
    napi_env env,
    napi_value constructor,
    size_t argc,
    const napi_value *argv,
    napi_value *result) {
  return CHECKED_ENV(env)->newInstance(constructor, argc, argv, result);
}

napi_status __cdecl napi_instanceof(
    napi_env env,
    napi_value object,
    napi_value constructor,
    bool *result) {
  return CHECKED_ENV(env)->instanceOf(object, constructor, result);
}

//-----------------------------------------------------------------------------
// Methods to work with napi_callbacks
//-----------------------------------------------------------------------------

napi_status __cdecl napi_get_cb_info(
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

napi_status __cdecl napi_get_new_target(
    napi_env env,
    napi_callback_info cbinfo,
    napi_value *result) {
  return CHECKED_ENV(env)->getNewTarget(
      reinterpret_cast<hermes::napi::CallbackInfo *>(cbinfo), result);
}

napi_status __cdecl napi_define_class(
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

//-----------------------------------------------------------------------------
// Methods to work with external data objects
//-----------------------------------------------------------------------------

napi_status __cdecl napi_wrap(
    napi_env env,
    napi_value js_object,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) {
  return CHECKED_ENV(env)->wrapObject(
      js_object, native_object, finalize_cb, finalize_hint, result);
}

napi_status __cdecl napi_unwrap(napi_env env, napi_value obj, void **result) {
  return CHECKED_ENV(env)->unwrapObject(
      obj, hermes::napi::UnwrapAction::KeepWrap, result);
}

napi_status __cdecl napi_remove_wrap(
    napi_env env,
    napi_value obj,
    void **result) {
  return CHECKED_ENV(env)->unwrapObject(
      obj, hermes::napi::UnwrapAction::RemoveWrap, result);
}

napi_status __cdecl napi_create_external(
    napi_env env,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  return CHECKED_ENV(env)->createExternal(
      data, finalize_cb, finalize_hint, result);
}

napi_status __cdecl napi_get_value_external(
    napi_env env,
    napi_value value,
    void **result) {
  return CHECKED_ENV(env)->getValueExternal(value, result);
}

//-----------------------------------------------------------------------------
// Methods to control object lifespan
//-----------------------------------------------------------------------------

napi_status __cdecl napi_create_reference(
    napi_env env,
    napi_value value,
    uint32_t initial_refcount,
    napi_ref *result) {
  return CHECKED_ENV(env)->createReference(value, initial_refcount, result);
}

napi_status __cdecl napi_delete_reference(napi_env env, napi_ref ref) {
  return CHECKED_ENV(env)->deleteReference(ref);
}

napi_status __cdecl napi_reference_ref(
    napi_env env,
    napi_ref ref,
    uint32_t *result) {
  return CHECKED_ENV(env)->incReference(ref, result);
}

napi_status __cdecl napi_reference_unref(
    napi_env env,
    napi_ref ref,
    uint32_t *result) {
  return CHECKED_ENV(env)->decReference(ref, result);
}

napi_status __cdecl napi_get_reference_value(
    napi_env env,
    napi_ref ref,
    napi_value *result) {
  return CHECKED_ENV(env)->getReferenceValue(ref, result);
}

napi_status __cdecl napi_open_handle_scope(
    napi_env env,
    napi_handle_scope *result) {
  return CHECKED_ENV(env)->openHandleScope(result);
}

napi_status __cdecl napi_close_handle_scope(
    napi_env env,
    napi_handle_scope scope) {
  return CHECKED_ENV(env)->closeHandleScope(scope);
}

napi_status __cdecl napi_open_escapable_handle_scope(
    napi_env env,
    napi_escapable_handle_scope *result) {
  return CHECKED_ENV(env)->openEscapableHandleScope(result);
}

napi_status __cdecl napi_close_escapable_handle_scope(
    napi_env env,
    napi_escapable_handle_scope scope) {
  return CHECKED_ENV(env)->closeEscapableHandleScope(scope);
}

napi_status __cdecl napi_escape_handle(
    napi_env env,
    napi_escapable_handle_scope scope,
    napi_value escapee,
    napi_value *result) {
  return CHECKED_ENV(env)->escapeHandle(scope, escapee, result);
}

//-----------------------------------------------------------------------------
// Methods to support error handling
//-----------------------------------------------------------------------------

napi_status __cdecl napi_throw(napi_env env, napi_value error) {
  return CHECKED_ENV(env)->throwError(error);
}

napi_status __cdecl napi_throw_error(
    napi_env env,
    const char *code,
    const char *msg) {
  return CHECKED_ENV(env)->throwError(code, msg);
}

napi_status __cdecl napi_throw_type_error(
    napi_env env,
    const char *code,
    const char *msg) {
  return CHECKED_ENV(env)->throwTypeError(code, msg);
}

napi_status __cdecl napi_throw_range_error(
    napi_env env,
    const char *code,
    const char *msg) {
  return CHECKED_ENV(env)->throwRangeError(code, msg);
}

napi_status __cdecl napi_is_error(
    napi_env env,
    napi_value value,
    bool *result) {
  return CHECKED_ENV(env)->isError(value, result);
}

//-----------------------------------------------------------------------------
// Methods to support catching exceptions
//-----------------------------------------------------------------------------

napi_status __cdecl napi_is_exception_pending(napi_env env, bool *result) {
  return CHECKED_ENV(env)->isExceptionPending(result);
}

napi_status __cdecl napi_get_and_clear_last_exception(
    napi_env env,
    napi_value *result) {
  return CHECKED_ENV(env)->getAndClearLastException(result);
}

//-----------------------------------------------------------------------------
// Methods to work with array buffers and typed arrays
//-----------------------------------------------------------------------------

napi_status __cdecl napi_is_arraybuffer(
    napi_env env,
    napi_value value,
    bool *result) {
  return CHECKED_ENV(env)->isArrayBuffer(value, result);
}

napi_status __cdecl napi_create_arraybuffer(
    napi_env env,
    size_t byte_length,
    void **data,
    napi_value *result) {
  return CHECKED_ENV(env)->createArrayBuffer(byte_length, data, result);
}

napi_status __cdecl napi_create_external_arraybuffer(
    napi_env env,
    void *external_data,
    size_t byte_length,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  return CHECKED_ENV(env)->createExternalArrayBuffer(
      external_data, byte_length, finalize_cb, finalize_hint, result);
}

napi_status __cdecl napi_get_arraybuffer_info(
    napi_env env,
    napi_value arraybuffer,
    void **data,
    size_t *byte_length) {
  return CHECKED_ENV(env)->getArrayBufferInfo(arraybuffer, data, byte_length);
}

napi_status __cdecl napi_is_typedarray(
    napi_env env,
    napi_value value,
    bool *result) {
  return CHECKED_ENV(env)->isTypedArray(value, result);
}

napi_status __cdecl napi_create_typedarray(
    napi_env env,
    napi_typedarray_type type,
    size_t length,
    napi_value arraybuffer,
    size_t byte_offset,
    napi_value *result) {
  return CHECKED_ENV(env)->createTypedArray(
      type, length, arraybuffer, byte_offset, result);
}

napi_status __cdecl napi_get_typedarray_info(
    napi_env env,
    napi_value typedarray,
    napi_typedarray_type *type,
    size_t *length,
    void **data,
    napi_value *arraybuffer,
    size_t *byte_offset) {
  return CHECKED_ENV(env)->getTypedArrayInfo(
      typedarray, type, length, data, arraybuffer, byte_offset);
}

napi_status __cdecl napi_create_dataview(
    napi_env env,
    size_t byte_length,
    napi_value arraybuffer,
    size_t byte_offset,
    napi_value *result) {
  return CHECKED_ENV(env)->createDataView(
      byte_length, arraybuffer, byte_offset, result);
}

napi_status __cdecl napi_is_dataview(
    napi_env env,
    napi_value value,
    bool *result) {
  return CHECKED_ENV(env)->isDataView(value, result);
}

napi_status __cdecl napi_get_dataview_info(
    napi_env env,
    napi_value dataview,
    size_t *byte_length,
    void **data,
    napi_value *arraybuffer,
    size_t *byte_offset) {
  return CHECKED_ENV(env)->getDataViewInfo(
      dataview, byte_length, data, arraybuffer, byte_offset);
}

//-----------------------------------------------------------------------------
// Version management
//-----------------------------------------------------------------------------

napi_status __cdecl napi_get_version(napi_env env, uint32_t *result) {
  return CHECKED_ENV(env)->getVersion(result);
}

//-----------------------------------------------------------------------------
// Promises
//-----------------------------------------------------------------------------

napi_status __cdecl napi_create_promise(
    napi_env env,
    napi_deferred *deferred,
    napi_value *promise) {
  return CHECKED_ENV(env)->createPromise(deferred, promise);
}

napi_status __cdecl napi_resolve_deferred(
    napi_env env,
    napi_deferred deferred,
    napi_value resolution) {
  return CHECKED_ENV(env)->resolveDeferred(deferred, resolution);
}

napi_status __cdecl napi_reject_deferred(
    napi_env env,
    napi_deferred deferred,
    napi_value resolution) {
  return CHECKED_ENV(env)->rejectDeferred(deferred, resolution);
}

napi_status __cdecl napi_is_promise(
    napi_env env,
    napi_value value,
    bool *is_promise) {
  return CHECKED_ENV(env)->isPromise(value, is_promise);
}

//-----------------------------------------------------------------------------
// Running a script
//-----------------------------------------------------------------------------

napi_status __cdecl napi_run_script(
    napi_env env,
    napi_value script,
    napi_value *result) {
  return CHECKED_ENV(env)->runScript(script, nullptr, result);
}

//-----------------------------------------------------------------------------
// Memory management
//-----------------------------------------------------------------------------

napi_status __cdecl napi_adjust_external_memory(
    napi_env env,
    int64_t change_in_bytes,
    int64_t *adjusted_value) {
  return CHECKED_ENV(env)->adjustExternalMemory(
      change_in_bytes, adjusted_value);
}

#if NAPI_VERSION >= 5

//-----------------------------------------------------------------------------
// Dates
//-----------------------------------------------------------------------------

napi_status __cdecl napi_create_date(
    napi_env env,
    double time,
    napi_value *result) {
  return CHECKED_ENV(env)->createDate(time, result);
}

napi_status __cdecl napi_is_date(
    napi_env env,
    napi_value value,
    bool *is_date) {
  return CHECKED_ENV(env)->isDate(value, is_date);
}

napi_status __cdecl napi_get_date_value(
    napi_env env,
    napi_value value,
    double *result) {
  return CHECKED_ENV(env)->getDateValue(value, result);
}

//-----------------------------------------------------------------------------
// Add finalizer for pointer
//-----------------------------------------------------------------------------

napi_status __cdecl napi_add_finalizer(
    napi_env env,
    napi_value js_object,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ref *result) {
  return CHECKED_ENV(env)->addFinalizer(
      js_object, native_object, finalize_cb, finalize_hint, result);
}

#endif // NAPI_VERSION >= 5

#if NAPI_VERSION >= 6

//-----------------------------------------------------------------------------
// BigInt
//-----------------------------------------------------------------------------

napi_status __cdecl napi_create_bigint_int64(
    napi_env env,
    int64_t value,
    napi_value *result) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->setLastError(napi_generic_failure);
}

napi_status __cdecl napi_create_bigint_uint64(
    napi_env env,
    uint64_t value,
    napi_value *result) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->setLastError(napi_generic_failure);
}

napi_status __cdecl napi_create_bigint_words(
    napi_env env,
    int sign_bit,
    size_t word_count,
    const uint64_t *words,
    napi_value *result) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->setLastError(napi_generic_failure);
}

napi_status __cdecl napi_get_value_bigint_int64(
    napi_env env,
    napi_value value,
    int64_t *result,
    bool *lossless) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->setLastError(napi_generic_failure);
}

napi_status __cdecl napi_get_value_bigint_uint64(
    napi_env env,
    napi_value value,
    uint64_t *result,
    bool *lossless) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->setLastError(napi_generic_failure);
}

napi_status __cdecl napi_get_value_bigint_words(
    napi_env env,
    napi_value value,
    int *sign_bit,
    size_t *word_count,
    uint64_t *words) {
  // Not implemented in Hermes
  return CHECKED_ENV(env)->setLastError(napi_generic_failure);
}

//-----------------------------------------------------------------------------
// Object
//-----------------------------------------------------------------------------

napi_status __cdecl napi_get_all_property_names(
    napi_env env,
    napi_value object,
    napi_key_collection_mode key_mode,
    napi_key_filter key_filter,
    napi_key_conversion key_conversion,
    napi_value *result) {
  return CHECKED_ENV(env)->getAllPropertyNames(
      object, key_mode, key_filter, key_conversion, result);
}

//-----------------------------------------------------------------------------
// Instance data
//-----------------------------------------------------------------------------

napi_status __cdecl napi_set_instance_data(
    napi_env env,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint) {
  return CHECKED_ENV(env)->setInstanceData(data, finalize_cb, finalize_hint);
}

napi_status __cdecl napi_get_instance_data(napi_env env, void **data) {
  return CHECKED_ENV(env)->getInstanceData(data);
}

#endif // NAPI_VERSION >= 6

#if NAPI_VERSION >= 7

//-----------------------------------------------------------------------------
// ArrayBuffer detaching
//-----------------------------------------------------------------------------

napi_status __cdecl napi_detach_arraybuffer(
    napi_env env,
    napi_value arraybuffer) {
  return CHECKED_ENV(env)->detachArrayBuffer(arraybuffer);
}

napi_status __cdecl napi_is_detached_arraybuffer(
    napi_env env,
    napi_value arraybuffer,
    bool *result) {
  return CHECKED_ENV(env)->isDetachedArrayBuffer(arraybuffer, result);
}

#endif // NAPI_VERSION >= 7

#if NAPI_VERSION >= 8

//-----------------------------------------------------------------------------
// Type tagging
//-----------------------------------------------------------------------------

napi_status __cdecl napi_type_tag_object(
    napi_env env,
    napi_value object,
    const napi_type_tag *type_tag) {
  return CHECKED_ENV(env)->typeTagObject(object, type_tag);
}

napi_status __cdecl napi_check_object_type_tag(
    napi_env env,
    napi_value object,
    const napi_type_tag *type_tag,
    bool *result) {
  return CHECKED_ENV(env)->checkObjectTypeTag(object, type_tag, result);
}

napi_status __cdecl napi_object_freeze(napi_env env, napi_value object) {
  return CHECKED_ENV(env)->objectFreeze(object);
}

napi_status __cdecl napi_object_seal(napi_env env, napi_value object) {
  return CHECKED_ENV(env)->objectSeal(object);
}

#endif // NAPI_VERSION >= 8

//=============================================================================
// Hermes specific API
//=============================================================================

napi_status __cdecl napi_create_hermes_env(napi_env *env) {
  if (!env) {
    return napi_status::napi_invalid_arg;
  }
  *env = reinterpret_cast<napi_env>(new hermes::napi::NodeApiEnvironment());
  return napi_status::napi_ok;
}

//=============================================================================
// Node-API extensions to host JS engine and to implement JSI
//=============================================================================

napi_status __cdecl napi_ext_create_env(
    napi_ext_env_settings *settings,
    napi_env *env) {
  return napi_create_hermes_env(env);
}

napi_status __cdecl napi_ext_env_ref(napi_env env) {
  return CHECKED_ENV(env)->incRefCount();
}

napi_status __cdecl napi_ext_env_unref(napi_env env) {
  return CHECKED_ENV(env)->decRefCount();
}

napi_status __cdecl napi_ext_open_env_scope(
    napi_env env,
    napi_ext_env_scope *result) {
  // TODO: implement
  return napi_generic_failure;
}

napi_status __cdecl napi_ext_close_env_scope(
    napi_env env,
    napi_ext_env_scope scope) {
  // TODO: implement
  return napi_generic_failure;
}

napi_status __cdecl napi_ext_collect_garbage(napi_env env) {
  return CHECKED_ENV(env)->collectGarbage();
}

napi_status __cdecl napi_ext_has_unhandled_promise_rejection(
    napi_env env,
    bool *result) {
  // TODO: implement
  return napi_generic_failure;
}

napi_status __cdecl napi_get_and_clear_last_unhandled_promise_rejection(
    napi_env env,
    napi_value *result) {
  // TODO: implement
  return napi_generic_failure;
}

napi_status __cdecl napi_ext_get_unique_string_utf8_ref(
    napi_env env,
    const char *str,
    size_t length,
    napi_ext_ref *result) {
  // TODO: implement
  return napi_generic_failure;
}

napi_status __cdecl napi_ext_get_unique_string_ref(
    napi_env env,
    napi_value str_value,
    napi_ext_ref *result) {
  // TODO: implement
  return napi_generic_failure;
}

//-----------------------------------------------------------------------------
// Methods to control object lifespan.
// The NAPI's napi_ref can be used only for objects.
// The napi_ext_ref can be used for any value type.
//-----------------------------------------------------------------------------

napi_status __cdecl napi_ext_create_reference(
    napi_env env,
    napi_value value,
    napi_ext_ref *result) {
  return CHECKED_ENV(env)->createStrongReference(value, result);
}

napi_status __cdecl napi_ext_create_reference_with_data(
    napi_env env,
    napi_value value,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ext_ref *result) {
  return CHECKED_ENV(env)->createStrongReferenceWithData(
      value, native_object, finalize_cb, finalize_hint, result);
}

napi_status __cdecl napi_ext_create_weak_reference(
    napi_env env,
    napi_value value,
    napi_ext_ref *result) {
  return CHECKED_ENV(env)->createWeakReference(value, result);
}

napi_status __cdecl napi_ext_reference_ref(napi_env env, napi_ext_ref ref) {
  return CHECKED_ENV(env)->incReference(ref);
}

napi_status __cdecl napi_ext_reference_unref(napi_env env, napi_ext_ref ref) {
  return CHECKED_ENV(env)->decReference(ref);
}

napi_status __cdecl napi_ext_get_reference_value(
    napi_env env,
    napi_ext_ref ref,
    napi_value *result) {
  return CHECKED_ENV(env)->getReferenceValue(ref, result);
}

//-----------------------------------------------------------------------------
// Script running, preparing, and serialization.
//
// Script is usually converted to byte code, or in other words - prepared - for
// execution. The APIs below allow not only running the script, but also control
// its preparation phase where we can explicitly prepare the script for running,
// run the prepared script, and serialize or deserialize the prepared script.
//-----------------------------------------------------------------------------

napi_status __cdecl napi_ext_run_script(
    napi_env env,
    napi_value source,
    const char *source_url,
    napi_value *result) {
  return CHECKED_ENV(env)->runScript(source, source_url, result);
}

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

napi_status __cdecl napi_ext_serialize_script(
    napi_env env,
    napi_value source,
    const char *source_url,
    napi_ext_buffer_callback buffer_cb,
    void *buffer_hint) {
  return CHECKED_ENV(env)->serializeScript(
      source, source_url, buffer_cb, buffer_hint);
}

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

napi_status __cdecl napi_ext_run_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script,
    napi_value *result) {
  return CHECKED_ENV(env)->runPreparedScript(prepared_script, result);
}

napi_status __cdecl napi_ext_delete_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script) {
  return CHECKED_ENV(env)->deletePreparedScript(prepared_script);
}

napi_status __cdecl napi_ext_serialize_prepared_script(
    napi_env env,
    napi_ext_prepared_script prepared_script,
    napi_ext_buffer_callback buffer_cb,
    void *buffer_hint) {
  return CHECKED_ENV(env)->serializePreparedScript(
      prepared_script, buffer_cb, buffer_hint);
}
