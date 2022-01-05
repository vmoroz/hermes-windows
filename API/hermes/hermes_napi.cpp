/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * Copyright notices for portions of code adapted from Hermes, Node.js, and V8
 * projects:
 *
 * Copyright (c) Facebook, Inc. and its affiliates.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Copyright Node.js contributors. All rights reserved.
 * https://github.com/nodejs/node/blob/master/LICENSE
 *
 * Copyright 2011 the V8 project authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 * https://github.com/v8/v8/blob/main/LICENSE
 */

//
// Implementation of Node-API for Hermes engine.
//
// The Node-API C functions are redirecting all calls to the NapiEnvironment
// class which implements the API details.
// The most notable parts of the implementation are:
// - The NapiEnvironment class is ref-counted.
// - It maintains local stack-based GC roots as gcRootStack_.
//   - The gcRootStackScopes_ is used to control gcRootStack_ handle scopes.
//   - The gcRootStack_ and gcRootStackScopes_ are instances of
//     StableAddressStack to maintain stable address of returned napi_value and
//     handle scopes.
//   - napi_value is a pointer to the vm::PinnedHermesValue stored in
//     gcRootStack_.
// - The heap-based GC roots are in the gcRoots_ and finalizingGCRoots_.
//   - gcRoots_ vs finalizingGCRoots_ is chosen based on whether the root needs
//     finalizer call or not.
//   - gcRoots_ and finalizingGCRoots_ are double-linked list.
//   - All heap-based GC roots are stored as references - instances of classes
//     derived from Reference class. There are many varieties of that class to
//     accommodate different lifetime strategies and to optimize storage size.
//   - napi_ref and napi_ext_ref are pointers to gcRoots_ and finalizingGCRoots_
//     items.
//   - Reference finalizers are run in JS thread by runReferenceFinalizers
//     method which is called by setResultAndRunFinalizers from methods that may
//     cause memory garbage collection.
// - Each returned error status is backed up by the extended error message
//   stored in lastError_ that can be retrived by napi_get_last_error_info.
// - We use macros to handle error statuses. It is done to reduce extensive use
//   of "if-return" statements, and to report failing expressions along with the
//   file name and code line number.

// TODO: Implement unit tests

// TODO: Better native error handling
// TODO: use extended message for errors

// TODO: adjustExternalMemory
// TODO: see if finalizers can return error or exception

// TODO: use modern C++ initialization syntax.

#define NAPI_EXPERIMENTAL

#include "napi/hermes_napi.h"

#include "hermes/BCGen/HBC/BytecodeProviderFromSrc.h"
#include "hermes/DebuggerAPI.h"
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
#include "hermes/VM/PropertyAccessor.h"
#include "hermes/VM/Runtime.h"
#include "llvh/ADT/SmallVector.h"
#include "llvh/Support/ConvertUTF.h"

#include <algorithm>
#include <atomic>

//=============================================================================
// Macros
//=============================================================================

// Check the NAPI status and return it if it is not napi_ok.
#define CHECK_NAPI(...)                       \
  do {                                        \
    if (napi_status status = (__VA_ARGS__)) { \
      return status;                          \
    }                                         \
  } while (false)

