cmake_minimum_required(VERSION 3.18)

if(DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
endif()

project(ps2vita LANGUAGES C CXX)

option(ASTRART_DEFER_GPR_WRITEBACK
       "Cache low GPRs and flush dirty values at generated exits" ON)
option(ASTRART_DIRECT_TRACES
       "Enable fingerprint-bound guard-free direct trace wrappers" OFF)
# Optional privately supplied PS2 ELF. No retail binary or its translated code
# is included in the source tree or ordinary release builds.
set(ASTRART_GAME_ELF "" CACHE FILEPATH "Local PS2 ELF for host-only startup differential probe")
# Strictly opt-in, locally generated title code for personal Vita builds.
# The ELF and generated C++ remain in the user's local build directory.
set(ASTRART_PRIVATE_VITA_ELF "" CACHE FILEPATH "Local ELF for a private Vita title-specific AOT experiment")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASTRART_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ASTRART_SMOKE_ELF "${ASTRART_GENERATED_DIR}/astrarecomp-smoke.elf")
set(ASTRART_CHAIN_ELF "${ASTRART_GENERATED_DIR}/aot-chain.elf")
set(ASTRART_MEMORY_ELF "${ASTRART_GENERATED_DIR}/aot-memory.elf")
set(ASTRART_BYTE_QUAD_ELF "${ASTRART_GENERATED_DIR}/aot-byte-quad.elf")
set(ASTRART_UNSUPPORTED_ELF "${ASTRART_GENERATED_DIR}/aot-unsupported.elf")
set(ASTRART_BRANCH_ELF "${ASTRART_GENERATED_DIR}/aot-branch.elf")
set(ASTRART_BNE_ELF "${ASTRART_GENERATED_DIR}/aot-bne.elf")
set(ASTRART_HOT_SCALAR_ELF "${ASTRART_GENERATED_DIR}/aot-hot-scalars.elf")
set(ASTRART_BENCHMARK_ELF "${ASTRART_GENERATED_DIR}/aot-benchmark.elf")
set(ASTRART_BENCHMARK_HEADER "${ASTRART_GENERATED_DIR}/aot_benchmark_program.hpp")
set(ASTRART_PHASE0_CPP "${ASTRART_GENERATED_DIR}/phase0_aot_package.cpp")
set(ASTRART_ORACLE_CENSUS "${ASTRART_GENERATED_DIR}/aot-oracle-census.json")
set(ASTRART_TRACE_PLAN "${ASTRART_GENERATED_DIR}/aot-trace-plan.json")
set(ASTRART_OPTIMIZATION_ARGS)
if(ASTRART_DEFER_GPR_WRITEBACK)
  list(APPEND ASTRART_OPTIMIZATION_ARGS --defer-gpr-writeback)
endif()
if(ASTRART_DIRECT_TRACES)
  list(APPEND ASTRART_OPTIMIZATION_ARGS --enable-direct-traces)
