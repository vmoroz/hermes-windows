/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_PUBLIC_RUNTIMECONFIG_H
#define HERMES_PUBLIC_RUNTIMECONFIG_H

#include "hermes/Public/CrashManager.h"
#include "hermes/Public/CtorConfig.h"
#include "hermes/Public/GCConfig.h"

#include <memory>

namespace hermes {
namespace vm {

enum CompilationMode {
  SmartCompilation,
  ForceEagerCompilation,
  ForceLazyCompilation
};

class PinnedHermesValue;

// Parameters for Runtime initialisation.  Check documentation in README.md
// constexpr indicates that the default value is constexpr.
#define RUNTIME_FIELDS(F)                                              \
  /* Parameters to be passed on to the GC. */                          \
  F(HERMES_NON_CONSTEXPR, vm::GCConfig, GCConfig)                      \
                                                                       \
  /* Pre-allocated Register Stack */                                   \
  F(constexpr, PinnedHermesValue *, RegisterStack, nullptr)            \
                                                                       \
  /* Register Stack Size */                                            \
  F(constexpr, unsigned, MaxNumRegisters, 64 * 1024)                   \
                                                                       \
  /* Whether or not the JIT is enabled */                              \
  F(constexpr, bool, EnableJIT, false)                                 \
                                                                       \
  /* Whether to allow eval and Function ctor */                        \
  F(constexpr, bool, EnableEval, true)                                 \
                                                                       \
  /* Whether to verify the IR generated by eval and Function ctor */   \
  F(constexpr, bool, VerifyEvalIR, false)                              \
                                                                       \
  /* Whether to optimize the code inside eval and Function ctor */     \
  F(constexpr, bool, OptimizedEval, false)                             \
                                                                       \
  /* Whether to emit async break check instructions in eval code */    \
  F(constexpr, bool, AsyncBreakCheckInEval, false)                     \
                                                                       \
  /* Support for ES6 Promise. */                                       \
  F(constexpr, bool, ES6Promise, true)                                 \
                                                                       \
  /* Support for ES6 Proxy. */                                         \
  F(constexpr, bool, ES6Proxy, true)                                   \
                                                                       \
  /* Support for ECMA-402 Intl APIs. */                                \
  F(constexpr, bool, Intl, true)                                       \
                                                                       \
  /* Support for ArrayBuffer, DataView and typed arrays. */            \
  F(constexpr, bool, ArrayBuffer, true)                                \
                                                                       \
  /* Support for using microtasks. */                                  \
  F(constexpr, bool, MicrotaskQueue, false)                            \
                                                                       \
  /* Enable synth trace. */                                            \
  F(constexpr, bool, TraceEnabled, false)                              \
                                                                       \
  /* Scratch path for synth trace. */                                  \
  F(HERMES_NON_CONSTEXPR, std::string, TraceScratchPath, "")           \
                                                                       \
  /* Result path for synth trace. */                                   \
  F(HERMES_NON_CONSTEXPR, std::string, TraceResultPath, "")            \
                                                                       \
  /* Callout to register an interesting (e.g. lead to crash) */        \
  /* and completed trace. */                                           \
  F(HERMES_NON_CONSTEXPR,                                              \
    std::function<bool()>,                                             \
    TraceRegisterCallback,                                             \
    nullptr)                                                           \
                                                                       \
  /* Enable sampling certain statistics. */                            \
  F(constexpr, bool, EnableSampledStats, false)                        \
                                                                       \
  /* Whether to enable automatic sampling profiler registration */     \
  F(constexpr, bool, EnableSampleProfiling, false)                     \
                                                                       \
  /* Whether to randomize stack placement etc. */                      \
  F(constexpr, bool, RandomizeMemoryLayout, false)                     \
                                                                       \
  /* Eagerly read bytecode into page cache. */                         \
  F(constexpr, unsigned, BytecodeWarmupPercent, 0)                     \
                                                                       \
  /* Signal-based I/O tracking. Slows down execution. If enabled, */   \
  /* all bytecode buffers > 64 kB passed to Hermes must be mmap:ed. */ \
  F(constexpr, bool, TrackIO, false)                                   \
                                                                       \
  /* Enable contents of HermesInternal */                              \
  F(constexpr, bool, EnableHermesInternal, true)                       \
                                                                       \
  /* Enable methods exposed to JS for testing */                       \
  F(constexpr, bool, EnableHermesInternalTestMethods, false)           \
                                                                       \
  /* Choose lazy/eager compilation mode. */                            \
  F(constexpr,                                                         \
    CompilationMode,                                                   \
    CompilationMode,                                                   \
    CompilationMode::SmartCompilation)                                 \
                                                                       \
  /* Choose whether generators are enabled. */                         \
  F(constexpr, bool, EnableGenerator, true)                            \
                                                                       \
  /* An interface for managing crashes. */                             \
  F(HERMES_NON_CONSTEXPR,                                              \
    std::shared_ptr<CrashManager>,                                     \
    CrashMgr,                                                          \
    new NopCrashManager)                                               \
                                                                       \
  /* The flags passed from a VM experiment */                          \
  F(constexpr, uint32_t, VMExperimentFlags, 0)                         \
  /* RUNTIME_FIELDS END */

_HERMES_CTORCONFIG_STRUCT(RuntimeConfig, RUNTIME_FIELDS, {});

#undef RUNTIME_FIELDS

} // namespace vm
} // namespace hermes

#endif // HERMES_PUBLIC_RUNTIMECONFIG_H