// Crash if the condition is false.
#define CRASH_IF_FALSE(condition)  \
  do {                             \
    if (!(condition)) {            \
      assert(false && #condition); \
      *((int *)nullptr) = 1;       \
      std::terminate();            \
    }                              \
  } while (false)

// Return error status with message.
#define ERROR_STATUS(status, ...) \
  env.setLastError((status), (__FILE__), (uint32_t)(__LINE__), __VA_ARGS__)

// Return napi_generic_failure with message.
#define GENERIC_FAILURE(...) ERROR_STATUS(napi_generic_failure, __VA_ARGS__)

// Cast env to NapiEnvironment if it is not null.
#define CHECKED_ENV(env)                \
  ((env) == nullptr) ? napi_invalid_arg \
                     : reinterpret_cast<hermes::napi::NapiEnvironment *>(env)

// Check env and return error status with message.
#define CHECKED_ENV_ERROR_STATUS(env, status, ...)                            \
  ((env) == nullptr)                                                          \
      ? napi_invalid_arg                                                      \
      : reinterpret_cast<hermes::napi::NapiEnvironment *>(env)->setLastError( \
            (status), (__FILE__), (uint32_t)(__LINE__), __VA_ARGS__)

// Check env and return napi_generic_failure with message.
#define CHECKED_ENV_GENERIC_FAILURE(env, ...) \
  CHECKED_ENV_ERROR_STATUS(env, napi_generic_failure, __VA_ARGS__)

// Check conditions and return error status with message if it is false.
#define RETURN_STATUS_IF_FALSE_WITH_MESSAGE(condition, status, ...) \
  do {                                                              \
    if (!(condition)) {                                             \
      return env.setLastError(                                      \
          (status), (__FILE__), (uint32_t)(__LINE__), __VA_ARGS__); \
    }                                                               \
  } while (false)

// Check conditions and return error status if it is false.
#define RETURN_STATUS_IF_FALSE(condition, status) \
  RETURN_STATUS_IF_FALSE_WITH_MESSAGE(            \
      (condition), (status), "Condition is false: " #condition)

// Check conditions and return napi_generic_failure if it is false.
#define RETURN_FAILURE_IF_FALSE(condition) \
  RETURN_STATUS_IF_FALSE_WITH_MESSAGE(     \
      (condition), napi_generic_failure, "Condition is false: " #condition)

// Check that the argument is not nullptr.
#define CHECK_ARG(arg)                 \
  RETURN_STATUS_IF_FALSE_WITH_MESSAGE( \
      (arg) != nullptr, napi_invalid_arg, "Argument is null: " #arg)

// Check that the argument is of Object or Function type.
#define CHECK_OBJECT_ARG(arg)                \
  do {                                       \
    CHECK_ARG(arg);                          \
    RETURN_STATUS_IF_FALSE_WITH_MESSAGE(     \
        phv(arg)->isObject(),                \
        napi_object_expected,                \
        "Argument is not an Object: " #arg); \
  } while (false)

// Check that the argument is of String type.
#define CHECK_STRING_ARG(arg)               \
  do {                                      \
    CHECK_ARG(arg);                         \
    RETURN_STATUS_IF_FALSE_WITH_MESSAGE(    \
        phv(arg)->isString(),               \
        napi_string_expected,               \
        "Argument is not a String: " #arg); \
  } while (false)

namespace hermes {
namespace napi {
namespace {

//=============================================================================
// Forward declaration of all classes.
//=============================================================================

class CallbackInfo;
class DoubleConversion;
class EscapableHandleScope;
class ExternalBuffer;
class ExternalValue;
class HandleScope;
class HermesPreparedJavaScript;
class HostFunctionContext;
class NapiEnvironment;
template <class T>
class OrderedSet;
template <class T>
class StableAddressStack;
class StringBuilder;

// Forward declaration of Reference-related classes.
class AtomicRefCountReference;
class ComplexReference;
template <class TBaseReference>
class FinalizeCallbackHolder;
template <class TBaseReference>
class FinalizeHintHolder;
class Finalizer;
class FinalizingAnonymousReference;
class FinalizingComplexReference;
template <class TBaseReference>
class FinalizingReference;
template <class TReference>
class FinalizingReferenceFactory;
class FinalizingStrongReference;
class InstanceData;
template <class T>
class LinkedList;
template <class TBaseReference>
class NativeDataHolder;
class Reference;
class StrongReference;
class WeakReference;

//=============================================================================
// Enums
//=============================================================================

// Controls behavior of NapiEnvironment::unwrapObject.
enum class UnwrapAction { KeepWrap, RemoveWrap };

// Predefined values used by NapiEnvironment.
enum class NapiPredefined {
  Promise,
  code,
  napi_externalValue,
  napi_typeTag,
  reject,
  resolve,
  undefined,
  PredefinedCount // a special value that must be last in the enum
};

// The action to take when an external value is not found.
enum class IfNotFound {
  ThenCreate,
  ThenReturnNull,
};

//=============================================================================
// Forward declaration of standalone functions.
//=============================================================================

// Size of an array - it must be replaced by std::size after switching to C++17
template <class T, std::size_t N>
constexpr std::size_t size(const T (&array)[N]) noexcept;

// Check if the enum value is in the provided range.
template <class TEnum>
bool isInEnumRange(
    TEnum value,
    TEnum lowerBoundInclusive,
    TEnum upperBoundInclusive) noexcept;

// Reinterpret cast NapiEnvironment to napi_env
napi_env napiEnv(NapiEnvironment *env) noexcept;

// Reinterpret cast vm::PinnedHermesValue pointer to napi_value
napi_value napiValue(const vm::PinnedHermesValue *value) noexcept;

// Reinterpret cast napi_value to vm::PinnedHermesValue pointer
const vm::PinnedHermesValue *phv(napi_value value) noexcept;
// Useful in templates and macros
const vm::PinnedHermesValue *phv(const vm::PinnedHermesValue *value) noexcept;

// Reinterpret cast napi_ext_ref to Reference pointer
Reference *asReference(napi_ext_ref ref) noexcept;
// Reinterpret cast napi_ref to Reference pointer
Reference *asReference(napi_ref ref) noexcept;
// Reinterpret cast void* to Reference pointer
Reference *asReference(void *ref) noexcept;

// Reinterpret cast to HostFunctionContext::CallbackInfo
CallbackInfo *asCallbackInfo(napi_callback_info callbackInfo) noexcept;

// Get object from HermesValue and cast it to JSObject
vm::JSObject *getObjectUnsafe(const vm::HermesValue &value) noexcept;

// Copy ASCII input to UTF8 buffer. It is a convenience function to match the
// convertUTF16ToUTF8WithReplacements signature when using std::copy.
size_t copyASCIIToUTF8(
    llvh::ArrayRef<char> input,
    char *buf,
    size_t maxCharacters) noexcept;

//=============================================================================
// Definitions of classes and structs.
//=============================================================================

// Stack of elements where the address of items is not changed as we add new
// values. It is achieved by keeping a SmallVector of the ChunkSize arrays
// called chunks. We use it to keep addresses of GC roots associated with the
// call stack and the related handle scopes. The GC roots are the
// vm::PinnedHermesValue instances. Considering our use case, we do not call the
// destructors for items and require that T has a trivial destructor.
template <class T>
class StableAddressStack final {
  static_assert(
      std::is_trivially_destructible_v<T>,
      "T must be trivially destructible.");

 public:
  StableAddressStack() noexcept {
    // There is always at least one chunk in the storage
    storage_.emplace_back(new T[ChunkSize]);
  }

  template <class... TArgs>
  void emplace(TArgs &&...args) noexcept {
    size_t newIndex = size_;
    size_t chunkIndex = newIndex / ChunkSize;
    size_t chunkOffset = newIndex % ChunkSize;
    if (chunkOffset == 0 && chunkIndex == storage_.size()) {
      storage_.emplace_back(new T[ChunkSize]);
    }
    new (std::addressof(storage_[chunkIndex][chunkOffset]))
        T(std::forward<TArgs>(args)...);
    ++size_;
  }

  void pop() noexcept {
    CRASH_IF_FALSE(size_ > 0 && "Size must be non zero.");
    --size_;
    reduceChunkCount();
  }

  void resize(size_t newSize) noexcept {
    CRASH_IF_FALSE(newSize <= size_ && "Size cannot be increased by resizing.");
    size_ = newSize;
    reduceChunkCount();
  }

  size_t size() const noexcept {
    return size_;
  }

  bool empty() const noexcept {
    return size_ == 0;
  }

  T &top() noexcept {
    CRASH_IF_FALSE(size_ > 0 && "Size must be non zero.");
    size_t lastIndex = size_ - 1;
    return storage_[lastIndex / ChunkSize][lastIndex % ChunkSize];
  }

  T &operator[](size_t index) noexcept {
    CRASH_IF_FALSE(index < size_ && "Index must be less than size.");
    return storage_[index / ChunkSize][index % ChunkSize];
  }

  template <class F>
  void forEach(const F &f) noexcept {
    size_t remaining = size_;
    for (std::unique_ptr<T[]> &chunk : storage_) {
      size_t chunkSize = std::min(ChunkSize, remaining);
      for (size_t i = 0; i < chunkSize; ++i) {
        f(chunk[i]);
      }
      remaining -= chunkSize;
    }
  }

 private:
  void reduceChunkCount() noexcept {
    // There must be at least one chunk.
    // To reduce number of allocations/deallocations the last chunk must be half
    // full before we delete the next empty chunk.
    size_t requiredChunkCount = std::max<size_t>(
        1, (size_ + ChunkSize / 2 + ChunkSize - 1) / ChunkSize);
    if (requiredChunkCount < storage_.size()) {
      storage_.resize(requiredChunkCount);
    }
  }

 private:
  static const size_t ChunkSize = 64;

  llvh::SmallVector<std::unique_ptr<T[]>, ChunkSize> storage_;
  size_t size_{0};
};

// An intrusive double linked list of items.
// Items in the list must inherit from LinkedList<T>::Item.
// We use it instead of std::list to allow item to delete itself in its
// destructor and conveniently move items from list to another. The LinkedList
// is used for References - the GC roots that are allocated in heap. The GC
// roots are the vm::PinnedHermesValue instances.
template <class T>
class LinkedList final {
 public:
  LinkedList() noexcept {
    // The list is circular:
    // head.next_ points to the first item
    // head.prev_ points to the last item
    head_.next_ = &head_;
    head_.prev_ = &head_;
  }

  class Item {
   public:
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

  template <class TLambda>
  void forEach(TLambda lambda) noexcept {
    for (T *item = begin(); item != end();) {
      // lambda can delete the item - get the next one before calling it.
      T *nextItem = static_cast<T *>(item->next_);
      lambda(item);
      item = nextItem;
    }
  }

 private:
  Item head_;
};

// The main class representing the NAPI environment.
// All NAPI functions are calling methods from this class.
class NapiEnvironment final {
 public:
  explicit NapiEnvironment(
      const vm::RuntimeConfig &runtimeConfig = {}) noexcept;

  napi_status incRefCount() noexcept;
  napi_status decRefCount() noexcept;

  vm::Runtime &runtime() noexcept;

 private:
  // Only the internal ref count can call the destructor
  ~NapiEnvironment();

  //---------------------------------------------------------------------------
  // Native error handling methods
  //---------------------------------------------------------------------------
 public:
  template <class... TArgs>
  napi_status setLastError(
      napi_status status,
      const char *fileName,
      uint32_t line,
      TArgs &&...args) noexcept;
  napi_status clearLastError() noexcept;
  napi_status getLastErrorInfo(
      const napi_extended_error_info **result) noexcept;

  napi_status checkHermesStatus(
      vm::ExecutionStatus hermesStatus,
      napi_status status = napi_generic_failure) noexcept;

  template <class T>
  napi_status checkHermesStatus(
      const vm::CallResult<T> &callResult,
      napi_status status = napi_generic_failure) noexcept;

  template <class F>
  napi_status handleExceptions(const F &f) noexcept;

  napi_status checkPendingExceptions() noexcept;

  //-----------------------------------------------------------------------------
  // Getters for defined singletons
  //-----------------------------------------------------------------------------
 public:
  napi_status getUndefined(napi_value *result) noexcept;
  napi_status getNull(napi_value *result) noexcept;
  napi_status getGlobal(napi_value *result) noexcept;
  napi_status getBoolean(bool value, napi_value *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to create Primitive types and Objects
  //-----------------------------------------------------------------------------
 public:
  napi_status createObject(napi_value *result) noexcept;
  napi_status createArray(napi_value *result) noexcept;
  napi_status createArray(size_t length, napi_value *result) noexcept;
  template <class T, std::enable_if_t<std::is_arithmetic_v<T>, bool> = true>
  napi_status createNumber(T value, napi_value *result) noexcept;
  napi_status createStringASCII(
      const char *str,
      size_t length,
      napi_value *result) noexcept;
  napi_status createStringLatin1(
      const char *str,
      size_t length,
      napi_value *result) noexcept;
  napi_status
  createStringUTF8(const char *str, size_t length, napi_value *result) noexcept;
  napi_status createStringUTF8(const char *str, napi_value *result) noexcept;
  napi_status convertUTF8ToUTF16(
      const char *utf8,
      size_t length,
      std::u16string &out) noexcept;
  napi_status createStringUTF16(
      const char16_t *str,
      size_t length,
      napi_value *result) noexcept;

  napi_status getUniqueStringRef(
      const char *utf8,
      size_t length,
      napi_ext_ref *result) noexcept;
  napi_status getUniqueStringRef(
      napi_value strValue,
      napi_ext_ref *result) noexcept;

  napi_status createSymbolID(
      const char *utf8,
      size_t length,
      vm::MutableHandle<vm::SymbolID> *result) noexcept;
  napi_status createSymbolID(
      napi_value strValue,
      vm::MutableHandle<vm::SymbolID> *result) noexcept;

  napi_status createSymbol(napi_value description, napi_value *result) noexcept;

  napi_status createFunction(
      const char *utf8Name,
      size_t length,
      napi_callback callback,
      void *callbackData,
      napi_value *result) noexcept;
  napi_status newFunction(
      vm::SymbolID name,
      napi_callback callback,
      void *callbackData,
      napi_value *result) noexcept;

  napi_status createError(
      const vm::PinnedHermesValue &errorPrototype,
      napi_value code,
      napi_value message,
      napi_value *result) noexcept;
  napi_status
  createError(napi_value code, napi_value message, napi_value *result) noexcept;
  napi_status createTypeError(
      napi_value code,
      napi_value message,
      napi_value *result) noexcept;
  napi_status createRangeError(
      napi_value code,
      napi_value message,
      napi_value *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to get the native napi_value from Primitive type
  //-----------------------------------------------------------------------------
 public:
  napi_status typeOf(napi_value value, napi_valuetype *result) noexcept;
  napi_status getNumberValue(napi_value value, double *result) noexcept;
  napi_status getNumberValue(napi_value value, int32_t *result) noexcept;
  napi_status getNumberValue(napi_value value, uint32_t *result) noexcept;
  napi_status getNumberValue(napi_value value, int64_t *result) noexcept;
  napi_status getBoolValue(napi_value value, bool *result) noexcept;
  napi_status getValueStringLatin1(
      napi_value value,
      char *buf,
      size_t bufSize,
      size_t *result) noexcept;
  napi_status getValueStringUTF8(
      napi_value value,
      char *buf,
      size_t bufSize,
      size_t *result) noexcept;
  napi_status getValueStringUTF16(
      napi_value value,
      char16_t *buf,
      size_t bufSize,
      size_t *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to coerce values
  //-----------------------------------------------------------------------------
 public:
  napi_status coerceToBool(napi_value value, napi_value *result) noexcept;
  napi_status coerceToNumber(napi_value value, napi_value *result) noexcept;
  napi_status coerceToObject(napi_value value, napi_value *result) noexcept;
  napi_status coerceToString(napi_value value, napi_value *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to work with Objects
  //-----------------------------------------------------------------------------
 public:
  napi_status getPrototype(napi_value object, napi_value *result) noexcept;
  napi_status getPropertyNames(napi_value object, napi_value *result) noexcept;
  napi_status getAllPropertyNames(
      napi_value object,
      napi_key_collection_mode keyMode,
      napi_key_filter keyFilter,
      napi_key_conversion keyConversion,
      napi_value *result) noexcept;
  napi_status getForInPropertyNames(
      napi_value object,
      napi_key_conversion keyConversion,
      napi_value *result) noexcept;
  napi_status convertKeyStorageToArray(
      vm::Handle<vm::BigStorage> keyStorage,
      uint32_t startIndex,
      uint32_t length,
      napi_key_conversion keyConversion,
      napi_value *result) noexcept;
  napi_status convertToStringKeys(vm::Handle<vm::JSArray> array) noexcept;
  napi_status convertIndexToString(
      double value,
      vm::MutableHandle<> *result) noexcept;
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
  napi_status symbolIDFromPropertyDescriptor(
      const napi_property_descriptor *p,
      vm::MutableHandle<vm::SymbolID> *result) noexcept;
  napi_status objectFreeze(napi_value object) noexcept;
  napi_status objectSeal(napi_value object) noexcept;

  //---------------------------------------------------------------------------
  // Property access helpers
  //---------------------------------------------------------------------------
 public:
  const vm::PinnedHermesValue &getPredefined(
      NapiPredefined predefinedKey) noexcept;
  template <class TObject, class TValue>
  napi_status putPredefined(
      TObject object,
      NapiPredefined key,
      TValue value,
      bool *optResult = nullptr) noexcept;
  template <class TObject>
  napi_status
  hasPredefined(TObject object, NapiPredefined key, bool *result) noexcept;
  template <class TObject>
  napi_status getPredefined(
      TObject object,
      NapiPredefined key,
      napi_value *result) noexcept;

  template <class TObject, class TValue>
  napi_status putNamed(
      TObject object,
      vm::SymbolID key,
      TValue value,
      bool *optResult = nullptr) noexcept;
  template <class TObject>
  napi_status hasNamed(TObject object, vm::SymbolID key, bool *result) noexcept;
  template <class TObject>
  napi_status
  getNamed(TObject object, vm::SymbolID key, napi_value *result) noexcept;

  template <class TObject, class TKey, class TValue>
  napi_status putComputed(
      TObject object,
      TKey key,
      TValue value,
      bool *optResult = nullptr) noexcept;
  template <class TObject, class TKey>
  napi_status hasComputed(TObject object, TKey key, bool *result) noexcept;
  template <class TObject, class TKey>
  napi_status
  getComputed(TObject object, TKey key, napi_value *result) noexcept;
  template <class TObject, class TKey>
  napi_status
  deleteComputed(TObject object, TKey key, bool *optResult = nullptr) noexcept;

  template <class TObject, class TKey>
  napi_status getOwnComputedDescriptor(
      TObject object,
      TKey key,
      vm::MutableHandle<vm::SymbolID> &tmpSymbolStorage,
      vm::ComputedPropertyDescriptor &desc,
      bool *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to work with Arrays
  //-----------------------------------------------------------------------------
 public:
  napi_status isArray(napi_value value, bool *result) noexcept;
  napi_status getArrayLength(napi_value value, uint32_t *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to compare values
  //-----------------------------------------------------------------------------
 public:
  napi_status
  strictEquals(napi_value lhs, napi_value rhs, bool *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to work with Functions
  //-----------------------------------------------------------------------------
 public:
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

  //-----------------------------------------------------------------------------
  // Methods to work with napi_callbacks
  //-----------------------------------------------------------------------------
 public:
  napi_status getCallbackInfo(
      napi_callback_info callbackInfo,
      size_t *argCount,
      napi_value *args,
      napi_value *thisArg,
      void **data) noexcept;
  napi_status getNewTarget(
      napi_callback_info callbackInfo,
      napi_value *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to work with external data objects
  //-----------------------------------------------------------------------------
 public:
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
  napi_status addFinalizer(
      napi_value object,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_ref *result) noexcept;
  napi_status
  unwrapObject(napi_value object, UnwrapAction action, void **result) noexcept;
  napi_status typeTagObject(
      napi_value object,
      const napi_type_tag *typeTag) noexcept;
  napi_status checkObjectTypeTag(
      napi_value object,
      const napi_type_tag *typeTag,
      bool *result) noexcept;
  napi_status createExternal(
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      napi_value *result) noexcept;
  vm::PseudoHandle<vm::DecoratedObject> createExternal(
      void *nativeData,
      ExternalValue **externalValue) noexcept;
  napi_status getValueExternal(napi_value value, void **result) noexcept;
  ExternalValue *getExternalValue(const vm::HermesValue &value) noexcept;
  template <class TObject>
  napi_status getExternalValue(
      TObject object,
      IfNotFound ifNotFound,
      ExternalValue **result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to control object lifespan
  //-----------------------------------------------------------------------------
 public:
  napi_status createReference(
      napi_value value,
      uint32_t initialRefCount,
      napi_ref *result) noexcept;
  napi_status deleteReference(napi_ref ref) noexcept;
  napi_status incReference(napi_ref ref, uint32_t *result) noexcept;
  napi_status decReference(napi_ref ref, uint32_t *result) noexcept;
  napi_status getReferenceValue(napi_ref ref, napi_value *result) noexcept;

  napi_status addObjectFinalizer(
      const vm::PinnedHermesValue *value,
      Finalizer *finalizer) noexcept;
  template <class TLambda>
  void callIntoModule(TLambda &&call) noexcept;
  napi_status callFinalizer(
      napi_finalize finalizeCallback,
      void *nativeData,
      void *finalizeHint) noexcept;

  napi_status runReferenceFinalizers() noexcept;

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

  void addToFinalizerQueue(Finalizer *finalizer) noexcept;
  void addGCRoot(Reference *reference) noexcept;
  void addFinalizingGCRoot(Reference *reference) noexcept;

  void pushOrderedSet(OrderedSet<vm::HermesValue> &set) noexcept;
  void popOrderedSet() noexcept;

  napi_value addGCRootStackValue(vm::HermesValue value) noexcept;

  vm::WeakRoot<vm::JSObject> createWeakRoot(vm::JSObject *object) noexcept;
  const vm::PinnedHermesValue &lockWeakObject(
      vm::WeakRoot<vm::JSObject> &weakRoot) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to support JS error handling
  //-----------------------------------------------------------------------------
 public:
  napi_status throwError(napi_value error) noexcept;
  napi_status throwError(
      const vm::PinnedHermesValue &prototype,
      const char *code,
      const char *message) noexcept;
  napi_status throwError(const char *code, const char *message) noexcept;
  napi_status throwTypeError(const char *code, const char *message) noexcept;
  napi_status throwRangeError(const char *code, const char *message) noexcept;
  napi_status isError(napi_value value, bool *result) noexcept;
  napi_status setErrorCode(
      vm::Handle<vm::JSError> error,
      napi_value code,
      const char *codeCString) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to support catching JS exceptions
  //-----------------------------------------------------------------------------
 public:
  napi_status isExceptionPending(bool *result) noexcept;
  napi_status getAndClearLastException(napi_value *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to work with array buffers and typed arrays
  //-----------------------------------------------------------------------------
 public:
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
  napi_status detachArrayBuffer(napi_value arrayBuffer) noexcept;
  napi_status isDetachedArrayBuffer(
      napi_value arrayBuffer,
      bool *result) noexcept;
  napi_status isTypedArray(napi_value value, bool *result) noexcept;
  template <vm::CellKind CellKind>
  static constexpr const char *getTypedArrayName() noexcept;
  template <class TItem, vm::CellKind CellKind>
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

  //-----------------------------------------------------------------------------
  // Version management
  //-----------------------------------------------------------------------------
 public:
  napi_status getVersion(uint32_t *result) noexcept;

  //-----------------------------------------------------------------------------
  // Methods to work with Promises
  //-----------------------------------------------------------------------------
 public:
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
      NapiPredefined predefinedProperty,
      napi_value result) noexcept;
  napi_status isPromise(napi_value value, bool *result) noexcept;

  //-----------------------------------------------------------------------------
  // Memory management
  //-----------------------------------------------------------------------------
 public:
  napi_status adjustExternalMemory(
      int64_t change_in_bytes,
      int64_t *adjusted_value) noexcept;
  napi_status collectGarbage() noexcept;

  //-----------------------------------------------------------------------------
  // Methods to work with Dates
  //-----------------------------------------------------------------------------
 public:
  napi_status createDate(double dateTime, napi_value *result) noexcept;
  napi_status isDate(napi_value value, bool *result) noexcept;
  napi_status getDateValue(napi_value value, double *result) noexcept;

  //-----------------------------------------------------------------------------
  // Instance data
  //-----------------------------------------------------------------------------
 public:
  napi_status setInstanceData(
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint) noexcept;
  napi_status getInstanceData(void **nativeData) noexcept;

  //---------------------------------------------------------------------------
  // Script running
  //---------------------------------------------------------------------------
 public:
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
      std::unique_ptr<hermes::Buffer> script,
      std::unique_ptr<hermes::Buffer> sourceMap,
      const char *sourceURL,
      napi_value *result) noexcept;
  napi_status prepareScriptWithSourceMap(
      std::unique_ptr<hermes::Buffer> script,
      std::unique_ptr<hermes::Buffer> sourceMap,
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
  static bool isHermesBytecode(const uint8_t *data, size_t len) noexcept;

  //---------------------------------------------------------------------------
  // Handle creation helpers
  //
  // vm::Handle is a GC root kept on the stack.
  // The vm::Handle<> is a shortcut for vm::Handle<vm::HermesValue>.
  //---------------------------------------------------------------------------
 public:
  vm::Handle<> makeHandle(napi_value value) noexcept;
  vm::Handle<> makeHandle(const vm::PinnedHermesValue *value) noexcept;
  vm::Handle<> makeHandle(vm::HermesValue value) noexcept;
  vm::Handle<> makeHandle(vm::Handle<> value) noexcept;
  vm::Handle<> makeHandle(uint32_t value) noexcept;
  template <class T>
  vm::Handle<T> makeHandle(napi_value value) noexcept;
  template <class T>
  vm::Handle<T> makeHandle(const vm::PinnedHermesValue *value) noexcept;
  template <class T>
  vm::Handle<T> makeHandle(vm::Handle<T> value) noexcept;
  template <class T>
  vm::Handle<T> makeHandle(vm::PseudoHandle<T> &&value) noexcept;
  template <class T>
  vm::CallResult<vm::Handle<T>> makeHandle(
      vm::CallResult<vm::PseudoHandle<T>> &&callResult) noexcept;
  template <class T>
  vm::CallResult<vm::MutableHandle<T>> makeMutableHandle(
      vm::CallResult<vm::PseudoHandle<T>> &&callResult) noexcept;

  //---------------------------------------------------------------------------
  // Result setting helpers
  //---------------------------------------------------------------------------
 public:
  template <class T, class TResult>
  napi_status setResult(T &&value, TResult *result) noexcept;
  template <class T, class TResult>
  napi_status setOptionalResult(T &&value, TResult *result) noexcept;
  template <class T>
  napi_status setOptionalResult(T &&value, std::nullptr_t) noexcept;
  napi_status setPredefinedResult(
      const vm::PinnedHermesValue *value,
      napi_value *result) noexcept;
  template <class T, class TResult>
  napi_status setResultAndRunFinalizers(T &&value, TResult *result) noexcept;
  template <class T>
  napi_status setResultUnsafe(T &&value, T *result) noexcept;
  napi_status setResultUnsafe(
      vm::HermesValue value,
      napi_value *result) noexcept;
  napi_status setResultUnsafe(vm::SymbolID value, napi_value *result) noexcept;
  napi_status setResultUnsafe(bool value, napi_value *result) noexcept;
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
  napi_status setResultUnsafe(
      vm::HermesValue value,
      vm::MutableHandle<> *result) noexcept;
  template <class T, class TResult>
  napi_status setResultUnsafe(
      vm::CallResult<T> &&value,
      TResult *result) noexcept;
  template <class T, class TResult>
  napi_status setResultUnsafe(
      vm::CallResult<T> &&,
      napi_status onException,
      TResult *result) noexcept;

 private:
  std::atomic<int> refCount_{1};

  std::shared_ptr<vm::Runtime> rt_;
  vm::Runtime &runtime_;

  // Convenience field used in macros
  NapiEnvironment &env{*this};

  vm::experiments::VMExperimentFlags vmExperimentFlags_{0};

  // Compilation flags used by prepareJavaScript().
  hbc::CompileFlags compileFlags_{};
  // The default setting of "emit async break check" in this runtime.
  bool defaultEmitAsyncBreakCheck_{false};

  std::array<
      vm::PinnedHermesValue,
      static_cast<size_t>(NapiPredefined::PredefinedCount)>
      predefinedValues_{};

  StableAddressStack<vm::PinnedHermesValue> gcRootStack_;
  StableAddressStack<size_t> gcRootStackScopes_;

  // We store references in two different lists, depending on whether they
  // have `napi_finalizer` callbacks, because we must first finalize the
  // ones that have such a callback. See `~NapiEnvironment()` above for
  // details.
  LinkedList<Reference> gcRoots_{};
  LinkedList<Reference> finalizingGCRoots_{};
  LinkedList<Finalizer> finalizerQueue_{};
  bool isRunningFinalizers_{};

  llvh::SmallVector<OrderedSet<vm::HermesValue> *, 16> orderedSets_;

  vm::PinnedHermesValue lastException_{EmptyHermesValue};
  std::string lastErrorMessage_;
  napi_extended_error_info lastError_{"", 0, 0, napi_ok};

  InstanceData *instanceData_{};

  static constexpr uint32_t kEscapeableSentinelTag = 0x35456789;
  static constexpr uint32_t kUsedEscapeableSentinelTag =
      kEscapeableSentinelTag + 1;
  static constexpr uint32_t kExternalValueTag = 0x00353637;
  static constexpr int32_t kExternalTagSlot = 0;
  static constexpr vm::HermesValue EmptyHermesValue{
      vm::HermesValue::encodeEmptyValue()};
};

// RAII class to open and close GC stack value scope.
class HandleScope final {
 public:
  HandleScope(NapiEnvironment &env) noexcept : env_(env) {
    CRASH_IF_FALSE(env_.openHandleScope(&scope_) == napi_ok);
  }

  ~HandleScope() noexcept {
    CRASH_IF_FALSE(env_.closeHandleScope(scope_) == napi_ok);
  }

 private:
  NapiEnvironment &env_;
  napi_handle_scope scope_{};
};

// RAII class to open and close GC stack value scope.
// Allow to escape one result value from the handle scope.
class EscapableHandleScope final {
 public:
  EscapableHandleScope(NapiEnvironment &env) noexcept : env_(env) {
    CRASH_IF_FALSE(env_.openEscapableHandleScope(&scope_) == napi_ok);
  }

  ~EscapableHandleScope() noexcept {
    CRASH_IF_FALSE(env_.closeEscapableHandleScope(scope_) == napi_ok);
  }

  napi_status escape(napi_value *value) noexcept {
    return env_.escapeHandle(scope_, *value, value);
  }

 private:
  NapiEnvironment &env_;
  napi_escapable_handle_scope scope_{};
};

// Keep external data with an object.
class ExternalValue final : public vm::DecoratedObject::Decoration {
 public:
  ExternalValue(NapiEnvironment &env) noexcept : env_(env) {}
  ExternalValue(NapiEnvironment &env, void *nativeData) noexcept
      : env_(env), nativeData_(nativeData) {}

  ExternalValue(const ExternalValue &other) = delete;
  ExternalValue &operator=(const ExternalValue &other) = delete;

  ~ExternalValue() override {
    finalizers_.forEach(
        [&](Finalizer *finalizer) { env_.addToFinalizerQueue(finalizer); });
  }

  size_t getMallocSize() const override {
    return sizeof(*this);
  }

  void addFinalizer(Finalizer *finalizer) noexcept {
    finalizers_.pushBack(finalizer);
  }

  void *nativeData() noexcept {
    return nativeData_;
  }

  void setNativeData(void *value) noexcept {
    nativeData_ = value;
  }

 private:
  NapiEnvironment &env_;
  void *nativeData_{};
  LinkedList<Finalizer> finalizers_;
};

// Keep native data associated with a function.
class HostFunctionContext final {
 public:
  HostFunctionContext(
      NapiEnvironment &env,
      napi_callback hostCallback,
      void *nativeData) noexcept
      : env_(env), hostCallback_(hostCallback), nativeData_(nativeData) {}

  static vm::CallResult<vm::HermesValue>
  func(void *context, vm::Runtime *runtime, vm::NativeArgs hvArgs);

  static void finalize(void *context) {
    delete reinterpret_cast<class HostFunctionContext *>(context);
  }

  void *nativeData() noexcept {
    return nativeData_;
  }

 private:
  NapiEnvironment &env_;
  napi_callback hostCallback_;
  void *nativeData_;
};

class CallbackInfo final {
 public:
  CallbackInfo(
      HostFunctionContext &context,
      vm::NativeArgs &nativeArgs) noexcept
      : context_(context), nativeArgs_(nativeArgs) {}

  void args(napi_value *args, size_t *argCount) noexcept {
    *args = napiValue(&*nativeArgs_.begin());
    *argCount = nativeArgs_.getArgCount();
  }

  size_t argCount() noexcept {
    return nativeArgs_.getArgCount();
  }

  napi_value thisArg() noexcept {
    return napiValue(&nativeArgs_.getThisArg());
  }

  void *nativeData() noexcept {
    return context_.nativeData();
  }

  napi_value getNewTarget() noexcept {
    return napiValue(&nativeArgs_.getNewTarget());
  }

 private:
  HostFunctionContext &context_;
  vm::NativeArgs &nativeArgs_;
};

/*static*/ vm::CallResult<vm::HermesValue> HostFunctionContext::func(
    void *context,
    vm::Runtime *runtime,
    vm::NativeArgs hvArgs) {
  HostFunctionContext *hfc = reinterpret_cast<HostFunctionContext *>(context);
  NapiEnvironment &env = hfc->env_;
  assert(runtime == &env.runtime());
  vm::instrumentation::RuntimeStats &stats = env.runtime().getRuntimeStats();
  const vm::instrumentation::RAIITimer timer{
      "Host Function", stats, stats.hostFunction};

  CallbackInfo callbackInfo{*hfc, hvArgs};
  napi_value result = hfc->hostCallback_(
      napiEnv(&env), reinterpret_cast<napi_callback_info>(&callbackInfo));
  return *phv(result);
  // TODO: handle errors
  // TODO: Add call in module
}

// Different types of references:
// 1. Strong reference - it can wrap up object of any type
//   a. Ref count maintains the reference lifetime. When it reaches zero it is
//   removed.
// 2. Weak reference - it can wrap up only objects
//   a. Ref count maintains the lifetime of the reference. When it reaches zero
//   it is removed.
// 3. Combined reference - it can wrap up only objects
//   a. Ref count only for strong references. Zero converts it to a weak ref.
//   Removal is explicit if external code holds a reference.

// A base class for References that wrap native data and must be finalized.
class Finalizer : public LinkedList<Finalizer>::Item {
 public:
  virtual void finalize(NapiEnvironment &env) noexcept = 0;

 protected:
  Finalizer() = default;

  ~Finalizer() noexcept {
    unlink();
  }
};

// A base class for all references.
class Reference : public LinkedList<Reference>::Item {
 public:
  enum class ReasonToDelete {
    ZeroRefCount,
    FinalizerCall,
    ExternalCall,
    EnvironmentShutdown,
  };

  static napi_status deleteReference(
      NapiEnvironment &env,
      Reference *reference,
      ReasonToDelete reason) noexcept {
    if (reference && reference->startDeleting(env, reason)) {
      delete reference;
    }
    return env.clearLastError();
  }

  virtual napi_status incRefCount(
      NapiEnvironment &env,
      uint32_t & /*result*/) noexcept {
    return GENERIC_FAILURE("This reference does not support ref count.");
  }

  virtual napi_status decRefCount(
      NapiEnvironment &env,
      uint32_t & /*result*/) noexcept {
    return GENERIC_FAILURE("This reference does not support ref count.");
  }

  virtual const vm::PinnedHermesValue &value(NapiEnvironment &env) noexcept {
    return env.getPredefined(NapiPredefined::undefined);
  }

  virtual void *nativeData() noexcept {
    return nullptr;
  }

  virtual void *finalizeHint() noexcept {
    return nullptr;
  }

  virtual vm::PinnedHermesValue *getGCRoot(NapiEnvironment & /*env*/) noexcept {
    return nullptr;
  }

  virtual vm::WeakRoot<vm::JSObject> *getGCWeakRoot(
      NapiEnvironment & /*env*/) noexcept {
    return nullptr;
  }

  static void getGCRoots(
      NapiEnvironment &env,
      LinkedList<Reference> &list,
      vm::RootAcceptor &acceptor) noexcept {
    list.forEach([&](Reference *ref) {
      if (vm::PinnedHermesValue *value = ref->getGCRoot(env)) {
        acceptor.accept(*value);
      }
    });
  }

  static void getGCWeakRoots(
      NapiEnvironment &env,
      LinkedList<Reference> &list,
      vm::WeakRootAcceptor &acceptor) noexcept {
    list.forEach([&](Reference *ref) {
      if (vm::WeakRoot<vm::JSObject> *weakRoot = ref->getGCWeakRoot(env)) {
        acceptor.acceptWeak(*weakRoot);
      }
    });
  }

  virtual napi_status callFinalizeCallback(NapiEnvironment &env) noexcept {
    return napi_ok;
  }

  virtual void finalize(NapiEnvironment &env) noexcept {}

  template <class TItem>
  static void finalizeAll(
      NapiEnvironment &env,
      LinkedList<TItem> &list) noexcept {
    for (TItem *item = list.begin(); item != list.end(); item = list.begin()) {
      item->finalize(env);
    }
  }

  static void deleteAll(
      NapiEnvironment &env,
      LinkedList<Reference> &list,
      ReasonToDelete reason) noexcept {
    for (Reference *ref = list.begin(); ref != list.end(); ref = list.begin()) {
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
      NapiEnvironment &env,
      ReasonToDelete /*reason*/) noexcept {
    return true;
  }
};

// A reference with a ref count that can be changed from any thread.
// The reference deletion is done as a part of GC root detection to avoid
// deletion in a random thread.
class AtomicRefCountReference : public Reference {
 public:
  napi_status incRefCount(NapiEnvironment &env, uint32_t &result) noexcept
      override {
    result = refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    CRASH_IF_FALSE(result > 1 && "The ref count cannot bounce from zero.");
    CRASH_IF_FALSE(result < MaxRefCount && "The ref count is too big.");
    return napi_ok;
  }

  napi_status decRefCount(NapiEnvironment &env, uint32_t &result) noexcept
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

  bool startDeleting(NapiEnvironment &env, ReasonToDelete reason) noexcept
      override {
    return reason != ReasonToDelete::ExternalCall;
  }

 private:
  std::atomic<uint32_t> refCount_{1};

  static constexpr uint32_t MaxRefCount =
      std::numeric_limits<uint32_t>::max() / 2;
};

// Atomic ref counting for vm::PinnedHermesValue.
class StrongReference : public AtomicRefCountReference {
 public:
  static napi_status create(
      NapiEnvironment &env,
      vm::HermesValue value,
      StrongReference **result) noexcept {
    CHECK_ARG(result);
    *result = new StrongReference(value);
    env.addGCRoot(*result);
    return env.clearLastError();
  }

  const vm::PinnedHermesValue &value(NapiEnvironment &env) noexcept override {
    return value_;
  }

  vm::PinnedHermesValue *getGCRoot(NapiEnvironment &env) noexcept override {
    if (refCount() > 0) {
      return &value_;
    } else {
      deleteReference(env, this, ReasonToDelete::ZeroRefCount);
      return nullptr;
    }
  }

 protected:
  StrongReference(vm::HermesValue value) noexcept : value_(value) {}

 private:
  vm::PinnedHermesValue value_;
};

// Atomic ref counting for a vm::WeakRef<vm::HermesValue>.
class WeakReference final : public AtomicRefCountReference {
 public:
  static napi_status create(
      NapiEnvironment &env,
      const vm::PinnedHermesValue *value,
      WeakReference **result) noexcept {
    CHECK_OBJECT_ARG(value);
    CHECK_ARG(result);
    *result = new WeakReference(env.createWeakRoot(getObjectUnsafe(*value)));
    env.addGCRoot(*result);
    return env.clearLastError();
  }

  const vm::PinnedHermesValue &value(NapiEnvironment &env) noexcept override {
    return env.lockWeakObject(weakRoot_);
  }

  vm::WeakRoot<vm::JSObject> *getGCWeakRoot(
      NapiEnvironment &env) noexcept override {
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
class ComplexReference : public Reference {
 public:
  static napi_status create(
      NapiEnvironment &env,
      const vm::PinnedHermesValue *value,
      uint32_t initialRefCount,
      ComplexReference **result) noexcept {
    CHECK_OBJECT_ARG(value);
    CHECK_ARG(result);
    *result = new ComplexReference(
        initialRefCount,
        *value,
        initialRefCount == 0 ? env.createWeakRoot(getObjectUnsafe(*value))
                             : vm::WeakRoot<vm::JSObject>{});
    env.addGCRoot(*result);
    return env.clearLastError();
  }

  napi_status incRefCount(NapiEnvironment &env, uint32_t &result) noexcept
      override {
    if (refCount_ == 0) {
      value_ = env.lockWeakObject(weakRoot_);
    }
    CRASH_IF_FALSE(++refCount_ >= MaxRefCount && "The ref count is too big.");
    result = refCount_;
    return env.clearLastError();
  }

  napi_status decRefCount(NapiEnvironment &env, uint32_t &result) noexcept
      override {
    if (refCount_ == 0) {
      // Ignore this error situation to match NAPI for V8 implementation.
      result = 0;
      return napi_ok;
    }
    if (--refCount_ == 0) {
      if (value_.isObject()) {
        weakRoot_ = env.createWeakRoot(getObjectUnsafe(value_));
      } else {
        weakRoot_ = vm::WeakRoot<vm::JSObject>{};
      }
    }
    result = refCount_;
    return env.clearLastError();
  }

  const vm::PinnedHermesValue &value(NapiEnvironment &env) noexcept override {
    if (refCount_ > 0) {
      return value_;
    } else {
      return env.lockWeakObject(weakRoot_);
    }
  }

  vm::PinnedHermesValue *getGCRoot(
      NapiEnvironment & /*env*/) noexcept override {
    return (refCount_ > 0) ? &value_ : nullptr;
  }

  vm::WeakRoot<vm::JSObject> *getGCWeakRoot(
      NapiEnvironment & /*env*/) noexcept override {
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

// Store finalizeHint if it is not null.
template <class TBaseReference>
class FinalizeHintHolder : public TBaseReference {
 public:
  template <class... TArgs>
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
template <class TBaseReference>
class FinalizeCallbackHolder : public TBaseReference {
 public:
  template <class... TArgs>
  FinalizeCallbackHolder(
      napi_finalize finalizeCallback,
      TArgs &&...args) noexcept
      : TBaseReference(std::forward<TArgs>(args)...),
        finalizeCallback_(finalizeCallback) {}

  napi_status callFinalizeCallback(NapiEnvironment &env) noexcept override {
    if (finalizeCallback_) {
      napi_finalize finalizeCallback =
          std::exchange(finalizeCallback_, nullptr);
      return env.callFinalizer(finalizeCallback, nativeData(), finalizeHint());
    }
    return napi_ok;
  }

 private:
  napi_finalize finalizeCallback_{};
};

// Store nativeData if it is not null.
template <class TBaseReference>
class NativeDataHolder : public TBaseReference {
 public:
  template <class... TArgs>
  NativeDataHolder(void *nativeData, TArgs &&...args) noexcept
      : TBaseReference(std::forward<TArgs>(args)...), nativeData_(nativeData) {}

  void *nativeData() noexcept override {
    return nativeData_;
  }

 private:
  void *nativeData_;
};

// Common code for references inherited from Finalizer.
template <class TBaseReference>
class FinalizingReference final : public TBaseReference {
 public:
  template <class... TArgs>
  FinalizingReference(TArgs &&...args) noexcept
      : TBaseReference(std::forward<TArgs>(args)...) {}

  void finalize(NapiEnvironment &env) noexcept override {
    callFinalizeCallback(env);
    Reference::deleteReference(
        env, this, Reference::ReasonToDelete::FinalizerCall);
  }
};

// Create FinalizingReference with the optimized storage.
template <class TReference>
class FinalizingReferenceFactory final {
 public:
  template <class... TArgs>
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
class FinalizingAnonymousReference : public Reference, public Finalizer {
 public:
  static napi_status create(
      NapiEnvironment &env,
      const vm::PinnedHermesValue *value,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      /*optional*/ FinalizingAnonymousReference **result) noexcept {
    CHECK_OBJECT_ARG(value);
    FinalizingAnonymousReference *ref =
        FinalizingReferenceFactory<FinalizingAnonymousReference>::create(
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
class FinalizingStrongReference : public StrongReference, public Finalizer {
 public:
  static napi_status create(
      NapiEnvironment &env,
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

  bool startDeleting(NapiEnvironment &env, ReasonToDelete reason) noexcept
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
class FinalizingComplexReference : public ComplexReference, public Finalizer {
 public:
  static napi_status create(
      NapiEnvironment &env,
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
        initialRefCount == 0 ? env.createWeakRoot(getObjectUnsafe(*value))
                             : vm::WeakRoot<vm::JSObject>{});
    if (initialRefCount == 0) {
      env.addObjectFinalizer(value, *result);
    }
    env.addFinalizingGCRoot(*result);
    return env.clearLastError();
  }

  napi_status incRefCount(NapiEnvironment &env, uint32_t &result) noexcept
      override {
    CHECK_NAPI(ComplexReference::incRefCount(env, result));
    if (result == 1) {
      LinkedList<Finalizer>::Item::unlink();
    }
    return env.clearLastError();
  }

  napi_status decRefCount(NapiEnvironment &env, uint32_t &result) noexcept
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

  bool startDeleting(NapiEnvironment &env, ReasonToDelete reason) noexcept
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

// Hold custom data associated with the NapiEnvironment.
class InstanceData : public Reference {
 public:
  static napi_status create(
      NapiEnvironment &env,
      void *nativeData,
      napi_finalize finalizeCallback,
      void *finalizeHint,
      /*optional*/ InstanceData **result) noexcept {
    InstanceData *ref = FinalizingReferenceFactory<InstanceData>::create(
        nativeData, finalizeCallback, finalizeHint);
    if (result) {
      *result = ref;
    }
    return env.clearLastError();
  }
};

// Sorted list of unique HermesValues.
template <>
class OrderedSet<vm::HermesValue> final {
 public:
  using Compare =
      int32_t(const vm::HermesValue &item1, const vm::HermesValue &item2);

  OrderedSet(NapiEnvironment &env, Compare *compare) noexcept
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
    for (OrderedSet *set : range) {
      for (vm::PinnedHermesValue &value : set->items_) {
        acceptor.accept(value);
      }
    }
  }

 private:
  NapiEnvironment &env_;
  llvh::SmallVector<vm::PinnedHermesValue, 16> items_;
  Compare *compare_{};
};

// Sorted list of unique uint32_t.
template <>
class OrderedSet<uint32_t> final {
 public:
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

// Helper class to build a string.
class StringBuilder final {
 public:
  // To adopt an existing string instead of creating a new one.
  class AdoptStringTag {};
  constexpr static AdoptStringTag AdoptString{};

  StringBuilder(AdoptStringTag, std::string &&str) noexcept
      : str_(std::move(str)), stream_(str_) {}

  template <class... TArgs>
  StringBuilder(TArgs &&...args) noexcept : stream_(str_) {
    append(std::forward<TArgs>(args)...);
  }

  StringBuilder &append() noexcept {
    return *this;
  }

  template <class TArg0, class... TArgs>
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

  napi_status makeHVString(
      NapiEnvironment &env,
      vm::MutableHandle<> *result) noexcept {
    stream_.flush();
    vm::CallResult<vm::HermesValue> res = vm::StringPrimitive::createEfficient(
        &env.runtime(), llvh::makeArrayRef(str_.data(), str_.size()));
    return env.setResult(std::move(res), result);
  }

 private:
  std::string str_;
  llvh::raw_string_ostream stream_;
};

// The external buffer that implements hermes::Buffer
class ExternalBuffer final : public hermes::Buffer {
 public:
  static std::unique_ptr<ExternalBuffer> make(
      napi_env env,
      const napi_ext_buffer &buffer) noexcept {
    return buffer.data ? std::make_unique<ExternalBuffer>(
                             *reinterpret_cast<NapiEnvironment *>(env),
                             buffer.data,
                             buffer.byte_length,
                             buffer.finalize_cb,
                             buffer.finalize_hint)
                       : nullptr;
  }

  ExternalBuffer(
      NapiEnvironment &env,
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
  NapiEnvironment &env_;
  FinalizingAnonymousReference *finalizer_;
};

// An implementation of PreparedJavaScript that wraps a BytecodeProvider.
class HermesPreparedJavaScript final {
 public:
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

// Conversion routines from double to int32, uin32 and int64.
// The code is adopted from V8 source code to match the NAPI for V8 behavior.
// https://github.com/v8/v8/blob/main/src/numbers/conversions-inl.h
// https://github.com/v8/v8/blob/main/src/base/numbers/double.h
class DoubleConversion final {
 public:
  // Implements most of https://tc39.github.io/ecma262/#sec-toint32.
  static int32_t toInt32(double value) noexcept {
    if (!std::isnormal(value)) {
      return 0;
    }
    if (value >= std::numeric_limits<int32_t>::min() &&
        value <= std::numeric_limits<int32_t>::max()) {
      // All doubles within these limits are trivially convertable to an int32.
      return static_cast<int32_t>(value);
    }
    uint64_t u64 = toUint64Bits(value);
    int exponent = getExponent(u64);
    uint64_t bits;
    if (exponent < 0) {
      if (exponent <= -kSignificandSize) {
        return 0;
      }
      bits = getSignificand(u64) >> -exponent;
    } else {
      if (exponent > 31) {
        return 0;
      }
      bits = getSignificand(u64) << exponent;
    }
    return static_cast<int32_t>(
        getSign(u64) * static_cast<int64_t>(bits & 0xFFFFFFFFul));
  }

  static uint32_t toUint32(double value) noexcept {
    return static_cast<uint32_t>(toInt32(value));
  }

  static int64_t toInt64(double value) {
    // This code has the NAPI for V8 special behavior.
    // The comment from the napi_get_value_int64 code:
    // https://github.com/nodejs/node/blob/master/src/js_native_api_v8.cc
    //
    // v8::Value::IntegerValue() converts NaN, +Inf, and -Inf to INT64_MIN,
    // inconsistent with v8::Value::Int32Value() which converts those values to
    // 0. Special-case all non-finite values to match that behavior.
    //
    if (!std::isnormal(value)) {
      return 0;
    }
    if (value >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
      return std::numeric_limits<int64_t>::max();
    }
    if (value <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
      return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(value);
  }

 private:
  static uint64_t toUint64Bits(double value) noexcept {
    uint64_t result;
    std::memcpy(&result, &value, sizeof(value));
    return result;
  }

  static int getSign(uint64_t u64) noexcept {
    return (u64 & kSignMask) == 0 ? 1 : -1;
  }

  static int getExponent(uint64_t u64) noexcept {
    int biased_e =
        static_cast<int>((u64 & kExponentMask) >> kPhysicalSignificandSize);
    return biased_e - kExponentBias;
  }

  static uint64_t getSignificand(uint64_t u64) noexcept {
    return (u64 & kSignificandMask) + kHiddenBit;
  }

  static constexpr uint64_t kSignMask = 0x8000'0000'0000'0000;
  static constexpr uint64_t kExponentMask = 0x7FF0'0000'0000'0000;
  static constexpr uint64_t kSignificandMask = 0x000F'FFFF'FFFF'FFFF;
  static constexpr uint64_t kHiddenBit = 0x0010'0000'0000'0000;
  static constexpr int kPhysicalSignificandSize = 52;
  static constexpr int kSignificandSize = 53;
  static constexpr int kExponentBias = 0x3FF + kPhysicalSignificandSize;
};

// Max size of the runtime's register stack.
// The runtime register stack needs to be small enough to be allocated on the
// native thread stack in Android (1MiB) and on MacOS's thread stack (512 KiB)
// Calculated by: (thread stack size - size of runtime -
// 8 memory pages for other stuff in the thread)
constexpr unsigned kMaxNumRegisters =
    (512 * 1024 - sizeof(vm::Runtime) - 4096 * 8) /
    sizeof(vm::PinnedHermesValue);

template <class T, std::size_t N>
constexpr std::size_t size(const T (&array)[N]) noexcept {
  return N;
}

template <class TEnum>
bool isInEnumRange(
    TEnum value,
    TEnum lowerBoundInclusive,
    TEnum upperBoundInclusive) noexcept {
  return lowerBoundInclusive <= value && value <= upperBoundInclusive;
}

napi_env napiEnv(NapiEnvironment *env) noexcept {
  return reinterpret_cast<napi_env>(env);
}

napi_value napiValue(const vm::PinnedHermesValue *value) noexcept {
  return reinterpret_cast<napi_value>(
      const_cast<vm::PinnedHermesValue *>(value));
}

const vm::PinnedHermesValue *phv(napi_value value) noexcept {
  return reinterpret_cast<const vm::PinnedHermesValue *>(value);
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

CallbackInfo *asCallbackInfo(napi_callback_info callbackInfo) noexcept {
  return reinterpret_cast<CallbackInfo *>(callbackInfo);
}

vm::JSObject *getObjectUnsafe(const vm::HermesValue &value) noexcept {
  return reinterpret_cast<vm::JSObject *>(value.getObject());
}

size_t copyASCIIToUTF8(
    llvh::ArrayRef<char> input,
    char *buf,
    size_t maxCharacters) noexcept {
  size_t size = std::min(input.size(), maxCharacters);
  std::char_traits<char>::copy(buf, input.data(), size);
  return size;
}

//=============================================================================
// NapiEnvironment implementation
//=============================================================================

NapiEnvironment::NapiEnvironment(
    const vm::RuntimeConfig &runtimeConfig) noexcept
    : rt_(vm::Runtime::create(runtimeConfig.rebuild()
                                  .withRegisterStack(nullptr)
                                  .withMaxNumRegisters(kMaxNumRegisters)
                                  .build())),
      runtime_(*rt_),
      vmExperimentFlags_(runtimeConfig.getVMExperimentFlags()) {
  compileFlags_.optimize = false;
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

  runtime_.addCustomRootsFunction([this](vm::GC *, vm::RootAcceptor &acceptor) {
    gcRootStack_.forEach([&](const vm::PinnedHermesValue &value) {
      acceptor.accept(const_cast<vm::PinnedHermesValue &>(value));
    });
    Reference::getGCRoots(*this, gcRoots_, acceptor);
    Reference::getGCRoots(*this, finalizingGCRoots_, acceptor);
    if (!lastException_.isEmpty()) {
      acceptor.accept(lastException_);
    }
    for (vm::PinnedHermesValue &value : predefinedValues_) {
      acceptor.accept(value);
    }
    OrderedSet<vm::HermesValue>::getGCRoots(orderedSets_, acceptor);
  });
  runtime_.addCustomWeakRootsFunction(
      [this](vm::GC *, vm::WeakRootAcceptor &acceptor) {
        Reference::getGCWeakRoots(*this, gcRoots_, acceptor);
        Reference::getGCWeakRoots(*this, finalizingGCRoots_, acceptor);
      });

  vm::GCScope gcScope(&runtime_);
  auto setPredefined = [this](
                           NapiPredefined key, vm::HermesValue value) noexcept {
    predefinedValues_[static_cast<size_t>(key)] = value;
  };
  setPredefined(
      NapiPredefined::Promise,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().registerLazyIdentifier("Promise")));
  setPredefined(
      NapiPredefined::code,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().registerLazyIdentifier("code")));
  setPredefined(
      NapiPredefined::napi_externalValue,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().createNotUniquedLazySymbol(
              "napi.externalValue.735e14c9-354f-489b-9f27-02acbc090975")));
  setPredefined(
      NapiPredefined::napi_typeTag,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().createNotUniquedLazySymbol(
              "napi.typeTag.026ae0ec-b391-49da-a935-0cab733ab615")));
  setPredefined(
      NapiPredefined::reject,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().registerLazyIdentifier("reject")));
  setPredefined(
      NapiPredefined::resolve,
      vm::HermesValue::encodeSymbolValue(
          runtime_.getIdentifierTable().registerLazyIdentifier("resolve")));
  setPredefined(
      NapiPredefined::undefined, vm::HermesValue::encodeUndefinedValue());
}

napi_status NapiEnvironment::incRefCount() noexcept {
  refCount_++;
  return napi_status::napi_ok;
}

napi_status NapiEnvironment::decRefCount() noexcept {
  if (--refCount_ == 0) {
    delete this;
  }
  return napi_status::napi_ok;
}

vm::Runtime &NapiEnvironment::runtime() noexcept {
  return runtime_;
}

NapiEnvironment::~NapiEnvironment() {
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

//---------------------------------------------------------------------------
// Native error handling methods
//---------------------------------------------------------------------------

template <class... TArgs>
napi_status NapiEnvironment::setLastError(
    napi_status status,
    const char *fileName,
    uint32_t line,
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

  if (status < napi_ok || status >= lastStatus) {
    status = napi_generic_failure;
  }

  lastErrorMessage_.clear();
  StringBuilder sb{StringBuilder::AdoptString, std::move(lastErrorMessage_)};
  sb.append(errorMessages[status]);
  if (sizeof...(args) > 0) {
    sb.append(": ", std::forward<TArgs>(args)...);
  }
  sb.append("\nFile: ", fileName);
  sb.append("\nLine: ", line);
  lastErrorMessage_ = std::move(sb.str());
  lastError_ = {lastErrorMessage_.c_str(), 0, 0, status};
  return status;
}

napi_status NapiEnvironment::clearLastError() noexcept {
  lastErrorMessage_.clear();
  lastError_ = {"", 0, 0, napi_ok};
  return napi_ok;
}

napi_status NapiEnvironment::getLastErrorInfo(
    const napi_extended_error_info **result) noexcept {
  CHECK_ARG(result);
  *result = &lastError_;
  return napi_ok;
}

napi_status NapiEnvironment::checkHermesStatus(
    vm::ExecutionStatus hermesStatus,
    napi_status status) noexcept {
  if (LLVM_LIKELY(hermesStatus != vm::ExecutionStatus::EXCEPTION)) {
    return napi_ok;
  }

  lastException_ = runtime_.getThrownValue();
  runtime_.clearThrownValue();
  return status;
}

template <class T>
napi_status NapiEnvironment::checkHermesStatus(
    const vm::CallResult<T> &callResult,
    napi_status status) noexcept {
  return checkHermesStatus(callResult.getStatus());
}

template <class F>
napi_status NapiEnvironment::handleExceptions(const F &f) noexcept {
  CHECK_NAPI(checkPendingExceptions());
  {
    vm::GCScope gcScope(&runtime_);
    CHECK_NAPI(f());
  }
  return runReferenceFinalizers();
}

napi_status NapiEnvironment::checkPendingExceptions() noexcept {
  RETURN_STATUS_IF_FALSE(lastException_.isEmpty(), napi_pending_exception);
  return clearLastError();
}

//-----------------------------------------------------------------------------
// Getters for defined singletons
// [X] Matches NAPI for V8
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::getUndefined(napi_value *result) noexcept {
  return setPredefinedResult(
      runtime_.getUndefinedValue().unsafeGetPinnedHermesValue(), result);
}

napi_status NapiEnvironment::getNull(napi_value *result) noexcept {
  return setPredefinedResult(
      runtime_.getNullValue().unsafeGetPinnedHermesValue(), result);
}

napi_status NapiEnvironment::getGlobal(napi_value *result) noexcept {
  return setPredefinedResult(
      runtime_.getGlobal().unsafeGetPinnedHermesValue(), result);
}

napi_status NapiEnvironment::getBoolean(
    bool value,
    napi_value *result) noexcept {
  return setPredefinedResult(
      runtime_.getBoolValue(value).unsafeGetPinnedHermesValue(), result);
}

//-----------------------------------------------------------------------------
// Methods to create Primitive types and Objects
// [X] Matches NAPI for V8
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::createObject(napi_value *result) noexcept {
  vm::GCScope gcScope(&runtime_);
  return setResultAndRunFinalizers(vm::JSObject::create(&runtime_), result);
}

napi_status NapiEnvironment::createArray(napi_value *result) noexcept {
  vm::GCScope gcScope(&runtime_);
  return setResultAndRunFinalizers(
      vm::JSArray::create(&runtime_, /*capacity:*/ 0, /*length:*/ 0), result);
}

napi_status NapiEnvironment::createArray(
    size_t length,
    napi_value *result) noexcept {
  vm::GCScope gcScope(&runtime_);
  return setResultAndRunFinalizers(
      vm::JSArray::create(&runtime_, /*capacity:*/ length, /*length:*/ length),
      result);
}

template <class T, std::enable_if_t<std::is_arithmetic_v<T>, bool>>
napi_status NapiEnvironment::createNumber(
    T value,
    napi_value *result) noexcept {
  return setResult(
      vm::HermesValue::encodeNumberValue(static_cast<double>(value)), result);
}

napi_status NapiEnvironment::createStringASCII(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  vm::GCScope gcScope(&runtime_);
  return setResultAndRunFinalizers(
      vm::StringPrimitive::createEfficient(
          &runtime_, llvh::makeArrayRef(str, length)),
      result);
}

napi_status NapiEnvironment::createStringLatin1(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  CHECK_ARG(str);
  if (length == NAPI_AUTO_LENGTH) {
    length = std::char_traits<char>::length(str);
  }
  RETURN_STATUS_IF_FALSE(
      length <= static_cast<size_t>(std::numeric_limits<int32_t>::max()),
      napi_invalid_arg);

  if (isAllASCII(str, str + length)) {
    return createStringASCII(str, length, result);
  }

  // Latin1 has the same codes as Unicode.
  // We just need to expand char to char16_t.
  std::u16string u16str(length, u'\0');
  std::copy(str, str + length, &u16str[0]);

  vm::GCScope gcScope(&runtime_);
  return setResultAndRunFinalizers(
      vm::StringPrimitive::createEfficient(&runtime_, std::move(u16str)),
      result);
}

napi_status NapiEnvironment::createStringUTF8(
    const char *str,
    size_t length,
    napi_value *result) noexcept {
  CHECK_ARG(str);
  if (length == NAPI_AUTO_LENGTH) {
    length = std::char_traits<char>::length(str);
  }
  RETURN_STATUS_IF_FALSE(
      length <= static_cast<size_t>(std::numeric_limits<int32_t>::max()),
      napi_invalid_arg);

  if (isAllASCII(str, str + length)) {
    return createStringASCII(str, length, result);
  }

  vm::GCScope gcScope(&runtime_);
  std::u16string u16str;
  CHECK_NAPI(convertUTF8ToUTF16(str, length, u16str));
  return setResultAndRunFinalizers(
      vm::StringPrimitive::createEfficient(&runtime_, std::move(u16str)),
      result);
}

napi_status NapiEnvironment::createStringUTF8(
    const char *str,
    napi_value *result) noexcept {
  return createStringUTF8(str, NAPI_AUTO_LENGTH, result);
}

napi_status NapiEnvironment::convertUTF8ToUTF16(
    const char *utf8,
    size_t length,
    std::u16string &out) noexcept {
  // length is the number of input bytes
  out.resize(length);
  const llvh::UTF8 *sourceStart = reinterpret_cast<const llvh::UTF8 *>(utf8);
  const llvh::UTF8 *sourceEnd = sourceStart + length;
  llvh::UTF16 *targetStart = reinterpret_cast<llvh::UTF16 *>(&out[0]);
  llvh::UTF16 *targetEnd = targetStart + out.size();
  llvh::ConversionResult convRes = ConvertUTF8toUTF16(
      &sourceStart,
      sourceEnd,
      &targetStart,
      targetEnd,
      llvh::lenientConversion);
  RETURN_STATUS_IF_FALSE_WITH_MESSAGE(
      convRes != llvh::ConversionResult::targetExhausted,
      napi_generic_failure,
      "not enough space allocated for UTF16 conversion");
  out.resize((char16_t *)targetStart - &out[0]);
  return clearLastError();
}

napi_status NapiEnvironment::createStringUTF16(
    const char16_t *str,
    size_t length,
    napi_value *result) noexcept {
  CHECK_ARG(str);
  if (length == NAPI_AUTO_LENGTH) {
    length = std::char_traits<char16_t>::length(str);
  }
  RETURN_STATUS_IF_FALSE(
      length <= static_cast<size_t>(std::numeric_limits<int32_t>::max()),
      napi_invalid_arg);

  vm::GCScope gcScope(&runtime_);
  return setResultAndRunFinalizers(
      vm::StringPrimitive::createEfficient(
          &runtime_, llvh::makeArrayRef(str, length)),
      result);
}

napi_status NapiEnvironment::getUniqueStringRef(
    const char *utf8,
    size_t length,
    napi_ext_ref *result) noexcept {
  CHECK_ARG(utf8);
  HandleScope handleScope{*this};
  napi_value strValue{};
  CHECK_NAPI(createStringUTF8(utf8, length, &strValue));
  return getUniqueStringRef(strValue, result);
}

napi_status NapiEnvironment::getUniqueStringRef(
    napi_value strValue,
    napi_ext_ref *result) noexcept {
  vm::GCScope scope{&runtime_};
  vm::MutableHandle<vm::SymbolID> symbolHandle{&runtime_};
  CHECK_NAPI(createSymbolID(strValue, &symbolHandle));
  return StrongReference::create(
      *this,
      symbolHandle.getHermesValue(),
      reinterpret_cast<StrongReference **>(result));
}

napi_status NapiEnvironment::createSymbolID(
    const char *utf8,
    size_t length,
    vm::MutableHandle<vm::SymbolID> *result) noexcept {
  HandleScope handleScope{*this};
  napi_value strValue{};
  CHECK_NAPI(createStringUTF8(utf8, length, &strValue));
  return createSymbolID(strValue, result);
}

napi_status NapiEnvironment::createSymbolID(
    napi_value strValue,
    vm::MutableHandle<vm::SymbolID> *result) noexcept {
  CHECK_STRING_ARG(strValue);
  vm::CallResult<vm::Handle<vm::SymbolID>> res = vm::stringToSymbolID(
      &runtime_, vm::createPseudoHandle(phv(strValue)->getString()));
  return setResultAndRunFinalizers(std::move(res), result);
}

napi_status NapiEnvironment::createSymbol(
    napi_value description,
    napi_value *result) noexcept {
  vm::GCScope gcScope(&runtime_);
  vm::MutableHandle<vm::StringPrimitive> descString{&runtime_};
  if (description != nullptr) {
    CHECK_STRING_ARG(description);
    descString = phv(description)->getString();
  } else {
    // If description is undefined, the descString will eventually be "".
    descString = runtime_.getPredefinedString(vm::Predefined::emptyString);
  }
  return setResultAndRunFinalizers(
      runtime_.getIdentifierTable().createNotUniquedSymbol(
          &runtime_, descString),
      result);
}

napi_status NapiEnvironment::createFunction(
    const char *utf8Name,
    size_t length,
    napi_callback callback,
    void *callbackData,
    napi_value *result) noexcept {
  CHECK_NAPI(checkPendingExceptions());
  CHECK_ARG(callback);
  vm::GCScope scope{&runtime_};
  vm::MutableHandle<vm::SymbolID> nameSymbolID{&runtime_};
  if (utf8Name != nullptr) {
    CHECK_NAPI(createSymbolID(utf8Name, length, &nameSymbolID));
  } else {
    CHECK_NAPI(createSymbolID("hostFunction", NAPI_AUTO_LENGTH, &nameSymbolID));
  }
  return newFunction(nameSymbolID.get(), callback, callbackData, result);
}

napi_status NapiEnvironment::newFunction(
    vm::SymbolID name,
    napi_callback callback,
    void *callbackData,
    napi_value *result) noexcept {
  std::unique_ptr<HostFunctionContext> context =
      std::make_unique<HostFunctionContext>(*this, callback, callbackData);
  vm::CallResult<vm::HermesValue> funcRes =
      vm::FinalizableNativeFunction::createWithoutPrototype(
          &runtime_,
          context.get(),
          &HostFunctionContext::func,
          &HostFunctionContext::finalize,
          name,
          /*paramCount:*/ 0);
  CHECK_NAPI(checkHermesStatus(funcRes));
  context.release(); // the context is now owned by the func.
  return setResultAndRunFinalizers(*funcRes, result);
}

napi_status NapiEnvironment::createError(
    const vm::PinnedHermesValue &errorPrototype,
    napi_value code,
    napi_value message,
    napi_value *result) noexcept {
  CHECK_STRING_ARG(message);
  vm::GCScope scope{&runtime_};
  vm::Handle<vm::JSError> errorHandle = makeHandle(vm::JSError::create(
      &runtime_, makeHandle<vm::JSObject>(&errorPrototype)));
  CHECK_NAPI(checkHermesStatus(
      vm::JSError::setMessage(errorHandle, &runtime_, makeHandle(message))));
  CHECK_NAPI(setErrorCode(errorHandle, code, nullptr));
  return setResultAndRunFinalizers(std::move(errorHandle), result);
}

napi_status NapiEnvironment::createError(
    napi_value code,
    napi_value message,
    napi_value *result) noexcept {
  return createError(runtime_.ErrorPrototype, code, message, result);
}

napi_status NapiEnvironment::createTypeError(
    napi_value code,
    napi_value message,
    napi_value *result) noexcept {
  return createError(runtime_.TypeErrorPrototype, code, message, result);
}

napi_status NapiEnvironment::createRangeError(
    napi_value code,
    napi_value message,
    napi_value *result) noexcept {
  return createError(runtime_.RangeErrorPrototype, code, message, result);
}

//-----------------------------------------------------------------------------
// Methods to get the native napi_value from Primitive type
// [X] Matches NAPI for V8
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::typeOf(
    napi_value value,
    napi_valuetype *result) noexcept {
  CHECK_ARG(value);
  CHECK_ARG(result);

  const vm::PinnedHermesValue *hv = phv(value);

  // BigInt is not supported by Hermes yet.
  if (hv->isNumber()) {
    *result = napi_number;
  } else if (hv->isString()) {
    *result = napi_string;
  } else if (hv->isObject()) {
    if (vm::vmisa<vm::Callable>(*hv)) {
      *result = napi_function;
    } else if (getExternalValue(*hv)) {
      *result = napi_external;
    } else {
      *result = napi_object;
    }
  } else if (hv->isBool()) {
    *result = napi_boolean;
  } else if (hv->isUndefined() || hv->isEmpty()) {
    *result = napi_undefined;
  } else if (hv->isSymbol()) {
    *result = napi_symbol;
  } else if (hv->isNull()) {
    *result = napi_null;
  } else {
    // Should not get here unless Hermes has added some new kind of value.
    return ERROR_STATUS(napi_invalid_arg, "Unknown value type");
  }

  return clearLastError();
}

napi_status NapiEnvironment::getNumberValue(
    napi_value value,
    double *result) noexcept {
  CHECK_ARG(value);
  RETURN_STATUS_IF_FALSE(phv(value)->isNumber(), napi_number_expected);
  return setResult(phv(value)->getDouble(), result);
}

napi_status NapiEnvironment::getNumberValue(
    napi_value value,
    int32_t *result) noexcept {
  CHECK_ARG(value);
  RETURN_STATUS_IF_FALSE(phv(value)->isNumber(), napi_number_expected);
  return setResult(DoubleConversion::toInt32(phv(value)->getDouble()), result);
}

napi_status NapiEnvironment::getNumberValue(
    napi_value value,
    uint32_t *result) noexcept {
  CHECK_ARG(value);
  RETURN_STATUS_IF_FALSE(phv(value)->isNumber(), napi_number_expected);
  return setResult(DoubleConversion::toUint32(phv(value)->getDouble()), result);
}

napi_status NapiEnvironment::getNumberValue(
    napi_value value,
    int64_t *result) noexcept {
  CHECK_ARG(value);
  RETURN_STATUS_IF_FALSE(phv(value)->isNumber(), napi_number_expected);
  return setResult(DoubleConversion::toInt64(phv(value)->getDouble()), result);
}

napi_status NapiEnvironment::getBoolValue(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  RETURN_STATUS_IF_FALSE(phv(value)->isBool(), napi_boolean_expected);
  return setResult(phv(value)->getBool(), result);
}

// Copies a JavaScript string into a LATIN-1 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufSize is insufficient, the string will be truncated and null terminated.
// If buf is nullptr, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is nullptr.
napi_status NapiEnvironment::getValueStringLatin1(
    napi_value value,
    char *buf,
    size_t bufSize,
    size_t *result) noexcept {
  CHECK_STRING_ARG(value);
  vm::GCScope scope{&runtime_};
  vm::StringView view = vm::StringPrimitive::createStringView(
      &runtime_, makeHandle<vm::StringPrimitive>(value));

  if (buf == nullptr) {
    return setResult(view.length(), result);
  } else if (bufSize != 0) {
    size_t copied = std::min(bufSize - 1, view.length());
    for (auto cur = view.begin(), end = view.begin() + copied; cur < end;
         ++cur) {
      *buf++ = static_cast<char>(*cur);
    }
    *buf = '\0';
    return setOptionalResult(std::move(copied), result);
  } else {
    return setOptionalResult(static_cast<size_t>(0), result);
  }
}

// Copies a JavaScript string into a UTF-8 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufSize is insufficient, the string will be truncated and null terminated.
// If buf is nullptr, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is nullptr.
napi_status NapiEnvironment::getValueStringUTF8(
    napi_value value,
    char *buf,
    size_t bufSize,
    size_t *result) noexcept {
  CHECK_STRING_ARG(value);
  vm::GCScope scope{&runtime_};
  vm::StringView view = vm::StringPrimitive::createStringView(
      &runtime_, makeHandle<vm::StringPrimitive>(value));

  if (buf == nullptr) {
    return setResult(
        view.isASCII() || view.length() == 0
            ? view.length()
            : utf8LengthWithReplacements(
                  vm::UTF16Ref(view.castToChar16Ptr(), view.length())),
        result);
  } else if (bufSize != 0) {
    size_t copied = view.length() > 0 ? view.isASCII()
            ? copyASCIIToUTF8(
                  vm::ASCIIRef(view.castToCharPtr(), view.length()),
                  buf,
                  bufSize - 1)
            : convertUTF16ToUTF8WithReplacements(
                  vm::UTF16Ref(view.castToChar16Ptr(), view.length()),
                  buf,
                  bufSize - 1)
                                      : 0;
    buf[copied] = '\0';
    return setOptionalResult(std::move(copied), result);
  } else {
    return setOptionalResult(static_cast<size_t>(0), result);
  }
}

// Copies a JavaScript string into a UTF-16 string buffer. The result is the
// number of 2-byte code units (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufSize is insufficient, the string will be truncated and null terminated.
// If buf is nullptr, this method returns the length of the string (in 2-byte
// code units) via the result parameter.
// The result argument is optional unless buf is nullptr.
napi_status NapiEnvironment::getValueStringUTF16(
    napi_value value,
    char16_t *buf,
    size_t bufSize,
    size_t *result) noexcept {
  CHECK_STRING_ARG(value);
  vm::GCScope scope{&runtime_};
  vm::StringView view = vm::StringPrimitive::createStringView(
      &runtime_, makeHandle<vm::StringPrimitive>(value));

  if (buf == nullptr) {
    return setResult(view.length(), result);
  } else if (bufSize != 0) {
    size_t copied = std::min(bufSize - 1, view.length());
    std::copy(view.begin(), view.begin() + copied, buf);
    buf[copied] = '\0';
    return setOptionalResult(std::move(copied), result);
  } else {
    return setOptionalResult(static_cast<size_t>(0), result);
  }
}

//-----------------------------------------------------------------------------
// Methods to coerce values
// [X] Matches NAPI for V8
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::coerceToBool(
    napi_value value,
    napi_value *result) noexcept {
  CHECK_ARG(value);
  CHECK_NAPI(checkPendingExceptions());
  return setResult(vm::toBoolean(*phv(value)), result);
}

napi_status NapiEnvironment::coerceToNumber(
    napi_value value,
    napi_value *result) noexcept {
  CHECK_ARG(value);
  CHECK_NAPI(checkPendingExceptions());
  return setResultAndRunFinalizers(
      vm::toNumber_RJS(&runtime_, makeHandle(value)), result);
}

napi_status NapiEnvironment::coerceToObject(
    napi_value value,
    napi_value *result) noexcept {
  CHECK_ARG(value);
  CHECK_NAPI(checkPendingExceptions());
  return setResultAndRunFinalizers(
      vm::toObject(&runtime_, makeHandle(value)), result);
}

napi_status NapiEnvironment::coerceToString(
    napi_value value,
    napi_value *result) noexcept {
  CHECK_ARG(value);
  CHECK_NAPI(checkPendingExceptions());
  return setResultAndRunFinalizers(
      vm::toString_RJS(&runtime_, makeHandle(value)), result);
}

//-----------------------------------------------------------------------------
// Methods to work with Objects
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::getPrototype(
    napi_value object,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    return setResult(
        vm::JSObject::getPrototypeOf(
            makeHandle<vm::JSObject>(object), &runtime_),
        result);
  });
}

napi_status NapiEnvironment::getPropertyNames(
    napi_value object,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return getForInPropertyNames(objValue, napi_key_numbers_to_strings, result);
  });
}

napi_status NapiEnvironment::getAllPropertyNames(
    napi_value object,
    napi_key_collection_mode keyMode,
    napi_key_filter keyFilter,
    napi_key_conversion keyConversion,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
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
      napi_value parent;
      CHECK_NAPI(getPrototype(object, &parent));
      hasParent = phv(parent)->isObject();
    }

    // The fast path used for the 'for..in' implementation.
    if (keyFilter == (napi_key_enumerable | napi_key_skip_symbols) &&
        (keyMode == napi_key_include_prototypes || !hasParent)) {
      return getForInPropertyNames(objValue, keyConversion, result);
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
      vm::CallResult<vm::Handle<vm::JSArray>> ownKeysRes =
          vm::JSObject::getOwnPropertyKeys(
              makeHandle<vm::JSObject>(objValue), &runtime_, ownKeyFlags);
      CHECK_NAPI(checkHermesStatus(ownKeysRes));
      if (keyConversion == napi_key_numbers_to_strings) {
        CHECK_NAPI(convertToStringKeys(*ownKeysRes));
      }
      return setResult(std::move(*ownKeysRes), result);
    }

    // Collect all properties into the keyStorage.
    vm::CallResult<vm::MutableHandle<vm::BigStorage>> keyStorageRes =
        makeMutableHandle(vm::BigStorage::create(&runtime_, 16));
    CHECK_NAPI(checkHermesStatus(keyStorageRes));
    uint32_t size{0};

    // Make sure that we do not include into the result properties that were
    // shadowed by the derived objects.
    bool useShadowTracking =
        keyMode == napi_key_include_prototypes && hasParent;
    OrderedSet<uint32_t> shadowIndexes;
    OrderedSet<vm::HermesValue> shadowStrings(
        *this, [](const vm::HermesValue &item1, const vm::HermesValue &item2) {
          return item1.getString()->compare(item2.getString());
        });
    OrderedSet<vm::HermesValue> shadowSymbols(
        *this, [](const vm::HermesValue &item1, const vm::HermesValue &item2) {
          vm::SymbolID::RawType rawItem1 = item1.getSymbol().unsafeGetRaw();
          vm::SymbolID::RawType rawItem2 = item2.getSymbol().unsafeGetRaw();
          return rawItem1 < rawItem2 ? -1 : rawItem1 > rawItem2 ? 1 : 0;
        });

    // Keep the mutable variables outside of loop for efficiency
    vm::MutableHandle<vm::JSObject> currentObj(
        &runtime_, *makeHandle<vm::JSObject>(objValue));
    vm::MutableHandle<> prop(&runtime_);
    OptValue<uint32_t> propIndexOpt{};
    vm::MutableHandle<vm::StringPrimitive> propString(&runtime_);

    while (currentObj.get()) {
      vm::GCScope gcScope(&runtime_);

      vm::CallResult<vm::Handle<vm::JSArray>> props =
          vm::JSObject::getOwnPropertyKeys(currentObj, &runtime_, ownKeyFlags);
      CHECK_NAPI(checkHermesStatus(props));

      vm::GCScope::Marker marker = gcScope.createMarker();
      for (uint32_t i = 0, end = props.getValue()->getEndIndex(); i < end;
           ++i) {
        gcScope.flushToMarker(marker);
        prop = props.getValue()->at(&runtime_, i);

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
          vm::CallResult<bool> hasDescriptorRes =
              vm::JSObject::getOwnComputedPrimitiveDescriptor(
                  currentObj,
                  &runtime_,
                  prop,
                  vm::JSObject::IgnoreProxy::No,
                  tmpSymbolStorage,
                  desc);
          CHECK_NAPI(checkHermesStatus(hasDescriptorRes));
          if (*hasDescriptorRes) {
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

        CHECK_NAPI(checkHermesStatus(
            vm::BigStorage::push_back(*keyStorageRes, &runtime_, prop)));
        ++size;
      }

      // Continue to follow the prototype chain.
      napi_value parent;
      CHECK_NAPI(getPrototype(object, &parent));
      currentObj = makeHandle<vm::JSObject>(parent);
    }

    return convertKeyStorageToArray(
        *keyStorageRes, 0, size, keyConversion, result);
  });
}

napi_status NapiEnvironment::getForInPropertyNames(
    napi_value object,
    napi_key_conversion keyConversion,
    napi_value *result) noexcept {
  // Hermes optimizes retrieving property names for the 'for..in' implementation
  // by caching its results. This function takes the advantage from using it.
  uint32_t beginIndex;
  uint32_t endIndex;
  vm::CallResult<vm::Handle<vm::BigStorage>> keyStorage =
      vm::getForInPropertyNames(
          &runtime_, makeHandle<vm::JSObject>(object), beginIndex, endIndex);
  CHECK_NAPI(checkHermesStatus(keyStorage));
  return convertKeyStorageToArray(
      *keyStorage, 0, endIndex - beginIndex, keyConversion, result);
}

napi_status NapiEnvironment::convertKeyStorageToArray(
    vm::Handle<vm::BigStorage> keyStorage,
    uint32_t startIndex,
    uint32_t length,
    napi_key_conversion keyConversion,
    napi_value *result) noexcept {
  vm::CallResult<vm::Handle<vm::JSArray>> cr =
      vm::JSArray::create(&runtime_, length, length);
  CHECK_NAPI(checkHermesStatus(cr));
  vm::Handle<vm::JSArray> array = *cr;
  if (keyConversion == napi_key_numbers_to_strings) {
    vm::GCScopeMarkerRAII marker{&runtime_};
    vm::MutableHandle<> key{&runtime_};
    for (size_t i = 0; i < length; ++i) {
      key = makeHandle(keyStorage->at(startIndex + i));
      if (key->isNumber()) {
        CHECK_NAPI(convertIndexToString(key->getNumber(), &key));
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

napi_status NapiEnvironment::convertToStringKeys(
    vm::Handle<vm::JSArray> array) noexcept {
  vm::GCScopeMarkerRAII marker{&runtime_};
  vm::MutableHandle<> strKey{&runtime_};
  size_t length = vm::JSArray::getLength(array.get(), &runtime_);
  for (size_t i = 0; i < length; ++i) {
    vm::HermesValue key = array->at(&runtime_, i);
    if (LLVM_UNLIKELY(key.isNumber())) {
      CHECK_NAPI(convertIndexToString(key.getNumber(), &strKey));
      vm::JSArray::setElementAt(array, &runtime_, i, strKey);
      marker.flush();
    }
  }
  return clearLastError();
}

napi_status NapiEnvironment::convertIndexToString(
    double value,
    vm::MutableHandle<> *result) noexcept {
  OptValue<uint32_t> index = doubleToArrayIndex(value);
  RETURN_STATUS_IF_FALSE_WITH_MESSAGE(
      index.hasValue(), napi_generic_failure, "Index property is out of range");
  return StringBuilder(*index).makeHVString(*this, result);
}

napi_status NapiEnvironment::setProperty(
    napi_value object,
    napi_value key,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(key);
    CHECK_ARG(value);
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return putComputed(objValue, key, value);
  });
}

napi_status NapiEnvironment::hasProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(key);
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return hasComputed(objValue, key, result);
  });
}

napi_status NapiEnvironment::getProperty(
    napi_value object,
    napi_value key,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(key);
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return getComputed(objValue, key, result);
  });
}

napi_status NapiEnvironment::deleteProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(key);
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return deleteComputed(objValue, key, result);
  });
}

napi_status NapiEnvironment::hasOwnProperty(
    napi_value object,
    napi_value key,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(key);
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    vm::MutableHandle<vm::SymbolID> tmpSymbolStorage(&runtime_);
    vm::ComputedPropertyDescriptor desc;
    return getOwnComputedDescriptor(
        objValue, key, tmpSymbolStorage, desc, result);
  });
}

napi_status NapiEnvironment::setNamedProperty(
    napi_value object,
    const char *utf8Name,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(utf8Name);
    CHECK_ARG(value);
    napi_value objValue;
    napi_value name;
    CHECK_NAPI(coerceToObject(object, &objValue));
    CHECK_NAPI(createStringUTF8(utf8Name, &name));
    return putComputed(objValue, name, value);
  });
}

napi_status NapiEnvironment::hasNamedProperty(
    napi_value object,
    const char *utf8Name,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(utf8Name);
    napi_value objValue;
    napi_value name;
    CHECK_NAPI(coerceToObject(object, &objValue));
    CHECK_NAPI(createStringUTF8(utf8Name, &name));
    return hasComputed(objValue, name, result);
  });
}

napi_status NapiEnvironment::getNamedProperty(
    napi_value object,
    const char *utf8Name,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(utf8Name);
    napi_value objValue;
    napi_value name;
    CHECK_NAPI(coerceToObject(object, &objValue));
    CHECK_NAPI(createStringUTF8(utf8Name, &name));
    return getComputed(objValue, name, result);
  });
}

napi_status NapiEnvironment::setElement(
    napi_value object,
    uint32_t index,
    napi_value value) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return putComputed(objValue, index, value);
  });
}

napi_status NapiEnvironment::hasElement(
    napi_value object,
    uint32_t index,
    bool *result) noexcept {
  return handleExceptions([&] {
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return hasComputed(objValue, index, result);
  });
}

napi_status NapiEnvironment::getElement(
    napi_value object,
    uint32_t index,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return getComputed(objValue, index, result);
  });
}

napi_status NapiEnvironment::deleteElement(
    napi_value object,
    uint32_t index,
    bool *result) noexcept {
  return handleExceptions([&] {
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));
    return deleteComputed(objValue, index, result);
  });
}

napi_status NapiEnvironment::defineProperties(
    napi_value object,
    size_t propertyCount,
    const napi_property_descriptor *properties) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    if (propertyCount > 0) {
      CHECK_ARG(properties);
    }

    vm::Handle<vm::JSObject> objHandle = makeHandle<vm::JSObject>(object);
    for (size_t i = 0; i < propertyCount; ++i) {
      const napi_property_descriptor *p = &properties[i];
      vm::MutableHandle<vm::SymbolID> name{&runtime_};
      CHECK_NAPI(symbolIDFromPropertyDescriptor(p, &name));

      vm::DefinePropertyFlags dpFlags =
          vm::DefinePropertyFlags::getDefaultNewPropertyFlags();
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

        vm::CallResult<vm::HermesValue> propRes = vm::PropertyAccessor::create(
            &runtime_,
            runtime_.makeHandle<vm::Callable>(*phv(localGetter)),
            runtime_.makeHandle<vm::Callable>(*phv(localSetter)));
        CHECK_NAPI(checkHermesStatus(propRes));
        CHECK_NAPI(checkHermesStatus(vm::JSObject::defineOwnProperty(
            objHandle,
            &runtime_,
            name.get(),
            dpFlags,
            makeHandle(*propRes),
            vm::PropOpFlags().plusThrowOnError())));
      } else if (p->method != nullptr) {
        napi_value method{};
        CHECK_NAPI(newFunction(name.get(), p->getter, p->data, &method));
        CHECK_NAPI(checkHermesStatus(vm::JSObject::defineOwnProperty(
            objHandle,
            &runtime_,
            name.get(),
            dpFlags,
            makeHandle(method),
            vm::PropOpFlags().plusThrowOnError())));
      } else {
        CHECK_NAPI(checkHermesStatus(vm::JSObject::defineOwnProperty(
            objHandle,
            &runtime_,
            name.get(),
            dpFlags,
            makeHandle(p->value),
            vm::PropOpFlags().plusThrowOnError())));
      }
    }

    return clearLastError();
  });
}

