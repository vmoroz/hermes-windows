# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_library(hermesabi STATIC hermes_vtable.cpp)
target_link_libraries(hermesabi PRIVATE hermesVMRuntime)
target_include_directories(hermesabi PUBLIC ..)
target_link_options(hermesabi PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})

add_library(hermesabi_lean STATIC hermes_vtable.cpp)
target_link_libraries(hermesabi_lean PRIVATE hermesVMRuntimeLean)
target_include_directories(hermesabi_lean PUBLIC ..)
target_link_options(hermesabi_lean PRIVATE ${HERMES_EXTRA_LINKER_FLAGS})

# TODO: Handle building with EXCEPTION_ON_OOM.
if(HERMES_ENABLE_EH)
  message(FATAL_ERROR "ABI does not support exceptions in the VM.")
endif()

if(GCC_COMPATIBLE)
  target_compile_options(hermesabi PRIVATE -fno-rtti -fno-exceptions)
  target_compile_options(hermesabi_lean PRIVATE -fno-rtti -fno-exceptions)
elseif (MSVC)
  target_compile_options(hermesabi PRIVATE /GR- /EHs-c-)
  target_compile_options(hermesabi_lean PRIVATE /GR- /EHs-c-)
endif()

add_library(hermesAbiRuntime STATIC HermesABIRuntime.cpp)
target_include_directories(hermesAbiRuntime PUBLIC ..)
target_link_libraries(hermesAbiRuntime PUBLIC jsi hermesPublic)

if (MSVC)
  target_compile_options(hermesAbiRuntime PRIVATE /GR /EHsc)
endif()

