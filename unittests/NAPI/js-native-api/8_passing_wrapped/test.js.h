#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_8_passing_wrapped_test_js, R"JavaScript(
'use strict';
// Flags: --expose-gc

const common = require('../../common');
const assert = require('assert');
const addon = require(`./build/${common.buildType}/binding`);

async function runTest() {
  // TODO: Hermes does not GC variables assigned with null.
  //       We had to rewrite the code below to enable GC.
  (() => {
    let obj1 = addon.createObject(10);
    let obj2 = addon.createObject(20);
    const result = addon.add(obj1, obj2);
    assert.strictEqual(result, 30);
  })();
  await common.gcUntil('8_passing_wrapped',
                       () => (addon.finalizeCount() === 2));
}
runTest();
)JavaScript");