napi_status NapiEnvironment::symbolIDFromPropertyDescriptor(
    const napi_property_descriptor *p,
    vm::MutableHandle<vm::SymbolID> *result) noexcept {
  if (p->utf8name != nullptr) {
    CHECK_NAPI(createSymbolID(p->utf8name, NAPI_AUTO_LENGTH, result));
  } else {
    RETURN_STATUS_IF_FALSE(p->name != nullptr, napi_name_expected);
    const vm::PinnedHermesValue &namePHV = *phv(p->name);
    if (namePHV.isString()) {
      CHECK_NAPI(createSymbolID(p->name, result));
    } else if (namePHV.isSymbol()) {
      *result = namePHV.getSymbol();
    } else {
      return ERROR_STATUS(
          napi_name_expected, "p->name must be String or Symbol");
    }
  }

  return napi_ok;
}

napi_status NapiEnvironment::objectFreeze(napi_value object) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_NAPI(checkHermesStatus(
        vm::JSObject::freeze(makeHandle<vm::JSObject>(object), &runtime_)));
    return clearLastError();
  });
}

napi_status NapiEnvironment::objectSeal(napi_value object) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_NAPI(checkHermesStatus(
        vm::JSObject::seal(makeHandle<vm::JSObject>(object), &runtime_)));
    return clearLastError();
  });
}

