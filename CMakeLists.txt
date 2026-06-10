cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(t3s3_lora VERSION 1.0.0 LANGUAGES C)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE
    src/main.c
    src/sensors/bm688.c
)
