# CMakeLists.txt --- CMake project settings
##############################################################################

# CMake minimum version
cmake_minimum_required(VERSION 3.10)

# project name and languages
project(MThreadJob CXX)

# MThreadJob_test.exe
add_executable(MThreadJob_test WIN32 MThreadJob_test.cpp)