//-----------------------------------------------------------------------------
// Property access helpers
//-----------------------------------------------------------------------------

const vm::PinnedHermesValue &NapiEnvironment::getPredefined(
    NapiPredefined predefinedKey) noexcept {
  return predefinedValues_[static_cast<size_t>(predefinedKey)];
}

template <class TObject, class TValue>
napi_status NapiEnvironment::putPredefined(
    TObject object,
    NapiPredefined key,
    TValue value,
    bool *optResult) noexcept {
  return putNamed(object, getPredefined(key).getSymbol(), value, optResult);
}

template <class TObject>
napi_status NapiEnvironment::hasPredefined(
    TObject object,
    NapiPredefined key,
    bool *result) noexcept {
  return hasNamed(object, getPredefined(key).getSymbol(), result);
}

template <class TObject>
napi_status NapiEnvironment::getPredefined(
    TObject object,
    NapiPredefined key,
    napi_value *result) noexcept {
  return getNamed(object, getPredefined(key).getSymbol(), result);
}

template <class TObject, class TValue>
napi_status NapiEnvironment::putNamed(
    TObject object,
    vm::SymbolID name,
    TValue value,
    bool *optResult) noexcept {
  vm::CallResult<bool> res = vm::JSObject::putNamed_RJS(
      makeHandle<vm::JSObject>(object),
      &runtime_,
      name,
      makeHandle(value),
      vm::PropOpFlags().plusThrowOnError());
  return setOptionalResult(std::move(res), optResult);
}

