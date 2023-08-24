/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_ABI_HERMES_ABI_H
#define HERMES_ABI_HERMES_ABI_H

#include <stddef.h>
#include <stdint.h>

struct HermesABIRuntimeConfig;
struct HermesABIContext;

struct HermesABIVTable {
  /// Create a new instance of a Hermes Runtime, and return a pointer to its
  /// associated context. The context must be released with
  /// release_hermes_runtime when it is no longer needed.
  HermesABIContext *(*make_hermes_runtime)(const HermesABIRuntimeConfig *);
  /// Release the Hermes Runtime associated with the given context.
  void (*release_hermes_runtime)(HermesABIContext *);
};

#endif
