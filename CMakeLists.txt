cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi.cmake CACHE FILEPATH "")

project(la_f103 C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()

set(TP   ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(TUSB ${TP}/tinyusb/src)

add_executable(la_f103
  src/main.c
  src/clock.c
  src/oled.c
  src/font6x8.c
  src/ui.c
  src/capture.c
  src/sump.c
  src/stream.c
  src/syscalls.c
  src/testsig.c
  src/usb_descriptors.c
  ${TP}/cmsis_device_f1/Source/Templates/system_stm32f1xx.c
  ${TP}/cmsis_device_f1/Source/Templates/gcc/startup_stm32f103xb.s
  # TinyUSB (device + CDC only)
  ${TUSB}/tusb.c
  ${TUSB}/common/tusb_fifo.c
  ${TUSB}/device/usbd.c
  ${TUSB}/class/cdc/cdc_device.c
  ${TUSB}/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
  ${TUSB}/portable/st/stm32_fsdev/fsdev_common.c
)

target_include_directories(la_f103 PRIVATE
  src
  ${TP}/CMSIS_6/CMSIS/Core/Include
  ${TP}/cmsis_device_f1/Include
  ${TUSB}
)

target_compile_definitions(la_f103 PRIVATE
  STM32F103xB
  CFG_TUSB_MCU=OPT_MCU_STM32F1
)

set(CPU_FLAGS -mcpu=cortex-m3 -mthumb)

# Hot paths (sample scanning / stream encoding) are built for speed, the rest for size.
set_source_files_properties(src/stream.c src/capture.c PROPERTIES COMPILE_OPTIONS -O2)

target_compile_options(la_f103 PRIVATE
  ${CPU_FLAGS}
  -Wall -Wextra
  -ffunction-sections -fdata-sections -fno-common
  $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
)

target_link_options(la_f103 PRIVATE
  ${CPU_FLAGS}
  -T${CMAKE_CURRENT_SOURCE_DIR}/ld/stm32f103c8.ld
  --specs=nano.specs --specs=nosys.specs
  -Wl,--gc-sections
  -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/la_f103.map
  -Wl,--print-memory-usage
  -Wl,--no-warn-rwx-segments
)
set_target_properties(la_f103 PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ld/stm32f103c8.ld)

add_custom_command(TARGET la_f103 POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:la_f103> la_f103.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:la_f103> la_f103.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:la_f103>
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Flash through the WCH-Link (CMSIS-DAP mode) with OpenOCD:  cmake --build build --target flash
set(OPENOCD_ARGS -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f1x.cfg -c "adapter speed 4000")
add_custom_target(flash
  COMMAND openocd ${OPENOCD_ARGS} -c "program $<TARGET_FILE:la_f103> verify reset exit"
  DEPENDS la_f103
  USES_TERMINAL
)
