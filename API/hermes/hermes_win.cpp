/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes_win.h"
#include "hermes/VM/Runtime.h"
#include "hermes/inspector/RuntimeAdapter.h"
#include "hermes/inspector/chrome/Registration.h"
#include "llvh/Support/raw_os_ostream.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <werapi.h>

#define CHECKED_RUNTIME(runtime) \
  (runtime == nullptr)           \
      ? hermes_error             \
      : reinterpret_cast<facebook::hermes::RuntimeWrapper *>(runtime)

#define CHECKED_CONFIG(config) \
  (config == nullptr)          \
      ? hermes_error           \
      : reinterpret_cast<facebook::hermes::ConfigWrapper *>(config)

#define CHECK_ARG(arg)   \
  if (arg == nullptr) {  \
    return hermes_error; \
  }

napi_status napi_create_hermes_env(
    ::hermes::vm::Runtime &runtime,
    napi_env *env);

namespace facebook::hermes {

// Forward declaration
extern ::hermes::vm::Runtime &getVMRuntime(HermesRuntime &runtime) noexcept;

class CrashManagerImpl : public ::hermes::vm::CrashManager {
 public:
  void registerMemory(void *mem, size_t length) override {
    if (length >
        WER_MAX_MEM_BLOCK_SIZE) { // Hermes thinks we should save the whole
                                  // block, but WER allows 64K max
      _largeMemBlocks[(intptr_t)mem] = length;

      auto pieceCount = length / WER_MAX_MEM_BLOCK_SIZE;
      for (auto i = 0; i < pieceCount; i++) {
        WerRegisterMemoryBlock(
            (char *)mem + i * WER_MAX_MEM_BLOCK_SIZE, WER_MAX_MEM_BLOCK_SIZE);
      }

      WerRegisterMemoryBlock(
          (char *)mem + pieceCount * WER_MAX_MEM_BLOCK_SIZE,
          length - pieceCount * WER_MAX_MEM_BLOCK_SIZE);
    } else {
      WerRegisterMemoryBlock(mem, static_cast<DWORD>(length));
    }
  }

  void unregisterMemory(void *mem) override {
    if (_largeMemBlocks.find((intptr_t)mem) != _largeMemBlocks.end()) {
      // This memory was larger than what WER supports so we split it up into
      // chunks of size WER_MAX_MEM_BLOCK_SIZE
      auto pieceCount = _largeMemBlocks[(intptr_t)mem] / WER_MAX_MEM_BLOCK_SIZE;
      for (auto i = 0; i < pieceCount; i++) {
        WerUnregisterMemoryBlock((char *)mem + i * WER_MAX_MEM_BLOCK_SIZE);
      }

      WerUnregisterMemoryBlock(
          (char *)mem + pieceCount * WER_MAX_MEM_BLOCK_SIZE);

      _largeMemBlocks.erase((intptr_t)mem);
    } else {
      WerUnregisterMemoryBlock(mem);
    }
  }

  void setCustomData(const char *key, const char *val) override {
    auto strKey = Utf8ToUtf16(key);
    auto strValue = Utf8ToUtf16(val);
    WerRegisterCustomMetadata(strKey.c_str(), strValue.c_str());
  }

  void removeCustomData(const char *key) override {
    auto strKey = Utf8ToUtf16(key);
    WerUnregisterCustomMetadata(strKey.c_str());
  }

  void setContextualCustomData(const char *key, const char *val) override {
    std::wstringstream sstream;
    sstream << "TID" << std::this_thread::get_id() << Utf8ToUtf16(key);

    auto strKey = sstream.str();
    // WER expects valid XML element names, Hermes embeds ':' characters that
    // need to be replaced
    std::replace(strKey.begin(), strKey.end(), L':', L'_');

    auto strValue = Utf8ToUtf16(val);
    WerRegisterCustomMetadata(strKey.c_str(), strValue.c_str());
  }