template <class TObject>
napi_status NapiEnvironment::hasNamed(
    TObject object,
    vm::SymbolID name,
    bool *result) noexcept {
  vm::CallResult<bool> res =
      vm::JSObject::hasNamed(makeHandle<vm::JSObject>(object), &runtime_, name);
  return setResult(std::move(res), result);
}

template <class TObject>
napi_status NapiEnvironment::getNamed(
    TObject object,
    vm::SymbolID name,
    napi_value *result) noexcept {
  vm::CallResult<vm::PseudoHandle<>> res = vm::JSObject::getNamed_RJS(
      makeHandle<vm::JSObject>(object),
      &runtime_,
      name,
      vm::PropOpFlags().plusThrowOnError());
  return setResult(std::move(res), result);
}

template <class TObject, class TKey, class TValue>
napi_status NapiEnvironment::putComputed(
    TObject object,
    TKey key,
    TValue value,
    bool *optResult) noexcept {
  vm::CallResult<bool> res = vm::JSObject::putComputed_RJS(
      makeHandle<vm::JSObject>(object),
      &runtime_,
      makeHandle(key),
      makeHandle(value),
      vm::PropOpFlags().plusThrowOnError());
  return setOptionalResult(std::move(res), optResult);
}

template <class TObject, class TKey>
napi_status
NapiEnvironment::hasComputed(TObject object, TKey key, bool *result) noexcept {
  vm::CallResult<bool> res = vm::JSObject::hasComputed(
      makeHandle<vm::JSObject>(object), &runtime_, makeHandle(key));
  return setResult(std::move(res), result);
}

