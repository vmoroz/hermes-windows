# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_subdirectory(node_api)

# Define libraries that provides Hermes wrapped in the C-API.
add_library(hermes_node_api STATIC hermes_node_api.cpp MurmurHash.cpp)
target_link_libraries(hermes_node_api PRIVATE hermesVMRuntime jsi)
target_include_directories(hermes_node_api PUBLIC .. ${HERMES_JSI_DIR} node_api)
target_link_options(hermes_node_api PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})

add_library(hermes_node_api_shared SHARED hermes_node_api.cpp MurmurHash.cpp)
target_link_libraries(hermes_node_api_shared PRIVATE hermesVMRuntime jsi)
target_include_directories(hermes_node_api_shared PUBLIC .. ${HERMES_JSI_DIR} node_api)
target_link_options(hermes_node_api_shared PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})

add_library(hermes_node_api_lean STATIC hermes_node_api.cpp MurmurHash.cpp)
target_link_libraries(hermes_node_api_lean PRIVATE hermesVMRuntimeLean jsi)
target_include_directories(hermes_node_api_lean PUBLIC .. ${HERMES_JSI_DIR} node_api)
target_link_options(hermes_node_api_lean PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})

# TODO: Handle building with EXCEPTION_ON_OOM.
if(HERMES_ENABLE_EH_RTTI)
  message(WARNING "Using the ABI with exceptions enabled is not supported.")
elseif (GCC_COMPATIBLE)
  target_compile_options(hermes_node_api PRIVATE -fno-rtti -fno-exceptions)
  target_compile_options(hermes_node_api_shared PRIVATE -fno-rtti -fno-exceptions)
  target_compile_options(hermes_node_api_lean PRIVATE -fno-rtti -fno-exceptions)
elseif (MSVC)
  target_compile_options(hermes_node_api PRIVATE /GR- /EHs-c-)
  target_compile_options(hermes_node_api_shared PRIVATE /GR- /EHs-c-)
  target_compile_options(hermes_node_api_lean PRIVATE /GR- /EHs-c-)
endif()
