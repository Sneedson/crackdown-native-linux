cmake_minimum_required (VERSION 3.8)

project("XexRepack")

add_executable(XexRepack "main.cpp")

target_link_libraries(XexRepack PRIVATE XenonUtils)

target_compile_definitions(XexRepack PRIVATE _CRT_SECURE_NO_WARNINGS)