template <class TObject, class TKey>
napi_status NapiEnvironment::getComputed(
    TObject object,
    TKey key,
    napi_value *result) noexcept {
  vm::CallResult<vm::PseudoHandle<>> res = vm::JSObject::getComputed_RJS(
      makeHandle<vm::JSObject>(object), &runtime_, makeHandle(key));
  return setResult(std::move(res), result);
}

template <class TObject, class TKey>
napi_status NapiEnvironment::deleteComputed(
    TObject object,
    TKey key,
    bool *optResult) noexcept {
  vm::CallResult<bool> res = vm::JSObject::deleteComputed(
      makeHandle<vm::JSObject>(object),
      &runtime_,
      makeHandle(key),
      vm::PropOpFlags().plusThrowOnError());
  return setOptionalResult(std::move(res), optResult);
}

template <class TObject, class TKey>
napi_status NapiEnvironment::getOwnComputedDescriptor(
    TObject object,
    TKey key,
    vm::MutableHandle<vm::SymbolID> &tmpSymbolStorage,
    vm::ComputedPropertyDescriptor &desc,
    bool *result) noexcept {
  vm::CallResult<bool> res = vm::JSObject::getOwnComputedDescriptor(
      makeHandle<vm::JSObject>(object),
      &runtime_,
      makeHandle(key),
      tmpSymbolStorage,
      desc);
  return setResult(std::move(res), result);
}

//-----------------------------------------------------------------------------
// Methods to work with Arrays
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::isArray(napi_value value, bool *result) noexcept {
  CHECK_ARG(value);
  return setResult(vm::vmisa<vm::JSArray>(*phv(value)), result);
}

napi_status NapiEnvironment::getArrayLength(
    napi_value value,
    uint32_t *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    vm::Handle<vm::JSArray> arrHandle =
        vm::Handle<vm::JSArray>::vmcast_or_null(phv(value));
    RETURN_STATUS_IF_FALSE(arrHandle, napi_array_expected);
    napi_value res;
    CHECK_NAPI(getNamed(
        arrHandle, vm::Predefined::getSymbolID(vm::Predefined::length), &res));
    RETURN_STATUS_IF_FALSE(phv(res)->isNumber(), napi_number_expected);
    return setResult(static_cast<uint32_t>(phv(res)->getDouble()), result);
  });
}

//-----------------------------------------------------------------------------
// Methods to compare values
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::strictEquals(
    napi_value lhs,
    napi_value rhs,
    bool *result) noexcept {
  const vm::PinnedHermesValue &lhsHV = *phv(lhs);
  const vm::PinnedHermesValue &rhsHV = *phv(rhs);
  vm::TagKind lhsTag = lhsHV.getTag();
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

//-----------------------------------------------------------------------------
// Methods to work with Functions
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::callFunction(
    napi_value object,
    napi_value func,
    size_t argCount,
    const napi_value *args,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    // TODO: review arg checks
    CHECK_ARG(object);
    if (argCount > 0) {
      CHECK_ARG(args);
    }
    vm::Handle<vm::Callable> handle = makeHandle<vm::Callable>(func);
    if (argCount > std::numeric_limits<uint32_t>::max() ||
        !runtime_.checkAvailableStack((uint32_t)argCount)) {
      return GENERIC_FAILURE("Unable to call function: stack overflow");
    }

    vm::instrumentation::RuntimeStats &stats = runtime_.getRuntimeStats();
    const vm::instrumentation::RAIITimer timer{
        "Incoming Function", stats, stats.incomingFunction};
    vm::ScopedNativeCallFrame newFrame{
        &runtime_,
        static_cast<uint32_t>(argCount),
        handle.getHermesValue(),
        vm::HermesValue::encodeUndefinedValue(),
        *phv(object)};
    if (LLVM_UNLIKELY(newFrame.overflowed())) {
      CHECK_NAPI(checkHermesStatus(runtime_.raiseStackOverflow(
          vm::StackRuntime::StackOverflowKind::NativeStack)));
    }

    for (uint32_t i = 0; i < argCount; ++i) {
      newFrame->getArgRef(i) = *phv(args[i]);
    }
    vm::CallResult<vm::PseudoHandle<>> callRes =
        vm::Callable::call(handle, &runtime_);
    CHECK_NAPI(checkHermesStatus(callRes));

    if (result) {
      RETURN_FAILURE_IF_FALSE(!callRes->get().isEmpty());
      *result = addGCRootStackValue(callRes->get());
    }
    return clearLastError();
  });
}

napi_status NapiEnvironment::newInstance(
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

    RETURN_STATUS_IF_FALSE(
        vm::vmisa<vm::Callable>(*phv(constructor)), napi_function_expected);
    vm::Handle<vm::Callable> funcHandle = makeHandle<vm::Callable>(constructor);

    if (argc > std::numeric_limits<uint32_t>::max() ||
        !runtime_.checkAvailableStack((uint32_t)argc)) {
      return GENERIC_FAILURE("Unable to call function: stack overflow");
    }

    vm::instrumentation::RuntimeStats &stats = runtime_.getRuntimeStats();
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
    vm::CallResult<vm::PseudoHandle<vm::JSObject>> thisRes =
        vm::Callable::createThisForConstruct(funcHandle, &runtime_);
    CHECK_NAPI(checkHermesStatus(thisRes));
    // We need to capture this in case the ctor doesn't return an object,
    // we need to return this object.
    vm::Handle<vm::JSObject> objHandle = makeHandle(std::move(*thisRes));

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
      CHECK_NAPI(checkHermesStatus(runtime_.raiseStackOverflow(
          ::hermes::vm::StackRuntime::StackOverflowKind::NativeStack)));
    }
    for (uint32_t i = 0; i != argc; ++i) {
      newFrame->getArgRef(i) = *phv(argv[i]);
    }
    // The last parameter indicates that this call should construct an object.
    vm::CallResult<vm::PseudoHandle<>> callRes =
        vm::Callable::call(funcHandle, &runtime_);
    CHECK_NAPI(checkHermesStatus(callRes));

    // 13.2.2.9:
    //    If Type(result) is Object then return result
    // 13.2.2.10:
    //    Return obj
    vm::HermesValue resultValue = callRes->get();
    return setResult(
        resultValue.isObject() ? resultValue : objHandle.getHermesValue(),
        result);
  });
}

napi_status NapiEnvironment::instanceOf(
    napi_value object,
    napi_value constructor,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    CHECK_ARG(constructor);
    RETURN_STATUS_IF_FALSE(
        vm::vmisa<vm::Callable>(*phv(constructor)), napi_function_expected);
    return setResult(
        vm::instanceOfOperator_RJS(
            &runtime_, makeHandle(object), makeHandle(constructor)),
        result);
  });
}

//-----------------------------------------------------------------------------
// Methods to work with napi_callbacks
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::getCallbackInfo(
    napi_callback_info callbackInfo,
    size_t *argCount,
    napi_value *args,
    napi_value *thisArg,
    void **data) noexcept {
  CHECK_ARG(callbackInfo);
  CallbackInfo *cbInfo = asCallbackInfo(callbackInfo);
  if (args != nullptr) {
    CHECK_ARG(argCount);
    cbInfo->args(args, argCount);
  }

  if (argCount != nullptr) {
    *argCount = cbInfo->argCount();
  }

  if (thisArg != nullptr) {
    *thisArg = cbInfo->thisArg();
  }

  if (data != nullptr) {
    *data = cbInfo->nativeData();
  }

  return clearLastError();
}

napi_status NapiEnvironment::getNewTarget(
    napi_callback_info callbackInfo,
    napi_value *result) noexcept {
  CHECK_ARG(callbackInfo);
  return setResult(
      reinterpret_cast<CallbackInfo *>(callbackInfo)->getNewTarget(), result);
}

//-----------------------------------------------------------------------------
// Methods to work with external data objects
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::defineClass(
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

    vm::Handle<vm::JSObject> classHandle = makeHandle<vm::JSObject>(*result);
    vm::Handle<vm::JSObject> prototypeHandle =
        makeHandle(vm::JSObject::create(&runtime_));
    napi_value prototype =
        addGCRootStackValue(prototypeHandle.getHermesValue());
    vm::PropertyFlags pf;
    pf.clear();
    pf.enumerable = 0;
    pf.writable = 1;
    pf.configurable = 0;
    CHECK_NAPI(checkHermesStatus(vm::JSObject::defineNewOwnProperty(
        classHandle,
        &runtime_,
        vm::Predefined::getSymbolID(vm::Predefined::prototype),
        pf,
        prototypeHandle)));
    pf.configurable = 1;
    CHECK_NAPI(checkHermesStatus(vm::JSObject::defineNewOwnProperty(
        prototypeHandle,
        &runtime_,
        vm::Predefined::getSymbolID(vm::Predefined::constructor),
        pf,
        classHandle)));

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

napi_status NapiEnvironment::wrapObject(
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
    CHECK_NAPI(
        getExternalValue(object, IfNotFound::ThenCreate, &externalValue));
    RETURN_STATUS_IF_FALSE(!externalValue->nativeData(), napi_invalid_arg);

    Reference *reference{};
    CHECK_NAPI(FinalizingComplexReference::create(
        *this,
        0,
        phv(object),
        nativeData,
        finalizeCallback,
        finalizeHint,
        reinterpret_cast<FinalizingComplexReference **>(reference)));
    externalValue->setNativeData(reference);
    return setOptionalResult(reinterpret_cast<napi_ref>(reference), result);
  });
}

napi_status NapiEnvironment::addFinalizer(
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

napi_status NapiEnvironment::unwrapObject(
    napi_value object,
    UnwrapAction action,
    void **result) noexcept {
  return handleExceptions([&] {
    CHECK_OBJECT_ARG(object);
    if (action == UnwrapAction::KeepWrap) {
      CHECK_ARG(result);
    }

    ExternalValue *externalValue = getExternalValue(*phv(object));
    if (!externalValue) {
      CHECK_NAPI(
          getExternalValue(object, IfNotFound::ThenReturnNull, &externalValue));
      RETURN_STATUS_IF_FALSE(externalValue, napi_invalid_arg);
    }

    Reference *reference = asReference(externalValue->nativeData());
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

napi_status NapiEnvironment::typeTagObject(
    napi_value object,
    const napi_type_tag *typeTag) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(typeTag);
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));

    // Fail if the tag already exists
    bool hasTag{};
    CHECK_NAPI(hasPredefined(objValue, NapiPredefined::napi_typeTag, &hasTag));
    RETURN_STATUS_IF_FALSE(!hasTag, napi_invalid_arg);

    napi_value tagBuffer;
    void *tagBufferData;
    CHECK_NAPI(
        createArrayBuffer(sizeof(napi_type_tag), &tagBufferData, &tagBuffer));

    const uint8_t *source = reinterpret_cast<const uint8_t *>(typeTag);
    uint8_t *dest = reinterpret_cast<uint8_t *>(tagBufferData);
    std::copy(source, source + sizeof(napi_type_tag), dest);

    return putPredefined(objValue, NapiPredefined::napi_typeTag, tagBuffer);
  });
}

napi_status NapiEnvironment::checkObjectTypeTag(
    napi_value object,
    const napi_type_tag *typeTag,
    bool *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(typeTag);
    napi_value objValue;
    CHECK_NAPI(coerceToObject(object, &objValue));

    napi_value tagBufferValue;
    CHECK_NAPI(
        getPredefined(objValue, NapiPredefined::napi_typeTag, &tagBufferValue));
    vm::JSArrayBuffer *tagBuffer =
        vm::vmcast_or_null<vm::JSArrayBuffer>(*phv(tagBufferValue));
    RETURN_FAILURE_IF_FALSE(tagBuffer != nullptr);

    const uint8_t *source = reinterpret_cast<const uint8_t *>(typeTag);
    const uint8_t *tagBufferData = tagBuffer->getDataBlock();
    return setResult(
        std::equal(
            source,
            source + sizeof(napi_type_tag),
            tagBufferData,
            tagBufferData + sizeof(napi_type_tag)),
        result);
  });
}

