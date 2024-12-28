# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_library(node_api INTERFACE)
target_include_directories(node_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

install(DIRECTORY "${PROJECT_SOURCE_DIR}/API/hermes_node_api/node_api/" DESTINATION include
  FILES_MATCHING PATTERN "*.h")
