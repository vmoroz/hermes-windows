/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

type Foo = {
  prop: string,
  ...T1,
  ...T2,
  ...T3,
  ...T4,
};