  void removeContextualCustomData(const char *key) override {
    std::wstringstream sstream;
    sstream << "TID" << std::this_thread::get_id() << Utf8ToUtf16(key);

    auto strKey = sstream.str();
    // WER expects valid XML element names, Hermes embeds ':' characters that
    // need to be replaced
    std::replace(strKey.begin(), strKey.end(), L':', L'_');

    WerUnregisterCustomMetadata(strKey.c_str());
  }

  CallbackKey registerCallback(CallbackFunc cb) override {
    CallbackKey key = static_cast<CallbackKey>((intptr_t)std::addressof(cb));
    _callbacks.insert({key, std::move(cb)});
    return key;
  }

  void unregisterCallback(CallbackKey key) override {
    _callbacks.erase(static_cast<size_t>(key));
  }

  void setHeapInfo(const HeapInformation &heapInfo) override {
    _lastHeapInformation = heapInfo;
  }

  void crashHandler(int fd) const noexcept {
    for (const auto &cb : _callbacks) {
      cb.second(fd);
    }
  }

 private:
  std::wstring Utf8ToUtf16(const char *s) {
    size_t strLength = strnlen_s(
        s, 64); // 64 is maximum key length for WerRegisterCustomMetadata
    size_t requiredSize = 0;

    if (strLength != 0) {
      mbstowcs_s(&requiredSize, nullptr, 0, s, strLength);

      if (requiredSize != 0) {
        std::wstring buffer;
        buffer.resize(requiredSize + sizeof(wchar_t));

        if (mbstowcs_s(&requiredSize, &buffer[0], requiredSize, s, strLength) ==
            0) {
          return buffer;
        }
      }
    }

    return std::wstring();
  }

  HeapInformation _lastHeapInformation;
  std::map<CallbackKey, CallbackFunc> _callbacks;
  std::map<intptr_t, size_t> _largeMemBlocks;
};

void hermesCrashHandler(HermesRuntime &runtime, int fd) {
  ::hermes::vm::Runtime &vmRuntime = getVMRuntime(runtime);

  // Run all callbacks registered to the crash manager
  auto &crashManager = vmRuntime.getCrashManager();
  if (auto *crashManagerImpl =
          dynamic_cast<CrashManagerImpl *>(&crashManager)) {
    crashManagerImpl->crashHandler(fd);
  }

  // Also serialize the current callstack
  auto callstack = vmRuntime.getCallStackNoAlloc();
  llvh::raw_fd_ostream jsonStream(fd, false);
  ::hermes::JSONEmitter json(jsonStream);
  json.openDict();
  json.emitKeyValue("callstack", callstack);
  json.closeDict();
  json.endJSONL();
}

class Task {
 public:
  virtual void invoke() noexcept = 0;

  static void run(void *task) {
    reinterpret_cast<Task *>(task)->invoke();
  }

  static void deleteTask(void *task, void * /*deleterData*/) {
    delete reinterpret_cast<Task *>(task);
  }
};

template <typename TLambda>
class LambdaTask : public Task {
 public:
  LambdaTask(TLambda &&lambda) : lambda_(std::move(lambda)) {}

  void invoke() noexcept override {
    lambda_();
  }

 private:
  TLambda lambda_;
};

class TaskRunner {
 public:
  TaskRunner(
      void *data,
      hermes_task_runner_post_task_cb postTaskCallback,
      hermes_data_delete_cb deleteCallback,
      void *deleterData)
      : data_(data),
        postTaskCallback_(postTaskCallback),
        deleteCallback_(deleteCallback),
        deleterData_(deleterData) {}

  ~TaskRunner() {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(data_, deleterData_);
    }
  }

  void post(std::unique_ptr<Task> task) {
    postTaskCallback_(
        this, task.release(), &Task::run, &Task::deleteTask, nullptr);
  }

 private:
  void *data_;
  hermes_task_runner_post_task_cb postTaskCallback_;
  hermes_data_delete_cb deleteCallback_;
  void *deleterData_;
};

class ScriptBuffer {
 public:
  ScriptBuffer(
      const uint8_t *data,
      size_t size,
      hermes_data_delete_cb deleteCallback,
      void *deleterData)
      : data_(data),
        size_(size),
        deleteCallback_(deleteCallback),
        deleterData_(deleterData) {}

