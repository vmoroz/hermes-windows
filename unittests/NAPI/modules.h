// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#ifndef NAPI_LIB_MODULES_H_
#define NAPI_LIB_MODULES_H_

#include <functional>
#include <map>
#include <string>

#define DEFINE_TEST_SCRIPT(cppId, script) \
  ::napitest::TestScriptInfo const cppId{script, __FILE__, (__LINE__ - napitest::GetEndOfLineCount(script))};

namespace napitest {

struct TestScriptInfo {
  std::string script;
  std::string file;
  int32_t line;
};

inline int32_t GetEndOfLineCount(char const *script) noexcept {
  return std::count(script, script + strlen(script), '\n');
}

} // namespace napitest

#endif // NAPI_LIB_MODULES_H_
