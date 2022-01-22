#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_new_target_test_js, R"JavaScript(
'use strict';

const common = require('../../common');
const assert = require('assert');
const binding = require(`./build/${common.buildType}/binding`);

// class Class extends binding.BaseClass {
//   constructor() {
//     super();
//     this.method();
//   }
//   method() {
//     this.ok = true;
//   }
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
var Class = /** @class */ (function (_super) {
    __extends(Class, _super);
    function Class() {
        var _this = Reflect.construct(_super,arguments,Class);
        _this.method();
        return _this;
    }
    Class.prototype.method = function () {
        this.ok = true;
    };
    return Class;
}(binding.BaseClass));

assert.ok(new Class() instanceof binding.BaseClass);
assert.ok(new Class().ok);
assert.ok(binding.OrdinaryFunction());
assert.ok(
  new binding.Constructor(binding.Constructor) instanceof binding.Constructor);
)JavaScript");