  ~ScriptBuffer() {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(const_cast<uint8_t *>(data_), deleterData_);
    }
  }

  const uint8_t *data() {
    return data_;
  }

  size_t size() {
    return size_;
  }

  static void deleteBuffer(void * /*data*/, void *scriptBuffer) {
    delete reinterpret_cast<ScriptBuffer *>(scriptBuffer);
  }

 private:
  const uint8_t *data_{};
  size_t size_{};
  hermes_data_delete_cb deleteCallback_{};
  void *deleterData_{};
};

class ScriptCache {
 public:
  ScriptCache(
      void *data,
      hermes_script_cache_load_cb loadCallback,
      hermes_script_cache_store_cb storeCallback,
      hermes_data_delete_cb deleteCallback,
      void *deleterData)
      : data_(data),
        loadCallback_(loadCallback),
        storeCallback_(storeCallback),
        deleteCallback_(deleteCallback),
        deleterData_(deleterData) {}

  ~ScriptCache() {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(data_, deleterData_);
    }
  }

  std::unique_ptr<ScriptBuffer> load(hermes_script_cache_metadata *metadata) {
    const uint8_t *buffer{};
    size_t size{};
    hermes_data_delete_cb deleteCallback{};
    void *deleterData{};
    loadCallback_(
        this, metadata, &buffer, &size, &deleteCallback, &deleterData);
    return std::make_unique<ScriptBuffer>(
        buffer, size, deleteCallback, deleterData);
  }

  void store(
      hermes_script_cache_metadata *metadata,
      std::unique_ptr<ScriptBuffer> scriptBuffer) {
    storeCallback_(
        this,
        metadata,
        scriptBuffer->data(),
        scriptBuffer->size(),
        &ScriptBuffer::deleteBuffer,
        scriptBuffer.get());
    scriptBuffer.release();
  }

 private:
  void *data_;
  hermes_script_cache_load_cb loadCallback_;
  hermes_script_cache_store_cb storeCallback_;
  hermes_data_delete_cb deleteCallback_;
  void *deleterData_;
};

class ConfigWrapper {
 public:
  hermes_status enableDefaultCrashHandler(bool value) {
    enableDefaultCrashHandler_ = value;
    return hermes_status::hermes_ok;
  }

  hermes_status enableDebugger(bool value) {
    enableDebugger_ = value;
    return hermes_status::hermes_ok;
  }

  hermes_status setDebuggerRuntimeName(std::string name) {
    debuggerRuntimeName_ = std::move(name);
    return hermes_status::hermes_ok;
  }

  hermes_status setDebuggerPort(uint16_t port) {
    debuggerPort_ = port;
    return hermes_status::hermes_ok;
  }

  hermes_status setDebuggerBreakOnStart(bool value) {
    debuggerBreakOnStart_ = value;
    return hermes_status::hermes_ok;
  }

  hermes_status setTaskRunner(std::unique_ptr<TaskRunner> taskRunner) {
    taskRunner_ = std::move(taskRunner);
    return hermes_status::hermes_ok;
  }

  hermes_status setScriptCache(std::unique_ptr<ScriptCache> scriptCache) {
    scriptCache_ = std::move(scriptCache);
    return hermes_status::hermes_ok;
  }

  bool enableDefaultCrashHandler() {
    return enableDefaultCrashHandler_;
  }

  bool enableDebugger() const {
    return enableDebugger_;
  }

  const std::string &debuggerRuntimeName() const {
    return debuggerRuntimeName_;
  }

  uint16_t debuggerPort() {
    return debuggerPort_;
  }

  bool debuggerBreakOnStart() {
    return debuggerBreakOnStart_;
  }

  std::shared_ptr<TaskRunner> taskRunner() const {
    return taskRunner_;
  }

  ScriptCache *scriptCache() {
    return scriptCache_.get();
  }

  ::hermes::vm::RuntimeConfig getRuntimeConfig() const {
    ::hermes::vm::RuntimeConfig::Builder config;
    if (enableDefaultCrashHandler_) {
      auto crashManager = std::make_shared<CrashManagerImpl>();
      config.withCrashMgr(crashManager);
    }
    return config.build();
  }

