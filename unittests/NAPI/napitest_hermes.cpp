// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include "napitest.h"
#include "hermes_napi.h"

namespace napitest {

std::vector<NapiEnvFactory> NapiEnvFactories() {
  return {[]() {
    napi_env env{};
    napi_create_hermes_env(&env);
    return env;
  }};
}

} // namespace napitest
