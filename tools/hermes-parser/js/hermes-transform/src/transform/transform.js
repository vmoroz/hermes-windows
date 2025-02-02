/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @flow strict-local
 * @format
 */

'use strict';

import type {Visitor} from '../traverse/traverse';
import type {TransformContext} from './TransformContext';

import * as prettier from 'prettier';
import {getTransformedAST} from './getTransformedAST';
import {SimpleTraverser} from '../traverse/SimpleTraverser';

export function transform(
  code: string,
  visitors: Visitor<TransformContext>,
  prettierOptions: {...} = {},
): string {
  const {ast, astWasMutated} = getTransformedAST(code, visitors);

  if (!astWasMutated) {
    return code;
  }

  // prettier fully expects the parent pointers are NOT set and
  // certain cases can crash due to prettier infinite-looping
  // whilst naively traversing the parent property
  // https://github.com/prettier/prettier/issues/11793
  SimpleTraverser.traverse(ast, {
    enter(node) {
      // $FlowExpectedError[cannot-write]
      delete node.parent;
    },
    leave() {},
  });

  // we need to delete the comments prop or else prettier will do
  // its own attachment pass after the mutation and duplicate the
  // comments on each node, borking the output
  // $FlowExpectedError[cannot-write]
  delete ast.comments;

  return prettier.format(code, {
    ...prettierOptions,
    parser() {
      return ast;
    },
  });
}
