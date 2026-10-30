cmake_minimum_required(VERSION 3.13)

# Pick a board that has PSRAM wired to QMI CS1.
# Pimoroni Pico Plus 2 shown (PSRAM CS on GPIO47). SparkFun boards use GPIO8.
set(PICO_BOARD pimoroni_pico_plus2_rp2350)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(psram_exec C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(psram_exec
    main.c
    psram.c
)

# GPIO used as PSRAM chip select (XIP_CS1 function)
target_compile_definitions(psram_exec PRIVATE PSRAM_CS_PIN=47)

target_link_libraries(psram_exec
    pico_stdlib
    hardware_clocks
    hardware_gpio
    hardware_irq
    hardware_sync
    hardware_xip_cache
)

# Extra linker script: augments the SDK script (it uses INSERT AFTER .rodata)
target_link_options(psram_exec PRIVATE
    "LINKER:-T,${CMAKE_CURRENT_LIST_DIR}/psram_text.ld")
set_property(TARGET psram_exec APPEND PROPERTY
    LINK_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/psram_text.ld)

pico_enable_stdio_usb(psram_exec 0)
pico_enable_stdio_uart(psram_exec 1)

pico_add_extra_outputs(psram_exec)
