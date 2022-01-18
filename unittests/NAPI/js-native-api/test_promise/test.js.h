#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_promise_test_js, R"JavaScript(
'use strict';

const common = require('../../common');

// This tests the promise-related n-api calls

const assert = require('assert');
const test_promise = require(`./build/${common.buildType}/test_promise`);

// A resolution
{
  const expected_result1 = 42;
  const promise = test_promise.createPromise();
  promise.then(
    common.mustCall(function(result) {
      assert.strictEqual(result, expected_result1);
    }),
    common.mustNotCall());
  test_promise.concludeCurrentPromise(expected_result1, true);
}

// A rejection
{
  const expected_result2 = 'It\'s not you, it\'s me.';
  const promise = test_promise.createPromise();
  promise.then(
    common.mustNotCall(),
    common.mustCall(function(result) {
      assert.strictEqual(result, expected_result2);
    }));
  test_promise.concludeCurrentPromise(expected_result2, false);
}

// Chaining
{
  const expected_result3 = 'chained answer';
  const promise = test_promise.createPromise();
  promise.then(
    common.mustCall(function(result) {
      assert.strictEqual(result, expected_result3);
    }),
    common.mustNotCall());
  test_promise.concludeCurrentPromise(Promise.resolve('chained answer'), true);
}

const promiseTypeTestPromise = test_promise.createPromise();
assert.strictEqual(test_promise.isPromise(promiseTypeTestPromise), true);
test_promise.concludeCurrentPromise(undefined, true);

const rejectPromise = Promise.reject(-1);
const expected_reason = -1;
assert.strictEqual(test_promise.isPromise(rejectPromise), true);
rejectPromise.catch((reason) => {
  assert.strictEqual(reason, expected_reason);
});

assert.strictEqual(test_promise.isPromise(2.4), false);
assert.strictEqual(test_promise.isPromise('I promise!'), false);
assert.strictEqual(test_promise.isPromise(undefined), false);
assert.strictEqual(test_promise.isPromise(null), false);
assert.strictEqual(test_promise.isPromise({}), false);
)JavaScript");
