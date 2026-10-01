# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED PICO_PLATFORM)
	set(PICO_PLATFORM rp2350)
endif()
if (NOT DEFINED PICO_BOARD)
	# same board definition hsdaoh-rp2350 uses for dual_external_adc (RP2350B)
	set(PICO_BOARD solderparty_rp2350_stamp_xl)
endif()

include(pico_sdk_import.cmake)

project(pcm1802_diag C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(pcm1802_diag
	pcm1802_diag.c
)

target_compile_options(pcm1802_diag PRIVATE -Wall)

target_link_libraries(pcm1802_diag
	pico_stdlib
	hardware_pio
	hardware_clocks
	hardware_vreg
)
pico_generate_pio_header(pcm1802_diag ${CMAKE_CURRENT_LIST_DIR}/pcm1802_diag.pio)

# run from RAM, so the raised system clock never touches flash timing
pico_set_binary_type(pcm1802_diag copy_to_ram)

# report over USB serial; keep UART off GP0/GP1 (PCM1802 #1 DOUT/BCK)
pico_enable_stdio_usb(pcm1802_diag 1)
pico_enable_stdio_uart(pcm1802_diag 0)

pico_add_extra_outputs(pcm1802_diag)
