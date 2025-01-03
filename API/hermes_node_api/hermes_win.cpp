/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ScriptStore.h"
#include "hermes/VM/Runtime.h"
#include "hermes/inspector/RuntimeAdapter.h"
#include "llvh/Support/raw_os_ostream.h"
#include "hermes_api.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <werapi.h>

#define CHECKED_RUNTIME(runtime) \
  (runtime == nullptr)           \
      ? napi_generic_failure     \
      : reinterpret_cast<facebook::hermes::RuntimeWrapper *>(runtime)

#define CHECKED_CONFIG(config) \
  (config == nullptr)          \
      ? napi_generic_failure   \
      : reinterpret_cast<facebook::hermes::ConfigWrapper *>(config)

#define CHECK_ARG(arg)           \
  if (arg == nullptr) {          \
    return napi_generic_failure; \
  }

// TODO: (vmoroz) Fix
// napi_status hermes_create_napi_env(
//     ::hermes::vm::Runtime &runtime,
//     bool isInspectable,
//     std::shared_ptr<facebook::jsi::PreparedScriptStore> preparedScript,
//     const ::hermes::vm::RuntimeConfig &runtimeConfig,
//     napi_env *env);

napi_status jsr_env_unref(napi_env env);

namespace facebook::hermes {

// Forward declaration
extern ::hermes::vm::Runtime &getVMRuntime(HermesRuntime &runtime) noexcept;

class ConfigWrapper {
 public:
  // napi_status enableDefaultCrashHandler(bool value) {
  //   enableDefaultCrashHandler_ = value;
  //   return napi_status::napi_ok;
  // }

  // napi_status enableInspector(bool value) {
  //   enableInspector_ = value;
  //   return napi_status::napi_ok;
  // }

  // napi_status setInspectorRuntimeName(std::string name) {
  //   inspectorRuntimeName_ = std::move(name);
  //   return napi_status::napi_ok;
  // }

  // napi_status setInspectorPort(uint16_t port) {
  //   inspectorPort_ = port;
  //   return napi_status::napi_ok;
  // }

  // napi_status setInspectorBreakOnStart(bool value) {
  //   inspectorBreakOnStart_ = value;
  //   return napi_status::napi_ok;
  // }

  // napi_status setTaskRunner(std::unique_ptr<TaskRunner> taskRunner) {
  //   taskRunner_ = std::move(taskRunner);
  //   return napi_status::napi_ok;
  // }

  // napi_status setScriptCache(std::unique_ptr<ScriptCache> scriptCache) {
  //   scriptCache_ = std::move(scriptCache);
  //   return napi_status::napi_ok;
  // }

  bool enableDefaultCrashHandler() {
    return enableDefaultCrashHandler_;
  }

  bool enableInspector() const {
    return enableInspector_;
  }

  const std::string &inspectorRuntimeName() const {
    return inspectorRuntimeName_;
  }

  uint16_t inspectorPort() {
    return inspectorPort_;
  }

  bool inspectorBreakOnStart() {
    return inspectorBreakOnStart_;
  }

  // const std::shared_ptr<TaskRunner> &taskRunner() const {
  //   return taskRunner_;
  // }

  // const std::shared_ptr<ScriptCache> &scriptCache() const {
  //   return scriptCache_;
  // }

  ::hermes::vm::RuntimeConfig getRuntimeConfig() const {
    ::hermes::vm::RuntimeConfig::Builder config;
    return config.build();
  }

 private:
  bool enableDefaultCrashHandler_{};
  bool enableInspector_{};
  std::string inspectorRuntimeName_;
  uint16_t inspectorPort_{};
  bool inspectorBreakOnStart_{};
  // std::shared_ptr<TaskRunner> taskRunner_;
  // std::shared_ptr<ScriptCache> scriptCache_;
};

class HermesRuntime;

// class HermesExecutorRuntimeAdapter final
//     : public facebook::hermes::inspector::RuntimeAdapter {
//  public:
//   HermesExecutorRuntimeAdapter(
//       std::shared_ptr<facebook::hermes::HermesRuntime> hermesRuntime,
//       std::shared_ptr<TaskRunner> taskRunner);

//   virtual ~HermesExecutorRuntimeAdapter() = default;
//   HermesRuntime &getRuntime() override;
//   void tickleJs() override;

//  private:
//   std::shared_ptr<facebook::hermes::HermesRuntime> hermesRuntime_;
// //  std::shared_ptr<TaskRunner> taskRunner_;
// };

class RuntimeWrapper {
 public:
  explicit RuntimeWrapper(const ConfigWrapper &config)
      : hermesRuntime_(makeHermesRuntime(config.getRuntimeConfig())),
        vmRuntime_(getVMRuntime(*hermesRuntime_)) {
    // hermes_create_napi_env(
    //     vmRuntime_, config.enableInspector(), config.scriptCache(), {}, &env_);

    // if (config.enableInspector()) {
    //   auto adapter = std::make_unique<HermesExecutorRuntimeAdapter>(
    //       hermesRuntime_, config.taskRunner());
    //   std::string inspectorRuntimeName = config.inspectorRuntimeName();
    //   if (inspectorRuntimeName.empty()) {
    //     inspectorRuntimeName = "Hermes";
    //   }
    //   facebook::hermes::inspector::chrome::enableDebugging(
    //       std::move(adapter), inspectorRuntimeName);
    // }
  }