napi_status NapiEnvironment::createExternal(
    void *nativeData,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(result);
    vm::PseudoHandle<vm::DecoratedObject> decoratedObj =
        createExternal(nativeData, nullptr);
    *result = addGCRootStackValue(decoratedObj.getHermesValue());
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

vm::PseudoHandle<vm::DecoratedObject> NapiEnvironment::createExternal(
    void *nativeData,
    ExternalValue **externalValue) noexcept {
  vm::PseudoHandle<vm::DecoratedObject> decoratedObj =
      vm::DecoratedObject::create(
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

napi_status NapiEnvironment::getValueExternal(
    napi_value value,
    void **result) noexcept {
  return handleExceptions([&] {
    ExternalValue *externalValue = getExternalValue(*phv(value));
    RETURN_STATUS_IF_FALSE(externalValue, napi_invalid_arg);
    return setResult(externalValue->nativeData(), result);
  });
}

ExternalValue *NapiEnvironment::getExternalValue(
    const vm::HermesValue &value) noexcept {
  if (vm::DecoratedObject *decoratedObj =
          vm::dyn_vmcast_or_null<vm::DecoratedObject>(value)) {
    vm::SmallHermesValue tag = vm::DecoratedObject::getAdditionalSlotValue(
        decoratedObj, &runtime_, kExternalTagSlot);
    if (tag.isNumber() && tag.getNumber(&runtime_) == kExternalValueTag) {
      return static_cast<ExternalValue *>(decoratedObj->getDecoration());
    }
  }

  return nullptr;
}

// TODO: simplify handling the external value
template <class TObject>
napi_status NapiEnvironment::getExternalValue(
    TObject object,
    IfNotFound ifNotFound,
    ExternalValue **result) noexcept {
  return handleExceptions([&] {
    ExternalValue *externalValue{};
    napi_value externalNapiValue;
    napi_status status = getPredefined(
        object, NapiPredefined::napi_externalValue, &externalNapiValue);
    if (status == napi_ok) {
      externalValue = getExternalValue(*phv(externalNapiValue));
      RETURN_FAILURE_IF_FALSE(externalValue != nullptr);
    } else if (ifNotFound == IfNotFound::ThenCreate) {
      vm::Handle<vm::DecoratedObject> decoratedObj =
          makeHandle(createExternal(nullptr, &externalValue));
      CHECK_NAPI(putPredefined(
          object, NapiPredefined::napi_externalValue, decoratedObj));
    }
    return setResult(std::move(externalValue), result);
  });
}

//-----------------------------------------------------------------------------
// Methods to control object lifespan
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::createReference(
    napi_value value,
    uint32_t initialRefCount,
    napi_ref *result) noexcept {
  return ComplexReference::create(
      *this,
      phv(value),
      initialRefCount,
      reinterpret_cast<ComplexReference **>(result));
}

napi_status NapiEnvironment::deleteReference(napi_ref ref) noexcept {
  CHECK_ARG(ref);
  return Reference::deleteReference(
      *this, asReference(ref), Reference::ReasonToDelete::ExternalCall);
}

napi_status NapiEnvironment::incReference(
    napi_ref ref,
    uint32_t *result) noexcept {
  CHECK_ARG(ref);
  uint32_t refCount{};
  CHECK_NAPI(asReference(ref)->incRefCount(*this, refCount));
  return setOptionalResult(std::move(refCount), result);
}

napi_status NapiEnvironment::decReference(
    napi_ref ref,
    uint32_t *result) noexcept {
  CHECK_ARG(ref);
  uint32_t refCount{};
  CHECK_NAPI(asReference(ref)->decRefCount(*this, refCount));
  return setOptionalResult(std::move(refCount), result);
}

napi_status NapiEnvironment::getReferenceValue(
    napi_ref ref,
    napi_value *result) noexcept {
  CHECK_ARG(ref);
  return setResult(asReference(ref)->value(env), result);
}

napi_status NapiEnvironment::addObjectFinalizer(
    const vm::PinnedHermesValue *value,
    Finalizer *finalizer) noexcept {
  return handleExceptions([&] {
    ExternalValue *externalValue = getExternalValue(*value);
    if (!externalValue) {
      CHECK_NAPI(
          getExternalValue(value, IfNotFound::ThenCreate, &externalValue));
    }

    externalValue->addFinalizer(finalizer);

    return clearLastError();
  });
}

template <class TLambda>
void NapiEnvironment::callIntoModule(TLambda &&call) noexcept {
  size_t openHandleScopesBefore = gcRootStackScopes_.size();
  clearLastError();
  call(this);
  CRASH_IF_FALSE(openHandleScopesBefore == gcRootStackScopes_.size());
  if (!lastException_.isEmpty()) {
    runtime_.setThrownValue(lastException_);
    lastException_ = EmptyHermesValue;
  }
}

napi_status NapiEnvironment::callFinalizer(
    napi_finalize finalizeCallback,
    void *nativeData,
    void *finalizeHint) noexcept {
  return handleExceptions([&] {
    callIntoModule([&](NapiEnvironment *env) {
      finalizeCallback(napiEnv(env), nativeData, finalizeHint);
    });
    return napi_ok;
  });
}

napi_status NapiEnvironment::runReferenceFinalizers() noexcept {
  if (!isRunningFinalizers_) {
    isRunningFinalizers_ = true;
    Reference::finalizeAll(*this, finalizerQueue_);
    isRunningFinalizers_ = false;
  }
  return napi_ok;
}

napi_status NapiEnvironment::createStrongReference(
    napi_value value,
    napi_ext_ref *result) noexcept {
  CHECK_ARG(value);
  return StrongReference::create(
      *this, *phv(value), reinterpret_cast<StrongReference **>(result));
}

napi_status NapiEnvironment::createStrongReferenceWithData(
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

napi_status NapiEnvironment::createWeakReference(
    napi_value value,
    napi_ext_ref *result) noexcept {
  return WeakReference::create(
      *this, phv(value), reinterpret_cast<WeakReference **>(result));
}

napi_status NapiEnvironment::incReference(napi_ext_ref ref) noexcept {
  CHECK_ARG(ref);
  uint32_t refCount{};
  return asReference(ref)->incRefCount(*this, /*ref*/ refCount);
}

napi_status NapiEnvironment::decReference(napi_ext_ref ref) noexcept {
  CHECK_ARG(ref);
  uint32_t refCount{};
  return asReference(ref)->decRefCount(*this, /*ref*/ refCount);
}

napi_status NapiEnvironment::getReferenceValue(
    napi_ext_ref ref,
    napi_value *result) noexcept {
  CHECK_ARG(ref);
  return setResult(asReference(ref)->value(*this), result);
}

napi_status NapiEnvironment::openHandleScope(
    napi_handle_scope *result) noexcept {
  size_t scope = gcRootStack_.size();
  gcRootStackScopes_.emplace(scope);
  return setResult(
      reinterpret_cast<napi_handle_scope>(&gcRootStackScopes_.top()), result);
}

napi_status NapiEnvironment::closeHandleScope(
    napi_handle_scope scope) noexcept {
  CHECK_ARG(scope);
  RETURN_STATUS_IF_FALSE(
      !gcRootStackScopes_.empty(), napi_handle_scope_mismatch);

  size_t &topScope = gcRootStackScopes_.top();
  RETURN_STATUS_IF_FALSE(
      reinterpret_cast<size_t *>(scope) == &topScope,
      napi_handle_scope_mismatch);

  gcRootStack_.resize(topScope);
  gcRootStackScopes_.pop();
  return clearLastError();
}

napi_status NapiEnvironment::openEscapableHandleScope(
    napi_escapable_handle_scope *result) noexcept {
  CHECK_ARG(result);

  // Escapable handle scope must have a parent scope
  RETURN_STATUS_IF_FALSE(
      !gcRootStackScopes_.empty(), napi_handle_scope_mismatch);

  gcRootStack_.emplace(); // value to escape to parent scope
  gcRootStack_.emplace(
      vm::HermesValue::encodeNativeUInt32(kEscapeableSentinelTag));

  return openHandleScope(reinterpret_cast<napi_handle_scope *>(result));
}

napi_status NapiEnvironment::closeEscapableHandleScope(
    napi_escapable_handle_scope scope) noexcept {
  CHECK_NAPI(closeHandleScope(reinterpret_cast<napi_handle_scope>(scope)));

  RETURN_STATUS_IF_FALSE(gcRootStack_.size() > 1, napi_handle_scope_mismatch);
  vm::PinnedHermesValue &sentinelTag = gcRootStack_.top();
  RETURN_STATUS_IF_FALSE(
      sentinelTag.isNativeValue(), napi_handle_scope_mismatch);
  uint32_t sentinelTagValue = sentinelTag.getNativeUInt32();
  RETURN_STATUS_IF_FALSE(
      sentinelTagValue == kEscapeableSentinelTag ||
          sentinelTagValue == kUsedEscapeableSentinelTag,
      napi_handle_scope_mismatch);

  gcRootStack_.pop();
  return clearLastError();
}

napi_status NapiEnvironment::escapeHandle(
    napi_escapable_handle_scope scope,
    napi_value escapee,
    napi_value *result) noexcept {
  CHECK_ARG(scope);
  CHECK_ARG(escapee);

  size_t *stackScope = reinterpret_cast<size_t *>(scope);
  RETURN_STATUS_IF_FALSE(*stackScope > 1, napi_invalid_arg);
  RETURN_STATUS_IF_FALSE(*stackScope <= gcRootStack_.size(), napi_invalid_arg);

  vm::PinnedHermesValue &sentinelTag = gcRootStack_[*stackScope - 1];
  RETURN_STATUS_IF_FALSE(sentinelTag.isNativeValue(), napi_invalid_arg);
  uint32_t sentinelTagValue = sentinelTag.getNativeUInt32();
  RETURN_STATUS_IF_FALSE(
      sentinelTagValue != kUsedEscapeableSentinelTag, napi_escape_called_twice);
  RETURN_STATUS_IF_FALSE(
      sentinelTagValue == kEscapeableSentinelTag, napi_invalid_arg);

  vm::PinnedHermesValue &escapedValue = gcRootStack_[*stackScope - 2];
  escapedValue = *phv(escapee);
  sentinelTag = vm::HermesValue::encodeNativeUInt32(kUsedEscapeableSentinelTag);

  return setResult(napiValue(&escapedValue), result);
}

void NapiEnvironment::addToFinalizerQueue(Finalizer *finalizer) noexcept {
  finalizerQueue_.pushBack(finalizer);
}

void NapiEnvironment::addGCRoot(Reference *reference) noexcept {
  gcRoots_.pushBack(reference);
}

void NapiEnvironment::addFinalizingGCRoot(Reference *reference) noexcept {
  finalizingGCRoots_.pushBack(reference);
}

void NapiEnvironment::pushOrderedSet(
    OrderedSet<vm::HermesValue> &set) noexcept {
  orderedSets_.push_back(&set);
}

void NapiEnvironment::popOrderedSet() noexcept {
  orderedSets_.pop_back();
}

napi_value NapiEnvironment::addGCRootStackValue(
    vm::HermesValue value) noexcept {
  gcRootStack_.emplace(value);
  return napiValue(&gcRootStack_.top());
}

vm::WeakRoot<vm::JSObject> NapiEnvironment::createWeakRoot(
    vm::JSObject *object) noexcept {
  return vm::WeakRoot<vm::JSObject>(object, &runtime_);
}

const vm::PinnedHermesValue &NapiEnvironment::lockWeakObject(
    vm::WeakRoot<vm::JSObject> &weakRoot) noexcept {
  if (vm::JSObject *ptr = weakRoot.get(&runtime_, &runtime_.getHeap())) {
    return *phv(addGCRootStackValue(vm::HermesValue::encodeObjectValue(ptr)));
  }
  return getPredefined(NapiPredefined::undefined);
}

//-----------------------------------------------------------------------------
// Methods to support JS error handling
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::throwError(napi_value error) noexcept {
  CHECK_ARG(error);
  runtime_.setThrownValue(*phv(error));
  // any VM calls after this point and before returning
  // to the javascript invoker will fail
  return clearLastError();
}

napi_status NapiEnvironment::throwError(
    const vm::PinnedHermesValue &prototype,
    const char *code,
    const char *message) noexcept {
  return handleExceptions([&] {
    // TODO: cleanup intermediate napi_value before return
    napi_value messageValue;
    CHECK_NAPI(createStringUTF8(message, &messageValue));

    vm::Handle<vm::JSError> errorHandle = makeHandle(
        vm::JSError::create(&runtime_, makeHandle<vm::JSObject>(&prototype)));
    CHECK_NAPI(checkHermesStatus(
        vm::JSError::recordStackTrace(errorHandle, &runtime_)));
    CHECK_NAPI(
        checkHermesStatus(vm::JSError::setupStack(errorHandle, &runtime_)));
    CHECK_NAPI(checkHermesStatus(vm::JSError::setMessage(
        errorHandle, &runtime_, makeHandle(messageValue))));
    CHECK_NAPI(setErrorCode(errorHandle, nullptr, code));

    runtime_.setThrownValue(errorHandle.getHermesValue());

    // any VM calls after this point and before returning
    // to the javascript invoker will fail
    return clearLastError();
  });
}

napi_status NapiEnvironment::throwError(
    const char *code,
    const char *message) noexcept {
  return throwError(runtime_.ErrorPrototype, code, message);
}

napi_status NapiEnvironment::throwTypeError(
    const char *code,
    const char *message) noexcept {
  return throwError(runtime_.TypeErrorPrototype, code, message);
}

napi_status NapiEnvironment::throwRangeError(
    const char *code,
    const char *message) noexcept {
  return throwError(runtime_.RangeErrorPrototype, code, message);
}

napi_status NapiEnvironment::isError(napi_value value, bool *result) noexcept {
  CHECK_ARG(value);
  return setResult(vm::vmisa<vm::JSError>(*phv(value)), result);
}

napi_status NapiEnvironment::setErrorCode(
    vm::Handle<vm::JSError> error,
    napi_value code,
    const char *codeCString) noexcept {
  if (code || codeCString) {
    if (code) {
      CHECK_STRING_ARG(code);
    } else {
      CHECK_NAPI(createStringUTF8(codeCString, &code));
    }
    return putPredefined(error, NapiPredefined::code, code, nullptr);
  }
  return napi_ok;
}

//-----------------------------------------------------------------------------
// Methods to support catching JS exceptions
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::isExceptionPending(bool *result) noexcept {
  return setResult(!lastException_.isEmpty(), result);
}

napi_status NapiEnvironment::getAndClearLastException(
    napi_value *result) noexcept {
  if (lastException_.isEmpty()) {
    return getUndefined(result);
  }
  return setResult(std::exchange(lastException_, EmptyHermesValue), result);
}

//-----------------------------------------------------------------------------
// Methods to work with array buffers and typed arrays
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::isArrayBuffer(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  return setResult(vm::vmisa<vm::JSArrayBuffer>(*phv(value)), result);
}

napi_status NapiEnvironment::createArrayBuffer(
    size_t byteLength,
    void **data,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    vm::Handle<vm::JSArrayBuffer> buffer = makeHandle(vm::JSArrayBuffer::create(
        &runtime_, makeHandle<vm::JSObject>(&runtime_.arrayBufferPrototype)));
    CHECK_NAPI(
        checkHermesStatus(buffer->createDataBlock(&runtime_, byteLength)));
    if (data != nullptr) {
      *data = buffer->getDataBlock();
    }
    return setResult(std::move(buffer), result);
  });
}

napi_status NapiEnvironment::createExternalArrayBuffer(
    void *externalData,
    size_t byteLength,
    napi_finalize finalizeCallback,
    void *finalizeHint,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    vm::Handle<vm::JSArrayBuffer> buffer = makeHandle(vm::JSArrayBuffer::create(
        &runtime_, makeHandle<vm::JSObject>(&runtime_.arrayBufferPrototype)));
    std::unique_ptr<ExternalBuffer> externalBuffer(new ExternalBuffer(
        env, externalData, byteLength, finalizeCallback, finalizeHint));
    buffer->setExternalBuffer(&runtime_, std::move(externalBuffer));
    return setResult(std::move(buffer), result);
  });
}

napi_status NapiEnvironment::getArrayBufferInfo(
    napi_value arrayBuffer,
    void **data,
    size_t *byteLength) noexcept {
  CHECK_ARG(arrayBuffer);
  RETURN_STATUS_IF_FALSE(
      vm::vmisa<vm::JSArrayBuffer>(*phv(arrayBuffer)), napi_invalid_arg);

  vm::JSArrayBuffer *buffer = vm::vmcast<vm::JSArrayBuffer>(*phv(arrayBuffer));
  if (data != nullptr) {
    *data = buffer->attached() ? buffer->getDataBlock() : nullptr;
  }

  if (byteLength != nullptr) {
    *byteLength = buffer->attached() ? buffer->size() : 0;
  }

  return clearLastError();
}

napi_status NapiEnvironment::detachArrayBuffer(
    napi_value arrayBuffer) noexcept {
  CHECK_ARG(arrayBuffer);
  vm::JSArrayBuffer *buffer =
      vm::vmcast_or_null<vm::JSArrayBuffer>(*phv(arrayBuffer));
  RETURN_STATUS_IF_FALSE(buffer, napi_arraybuffer_expected);
  buffer->detach(&runtime_.getHeap());
  return clearLastError();
}

napi_status NapiEnvironment::isDetachedArrayBuffer(
    napi_value arrayBuffer,
    bool *result) noexcept {
  CHECK_ARG(arrayBuffer);
  vm::JSArrayBuffer *buffer =
      vm::vmcast_or_null<vm::JSArrayBuffer>(*phv(arrayBuffer));
  RETURN_STATUS_IF_FALSE(buffer, napi_arraybuffer_expected);
  return setResult(buffer->attached(), result);
}

napi_status NapiEnvironment::isTypedArray(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  return setResult(vm::vmisa<vm::JSTypedArrayBase>(*phv(value)), result);
}

template <vm::CellKind CellKind>
/*static*/ constexpr const char *NapiEnvironment::getTypedArrayName() noexcept {
  static constexpr const char *names[] = {
#define TYPED_ARRAY(name, type) #name "Array",
#include "hermes/VM/TypedArrays.def"
  };
  return names
      [static_cast<int>(CellKind) -
       static_cast<int>(vm::CellKind::TypedArrayBaseKind_first)];
}

template <class TElement, vm::CellKind CellKind>
napi_status NapiEnvironment::createTypedArray(
    size_t length,
    vm::JSArrayBuffer *buffer,
    size_t byteOffset,
    vm::JSTypedArrayBase **result) noexcept {
  constexpr size_t elementSize = sizeof(TElement);
  if (elementSize > 1) {
    if (byteOffset % elementSize != 0) {
      StringBuilder sb(
          "start offset of ",
          getTypedArrayName<CellKind>(),
          " should be a multiple of ",
          elementSize);
      return env.throwRangeError(
          "ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT", sb.c_str());
    }
  }
  if (length * elementSize + byteOffset > buffer->size()) {
    return env.throwRangeError(
        "ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT", "Invalid typed array length");
  }
  using TypedArray = vm::JSTypedArray<TElement, CellKind>;
  vm::PseudoHandle<TypedArray> arrayHandle =
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

napi_status NapiEnvironment::createTypedArray(
    napi_typedarray_type type,
    size_t length,
    napi_value arrayBuffer,
    size_t byteOffset,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(arrayBuffer);

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
        return GENERIC_FAILURE(
            "BigInt64Array is not implemented in Hermes yet");
      case napi_biguint64_array:
        return GENERIC_FAILURE(
            "BigUint64Array is not implemented in Hermes yet");
      default:
        return ERROR_STATUS(
            napi_invalid_arg, "Unsupported TypedArray type: ", type);
    }

    return setResult(vm::HermesValue::encodeObjectValue(typedArray), result);
  });
}

napi_status NapiEnvironment::getTypedArrayInfo(
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
      return GENERIC_FAILURE("Unknown TypedArray type");
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
        ? addGCRootStackValue(
              vm::HermesValue::encodeObjectValue(array->getBuffer(&runtime_)))
        : (napi_value)&getPredefined(NapiPredefined::undefined);
  }

  if (byteOffset != nullptr) {
    *byteOffset = array->getByteOffset();
  }

  return clearLastError();
}

napi_status NapiEnvironment::createDataView(
    size_t byteLength,
    napi_value arrayBuffer,
    size_t byteOffset,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(arrayBuffer);

    vm::JSArrayBuffer *buffer =
        vm::vmcast_or_null<vm::JSArrayBuffer>(*phv(arrayBuffer));
    RETURN_STATUS_IF_FALSE(buffer, napi_invalid_arg);

    if (byteLength + byteOffset > buffer->size()) {
      return env.throwRangeError(
          "ERR_NAPI_INVALID_DATAVIEW_ARGS",
          "byte_offset + byte_length should be less than or "
          "equal to the size in bytes of the array passed in");
    }
    vm::PseudoHandle<vm::JSDataView> viewHandle = vm::JSDataView::create(
        &runtime_, makeHandle<vm::JSObject>(&runtime_.dataViewPrototype));
    viewHandle->setBuffer(&runtime_, buffer, byteOffset, byteLength);
    return setResult(std::move(viewHandle), result);
  });
}

napi_status NapiEnvironment::isDataView(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);
  return setResult(vm::vmisa<vm::JSDataView>(*phv(value)), result);
}

napi_status NapiEnvironment::getDataViewInfo(
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
        ? addGCRootStackValue(view->getBuffer(&runtime_).getHermesValue())
        : (napi_value)&getPredefined(NapiPredefined::undefined);
  }

  if (byteOffset != nullptr) {
    *byteOffset = view->byteOffset();
  }

  return clearLastError();
}

//-----------------------------------------------------------------------------
// Version management
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::getVersion(uint32_t *result) noexcept {
  return setResult(static_cast<uint32_t>(NAPI_VERSION), result);
}

//-----------------------------------------------------------------------------
// Methods to work with Promises
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::createPromise(
    napi_value *promise,
    napi_value *resolveFunction,
    napi_value *rejectFunction) noexcept {
  napi_value global;
  napi_value promiseConstructor;
  CHECK_NAPI(getGlobal(&global));
  CHECK_NAPI(getPredefined(
      makeHandle<vm::JSObject>(global),
      NapiPredefined::Promise,
      &promiseConstructor));

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
      *resolve = env_->addGCRootStackValue(args.getArg(0));
      *reject = env_->addGCRootStackValue(args.getArg(1));
      return vm::HermesValue();
    }

    NapiEnvironment *env_{};
    napi_value *resolve{};
    napi_value *reject{};
  } executorData{this, resolveFunction, rejectFunction};

  vm::Handle<vm::NativeFunction> executorFunction =
      vm::NativeFunction::createWithoutPrototype(
          &runtime_,
          &executorData,
          &ExecutorData::callback,
          getPredefined(NapiPredefined::Promise).getSymbol(),
          2);
  napi_value func = addGCRootStackValue(executorFunction.getHermesValue());
  return newInstance(promiseConstructor, 1, &func, promise);
}

napi_status NapiEnvironment::createPromise(
    napi_deferred *deferred,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(deferred);

    napi_value jsPromise{}, jsResolve{}, jsReject{}, jsDeferred{};
    CHECK_NAPI(createPromise(&jsPromise, &jsResolve, &jsReject));
    CHECK_NAPI(createObject(&jsDeferred));
    CHECK_NAPI(putPredefined(jsDeferred, NapiPredefined::resolve, jsResolve));
    CHECK_NAPI(putPredefined(jsDeferred, NapiPredefined::reject, jsReject));

    CHECK_NAPI(StrongReference::create(
        *this,
        *phv(jsDeferred),
        reinterpret_cast<StrongReference **>(deferred)));
    return setResult(jsPromise, result);
  });
}

napi_status NapiEnvironment::resolveDeferred(
    napi_deferred deferred,
    napi_value resolution) noexcept {
  return concludeDeferred(deferred, NapiPredefined::resolve, resolution);
}

napi_status NapiEnvironment::rejectDeferred(
    napi_deferred deferred,
    napi_value resolution) noexcept {
  return concludeDeferred(deferred, NapiPredefined::reject, resolution);
}

napi_status NapiEnvironment::concludeDeferred(
    napi_deferred deferred,
    NapiPredefined predefinedProperty,
    napi_value result) noexcept {
  CHECK_ARG(deferred);
  CHECK_ARG(result);

  Reference *ref = asReference(deferred);

  const vm::PinnedHermesValue &jsDeferred = ref->value(*this);
  napi_value resolver, callResult;
  CHECK_NAPI(getPredefined(
      makeHandle<vm::JSObject>(&jsDeferred), predefinedProperty, &resolver));
  CHECK_NAPI(callFunction(nullptr, resolver, 1, &result, &callResult));
  Reference::deleteReference(
      *this, ref, Reference::ReasonToDelete::ZeroRefCount);
  return clearLastError();
}

napi_status NapiEnvironment::isPromise(
    napi_value value,
    bool *result) noexcept {
  CHECK_ARG(value);

  napi_value global;
  napi_value promiseConstructor;
  CHECK_NAPI(getGlobal(&global));
  CHECK_NAPI(getPredefined(
      makeHandle<vm::JSObject>(global),
      NapiPredefined::Promise,
      &promiseConstructor));

  return instanceOf(value, promiseConstructor, result);
}

