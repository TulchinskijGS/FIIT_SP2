cmake_minimum_required(VERSION 3.15)
project(subd CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(include)

add_executable(prog
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/catalog.cpp
    src/pager.cpp
    src/table.cpp
    src/executor.cpp
)