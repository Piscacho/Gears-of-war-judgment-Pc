cmake_minimum_required(VERSION 3.20)
project(GOWJRecomp CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(RECOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# -DGOWJ_TU=ON: version con la actualizacion del titulo (ppc_tu/, GOWJTU.exe)
option(GOWJ_TU "Version con la actualizacion del titulo (v0x403)" OFF)
if(GOWJ_TU)
  set(PPC_DIR ${RECOMP_ROOT}/ppc_tu)
  set(GOWJ_OUT GOWJTU)
else()
  set(PPC_DIR ${RECOMP_ROOT}/ppc)
  set(GOWJ_OUT GOWJ)
endif()
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/ppc_recomp.*.cpp ${PPC_DIR}/ppc_func_mapping.cpp)

# Codigo PPC traducido (enorme): libreria aparte con flags propias
add_library(GOWJRecompLib STATIC ${PPC_SOURCES})
target_include_directories(GOWJRecompLib PUBLIC ${PPC_DIR} ${RECOMP_ROOT}/XenonRecomp/thirdparty/simde)
target_compile_options(GOWJRecompLib PRIVATE /O1 -fno-strict-aliasing -msse4.1 -Wno-everything -gline-tables-only /Z7 "/FI${RECOMP_ROOT}/compat/ppc_indirect.h")

set(FFMPEG "G:/Guitar Hero Warrior of rock/Recomp/toolchain/ffmpeg")
set(RT ${RECOMP_ROOT}/runtime)
add_executable(GOWJ ${RT}/main.cpp ${RT}/kernel.cpp ${RT}/kernel_gowj.cpp ${RT}/hooks_gowj.cpp ${RT}/fibers.cpp ${RT}/import_stubs.cpp ${RT}/threads.cpp ${RT}/fs.cpp ${RT}/gpu_stub.cpp ${RT}/diag.cpp
    ${RT}/gpu_cp.cpp ${RT}/gpu_window.cpp ${RT}/atomics.cpp ${RT}/xam.cpp ${RT}/audio_wasapi.cpp ${RT}/launcher.cpp ${RT}/launcher_gears.cpp ${RT}/net_gowj.cpp ${RT}/xexp_patch.cpp ${RT}/dualsense.cpp ${RT}/texdirty.cpp
    ${RT}/gpu_d3d12.cpp ${RT}/gpu_shader.cpp ${RT}/gpu_shadertest.cpp ${RT}/xma.cpp)
target_include_directories(GOWJ PRIVATE ${RT} ${FFMPEG}/include)
target_compile_options(GOWJ PRIVATE /O2 -fno-strict-aliasing -msse4.1 -Wno-unused-parameter /Z7)
target_compile_definitions(GOWJ PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN $<$<BOOL:${GOWJ_TU}>:GOWJ_TU>)
set_target_properties(GOWJ PROPERTIES OUTPUT_NAME ${GOWJ_OUT})
target_link_libraries(GOWJ PRIVATE GOWJRecompLib user32 gdi32 d3d12 dxgi d3dcompiler winmm ole32 xinput bcrypt comdlg32 hid setupapi comctl32 ws2_32 dwmapi uxtheme shell32 windowscodecs msimg32 psapi ${FFMPEG}/lib/avcodec.lib ${FFMPEG}/lib/avutil.lib)
add_custom_command(TARGET GOWJ POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG}/bin/avcodec-62.dll ${FFMPEG}/bin/avutil-60.dll ${FFMPEG}/bin/swresample-6.dll $<TARGET_FILE_DIR:GOWJ>)
target_link_options(GOWJ PRIVATE /DEBUG /SUBSYSTEM:CONSOLE)
