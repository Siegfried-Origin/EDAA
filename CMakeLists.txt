cmake_minimum_required(VERSION 3.20)
project(EDAA LANGUAGES CXX)

# Dependencies are git submodules in third_party/ (pinned versions):
#   git submodule update --init --depth 1
set(RESHADE_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/reshade/include)
set(IMGUI_INCLUDE ${CMAKE_SOURCE_DIR}/third_party/imgui)
if(NOT EXISTS ${RESHADE_INCLUDE}/reshade.hpp OR NOT EXISTS ${IMGUI_INCLUDE}/imgui.h)
	message(FATAL_ERROR "Missing dependencies: run 'git submodule update --init --depth 1'")
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
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_hud_mask /Vn g_ps_hud_mask /Fh ${SHADER_OUT}/ps_hud_mask.h ${TAA_SRC}
	COMMAND ${CMAKE_COMMAND} -DOUT=${TAA_HEADER} -DHEADERS=ps_taa.h,ps_copy.h,ps_hud_mask.h -P ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	DEPENDS ${TAA_SRC} ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	COMMENT "Compiling taa.hlsl"
)

# Version and build date/time shown in the overlay, refreshed on every build.
# The version comes from "git describe" unless EDAA_VERSION is set (release workflow).
set(EDAA_VERSION "" CACHE STRING "Version shown in the overlay (empty: git describe)")
add_custom_target(edaa_build_stamp
	COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
	COMMAND ${CMAKE_COMMAND} -DOUT=${SHADER_OUT}/build_stamp.h -DSOURCE_DIR=${CMAKE_SOURCE_DIR} "-DVERSION=${EDAA_VERSION}"
		-P ${CMAKE_SOURCE_DIR}/addon/build_stamp.cmake
	BYPRODUCTS ${SHADER_OUT}/build_stamp.h
	COMMENT "Updating build stamp"
)

add_library(edaa SHARED
	addon/src/main.cpp
	addon/src/camera.cpp
	addon/src/camera_tracker.cpp
	addon/src/frame_tracking.cpp
	addon/src/passes.cpp
	addon/src/mip_bias.cpp
	addon/src/object_motion.cpp
	addon/src/overlay.cpp
	addon/src/capture.cpp
	addon/src/settings.cpp
	addon/src/dlss.cpp
	addon/src/xess.cpp
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
# NVIDIA DLSS (DLAA): built in when the SDK submodule third_party/dlss is checked out.
# The submodule only references NVIDIA's repository: the SDK itself is shipped
# only inside the application (static lib + release DLL), as its license requires.
option(EDAA_WITH_DLSS "Build with NVIDIA DLSS when the SDK is present" ON)
set(DLSS_DIR ${CMAKE_SOURCE_DIR}/third_party/dlss)
if(EDAA_WITH_DLSS AND EXISTS ${DLSS_DIR}/include/nvsdk_ngx_helpers.h)
	target_include_directories(edaa SYSTEM PRIVATE ${DLSS_DIR}/include)
	set(DLSS_LIB ${DLSS_DIR}/lib/Windows_x86_64/x64)
	target_link_libraries(edaa PRIVATE $<IF:$<CONFIG:Debug>,${DLSS_LIB}/nvsdk_ngx_s_dbg.lib,${DLSS_LIB}/nvsdk_ngx_s.lib>)
	target_compile_definitions(edaa PRIVATE EDAA_DLSS=1)
	# The release nvngx_dlss.dll (never the watermarked dev one) next to
	# EDAA.addon64, to copy both into the game folder.
	add_custom_command(TARGET edaa POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLSS_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll $<TARGET_FILE_DIR:edaa>)
else()
	target_compile_definitions(edaa PRIVATE EDAA_DLSS=0)
	message(STATUS "Building without DLSS (EDAA_WITH_DLSS off or SDK not in third_party/dlss)")
endif()
# Intel XeSS (Native AA through a D3D12 bridge): built in when the SDK submodule
# third_party/xess is checked out. libxess.dll is delay-loaded, so the add-on
# still loads without it (or on systems where it cannot load).
option(EDAA_WITH_XESS "Build with Intel XeSS when the SDK is present" ON)
set(XESS_DIR ${CMAKE_SOURCE_DIR}/third_party/xess)
if(EDAA_WITH_XESS AND EXISTS ${XESS_DIR}/inc/xess/xess_d3d12.h)
	target_include_directories(edaa SYSTEM PRIVATE ${XESS_DIR}/inc)
	target_link_libraries(edaa PRIVATE ${XESS_DIR}/lib/libxess.lib d3d12.lib delayimp.lib)
	target_link_options(edaa PRIVATE /DELAYLOAD:libxess.dll /DELAYLOAD:d3d12.dll)
	target_compile_definitions(edaa PRIVATE EDAA_XESS=1)
	add_custom_command(TARGET edaa POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XESS_DIR}/bin/libxess.dll $<TARGET_FILE_DIR:edaa>)
else()
	target_compile_definitions(edaa PRIVATE EDAA_XESS=0)
	message(STATUS "Building without XeSS (EDAA_WITH_XESS off or SDK not in third_party/xess)")
endif()
# C4702 (unreachable code) is reported by the optimizer inside reshade_overlay.hpp.
target_compile_options(edaa PRIVATE /W4 /permissive- /utf-8 /wd4702)
# Static CRT: no VC++ redistributable needed on the user's machine.
set_property(TARGET edaa PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# ReShade loads 64-bit add-ons with the .addon64 extension.
set_target_properties(edaa PROPERTIES OUTPUT_NAME "EDAA" SUFFIX ".addon64")
