#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_general_test_js, R"JavaScript(
'use strict';
// Flags: --expose-gc

const common = require('../../common');
const test_general = require(`./build/${common.buildType}/test_general`);
const assert = require('assert');

const val1 = '1';
const val2 = 1;
const val3 = 1;

// class BaseClass {
// }

// class ExtendedClass extends BaseClass {
// }

var __extends = (this && this.__extends) || (function () {
    var extendStatics = function (d, b) {
        extendStatics = Object.setPrototypeOf ||
            ({ __proto__: [] } instanceof Array && function (d, b) { d.__proto__ = b; }) ||
            function (d, b) { for (var p in b) if (Object.prototype.hasOwnProperty.call(b, p)) d[p] = b[p]; };
        return extendStatics(d, b);
    };
    return function (d, b) {
        if (typeof b !== "function" && b !== null)
            throw new TypeError("Class extends value " + String(b) + " is not a constructor or null");
        extendStatics(d, b);
        function __() { this.constructor = d; }
        d.prototype = b === null ? Object.create(b) : (__.prototype = b.prototype, new __());
    };
})();
var BaseClass = /** @class */ (function () {
    function BaseClass() {
    }
    return BaseClass;
}());
var ExtendedClass = /** @class */ (function (_super) {
    __extends(ExtendedClass, _super);
    function ExtendedClass() {
        return _super !== null && _super.apply(this, arguments) || this;
    }
    return ExtendedClass;
}(BaseClass));

const baseObject = new BaseClass();
const extendedObject = new ExtendedClass();

// Test napi_strict_equals
assert.ok(test_general.testStrictEquals(val1, val1));
assert.strictEqual(test_general.testStrictEquals(val1, val2), false);
assert.ok(test_general.testStrictEquals(val2, val3));

// Test napi_get_prototype
assert.strictEqual(test_general.testGetPrototype(baseObject),
                   Object.getPrototypeOf(baseObject));
assert.strictEqual(test_general.testGetPrototype(extendedObject),
                   Object.getPrototypeOf(extendedObject));
// Prototypes for base and extended should be different.
assert.notStrictEqual(test_general.testGetPrototype(baseObject),
                      test_general.testGetPrototype(extendedObject));

// Test version management functions
assert.strictEqual(test_general.testGetVersion(), 8);

[
  123,
  'test string',
  function() {},
  new Object(),
  true,
  undefined,
  Symbol(),
].forEach((val) => {
  assert.strictEqual(test_general.testNapiTypeof(val), typeof val);
});

// Since typeof in js return object need to validate specific case
// for null
assert.strictEqual(test_general.testNapiTypeof(null), 'null');

// Assert that wrapping twice fails.
const x = {};
test_general.wrap(x);
assert.throws(() => test_general.wrap(x),
              { name: 'Error', message: 'Invalid argument' });
// Clean up here, otherwise derefItemWasCalled() will be polluted.
test_general.removeWrap(x);

// Ensure that wrapping, removing the wrap, and then wrapping again works.
const y = {};
test_general.wrap(y);
test_general.removeWrap(y);
// Wrapping twice succeeds if a remove_wrap() separates the instances
test_general.wrap(y);
// Clean up here, otherwise derefItemWasCalled() will be polluted.
test_general.removeWrap(y);

// Test napi_adjust_external_memory
// TODO: Hermes does not implement that API
// const adjustedValue = test_general.testAdjustExternalMemory();
// assert.strictEqual(typeof adjustedValue, 'number');
// assert(adjustedValue > 0);

async function runGCTests() {
  // Ensure that garbage collecting an object with a wrapped native item results
  // in the finalize callback being called.
  assert.strictEqual(test_general.derefItemWasCalled(), false);

  (() => test_general.wrap({}))();
  await common.gcUntil('deref_item() was called upon garbage collecting a ' +
                       'wrapped object.',
                       () => test_general.derefItemWasCalled());

  // Ensure that removing a wrap and garbage collecting does not fire the
  // finalize callback.
  let z = {};
  test_general.testFinalizeWrap(z);
  test_general.removeWrap(z);
  z = null;
  await common.gcUntil(
    'finalize callback was not called upon garbage collection.',
    () => (!test_general.finalizeWasCalled()));
}
runGCTests();
)JavaScript");
