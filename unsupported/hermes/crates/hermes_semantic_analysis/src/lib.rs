/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

<<<<<<<< HEAD:API/hermes_sandbox/external/hermes_sandbox_impl_compiled.h
#ifdef NDEBUG
#include "hermes_sandbox_impl_opt_compiled.h"
#else
#include "hermes_sandbox_impl_dbg_compiled.h"
#endif
|||||||| 49794cfc7:test/Optimizer/cjs/m1.js
// RUN: true

exports.foo = function() {
  print('foo');
  return 1;
}
========
mod analyzer;
mod scope_manager;
mod scope_view;

pub use analyzer::*;
pub use scope_manager::*;
pub use scope_view::*;
>>>>>>>> hermes-2024-07-01-RNv0.75.0-1edbe36ce92fef2c4d427f5c4e104f2758f4b692:unsupported/hermes/crates/hermes_semantic_analysis/src/lib.rs
