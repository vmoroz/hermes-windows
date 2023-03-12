// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include <memory>
#include "hermes_api.h"
#include "napitest.h"

namespace napitest {

class HermesRuntimeHolder : public IEnvHolder {
 public:
  HermesRuntimeHolder() noexcept {
    hermes_config config{};
    hermes_create_config(&config);
    hermes_create_runtime(config, &runtime_);
  }

  ~HermesRuntimeHolder() {
    hermes_delete_runtime(runtime_);
  }

  HermesRuntimeHolder(const HermesRuntimeHolder &) = delete;
  HermesRuntimeHolder &operator=(const HermesRuntimeHolder &) = delete;

  napi_env getEnv() override {
    napi_env env{};
    hermes_get_node_api_env(runtime_, &env);
    return env;
  }

 private:
  hermes_runtime runtime_{};
};

std::vector<NapiTestData> NapiEnvFactories() {
  return {{"../js", [] {
             return std::unique_ptr<IEnvHolder>(new HermesRuntimeHolder());
           }}};
}

} // namespace napitest
