/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
<<<<<<<< HEAD:tools/hermes-parser/js/flow-api-translator/__tests__/flowDefToTSDef/fixtures/types/utilities/$ReadOnlySet/spec.js
 *
 * @flow strict-local
 * @format
|||||||| 49794cfc7:test/hermes/cjs/cjs-exports-2.js
========
 *
 * @format
>>>>>>>> facebook/main:tools/hermes-parser/js/flow-api-translator/__tests__/TSDefToFlowDef/fixtures/class/members/constructor/spec.ts
 */

<<<<<<<< HEAD:tools/hermes-parser/js/flow-api-translator/__tests__/flowDefToTSDef/fixtures/types/utilities/$ReadOnlySet/spec.js
type T = $ReadOnlySet<Foo>;
|||||||| 49794cfc7:test/hermes/cjs/cjs-exports-2.js
// RUN: true

print('2: init');

module.exports = {x: 3};
========
declare class Foo {
  constructor(): Foo;
}
>>>>>>>> facebook/main:tools/hermes-parser/js/flow-api-translator/__tests__/TSDefToFlowDef/fixtures/class/members/constructor/spec.ts