 private:
  bool enableDefaultCrashHandler_{};
  bool enableDebugger_{};
  std::string debuggerRuntimeName_;
  uint16_t debuggerPort_{};
  bool debuggerBreakOnStart_{};
  std::shared_ptr<TaskRunner> taskRunner_;
  std::shared_ptr<ScriptCache> scriptCache_;
};

class HermesRuntime;

class HermesExecutorRuntimeAdapter final
    : public facebook::hermes::inspector::RuntimeAdapter {
 public:
  HermesExecutorRuntimeAdapter(
      std::shared_ptr<facebook::hermes::HermesRuntime> hermesRuntime,
      std::shared_ptr<TaskRunner> taskRunner);

  virtual ~HermesExecutorRuntimeAdapter() = default;
  HermesRuntime &getRuntime() override;
  void tickleJs() override;

 private:
  std::shared_ptr<facebook::hermes::HermesRuntime> hermesRuntime_;
  std::shared_ptr<TaskRunner> taskRunner_;
};

class RuntimeWrapper {
 public:
  explicit RuntimeWrapper(const ConfigWrapper &config)
      : hermesRuntime_(makeHermesRuntime(config.getRuntimeConfig())),
        vmRuntime_(getVMRuntime(*hermesRuntime_)) {
    napi_create_hermes_env(vmRuntime_, &env_);

    if (config.enableDebugger()) {
      auto adapter = std::make_unique<HermesExecutorRuntimeAdapter>(
          hermesRuntime_, config.taskRunner());
      std::string debuggerRuntimeName = config.debuggerRuntimeName();
      if (debuggerRuntimeName.empty()) {
        debuggerRuntimeName = "Hermes";
      }
      facebook::hermes::inspector::chrome::enableDebugging(
          std::move(adapter), debuggerRuntimeName);
    }
  }

  ~RuntimeWrapper() {
    napi_ext_env_unref(env_);
  }

  hermes_status getNonAbiSafeRuntime(void **nonAbiSafeRuntime) {
    CHECK_ARG(nonAbiSafeRuntime);
    *nonAbiSafeRuntime = hermesRuntime_.get();
    return hermes_ok;
  }

  hermes_status dumpCrashData(int32_t fd) {
    hermesCrashHandler(*hermesRuntime_, fd);
    return hermes_ok;
  }

  hermes_status addToProfiler() {
    hermesRuntime_->registerForProfiling();
    return hermes_ok;
  }

  hermes_status removeFromProfiler() {
    hermesRuntime_->unregisterForProfiling();
    return hermes_ok;
  }

  hermes_status getNodeApi(napi_env *env) {
    *env = env_;
    return hermes_ok;
  }

 private:
  ConfigWrapper config_;
  std::shared_ptr<HermesRuntime> hermesRuntime_;
  ::hermes::vm::Runtime &vmRuntime_;
  napi_env env_;
};

HermesExecutorRuntimeAdapter::HermesExecutorRuntimeAdapter(
    std::shared_ptr<facebook::hermes::HermesRuntime> hermesRuntime,
    std::shared_ptr<TaskRunner> taskRunner)
    : hermesRuntime_(std::move(hermesRuntime)),
      taskRunner_(std::move(taskRunner)) {}

HermesRuntime &HermesExecutorRuntimeAdapter::getRuntime() {
  return *hermesRuntime_;
}

void HermesExecutorRuntimeAdapter::tickleJs() {
  // The queue will ensure that hermesRuntime_ is still valid when this gets
  // invoked.
  taskRunner_->post(
      std::unique_ptr<Task>(new LambdaTask([&runtime = *hermesRuntime_]() {
        auto func =
            runtime.global().getPropertyAsFunction(runtime, "__tickleJs");
        func.call(runtime);
      })));
}

} // namespace facebook::hermes

HERMES_API hermes_create_runtime(
    hermes_config config,
    hermes_runtime *runtime) {
  CHECK_ARG(runtime);
  *runtime =
      reinterpret_cast<hermes_runtime>(new facebook::hermes::RuntimeWrapper(
          *reinterpret_cast<facebook::hermes::ConfigWrapper *>(config)));
  return hermes_ok;
}