endif()
add_custom_command(
  OUTPUT "${ASTRART_PHASE0_CPP}" "${ASTRART_BENCHMARK_HEADER}"
         "${ASTRART_ORACLE_CENSUS}" "${ASTRART_TRACE_PLAN}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${ASTRART_GENERATED_DIR}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py" "${ASTRART_SMOKE_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_chain_elf.py" "${ASTRART_CHAIN_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" "${ASTRART_MEMORY_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --byte-quad "${ASTRART_BYTE_QUAD_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --unsupported "${ASTRART_UNSUPPORTED_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --branches "${ASTRART_BRANCH_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --branches-bne "${ASTRART_BNE_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_memory_elf.py" --hot-scalars "${ASTRART_HOT_SCALAR_ELF}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_benchmark_elf.py"
          "${ASTRART_BENCHMARK_ELF}" --header "${ASTRART_BENCHMARK_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_aot_census.py"
          --output "${ASTRART_ORACLE_CENSUS}"
          "${ASTRART_SMOKE_ELF}" "${ASTRART_CHAIN_ELF}"
          "${ASTRART_MEMORY_ELF}" "${ASTRART_BYTE_QUAD_ELF}" "${ASTRART_UNSUPPORTED_ELF}"
          "${ASTRART_BRANCH_ELF}" "${ASTRART_BNE_ELF}"
          "${ASTRART_HOT_SCALAR_ELF}" "${ASTRART_BENCHMARK_ELF}"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_astrart.py"
          --output "${ASTRART_PHASE0_CPP}"
          --execution-census "${ASTRART_ORACLE_CENSUS}"
          --trace-plan-output "${ASTRART_TRACE_PLAN}"
          ${ASTRART_OPTIMIZATION_ARGS}
          "${ASTRART_SMOKE_ELF}" "${ASTRART_CHAIN_ELF}"
          "${ASTRART_MEMORY_ELF}" "${ASTRART_BYTE_QUAD_ELF}" "${ASTRART_UNSUPPORTED_ELF}"
          "${ASTRART_BRANCH_ELF}" "${ASTRART_BNE_ELF}"
          "${ASTRART_HOT_SCALAR_ELF}" "${ASTRART_BENCHMARK_ELF}"
  DEPENDS
    tools/generate_astrart.py
    tools/astrair/__init__.py
    tools/astrair/ir.py
    tools/astrair/builder.py
    tools/astrair/analysis_effects.py
    tools/astrair/analysis_width.py
    tools/astrair/analysis_liveness.py
    tools/astrair/deopt.py
    tools/astrair/profile.py
    tools/astrair/trace.py
    tools/make_aot_census.py
    tools/make_smoke_elf.py
    tools/make_aot_chain_elf.py
    tools/make_aot_memory_elf.py
    tools/make_aot_benchmark_elf.py
  VERBATIM
)

add_library(ps2core
  "${ASTRART_PHASE0_CPP}"
  "${ASTRART_BENCHMARK_HEADER}"
  src/core/aot.cpp
  src/core/cpu.cpp
  src/core/ee_block.cpp
  src/core/elf_loader.cpp
  src/core/emulator.cpp
  src/core/gif.cpp
  src/core/gs.cpp
  src/core/iop_cpu.cpp
  src/core/memory.cpp
  src/core/spu2_adpcm.cpp
  src/core/spu2_envelope.cpp
  src/core/spu2_voice.cpp
  src/core/vif.cpp
  src/core/vu.cpp
)
if(ASTRART_DIRECT_TRACES)
  target_compile_definitions(ps2core PUBLIC PS2VITA_ASTRART_DIRECT_TRACES=1)
endif()
if(NOT VITA)
  target_sources(ps2core PRIVATE src/core/execution_census.cpp)
endif()
target_include_directories(ps2core PUBLIC include PRIVATE "${ASTRART_GENERATED_DIR}")
if(MSVC)
  target_compile_options(ps2core PRIVATE /W4)
else()
  target_compile_options(ps2core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(VITA)
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

  set(VITA_APP_NAME "AstraRecomp")
  set(VITA_TITLEID "PS2VITA01")
  set(VITA_VERSION "01.00")

  add_executable(ps2vita
    src/vita/main.cpp
    src/vita/screen.cpp
  )
  target_include_directories(ps2vita PRIVATE include src/vita)
  target_compile_options(ps2vita PRIVATE -Wall -Wextra -Wpedantic -O2)
  target_link_libraries(ps2vita PRIVATE
    ps2core
    SceCtrl_stub
    SceDisplay_stub
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceProcessmgr_stub
    SceSysmem_stub
  )

  if(ASTRART_PRIVATE_VITA_ELF)
    if(NOT EXISTS "${ASTRART_PRIVATE_VITA_ELF}")
      message(FATAL_ERROR "ASTRART_PRIVATE_VITA_ELF does not exist: ${ASTRART_PRIVATE_VITA_ELF}")
    endif()
    set(ASTRART_PRIVATE_VITA_AOT "${CMAKE_CURRENT_BINARY_DIR}/private-title/private_aot.cpp")
    add_custom_command(
      OUTPUT "${ASTRART_PRIVATE_VITA_AOT}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_astrart.py"
              --output "${ASTRART_PRIVATE_VITA_AOT}"
              --package-name "private-vita-startup"
              --factory-name "generated_private_game_aot_package"
              --guard-guest-code --defer-gpr-writeback
              --max-block-instructions 8
              "${ASTRART_PRIVATE_VITA_ELF}"
      DEPENDS "${ASTRART_PRIVATE_VITA_ELF}" tools/generate_astrart.py
              tools/astrair/builder.py tools/astrair/ir.py
      VERBATIM
    )
    target_sources(ps2vita PRIVATE "${ASTRART_PRIVATE_VITA_AOT}")
    target_compile_definitions(ps2vita PRIVATE PS2VITA_PRIVATE_GAME_AOT=1)
  endif()

  vita_create_self(ps2vita.self ps2vita UNSAFE)
  vita_create_vpk(ps2vita.vpk ${VITA_TITLEID} ps2vita.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
  )
else()
  enable_testing()
  if(ASTRART_GAME_ELF)
    if(NOT EXISTS "${ASTRART_GAME_ELF}")
      message(FATAL_ERROR "ASTRART_GAME_ELF does not exist: ${ASTRART_GAME_ELF}")
    endif()
    set(ASTRART_GAME_AOT "${CMAKE_CURRENT_BINARY_DIR}/game-private/game_startup_aot.cpp")
    add_custom_command(
      OUTPUT "${ASTRART_GAME_AOT}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_astrart.py"
              --output "${ASTRART_GAME_AOT}" --package-name "private-game-entry"
              --factory-name "generated_private_host_game_aot_package"
              --guard-guest-code --defer-gpr-writeback --max-block-instructions 8
              "${ASTRART_GAME_ELF}"
      DEPENDS "${ASTRART_GAME_ELF}" tools/generate_astrart.py
              tools/astrair/builder.py tools/astrair/ir.py
      VERBATIM
    )
    add_executable(nfsmw_entry_probe
      tools/nfsmw_entry_probe.cpp "${ASTRART_GAME_AOT}")
    target_include_directories(nfsmw_entry_probe PRIVATE include)
    target_link_libraries(nfsmw_entry_probe PRIVATE ps2core)
    add_test(NAME nfsmw_entry_probe
      COMMAND nfsmw_entry_probe "${ASTRART_GAME_ELF}")
    # Proves the actual privately generated package links and executes native
    # code. CI supplies a redistributable synthetic ELF; no game bytes are
    # uploaded to GitHub.
    add_executable(private_game_aot_probe
      tools/private_game_aot_probe.cpp "${ASTRART_GAME_AOT}")
    target_include_directories(private_game_aot_probe PRIVATE include)
    target_link_libraries(private_game_aot_probe PRIVATE ps2core)
    add_test(NAME private_game_aot_probe
      COMMAND private_game_aot_probe "${ASTRART_GAME_ELF}")
  endif()
  add_executable(ps2core_tests tests/core_tests.cpp)
  target_link_libraries(ps2core_tests PRIVATE ps2core)
  add_executable(bare_elf_startup_syscalls_tests tests/bare_elf_startup_syscalls_tests.cpp)
  target_link_libraries(bare_elf_startup_syscalls_tests PRIVATE ps2core)
  if(NOT MSVC)
    target_compile_options(bare_elf_startup_syscalls_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME bare_elf_startup_syscalls_tests COMMAND bare_elf_startup_syscalls_tests)
  add_executable(cpu_dispatch_tests tests/cpu_dispatch_tests.cpp)
  target_link_libraries(cpu_dispatch_tests PRIVATE ps2core)
  add_executable(cpu_nop_tests tests/cpu_nop_tests.cpp)
  target_link_libraries(cpu_nop_tests PRIVATE ps2core)
  if(MSVC)
    target_compile_options(cpu_nop_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(cpu_nop_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cpu_nop_tests COMMAND cpu_nop_tests)
  if(NOT MSVC)
    target_compile_options(cpu_dispatch_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME cpu_dispatch_tests COMMAND cpu_dispatch_tests)
  add_executable(video_timing_tests tests/video_timing_tests.cpp)
  target_link_libraries(video_timing_tests PRIVATE ps2core)
  if(NOT MSVC)
    target_compile_options(video_timing_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME video_timing_tests COMMAND video_timing_tests)
  add_executable(iop_timer_prescaler_tests tests/iop_timer_prescaler_tests.cpp)
  target_link_libraries(iop_timer_prescaler_tests PRIVATE ps2core)
  if(MSVC)
    target_compile_options(iop_timer_prescaler_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(iop_timer_prescaler_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME iop_timer_prescaler_tests COMMAND iop_timer_prescaler_tests)
  add_executable(vu1_flag_tests tests/vu1_flag_tests.cpp)
  target_link_libraries(vu1_flag_tests PRIVATE ps2core)
  add_executable(vu1_div_tests tests/vu1_div_tests.cpp)
  target_link_libraries(vu1_div_tests PRIVATE ps2core)
  add_test(NAME vu1_div_tests COMMAND vu1_div_tests)
  add_executable(vu1_branch_tests tests/vu1_branch_tests.cpp)
  target_link_libraries(vu1_branch_tests PRIVATE ps2core)
  add_test(NAME vu1_branch_tests COMMAND vu1_branch_tests)
  add_executable(vu1_mul_tests tests/vu1_mul_tests.cpp)
  target_link_libraries(vu1_mul_tests PRIVATE ps2core)
  add_test(NAME vu1_mul_tests COMMAND vu1_mul_tests)
  add_executable(memory_dma_timing_tests tests/memory_dma_timing_tests.cpp)
  target_link_libraries(memory_dma_timing_tests PRIVATE ps2core)
  add_test(NAME memory_dma_timing_tests COMMAND memory_dma_timing_tests)
  add_executable(vif1_dma_tests tests/vif1_dma_tests.cpp)
  target_link_libraries(vif1_dma_tests PRIVATE ps2core)
  add_test(NAME vif1_dma_tests COMMAND vif1_dma_tests)
  add_executable(emulator_graphics_timing_tests tests/emulator_graphics_timing_tests.cpp)
  target_link_libraries(emulator_graphics_timing_tests PRIVATE ps2core)
  add_test(NAME emulator_graphics_timing_tests COMMAND emulator_graphics_timing_tests)
  add_executable(graphics_service_queue_tests tests/graphics_service_queue_tests.cpp)
  target_link_libraries(graphics_service_queue_tests PRIVATE ps2core)
  if(MSVC)
    target_compile_options(graphics_service_queue_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(graphics_service_queue_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME graphics_service_queue_tests COMMAND graphics_service_queue_tests)
  add_executable(vif_directhl_tests tests/vif_directhl_tests.cpp)
  target_link_libraries(vif_directhl_tests PRIVATE ps2core)
  if(MSVC)
    target_compile_options(vif_directhl_tests PRIVATE /W4 /WX)
  else()
    target_compile_options(vif_directhl_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME vif_directhl_tests COMMAND vif_directhl_tests)
  add_executable(execution_budget_tests tests/execution_budget_tests.cpp)
  target_include_directories(execution_budget_tests PRIVATE include)
  add_test(NAME execution_budget_tests COMMAND execution_budget_tests)
  add_executable(gs_display_preview_tests tests/gs_display_preview_tests.cpp)
  target_link_libraries(gs_display_preview_tests PRIVATE ps2core)
  add_test(NAME gs_display_preview_tests COMMAND gs_display_preview_tests)
  add_executable(gs_display_extent_tests tests/gs_display_extent_tests.cpp)
  target_include_directories(gs_display_extent_tests PRIVATE include)
  add_test(NAME gs_display_extent_tests COMMAND gs_display_extent_tests)
  add_executable(monitor_blit_tests tests/monitor_blit_tests.cpp)
  target_include_directories(monitor_blit_tests PRIVATE include)
  add_test(NAME monitor_blit_tests COMMAND monitor_blit_tests)
  add_executable(monitor_blit_benchmark tools/monitor_blit_benchmark.cpp)
  target_include_directories(monitor_blit_benchmark PRIVATE include)
  # Match the Vita frontend's optimization level for this host-only copy probe.
  if(MSVC)
    target_compile_options(monitor_blit_benchmark PRIVATE /W4 /O2)
  else()
    target_compile_options(monitor_blit_benchmark PRIVATE -Wall -Wextra -Wpedantic -O2)
  endif()
  add_test(NAME monitor_blit_benchmark_smoke COMMAND monitor_blit_benchmark 1)
  add_executable(vif_dma_physical_tests tests/vif_dma_physical_tests.cpp)
  target_link_libraries(vif_dma_physical_tests PRIVATE ps2core)
  add_test(NAME vif_dma_physical_tests COMMAND vif_dma_physical_tests)
  add_executable(gs_pixel_watch_tests tests/gs_pixel_watch_tests.cpp)
  target_link_libraries(gs_pixel_watch_tests PRIVATE ps2core)
  add_test(NAME gs_pixel_watch_tests COMMAND gs_pixel_watch_tests)
  add_executable(gs_signed_texture_tests tests/gs_signed_texture_tests.cpp)
  target_link_libraries(gs_signed_texture_tests PRIVATE ps2core)
  add_test(NAME gs_signed_texture_tests COMMAND gs_signed_texture_tests)
  add_executable(gif_sprite_coverage_tests tests/gif_sprite_coverage_tests.cpp)
  target_link_libraries(gif_sprite_coverage_tests PRIVATE ps2core)
  add_test(NAME gif_sprite_coverage_tests COMMAND gif_sprite_coverage_tests)
  add_executable(gif_bilinear_tests tests/gif_bilinear_tests.cpp)
  target_link_libraries(gif_bilinear_tests PRIVATE ps2core)
  add_test(NAME gif_bilinear_tests COMMAND gif_bilinear_tests)
  add_executable(gif_subpixel_triangle_tests tests/gif_subpixel_triangle_tests.cpp)
  target_link_libraries(gif_subpixel_triangle_tests PRIVATE ps2core)
  add_test(NAME gif_subpixel_triangle_tests COMMAND gif_subpixel_triangle_tests)
  add_executable(gif_trace_tests tests/gif_trace_tests.cpp)
  target_link_libraries(gif_trace_tests PRIVATE ps2core)
  add_test(NAME gif_trace_tests COMMAND gif_trace_tests)
  add_executable(ee_fpu_tests tests/ee_fpu_tests.cpp)
  target_link_libraries(ee_fpu_tests PRIVATE ps2core)
  add_test(NAME ee_fpu_tests COMMAND ee_fpu_tests)
  add_executable(ps2bios_trace tools/bios_trace.cpp)
  target_link_libraries(ps2bios_trace PRIVATE ps2core)
  add_executable(ps2bios_benchmark tools/bios_benchmark.cpp)
  target_link_libraries(ps2bios_benchmark PRIVATE ps2core)
  add_test(NAME bios_benchmark_self_test COMMAND ps2bios_benchmark --self-test)
  if(NOT MSVC)
    target_compile_options(ps2bios_benchmark PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_executable(ps2gif_replay tools/gif_replay.cpp)
  target_link_libraries(ps2gif_replay PRIVATE ps2core)
  add_executable(ps2gif_benchmark tools/gif_benchmark.cpp)
  target_link_libraries(ps2gif_benchmark PRIVATE ps2core)
  # Separate host diagnostic: one raster sample per native pixel, without
  # changing the default/Vita quarter grid or linking unlike configurations.
  add_library(ps2graphics_native src/core/gif.cpp src/core/gs.cpp)
  target_include_directories(ps2graphics_native PUBLIC include)
  target_compile_definitions(ps2graphics_native PUBLIC PS2VITA_GS_RASTER_SCALE=1)
  add_executable(ps2gif_replay_native tools/gif_replay.cpp)
  target_link_libraries(ps2gif_replay_native PRIVATE ps2graphics_native)
  add_executable(ps2gif_benchmark_native tools/gif_benchmark.cpp)
  target_link_libraries(ps2gif_benchmark_native PRIVATE ps2graphics_native)
  # Isolated host-only density experiment: keep the core and Vita at scale 4.
  add_library(ps2graphics_half src/core/gif.cpp src/core/gs.cpp)
  target_include_directories(ps2graphics_half PUBLIC include)
  target_compile_definitions(ps2graphics_half PUBLIC PS2VITA_GS_RASTER_SCALE=2)
  add_executable(ps2gif_replay_half tools/gif_replay.cpp)
  target_link_libraries(ps2gif_replay_half PRIVATE ps2graphics_half)
  add_executable(ps2gif_benchmark_half tools/gif_benchmark.cpp)
  target_link_libraries(ps2gif_benchmark_half PRIVATE ps2graphics_half)
  add_executable(gs_half_resolution_tests tests/gs_half_resolution_tests.cpp)
  target_link_libraries(gs_half_resolution_tests PRIVATE ps2graphics_half)
  add_test(NAME gs_half_resolution_tests COMMAND gs_half_resolution_tests)
  add_executable(gs_target_access_tests tests/gs_target_access_tests.cpp)
  target_link_libraries(gs_target_access_tests PRIVATE ps2core)
  add_test(NAME gs_target_access_tests COMMAND gs_target_access_tests)
  add_executable(gs_half_target_access_tests tests/gs_target_access_tests.cpp)
  target_link_libraries(gs_half_target_access_tests PRIVATE ps2graphics_half)
  add_test(NAME gs_half_target_access_tests COMMAND gs_half_target_access_tests)
  add_executable(gs_native_target_access_tests tests/gs_target_access_tests.cpp)
  target_link_libraries(gs_native_target_access_tests PRIVATE ps2graphics_native)
  add_test(NAME gs_native_target_access_tests COMMAND gs_native_target_access_tests)
  foreach(target gs_target_access_tests gs_half_target_access_tests gs_native_target_access_tests)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4)
    else()
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endforeach()
  add_test(NAME gif_benchmark_tool_tests
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gif_benchmark.py"
            "$<TARGET_FILE:ps2gif_benchmark>" "$<TARGET_FILE:ps2gif_replay>" 4)
  add_test(NAME gif_native_benchmark_tool_tests
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gif_benchmark.py"
            "$<TARGET_FILE:ps2gif_benchmark_native>" "$<TARGET_FILE:ps2gif_replay_native>" 1)
  add_test(NAME gif_half_benchmark_tool_tests
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gif_benchmark.py"
            "$<TARGET_FILE:ps2gif_benchmark_half>" "$<TARGET_FILE:ps2gif_replay_half>" 2)
  add_executable(gs_native_resolution_tests tests/gs_native_resolution_tests.cpp)
  target_link_libraries(gs_native_resolution_tests PRIVATE ps2graphics_native)
  add_test(NAME gs_native_resolution_tests COMMAND gs_native_resolution_tests)
  add_executable(gif_native_sprite_coverage_tests tests/gif_sprite_coverage_tests.cpp)
  target_link_libraries(gif_native_sprite_coverage_tests PRIVATE ps2graphics_native)
  add_test(NAME gif_native_sprite_coverage_tests COMMAND gif_native_sprite_coverage_tests)
  add_executable(gif_native_bilinear_tests tests/gif_bilinear_tests.cpp)
  target_link_libraries(gif_native_bilinear_tests PRIVATE ps2graphics_native)
  add_test(NAME gif_native_bilinear_tests COMMAND gif_native_bilinear_tests)
  add_executable(gif_native_subpixel_triangle_tests tests/gif_subpixel_triangle_tests.cpp)
  target_link_libraries(gif_native_subpixel_triangle_tests PRIVATE ps2graphics_native)
  add_test(NAME gif_native_subpixel_triangle_tests COMMAND gif_native_subpixel_triangle_tests)
  add_test(NAME gif_native_replay_tool_tests
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gif_native_replay.py"
            "$<TARGET_FILE:ps2gif_replay_native>" "$<TARGET_FILE:ps2gif_replay>")
  add_test(NAME gif_replay_tool_tests
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gif_replay.py"
            "$<TARGET_FILE:ps2gif_replay>" "$<TARGET_FILE:ps2bios_trace>")
  add_test(NAME cloud_vertex_tool_tests
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -p test_compare_cloud_vertices.py)
  add_test(NAME bios_trace_watch_tool_tests
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_bios_trace_watch.py"
            "$<TARGET_FILE:ps2bios_trace>")
  add_executable(ps2vif_replay tools/vif_replay.cpp)
  target_link_libraries(ps2vif_replay PRIVATE ps2core)
  add_executable(ps2vu_math_probe tools/vu_math_probe.cpp)
  target_link_libraries(ps2vu_math_probe PRIVATE ps2core)
  add_executable(ps2vu_benchmark tools/vu_benchmark.cpp)
  target_link_libraries(ps2vu_benchmark PRIVATE ps2core)
  add_test(NAME vu_math_mul_exact COMMAND ps2vu_math_probe mul 40000000 40400000 00000000 40C00000)
  add_test(NAME vu_math_madd_exact COMMAND ps2vu_math_probe madd 40000000 40400000 3F800000 40E00000)
  add_test(NAME vu_math_reject_malformed COMMAND ps2vu_math_probe mul 0x123456 00000000 00000000)
  set_tests_properties(vu_math_reject_malformed PROPERTIES WILL_FAIL TRUE)
  add_test(NAME vu_math_detect_mismatch COMMAND ps2vu_math_probe mul 40000000 40400000 00000000 00000000)
  set_tests_properties(vu_math_detect_mismatch PROPERTIES WILL_FAIL TRUE)
  add_executable(aot_benchmark tools/aot_benchmark.cpp)
  target_link_libraries(aot_benchmark PRIVATE ps2core)
  if(MSVC)
    target_compile_options(ps2core_tests PRIVATE /W4)
    target_compile_options(vu1_flag_tests PRIVATE /W4)
    target_compile_options(vu1_div_tests PRIVATE /W4)
    target_compile_options(vu1_branch_tests PRIVATE /W4)
    target_compile_options(vu1_mul_tests PRIVATE /W4)
    target_compile_options(memory_dma_timing_tests PRIVATE /W4)
    target_compile_options(vif1_dma_tests PRIVATE /W4)
    target_compile_options(emulator_graphics_timing_tests PRIVATE /W4)
    target_compile_options(execution_budget_tests PRIVATE /W4)
    target_compile_options(vif_dma_physical_tests PRIVATE /W4)
    target_compile_options(gs_pixel_watch_tests PRIVATE /W4)
    target_compile_options(gs_signed_texture_tests PRIVATE /W4)
    target_compile_options(gif_sprite_coverage_tests PRIVATE /W4)
    target_compile_options(gif_bilinear_tests PRIVATE /W4)
    target_compile_options(gif_native_bilinear_tests PRIVATE /W4)
    target_compile_options(gif_subpixel_triangle_tests PRIVATE /W4)
    target_compile_options(gif_native_subpixel_triangle_tests PRIVATE /W4)
    target_compile_options(gif_trace_tests PRIVATE /W4)
    target_compile_options(ee_fpu_tests PRIVATE /W4)
    target_compile_options(ps2gif_replay PRIVATE /W4)
    target_compile_options(ps2gif_benchmark PRIVATE /W4)
    target_compile_options(ps2graphics_native PRIVATE /W4)
    target_compile_options(ps2gif_replay_native PRIVATE /W4)
    target_compile_options(ps2gif_benchmark_native PRIVATE /W4)
    target_compile_options(ps2graphics_half PRIVATE /W4)
    target_compile_options(ps2gif_replay_half PRIVATE /W4)
    target_compile_options(ps2gif_benchmark_half PRIVATE /W4)
    target_compile_options(gs_half_resolution_tests PRIVATE /W4)
    target_compile_options(gs_native_resolution_tests PRIVATE /W4)
    target_compile_options(gif_native_sprite_coverage_tests PRIVATE /W4)
    target_compile_options(ps2bios_trace PRIVATE /W4)
    target_compile_options(ps2vif_replay PRIVATE /W4)
    target_compile_options(aot_benchmark PRIVATE /W4)
  else()
    target_compile_options(ps2core_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(vu1_flag_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(vu1_div_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(vu1_branch_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(vu1_mul_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(memory_dma_timing_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(vif1_dma_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(emulator_graphics_timing_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(execution_budget_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(vif_dma_physical_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gs_pixel_watch_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gs_signed_texture_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gif_sprite_coverage_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gif_bilinear_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gif_native_bilinear_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gif_subpixel_triangle_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gif_native_subpixel_triangle_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gif_trace_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ee_fpu_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2gif_replay PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2gif_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2graphics_native PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2gif_replay_native PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2gif_benchmark_native PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2graphics_half PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2gif_replay_half PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2gif_benchmark_half PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gs_half_resolution_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gs_native_resolution_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gif_native_sprite_coverage_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2bios_trace PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ps2vif_replay PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(aot_benchmark PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ps2core_tests COMMAND ps2core_tests)
  add_test(NAME vu1_flag_tests COMMAND vu1_flag_tests)
  add_executable(path3_graphics_tests tests/path3_graphics_tests.cpp)
  target_link_libraries(path3_graphics_tests PRIVATE ps2core)
  add_test(NAME path3_graphics_tests COMMAND path3_graphics_tests)
  add_test(NAME path3_stq_graphics_tests COMMAND path3_graphics_tests --stq)
  add_test(NAME path3_rgb_decal_tests COMMAND path3_graphics_tests --rgb-decal)
  add_test(NAME path3_rgb_modulate_tests COMMAND path3_graphics_tests --rgb-modulate)
  add_test(NAME path3_highlight_tests COMMAND path3_graphics_tests --highlight)
  add_test(NAME path3_highlight2_tests COMMAND path3_graphics_tests --highlight2)
  add_test(NAME path3_region_repeat_tests COMMAND path3_graphics_tests --region-repeat)
  add_test(NAME path3_feedback_tests COMMAND path3_graphics_tests --feedback)
  add_test(NAME astrair_builder_tests
           COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
                   "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_astrair.py")
  add_test(NAME vu1_reference_tool_tests
           COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
                   "${Python3_EXECUTABLE}" -m unittest discover
                   -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -p test_vu1_reference.py)
  add_test(NAME vu1_math_fixture_tool_tests
           COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
                   "${Python3_EXECUTABLE}" -m unittest discover
                   -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -p test_vu1_math_fixture.py)
  add_test(NAME vif_serialized_fixture_tool_tests
           COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
                   "${Python3_EXECUTABLE}" -m unittest discover
                   -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -p test_serialize_vif_fixture.py)
  add_test(NAME vu1_math_vif_replay
           COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
                   "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_vu1_math_replay.py"
                   "$<TARGET_FILE:ps2vif_replay>")
endif()
