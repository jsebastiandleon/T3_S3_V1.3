cmake_minimum_required(VERSION 3.20.0)

# Overlay que tú usas
list(APPEND EXTRA_DTC_OVERLAY_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/board.overlay
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(t3s3_lora VERSION 1.0.0 LANGUAGES C)

target_sources(app PRIVATE src/main.c)