HERMES_API hermes_delete_runtime(hermes_runtime runtime) {
  CHECK_ARG(runtime);
  delete reinterpret_cast<facebook::hermes::RuntimeWrapper *>(runtime);
  return hermes_ok;
}

HERMES_API hermes_get_node_api_env(hermes_runtime runtime, napi_env *env) {
  return CHECKED_RUNTIME(runtime)->getNodeApi(env);
}

HERMES_API hermes_dump_crash_data(hermes_runtime runtime, int32_t fd) {
  return CHECKED_RUNTIME(runtime)->dumpCrashData(fd);
}

HERMES_API hermes_sampling_profiler_enable() {
  facebook::hermes::HermesRuntime::enableSamplingProfiler();
  return hermes_ok;
}

HERMES_API hermes_sampling_profiler_disable() {
  facebook::hermes::HermesRuntime::disableSamplingProfiler();
  return hermes_ok;
}

HERMES_API hermes_sampling_profiler_add(hermes_runtime runtime) {
  return CHECKED_RUNTIME(runtime)->addToProfiler();
}

HERMES_API hermes_sampling_profiler_remove(hermes_runtime runtime) {
  return CHECKED_RUNTIME(runtime)->removeFromProfiler();
}

HERMES_API hermes_sampling_profiler_dump_to_file(const char *filename) {
  facebook::hermes::HermesRuntime::dumpSampledTraceToFile(filename);
  return hermes_ok;
}

HERMES_API hermes_create_config(hermes_config *config) {
  CHECK_ARG(config);
  *config =
      reinterpret_cast<hermes_config>(new facebook::hermes::ConfigWrapper());
  return hermes_ok;
}

HERMES_API hermes_delete_config(hermes_config config) {
  CHECK_ARG(config);
  delete reinterpret_cast<facebook::hermes::ConfigWrapper *>(config);
  return hermes_ok;
}

HERMES_API hermes_config_enable_default_crash_handler(
    hermes_config config,
    bool value) {
  return CHECKED_CONFIG(config)->enableDefaultCrashHandler(value);
}

HERMES_API hermes_config_enable_debugger(hermes_config config, bool value) {
  return CHECKED_CONFIG(config)->enableDebugger(value);
}

HERMES_API hermes_config_set_debugger_runtime_name(
    hermes_config config,
    const char *name) {
  return CHECKED_CONFIG(config)->setDebuggerRuntimeName(name);
}

HERMES_API hermes_config_set_debugger_port(
    hermes_config config,
    uint16_t port) {
  return CHECKED_CONFIG(config)->setDebuggerPort(port);
}

HERMES_API hermes_config_set_debugger_break_on_start(
    hermes_config config,
    bool value) {
  return CHECKED_CONFIG(config)->setDebuggerBreakOnStart(value);
}

HERMES_API hermes_config_set_task_runner(
    hermes_config config,
    void *task_runner_data,
    hermes_task_runner_post_task_cb task_runner_post_task_cb,
    hermes_data_delete_cb task_runner_data_delete_cb,
    void *deleter_data) {
  return CHECKED_CONFIG(config)->setTaskRunner(
      std::make_unique<facebook::hermes::TaskRunner>(
          task_runner_data,
          task_runner_post_task_cb,
          task_runner_data_delete_cb,
          deleter_data));
}

HERMES_API hermes_config_set_script_cache(
    hermes_config config,
    void *script_cache_data,
    hermes_script_cache_load_cb script_cache_load_cb,
    hermes_script_cache_store_cb script_cache_store_cb,
    hermes_data_delete_cb script_cache_data_delete_cb,
    void *deleter_data) {
  return CHECKED_CONFIG(config)->setScriptCache(
      std::make_unique<facebook::hermes::ScriptCache>(
          script_cache_data,
          script_cache_load_cb,
          script_cache_store_cb,
          script_cache_data_delete_cb,
          deleter_data));
}
