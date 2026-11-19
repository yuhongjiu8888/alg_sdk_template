# Standalone utility tests; no chip SDK, model files or OpenCV required.
cmake_minimum_required(VERSION 3.10)
project(alg_sdk_unit_tests LANGUAGES CXX)

enable_testing()
add_executable(postprocess_utils_test postprocess_utils_test.cpp)
target_compile_features(postprocess_utils_test PRIVATE cxx_std_14)
target_include_directories(postprocess_utils_test PRIVATE ../include ../src)
add_test(NAME postprocess_utils COMMAND postprocess_utils_test)
