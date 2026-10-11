cmake_minimum_required(VERSION 3.20)
project(EDAA LANGUAGES CXX)

# Third-party headers are fetched by third_party/fetch.ps1 (versions in third_party/VERSIONS.txt).
set(RESHADE_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/reshade/include)
set(IMGUI_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/imgui)
if(NOT EXISTS ${RESHADE_INCLUDE}/reshade.hpp OR NOT EXISTS ${IMGUI_INCLUDE}/imgui.h)
	message(FATAL_ERROR "Missing headers: run third_party/fetch.ps1 first")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Elite Dangerous is 64-bit: configure with -A x64")
endif()

# HLSL -> bytecode headers with fxc from the Windows SDK.
find_program(FXC fxc PATHS "C:/Program Files (x86)/Windows Kits/10/bin" PATH_SUFFIXES 10.0.26100.0/x64 10.0.22621.0/x64 10.0.19041.0/x64 REQUIRED)
set(SHADER_SRC ${CMAKE_SOURCE_DIR}/addon/shaders/motion.hlsl)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/generated)
set(SHADER_HEADER ${SHADER_OUT}/motion_shaders.h)
add_custom_command(
	OUTPUT ${SHADER_HEADER}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
	COMMAND ${FXC} /nologo /O3 /T vs_5_0 /E vs_fullscreen /Vn g_vs_fullscreen /Fh ${SHADER_OUT}/vs_fullscreen.h ${SHADER_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_motion /Vn g_ps_motion /Fh ${SHADER_OUT}/ps_motion.h ${SHADER_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_debug /Vn g_ps_debug /Fh ${SHADER_OUT}/ps_debug.h ${SHADER_SRC}
	COMMAND ${CMAKE_COMMAND} -DOUT=${SHADER_HEADER} -DHEADERS=vs_fullscreen.h,ps_motion.h,ps_debug.h -P ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	DEPENDS ${SHADER_SRC} ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	COMMENT "Compiling motion.hlsl"
)
set(TAA_SRC ${CMAKE_SOURCE_DIR}/addon/shaders/taa.hlsl)
set(TAA_HEADER ${SHADER_OUT}/taa_shaders.h)
add_custom_command(
	OUTPUT ${TAA_HEADER}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_taa /Vn g_ps_taa /Fh ${SHADER_OUT}/ps_taa.h ${TAA_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_copy /Vn g_ps_copy /Fh ${SHADER_OUT}/ps_copy.h ${TAA_SRC}
	COMMAND ${CMAKE_COMMAND} -DOUT=${TAA_HEADER} -DHEADERS=ps_taa.h,ps_copy.h -P ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	DEPENDS ${TAA_SRC} ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	COMMENT "Compiling taa.hlsl"
)

# Build date/time shown in the overlay, refreshed on every build.
add_custom_target(edaa_build_stamp
	COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
	COMMAND ${CMAKE_COMMAND} -DOUT=${SHADER_OUT}/build_stamp.h -P ${CMAKE_SOURCE_DIR}/addon/build_stamp.cmake
	BYPRODUCTS ${SHADER_OUT}/build_stamp.h
	COMMENT "Updating build stamp"
)

add_library(edaa SHARED
	addon/src/main.cpp
	addon/src/camera.cpp
	addon/src/camera_tracker.cpp
	addon/src/frame_tracking.cpp
	addon/src/passes.cpp
	addon/src/object_motion.cpp
	addon/src/overlay.cpp
	addon/src/capture.cpp
	addon/src/settings.cpp
	${SHADER_HEADER}
	${TAA_HEADER}
)
target_include_directories(edaa SYSTEM PRIVATE ${RESHADE_INCLUDE} ${IMGUI_INCLUDE})
target_include_directories(edaa PRIVATE ${SHADER_OUT})
add_dependencies(edaa edaa_build_stamp)
target_compile_features(edaa PRIVATE cxx_std_20)
target_compile_definitions(edaa PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
# Developer UI (debug views, capture, diagnostics): -DEDAA_DEVELOPER=ON, always on in Debug.
option(EDAA_DEVELOPER "Build with developer tools in the overlay" OFF)
target_compile_definitions(edaa PRIVATE EDAA_DEVELOPER=$<IF:$<OR:$<BOOL:${EDAA_DEVELOPER}>,$<CONFIG:Debug>>,1,0>)
# C4702 (unreachable code) is reported by the optimizer inside reshade_overlay.hpp.
target_compile_options(edaa PRIVATE /W4 /permissive- /utf-8 /wd4702)
# Static CRT: no VC++ redistributable needed on the user's machine.
set_property(TARGET edaa PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# ReShade loads 64-bit add-ons with the .addon64 extension.
set_target_properties(edaa PROPERTIES OUTPUT_NAME "EDAA" SUFFIX ".addon64")
