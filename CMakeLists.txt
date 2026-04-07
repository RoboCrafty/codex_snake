cmake_minimum_required(VERSION 3.16)

project(snake_terminal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(snake
    src/main.cpp
)

target_compile_options(snake PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_custom_command(TARGET snake POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/snake.conf
        $<TARGET_FILE_DIR:snake>/snake.conf
)