//-----------------------------------------------------------------------------
// Memory management
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::adjustExternalMemory(
    int64_t change_in_bytes,
    int64_t *adjusted_value) noexcept {
  return GENERIC_FAILURE("Not implemented");
}

napi_status NapiEnvironment::collectGarbage() noexcept {
  runtime_.collect("test");
  CHECK_NAPI(runReferenceFinalizers());
  return clearLastError();
}

//-----------------------------------------------------------------------------
// Methods to work with Dates
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::createDate(
    double dateTime,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    vm::PseudoHandle<vm::JSDate> dateHandle = vm::JSDate::create(
        &runtime_, dateTime, makeHandle<vm::JSObject>(&runtime_.datePrototype));
    return setResult(std::move(dateHandle), result);
  });
}

napi_status NapiEnvironment::isDate(napi_value value, bool *result) noexcept {
  CHECK_ARG(value);
  return setResult(vm::vmisa<vm::JSDate>(*phv(value)), result);
}

napi_status NapiEnvironment::getDateValue(
    napi_value value,
    double *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(value);
    vm::JSDate *date = vm::vmcast_or_null<vm::JSDate>(*phv(value));
    RETURN_STATUS_IF_FALSE(date, napi_date_expected);
    return setResult(date->getPrimitiveValue(), result);
  });
}

//-----------------------------------------------------------------------------
// Instance data
//-----------------------------------------------------------------------------

napi_status NapiEnvironment::setInstanceData(
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

napi_status NapiEnvironment::getInstanceData(void **nativeData) noexcept {
  return setResult(
      instanceData_ ? instanceData_->nativeData() : nullptr, nativeData);
}

//---------------------------------------------------------------------------
// Script running
//---------------------------------------------------------------------------

napi_status NapiEnvironment::runScript(
    napi_value source,
    const char *sourceURL,
    napi_value *result) noexcept {
  size_t sourceSize{};
  CHECK_NAPI(getValueStringUTF8(source, nullptr, 0, &sourceSize));
  std::unique_ptr<char[]> buffer =
      std::unique_ptr<char[]>(new char[sourceSize + 1]);
  CHECK_NAPI(getValueStringUTF8(source, buffer.get(), sourceSize + 1, nullptr));
  return runScriptWithSourceMap(
      ExternalBuffer::make(
          napiEnv(this),
          napi_ext_buffer{
              buffer.release(),
              sourceSize + 1,
              [](napi_env /*env*/, void *data, void * /*finalizeHint*/) {
                std::unique_ptr<char[]> buf(reinterpret_cast<char *>(data));
              },
              nullptr}),
      nullptr,
      sourceURL,
      result);
}

napi_status NapiEnvironment::runSerializedScript(
    const uint8_t *buffer,
    size_t bufferLength,
    napi_value /*source*/,
    const char *sourceURL,
    napi_value *result) noexcept {
  std::unique_ptr<uint8_t[]> bufferCopy =
      std::unique_ptr<uint8_t[]>(new uint8_t[bufferLength]);
  std::copy(buffer, buffer + bufferLength, bufferCopy.get());
  return runScriptWithSourceMap(
      ExternalBuffer::make(
          napiEnv(this),
          napi_ext_buffer{
              bufferCopy.release(),
              bufferLength,
              [](napi_env /*env*/, void *data, void * /*finalizeHint*/) {
                std::unique_ptr<uint8_t[]> buf(
                    reinterpret_cast<uint8_t *>(data));
              },
              nullptr}),
      nullptr,
      sourceURL,
      result);
}

napi_status NapiEnvironment::serializeScript(
    napi_value source,
    const char *sourceURL,
    napi_ext_buffer_callback bufferCallback,
    void *bufferHint) noexcept {
  size_t sourceSize{};
  CHECK_NAPI(getValueStringUTF8(source, nullptr, 0, &sourceSize));
  std::unique_ptr<char[]> buffer =
      std::unique_ptr<char[]>(new char[sourceSize + 1]);
  CHECK_NAPI(getValueStringUTF8(source, buffer.get(), sourceSize + 1, nullptr));
  napi_ext_prepared_script preparedScript{};
  CHECK_NAPI(prepareScriptWithSourceMap(
      ExternalBuffer::make(
          napiEnv(this),
          napi_ext_buffer{
              buffer.release(),
              sourceSize + 1,
              [](napi_env /*env*/, void *data, void * /*finalizeHint*/) {
                std::unique_ptr<char[]> buf(reinterpret_cast<char *>(data));
              },
              nullptr}),
      nullptr,
      sourceURL,
      &preparedScript));
  return serializePreparedScript(preparedScript, bufferCallback, bufferHint);
}

napi_status NapiEnvironment::runScriptWithSourceMap(
    std::unique_ptr<hermes::Buffer> script,
    std::unique_ptr<hermes::Buffer> sourceMap,
    const char *sourceURL,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    napi_ext_prepared_script preparedScript{nullptr};
    CHECK_NAPI(prepareScriptWithSourceMap(
        std::move(script), std::move(sourceMap), sourceURL, &preparedScript));
    return runPreparedScript(preparedScript, result);
  });
}

napi_status NapiEnvironment::prepareScriptWithSourceMap(
    std::unique_ptr<hermes::Buffer> buffer,
    std::unique_ptr<hermes::Buffer> sourceMapBuf,
    const char *sourceURL,
    napi_ext_prepared_script *preparedScript) noexcept {
  std::pair<std::unique_ptr<hbc::BCProvider>, std::string> bcErr{};
  vm::RuntimeModuleFlags runtimeFlags{};
  runtimeFlags.persistent = true;

  bool isBytecode = isHermesBytecode(buffer->data(), buffer->size());
  // Save the first few bytes of the buffer so that we can later append them
  // to any error message.
  uint8_t bufPrefix[16];
  const size_t bufSize = buffer->size();
  memcpy(bufPrefix, buffer->data(), std::min(sizeof(bufPrefix), bufSize));

  // Construct the BC provider either from buffer or source.
  if (isBytecode) {
    if (sourceMapBuf) {
      return GENERIC_FAILURE("Source map cannot be specified with bytecode");
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
        return GENERIC_FAILURE(
            "Error parsing source map: ", diag.getErrorString());
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
    for (size_t i = 0; i < sizeof(bufPrefix) && i < bufSize; ++i) {
      os << llvh::format_hex_no_prefix(bufPrefix[i], 2);
    }
    return GENERIC_FAILURE("Compiling JS failed: ", bcErr.second, os.str());
  }
  *preparedScript =
      reinterpret_cast<napi_ext_prepared_script>(new HermesPreparedJavaScript(
          std::move(bcErr.first),
          runtimeFlags,
          sourceURL ? sourceURL : "",
          isBytecode));
  return clearLastError();
}

napi_status NapiEnvironment::runPreparedScript(
    napi_ext_prepared_script preparedScript,
    napi_value *result) noexcept {
  return handleExceptions([&] {
    CHECK_ARG(preparedScript);
    vm::instrumentation::RuntimeStats &stats = runtime_.getRuntimeStats();
    const vm::instrumentation::RAIITimer timer{
        "Evaluate JS", stats, stats.evaluateJS};
    const HermesPreparedJavaScript *hermesPrep =
        reinterpret_cast<HermesPreparedJavaScript *>(preparedScript);
    vm::CallResult<vm::HermesValue> res = runtime_.runBytecode(
        hermesPrep->bytecodeProvider(),
        hermesPrep->runtimeFlags(),
        hermesPrep->sourceURL(),
        vm::Runtime::makeNullHandle<vm::Environment>());
    return setResult(std::move(res), result);
  });
}

napi_status NapiEnvironment::deletePreparedScript(
    napi_ext_prepared_script preparedScript) noexcept {
  CHECK_ARG(preparedScript);
  delete reinterpret_cast<HermesPreparedJavaScript *>(preparedScript);
  return clearLastError();
}

napi_status NapiEnvironment::serializePreparedScript(
    napi_ext_prepared_script preparedScript,
    napi_ext_buffer_callback bufferCallback,
    void *bufferHint) noexcept {
  CHECK_ARG(preparedScript);
  CHECK_ARG(bufferCallback);

  HermesPreparedJavaScript *hermesPreparedScript =
      reinterpret_cast<HermesPreparedJavaScript *>(preparedScript);

  if (hermesPreparedScript->isBytecode()) {
    std::shared_ptr<hbc::BCProviderFromBuffer> bytecodeProvider =
        std::static_pointer_cast<hbc::BCProviderFromBuffer>(
            hermesPreparedScript->bytecodeProvider());
    llvh::ArrayRef<uint8_t> bufferRef = bytecodeProvider->getRawBuffer();
    bufferCallback(
        napiEnv(this), bufferRef.data(), bufferRef.size(), bufferHint);
  } else {
    std::shared_ptr<hbc::BCProviderFromSrc> bytecodeProvider =
        std::static_pointer_cast<hbc::BCProviderFromSrc>(
            hermesPreparedScript->bytecodeProvider());
    hbc::BytecodeModule *bcModule = bytecodeProvider->getBytecodeModule();

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
    BytecodeGenerationOptions bytecodeGenOpts =
        BytecodeGenerationOptions::defaults();
    llvh::SmallVector<char, 0> bytecodeVector;
    llvh::raw_svector_ostream OS(bytecodeVector);
    hbc::BytecodeSerializer BS{OS, bytecodeGenOpts};
    BS.serialize(*bcModule, bytecodeProvider->getSourceHash());
    bufferCallback(
        napiEnv(this),
        reinterpret_cast<uint8_t *>(bytecodeVector.data()),
        bytecodeVector.size(),
        bufferHint);
  }

  return clearLastError();
}

/*static*/ bool NapiEnvironment::isHermesBytecode(
    const uint8_t *data,
    size_t len) noexcept {
  return hbc::BCProviderFromBuffer::isBytecodeStream(
      llvh::ArrayRef<uint8_t>(data, len));
}

//---------------------------------------------------------------------------
// Handle creation helpers
//---------------------------------------------------------------------------

vm::Handle<> NapiEnvironment::makeHandle(napi_value value) noexcept {
  return makeHandle(phv(value));
}

vm::Handle<> NapiEnvironment::makeHandle(
    const vm::PinnedHermesValue *value) noexcept {
  return vm::Handle<>(value);
}

vm::Handle<> NapiEnvironment::makeHandle(vm::HermesValue value) noexcept {
  return vm::Handle<>(&runtime_, value);
}

vm::Handle<> NapiEnvironment::makeHandle(vm::Handle<> value) noexcept {
  return value;
}

vm::Handle<> NapiEnvironment::makeHandle(uint32_t value) noexcept {
  return makeHandle(vm::HermesValue::encodeDoubleValue(value));
}

template <class T>
vm::Handle<T> NapiEnvironment::makeHandle(napi_value value) noexcept {
  return vm::Handle<T>::vmcast(phv(value));
}

template <class T>
vm::Handle<T> NapiEnvironment::makeHandle(
    const vm::PinnedHermesValue *value) noexcept {
  return vm::Handle<T>::vmcast(value);
}

template <class T>
vm::Handle<T> NapiEnvironment::makeHandle(vm::Handle<T> value) noexcept {
  return value;
}

template <class T>
vm::Handle<T> NapiEnvironment::makeHandle(
    vm::PseudoHandle<T> &&value) noexcept {
  return runtime_.makeHandle(std::move(value));
}

template <class T>
vm::CallResult<vm::Handle<T>> NapiEnvironment::makeHandle(
    vm::CallResult<vm::PseudoHandle<T>> &&callResult) noexcept {
  if (callResult.getStatus() == vm::ExecutionStatus::EXCEPTION) {
    return vm::ExecutionStatus::EXCEPTION;
  }
  return runtime_.makeHandle(std::move(*callResult));
}

template <class T>
vm::CallResult<vm::MutableHandle<T>> NapiEnvironment::makeMutableHandle(
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

//---------------------------------------------------------------------------
// Result setting helpers
//---------------------------------------------------------------------------

template <class T, class TResult>
napi_status NapiEnvironment::setResult(T &&value, TResult *result) noexcept {
  CHECK_ARG(result);
  return setResultUnsafe(std::forward<T>(value), result);
}

template <class T, class TResult>
napi_status NapiEnvironment::setOptionalResult(
    T &&value,
    TResult *result) noexcept {
  if (result) {
    return setResultUnsafe(std::forward<T>(value), result);
  }
  return clearLastError();
}

template <class T>
napi_status NapiEnvironment::setOptionalResult(
    T && /*value*/,
    std::nullptr_t) noexcept {
  return clearLastError();
}

napi_status NapiEnvironment::setPredefinedResult(
    const vm::PinnedHermesValue *value,
    napi_value *result) noexcept {
  CHECK_ARG(result);
  *result = napiValue(value);
  return clearLastError();
}

template <class T, class TResult>
napi_status NapiEnvironment::setResultAndRunFinalizers(
    T &&value,
    TResult *result) noexcept {
  CHECK_NAPI(setResult(std::forward<T>(value), result));
  return runReferenceFinalizers();
}

template <class T>
napi_status NapiEnvironment::setResultUnsafe(T &&value, T *result) noexcept {
  *result = std::forward<T>(value);
  return clearLastError();
}

napi_status NapiEnvironment::setResultUnsafe(
    vm::HermesValue value,
    napi_value *result) noexcept {
  *result = addGCRootStackValue(value);
  return clearLastError();
}

napi_status NapiEnvironment::setResultUnsafe(
    vm::SymbolID value,
    napi_value *result) noexcept {
  return setResultUnsafe(vm::HermesValue::encodeSymbolValue(value), result);
}

napi_status NapiEnvironment::setResultUnsafe(
    bool value,
    napi_value *result) noexcept {
  return setResultUnsafe(vm::HermesValue::encodeBoolValue(value), result);
}

template <class T>
napi_status NapiEnvironment::setResultUnsafe(
    vm::Handle<T> &&handle,
    napi_value *result) noexcept {
  return setResultUnsafe(handle.getHermesValue(), result);
}

template <class T>
napi_status NapiEnvironment::setResultUnsafe(
    vm::PseudoHandle<T> &&handle,
    napi_value *result) noexcept {
  return setResultUnsafe(handle.getHermesValue(), result);
}

template <class T>
napi_status NapiEnvironment::setResultUnsafe(
    vm::Handle<T> &&handle,
    vm::MutableHandle<T> *result) noexcept {
  *result = std::move(handle);
  return clearLastError();
}

napi_status NapiEnvironment::setResultUnsafe(
    vm::HermesValue value,
    vm::MutableHandle<> *result) noexcept {
  *result = value;
  return clearLastError();
}

template <class T, class TResult>
napi_status NapiEnvironment::setResultUnsafe(
    vm::CallResult<T> &&value,
    TResult *result) noexcept {
  return setResultUnsafe(std::move(value), napi_generic_failure, result);
}

template <class T, class TResult>
napi_status NapiEnvironment::setResultUnsafe(
    vm::CallResult<T> &&value,
    napi_status onException,
    TResult *result) noexcept {
  CHECK_NAPI(checkHermesStatus(value, onException));
  return setResultUnsafe(std::move(*value), result);
}

} // namespace
} // namespace napi
} // namespace hermes

//=============================================================================
// NAPI implementation
//=============================================================================

//-----------------------------------------------------------------------------
// Native error handling functions
//-----------------------------------------------------------------------------

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
  return CHECKED_ENV(env)->createStringUTF8(str, length, result);
}

napi_status __cdecl napi_create_string_utf16(
    napi_env env,
    const char16_t *str,
    size_t length,
    napi_value *result) {
  return CHECKED_ENV(env)->createStringUTF16(str, length, result);
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
  return CHECKED_ENV(env)->getValueStringUTF8(value, buf, bufsize, result);
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
  return CHECKED_ENV(env)->getValueStringUTF16(value, buf, bufsize, result);
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
  return CHECKED_ENV(env)->getCallbackInfo(cbinfo, argc, argv, this_arg, data);
}

napi_status __cdecl napi_get_new_target(
    napi_env env,
    napi_callback_info cbinfo,
    napi_value *result) {
  return CHECKED_ENV(env)->getNewTarget(cbinfo, result);
}

//-----------------------------------------------------------------------------
// Methods to work with external data objects
//-----------------------------------------------------------------------------

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
// Methods to support JS error handling
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
  return CHECKED_ENV_GENERIC_FAILURE(
      env, "BigInt is not implemented by Hermes");
}

napi_status __cdecl napi_create_bigint_uint64(
    napi_env env,
    uint64_t value,
    napi_value *result) {
  return CHECKED_ENV_GENERIC_FAILURE(
      env, "BigInt is not implemented by Hermes");
}

napi_status __cdecl napi_create_bigint_words(
    napi_env env,
    int sign_bit,
    size_t word_count,
    const uint64_t *words,
    napi_value *result) {
  return CHECKED_ENV_GENERIC_FAILURE(
      env, "BigInt is not implemented by Hermes");
}

napi_status __cdecl napi_get_value_bigint_int64(
    napi_env env,
    napi_value value,
    int64_t *result,
    bool *lossless) {
  return CHECKED_ENV_GENERIC_FAILURE(
      env, "BigInt is not implemented by Hermes");
}

napi_status __cdecl napi_get_value_bigint_uint64(
    napi_env env,
    napi_value value,
    uint64_t *result,
    bool *lossless) {
  return CHECKED_ENV_GENERIC_FAILURE(
      env, "BigInt is not implemented by Hermes");
}

napi_status __cdecl napi_get_value_bigint_words(
    napi_env env,
    napi_value value,
    int *sign_bit,
    size_t *word_count,
    uint64_t *words) {
  return CHECKED_ENV_GENERIC_FAILURE(
      env, "BigInt is not implemented by Hermes");
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
  *env = hermes::napi::napiEnv(new hermes::napi::NapiEnvironment());
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
  return napi_open_handle_scope(
      env, reinterpret_cast<napi_handle_scope *>(result));
}

napi_status __cdecl napi_ext_close_env_scope(
    napi_env env,
    napi_ext_env_scope scope) {
  return napi_close_handle_scope(
      env, reinterpret_cast<napi_handle_scope>(scope));
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
  return CHECKED_ENV(env)->getUniqueStringRef(str, length, result);
}

napi_status __cdecl napi_ext_get_unique_string_ref(
    napi_env env,
    napi_value str_value,
    napi_ext_ref *result) {
  return CHECKED_ENV(env)->getUniqueStringRef(str_value, result);
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
    napi_ext_buffer source_map,
    const char *source_url,
    napi_value *result) {
  return CHECKED_ENV(env)->runScriptWithSourceMap(
      hermes::napi::ExternalBuffer::make(env, script),
      hermes::napi::ExternalBuffer::make(env, source_map),
      source_url,
      result);
}

napi_status __cdecl napi_ext_prepare_script_with_source_map(
    napi_env env,
    napi_ext_buffer script,
    napi_ext_buffer source_map,
    const char *source_url,
    napi_ext_prepared_script *prepared_script) {
  return CHECKED_ENV(env)->prepareScriptWithSourceMap(
      hermes::napi::ExternalBuffer::make(env, script),
      hermes::napi::ExternalBuffer::make(env, source_map),
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
