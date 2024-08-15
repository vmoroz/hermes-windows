/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

<<<<<<<< HEAD:test/IRGen/es6/async-generators.js
// RUN: (! %hermesc %s --dump-bytecode 2>&1) | %FileCheck %s

async function* f() {};
// CHECK: async generators are unsupported
|||||||| 49794cfc7:test/Optimizer/cjs/m2.js
// RUN: true

exports.baz = function() {
  print('baz');
  return 2;
}
========
#ifdef NDEBUG
#include "hermes_sandbox_impl_opt_compiled.h"
#else
#include "hermes_sandbox_impl_dbg_compiled.h"
#endif
>>>>>>>> hermes-2024-07-01-RNv0.75.0-1edbe36ce92fef2c4d427f5c4e104f2758f4b692:API/hermes_sandbox/external/hermes_sandbox_impl_compiled.h
