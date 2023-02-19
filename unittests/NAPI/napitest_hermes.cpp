// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include <memory>
#include "hermes_api.h"
#include "napitest.h"

namespace napitest {

class HermesRuntimeHolder {
 public:
  HermesRuntimeHolder(hermes_runtime runtime) noexcept : runtime_(runtime) {}

  ~HermesRuntimeHolder() {
    hermes_delete_runtime(runtime_);
  }

  HermesRuntimeHolder(const HermesRuntimeHolder &) = delete;
  HermesRuntimeHolder &operator=(const HermesRuntimeHolder &) = delete;

 private:
  hermes_runtime runtime_{};
};

std::vector<NapiTestData> NapiEnvFactories() {
  return {{"../js", [runtimeHolder = std::shared_ptr<HermesRuntimeHolder>()]() mutable {
             hermes_runtime runtime{};
             hermes_create_runtime(&runtime);
             runtimeHolder = std::make_shared<HermesRuntimeHolder>(runtime);

             napi_env env{};
             hermes_get_napi_env(runtime, &env);
             return env;
           }}};
}

} // namespace napitest
