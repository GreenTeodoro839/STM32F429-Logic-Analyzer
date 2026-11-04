cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi.cmake CACHE FILEPATH "")

# Target board: F429 (STM32F429IGT6 dev board) or F103 (STM32F103C8T6 minimal board).
#   cmake -S . -B build/f429 -G Ninja
#   cmake -S . -B build/f103 -G Ninja -DLA_BOARD=F103
set(LA_BOARD F429 CACHE STRING "Target board: F429 or F103")
set_property(CACHE LA_BOARD PROPERTY STRINGS F429 F103)
string(TOLOWER "${LA_BOARD}" BOARD)
if(NOT BOARD MATCHES "^(f429|f103)$")
  message(FATAL_ERROR "LA_BOARD must be F429 or F103, not '${LA_BOARD}'")
endif()

project(la_${BOARD} C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(TP   ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(TUSB ${TP}/tinyusb/src)
set(APP  la_${BOARD})

# Protocol layer shared by both boards
set(COMMON_SOURCES
  common/sump.c
  common/stream.c
  common/usb_descriptors.c
  common/syscalls.c
  ${TUSB}/tusb.c
  ${TUSB}/common/tusb_fifo.c
  ${TUSB}/device/usbd.c
  ${TUSB}/class/cdc/cdc_device.c
)

if(BOARD STREQUAL "f103")
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
  endif()
  # (Kept in the original link order, so the image matches the tested F103 build.)
  add_executable(${APP}
    f103/main.c
    f103/clock.c
    f103/oled.c
    f103/font6x8.c
    f103/ui.c
    f103/capture.c
    common/sump.c
    common/stream.c
    common/syscalls.c
    f103/testsig.c
    common/usb_descriptors.c
    ${TP}/cmsis_device_f1/Source/Templates/system_stm32f1xx.c
    ${TP}/cmsis_device_f1/Source/Templates/gcc/startup_stm32f103xb.s
    ${TUSB}/tusb.c
    ${TUSB}/common/tusb_fifo.c
    ${TUSB}/device/usbd.c
    ${TUSB}/class/cdc/cdc_device.c
    ${TUSB}/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
    ${TUSB}/portable/st/stm32_fsdev/fsdev_common.c
  )
  target_include_directories(${APP} PRIVATE f103 common
    ${TP}/CMSIS_6/CMSIS/Core/Include ${TP}/cmsis_device_f1/Include ${TUSB})
  target_compile_definitions(${APP} PRIVATE STM32F103xB CFG_TUSB_MCU=OPT_MCU_STM32F1)
  set(CPU_FLAGS -mcpu=cortex-m3 -mthumb)
  set(LD_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/f103/stm32f103c8.ld)
  # Hot paths (sample scanning / stream encoding) are built for speed, the rest for size.
  set_source_files_properties(common/stream.c f103/capture.c PROPERTIES COMPILE_OPTIONS -O2)
  # WCH-Link in CMSIS-DAP mode
  set(OPENOCD_ARGS -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f1x.cfg
      -c "adapter speed 4000")
else()
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  endif()
  add_executable(${APP}
    ${COMMON_SOURCES}
    f429/main.c
    f429/clock.c
    f429/sdram.c
    f429/lcd.c
    f429/gfx.c
    f429/font.c
    f429/ui.c
    f429/keys.c
    f429/capture.c
    f429/testsig.c
    f429/selftest.c
    ${TP}/cmsis_device_f4/Source/Templates/system_stm32f4xx.c
    ${TP}/cmsis_device_f4/Source/Templates/gcc/startup_stm32f429xx.s
    ${TUSB}/portable/synopsys/dwc2/dcd_dwc2.c
    ${TUSB}/portable/synopsys/dwc2/dwc2_common.c
  )
  target_include_directories(${APP} PRIVATE f429 common
    ${TP}/CMSIS_6/CMSIS/Core/Include ${TP}/cmsis_device_f4/Include ${TUSB})
  target_compile_definitions(${APP} PRIVATE STM32F429xx CFG_TUSB_MCU=OPT_MCU_STM32F4)
  set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16)
  set(LD_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/f429/stm32f429ig.ld)
  # GCC false positive in TinyUSB's FIFO code at -O2 (index provably < 4)
  set_source_files_properties(${TUSB}/common/tusb_fifo.c PROPERTIES COMPILE_OPTIONS -Wno-stringop-overflow)
  # On-board DAP-Link (CMSIS-DAP). Pick it by VID:PID in case a second probe is plugged in.
  set(F429_DAP_VID_PID "0xc251 0xf001" CACHE STRING "VID PID of the F429 board's CMSIS-DAP probe")
  set(OPENOCD_ARGS -f interface/cmsis-dap.cfg -c "cmsis_dap_vid_pid ${F429_DAP_VID_PID}"
      -c "transport select swd" -f target/stm32f4x.cfg -c "adapter speed 4000")
endif()

target_compile_options(${APP} PRIVATE
  ${CPU_FLAGS}
  -Wall -Wextra
  -ffunction-sections -fdata-sections -fno-common
  $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
)

target_link_options(${APP} PRIVATE
  ${CPU_FLAGS}
  -T${LD_SCRIPT}
  --specs=nano.specs --specs=nosys.specs
  -Wl,--gc-sections
  -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${APP}.map
  -Wl,--print-memory-usage
  -Wl,--no-warn-rwx-segments
)
set_target_properties(${APP} PROPERTIES SUFFIX ".elf" LINK_DEPENDS ${LD_SCRIPT})

add_custom_command(TARGET ${APP} POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${APP}> ${APP}.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${APP}> ${APP}.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${APP}>
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# cmake --build <dir> --target flash
add_custom_target(flash
  COMMAND openocd ${OPENOCD_ARGS} -c "program $<TARGET_FILE:${APP}> verify reset exit"
  DEPENDS ${APP}
  USES_TERMINAL
)
