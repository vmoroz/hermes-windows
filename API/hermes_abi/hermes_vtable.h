/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_ABI_HERMES_VTABLE_H
#define HERMES_ABI_HERMES_VTABLE_H

#include "hermes_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

const HermesABIVTable *HERMES_CALL get_hermes_abi_vtable(void);

#ifdef __cplusplus
}
#endif

#endif
