cmake_minimum_required(VERSION 3.20)
project(nui_provider LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NUI_PROVIDER_BUILD_TESTS "Build the Catch2 test suite" ON)
option(NUI_PROVIDER_WERROR "Treat warnings as errors" OFF)

# Everything lands in one place the way xenia-build.py does it, so the
# provider can be dropped next to the emulator without hunting.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/obj)

add_library(nui_format INTERFACE)
target_include_directories(nui_format INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# fopen/strerror are not deprecated by MSVC's own standard library, only by its
# "secure CRT" wrappers. The provider is a little CLI tool, not a service that
# needs them silenced per call site.
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

add_library(nui_core STATIC
  src/backends/synthetic/synthetic.cc
  src/ipc/file_writer.cc
)
target_include_directories(nui_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(nui_core PUBLIC nui_format)

if(MSVC)
  target_compile_options(nui_core PRIVATE /W4 /permissive-)
  if(NUI_PROVIDER_WERROR)
    target_compile_options(nui_core PRIVATE /WX)
  endif()
else()
  target_compile_options(nui_core PRIVATE -Wall -Wextra)
  if(NUI_PROVIDER_WERROR)
    target_compile_options(nui_core PRIVATE -Werror)
  endif()
endif()

add_executable(nui-provider src/main.cc)
target_link_libraries(nui-provider PRIVATE nui_core)
if(MSVC)
  target_compile_options(nui-provider PRIVATE /W4 /permissive-)
endif()

if(NUI_PROVIDER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(third_party/catch EXCLUDE_FROM_ALL)
  add_executable(nui_tests tests/codec_test.cc tests/ipc_test.cc)
  target_link_libraries(nui_tests PRIVATE nui_core Catch2::Catch2WithMain)
  add_test(NAME nui_tests COMMAND nui_tests)
endif()

install(TARGETS nui-provider RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)