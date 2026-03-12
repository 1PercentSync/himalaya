cmake_minimum_required(VERSION 4.1)
project(himalaya LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(rhi)
add_subdirectory(framework)
add_subdirectory(passes)
add_subdirectory(app)