  ~RuntimeWrapper() {
    jsr_env_unref(env_);
  }

  napi_status getNonAbiSafeRuntime(void **nonAbiSafeRuntime) {
    CHECK_ARG(nonAbiSafeRuntime);
    *nonAbiSafeRuntime = hermesRuntime_.get();
    return napi_ok;
  }

  napi_status dumpCrashData(int32_t fd) {
    //hermesCrashHandler(*hermesRuntime_, fd);
    return napi_ok;
  }

  napi_status addToProfiler() {
    hermesRuntime_->registerForProfiling();
    return napi_ok;
  }

  napi_status removeFromProfiler() {
    hermesRuntime_->unregisterForProfiling();
    return napi_ok;
  }

  napi_status getNodeApi(napi_env *env) {
    *env = env_;
    return napi_ok;
  }

 private:
  std::shared_ptr<HermesRuntime> hermesRuntime_;
  ::hermes::vm::Runtime &vmRuntime_;
  napi_env env_;
};

// HermesExecutorRuntimeAdapter::HermesExecutorRuntimeAdapter(
//     std::shared_ptr<facebook::hermes::HermesRuntime> hermesRuntime,
//     std::shared_ptr<TaskRunner> taskRunner)
//     : hermesRuntime_(std::move(hermesRuntime)),
//       taskRunner_(std::move(taskRunner)) {}

// HermesRuntime &HermesExecutorRuntimeAdapter::getRuntime() {
//   return *hermesRuntime_;
// }

// void HermesExecutorRuntimeAdapter::tickleJs() {
//   // The queue will ensure that hermesRuntime_ is still valid when this gets
//   // invoked.
//   taskRunner_->post(
//       std::unique_ptr<Task>(new LambdaTask([&runtime = *hermesRuntime_]() {
//         auto func =
//             runtime.global().getPropertyAsFunction(runtime, "__tickleJs");
//         func.call(runtime);
//       })));
// }

} // namespace facebook::hermes

JSR_API jsr_create_runtime(jsr_config config, jsr_runtime *runtime) {
  CHECK_ARG(config);
  CHECK_ARG(runtime);
  *runtime = reinterpret_cast<jsr_runtime>(new facebook::hermes::RuntimeWrapper(
      *reinterpret_cast<facebook::hermes::ConfigWrapper *>(config)));
  return napi_ok;
}

JSR_API jsr_delete_runtime(jsr_runtime runtime) {
  CHECK_ARG(runtime);
  delete reinterpret_cast<facebook::hermes::RuntimeWrapper *>(runtime);
  return napi_ok;
}

JSR_API jsr_runtime_get_node_api_env(jsr_runtime runtime, napi_env *env) {
  return CHECKED_RUNTIME(runtime)->getNodeApi(env);
}

JSR_API hermes_dump_crash_data(jsr_runtime runtime, int32_t fd) {
  return CHECKED_RUNTIME(runtime)->dumpCrashData(fd);
}

JSR_API hermes_sampling_profiler_enable() {
  facebook::hermes::HermesRuntime::enableSamplingProfiler();
  return napi_ok;
}

JSR_API hermes_sampling_profiler_disable() {
  facebook::hermes::HermesRuntime::disableSamplingProfiler();
  return napi_ok;
}

JSR_API hermes_sampling_profiler_add(jsr_runtime runtime) {
  return CHECKED_RUNTIME(runtime)->addToProfiler();
}

JSR_API hermes_sampling_profiler_remove(jsr_runtime runtime) {
  return CHECKED_RUNTIME(runtime)->removeFromProfiler();
}

JSR_API hermes_sampling_profiler_dump_to_file(const char *filename) {
  facebook::hermes::HermesRuntime::dumpSampledTraceToFile(filename);
  return napi_ok;
}

JSR_API jsr_create_config(jsr_config *config) {
  CHECK_ARG(config);
  *config = reinterpret_cast<jsr_config>(new facebook::hermes::ConfigWrapper());
  return napi_ok;
}

JSR_API jsr_delete_config(jsr_config config) {
  CHECK_ARG(config);
  delete reinterpret_cast<facebook::hermes::ConfigWrapper *>(config);
  return napi_ok;
}

JSR_API jsr_config_enable_gc_api(jsr_config /*config*/, bool /*value*/) {
  // We do nothing for now.
  return napi_ok;
}
