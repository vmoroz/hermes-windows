/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_ABI_HERMES_VTABLE_H
#define HERMES_ABI_HERMES_VTABLE_H

#ifdef __cplusplus
extern "C" {
#endif

struct HermesABIVTable;

const HermesABIVTable *get_hermes_abi_vtable();

#ifdef __cplusplus
}
#endif

#endif
