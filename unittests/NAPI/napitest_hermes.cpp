// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include "hermes_napi.h"
#include "napitest.h"

namespace napitest {

std::vector<NapiTestData> NapiEnvFactories() {
  return {{"../js", []() {
             napi_env env{};
             napi_create_hermes_env(&env);
             return env;
           }}};
}

} // namespace napitest
