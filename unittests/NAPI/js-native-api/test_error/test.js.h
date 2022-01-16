#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_error_test_js, R"JavaScript(
'use strict';

const common = require('../../common');
const test_error = require(`./build/${common.buildType}/test_error`);
const assert = require('assert');
const theError = new Error('Some error');
const theTypeError = new TypeError('Some type error');
const theSyntaxError = new SyntaxError('Some syntax error');
const theRangeError = new RangeError('Some type error');
const theReferenceError = new ReferenceError('Some reference error');
const theURIError = new URIError('Some URI error');
const theEvalError = new EvalError('Some eval error');

// class MyError extends Error { }
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
var MyError = /** @class */ (function (_super) {
    __extends(MyError, _super);
    function MyError() {
        return _super !== null && _super.apply(this, arguments) || this;
    }
    return MyError;
}(Error));

const myError = new MyError('Some MyError');

// Test that native error object is correctly classed
assert.strictEqual(test_error.checkError(theError), true);

// Test that native type error object is correctly classed
assert.strictEqual(test_error.checkError(theTypeError), true);

// Test that native syntax error object is correctly classed
assert.strictEqual(test_error.checkError(theSyntaxError), true);

// Test that native range error object is correctly classed
assert.strictEqual(test_error.checkError(theRangeError), true);

// Test that native reference error object is correctly classed
assert.strictEqual(test_error.checkError(theReferenceError), true);

// Test that native URI error object is correctly classed
assert.strictEqual(test_error.checkError(theURIError), true);

// Test that native eval error object is correctly classed
assert.strictEqual(test_error.checkError(theEvalError), true);

// Test that class derived from native error is correctly classed
assert.strictEqual(test_error.checkError(myError), true);

// Test that non-error object is correctly classed
assert.strictEqual(test_error.checkError({}), false);

// Test that non-error primitive is correctly classed
assert.strictEqual(test_error.checkError('non-object'), false);

assert.throws(() => {
  test_error.throwExistingError();
}, /^Error: existing error$/);

assert.throws(() => {
  test_error.throwError();
}, /^Error: error$/);

assert.throws(() => {
  test_error.throwRangeError();
}, /^RangeError: range error$/);

assert.throws(() => {
  test_error.throwTypeError();
}, /^TypeError: type error$/);

[42, {}, [], Symbol('xyzzy'), true, 'ball', undefined, null, NaN]
  .forEach((value) => assert.throws(
    () => test_error.throwArbitrary(value),
    (err) => {
      assert.strictEqual(err, value);
      return true;
    }
  ));

assert.throws(
  () => test_error.throwErrorCode(),
  {
    code: 'ERR_TEST_CODE',
    message: 'Error [error]'
  });

assert.throws(
  () => test_error.throwRangeErrorCode(),
  {
    code: 'ERR_TEST_CODE',
    message: 'RangeError [range error]'
  });

assert.throws(
  () => test_error.throwTypeErrorCode(),
  {
    code: 'ERR_TEST_CODE',
    message: 'TypeError [type error]'
  });

let error = test_error.createError();
assert.ok(error instanceof Error, 'expected error to be an instance of Error');
assert.strictEqual(error.message, 'error');

error = test_error.createRangeError();
assert.ok(error instanceof RangeError,
          'expected error to be an instance of RangeError');
assert.strictEqual(error.message, 'range error');

error = test_error.createTypeError();
assert.ok(error instanceof TypeError,
          'expected error to be an instance of TypeError');
assert.strictEqual(error.message, 'type error');

error = test_error.createErrorCode();
assert.ok(error instanceof Error, 'expected error to be an instance of Error');
assert.strictEqual(error.code, 'ERR_TEST_CODE');
assert.strictEqual(error.message, 'Error [error]');
assert.strictEqual(error.name, 'Error');

error = test_error.createRangeErrorCode();
assert.ok(error instanceof RangeError,
          'expected error to be an instance of RangeError');
assert.strictEqual(error.message, 'RangeError [range error]');
assert.strictEqual(error.code, 'ERR_TEST_CODE');
assert.strictEqual(error.name, 'RangeError');

error = test_error.createTypeErrorCode();
assert.ok(error instanceof TypeError,
          'expected error to be an instance of TypeError');
assert.strictEqual(error.message, 'TypeError [type error]');
assert.strictEqual(error.code, 'ERR_TEST_CODE');
assert.strictEqual(error.name, 'TypeError');
)JavaScript");
