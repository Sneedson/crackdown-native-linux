# The Crackdown port's launcher: install (the disc folder, title update 6, downloadable content), build,
# profiles, play. Build: cmake -S launcher -B build/launcher -G Ninja && ninja -C build/launcher
cmake_minimum_required(VERSION 3.20)
project(crackdown-launcher LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(crackdown-launcher main.cpp)
target_link_libraries(crackdown-launcher PRIVATE Qt6::Widgets)
