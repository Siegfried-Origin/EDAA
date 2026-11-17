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
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_hud_motion /Vn g_ps_hud_motion /Fh ${SHADER_OUT}/ps_hud_motion.h ${SHADER_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_ship_auto /Vn g_ps_ship_auto /Fh ${SHADER_OUT}/ps_ship_auto.h ${SHADER_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_exposure /Vn g_ps_exposure /Fh ${SHADER_OUT}/ps_exposure.h ${SHADER_SRC}
	COMMAND ${CMAKE_COMMAND} -DOUT=${SHADER_HEADER} -DHEADERS=vs_fullscreen.h,ps_motion.h,ps_debug.h,ps_hud_motion.h,ps_ship_auto.h,ps_exposure.h -P ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
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
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_taau /Vn g_ps_taau /Fh ${SHADER_OUT}/ps_taau.h ${TAA_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_sharpen /Vn g_ps_sharpen /Fh ${SHADER_OUT}/ps_sharpen.h ${TAA_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_sharpen_hdr /Vn g_ps_sharpen_hdr /Fh ${SHADER_OUT}/ps_sharpen_hdr.h ${TAA_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_fg_motion /Vn g_ps_fg_motion /Fh ${SHADER_OUT}/ps_fg_motion.h ${TAA_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_fg_motion_keep /Vn g_ps_fg_motion_keep /Fh ${SHADER_OUT}/ps_fg_motion_keep.h ${TAA_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_reactive /Vn g_ps_reactive /Fh ${SHADER_OUT}/ps_reactive.h ${TAA_SRC}
	COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_spec_stable /Vn g_ps_spec_stable /Fh ${SHADER_OUT}/ps_spec_stable.h ${TAA_SRC}
	COMMAND ${CMAKE_COMMAND} -DOUT=${TAA_HEADER} -DHEADERS=ps_taa.h,ps_copy.h,ps_hud_mask.h,ps_taau.h,ps_sharpen.h,ps_sharpen_hdr.h,ps_fg_motion.h,ps_fg_motion_keep.h,ps_reactive.h,ps_spec_stable.h -P ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	DEPENDS ${TAA_SRC} ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	COMMENT "Compiling taa.hlsl"
)
set(SHADOW_SRC ${CMAKE_SOURCE_DIR}/addon/shaders/shadow_fix.hlsl)
set(SHADOW_HEADER ${SHADER_OUT}/shadow_shaders.h)
add_custom_command(
	OUTPUT ${SHADOW_HEADER}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
	COMMAND ${FXC} /nologo /O3 /T cs_5_0 /E cs_shadow_constants /Vn g_cs_shadow_constants /Fh ${SHADER_OUT}/cs_shadow_constants.h ${SHADOW_SRC}
	COMMAND ${CMAKE_COMMAND} -DOUT=${SHADOW_HEADER} -DHEADERS=cs_shadow_constants.h -P ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	DEPENDS ${SHADOW_SRC} ${CMAKE_SOURCE_DIR}/addon/shaders/combine.cmake
	COMMENT "Compiling shadow_fix.hlsl"
)

# Version and build date/time shown in the overlay, refreshed on every build.
# The version comes from "git describe" unless EDAA_VERSION is set (release workflow).
set(EDAA_VERSION "" CACHE STRING "Version shown in the overlay (empty: git describe)")
add_custom_target(edaa_build_stamp
	COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
	COMMAND ${CMAKE_COMMAND} -DOUT=${SHADER_OUT}/build_stamp.h -DSOURCE_DIR=${CMAKE_SOURCE_DIR} "-DVERSION=${EDAA_VERSION}"
		-P ${CMAKE_SOURCE_DIR}/addon/build_stamp.cmake
	BYPRODUCTS ${SHADER_OUT}/build_stamp.h ${SHADER_OUT}/version.h
	COMMENT "Updating build stamp"
)

add_library(edaa SHARED
	addon/src/main.cpp
	addon/src/camera.cpp
	addon/src/camera_tracker.cpp
	addon/src/frame_tracking.cpp
	addon/src/mip_bias.cpp
	addon/src/passes.cpp
	addon/src/shadow_fix.cpp
	addon/src/object_motion.cpp
	addon/src/overlay.cpp
	addon/src/capture.cpp
	addon/src/recorder.cpp
	addon/src/settings.cpp
	addon/src/dlss.cpp
	addon/src/xess.cpp
	addon/src/fg_present.cpp
	addon/src/gpu_profiler.cpp
	addon/edaa.rc
	${SHADER_HEADER}
	${TAA_HEADER}
	${SHADOW_HEADER}
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
# Vendor build (docs/vendors.md): every build carries what runs on that vendor's
# cards. -DEDAA_VENDOR=amd leaves out DLSS, DLSS-G and Reflex (NVIDIA only).
set(EDAA_VENDOR "nvidia" CACHE STRING "Vendor build: nvidia or amd")
set_property(CACHE EDAA_VENDOR PROPERTY STRINGS nvidia amd)
if(EDAA_VENDOR STREQUAL "amd")
	target_compile_definitions(edaa PRIVATE EDAA_VENDOR_AMD=1)
	message(STATUS "AMD build: without DLSS, DLSS-G and Reflex")
elseif(EDAA_VENDOR STREQUAL "nvidia")
	target_compile_definitions(edaa PRIVATE EDAA_VENDOR_AMD=0)
else()
	message(FATAL_ERROR "EDAA_VENDOR must be nvidia or amd")
endif()
option(EDAA_WITH_DLSS "Build with NVIDIA DLSS when the SDK is present" ON)
set(DLSS_DIR ${CMAKE_SOURCE_DIR}/third_party/dlss)
if(EDAA_WITH_DLSS AND NOT EDAA_VENDOR STREQUAL "amd" AND EXISTS ${DLSS_DIR}/include/nvsdk_ngx_helpers.h)
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

# Offline replay of recordings (tools/replay, docs/replay.md): developer tool,
# needs the DLSS SDK. Plays the recorded inputs through the EDAA TAA shaders
# and dlss.cpp and measures the results.
option(EDAA_BUILD_REPLAY "Build the edaa-replay developer tool" OFF)
if(EDAA_BUILD_REPLAY AND DLSS_LIB)
	add_executable(edaa-replay
		tools/replay/replay.cpp
		tools/replay/reshade_shim.cpp
		addon/src/dlss.cpp
		${SHADER_HEADER}
		${TAA_HEADER}
	)
	target_include_directories(edaa-replay SYSTEM PRIVATE ${RESHADE_INCLUDE} ${IMGUI_INCLUDE} ${DLSS_DIR}/include)
	target_include_directories(edaa-replay PRIVATE ${SHADER_OUT} ${CMAKE_SOURCE_DIR}/addon/src)
	target_compile_features(edaa-replay PRIVATE cxx_std_20)
	# RESHADE_API_LIBRARY: reshade::log goes to ReShadeLogMessage (reshade_shim.cpp).
	target_compile_definitions(edaa-replay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX RESHADE_API_LIBRARY EDAA_DLSS=1 EDAA_VENDOR_AMD=0 EDAA_DEVELOPER=1)
	target_link_libraries(edaa-replay PRIVATE d3d11 windowscodecs ole32 $<IF:$<CONFIG:Debug>,${DLSS_LIB}/nvsdk_ngx_s_dbg.lib,${DLSS_LIB}/nvsdk_ngx_s.lib>)
	target_link_options(edaa-replay PRIVATE /ignore:4217)
	# Static runtime like the add-on: the NGX static library is built with /MT.
	set_property(TARGET edaa-replay PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	add_custom_command(TARGET edaa-replay POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLSS_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll $<TARGET_FILE_DIR:edaa-replay>)
elseif(EDAA_BUILD_REPLAY)
	message(STATUS "edaa-replay needs the DLSS SDK (third_party/dlss): not built")
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
# NVIDIA Streamline (DLSS Frame Generation, experimental): headers only, the DLLs
# are loaded at run time from the add-on folder. third_party/streamline is the
# extracted SDK zip (CI downloads it); the DLLs are shipped next to EDAA.addon64.
option(EDAA_WITH_STREAMLINE "Build with NVIDIA Streamline when the SDK is present" ON)
set(SL_DIR ${CMAKE_SOURCE_DIR}/third_party/streamline)
if(EDAA_WITH_STREAMLINE AND NOT EDAA_VENDOR STREQUAL "amd" AND EXISTS ${SL_DIR}/include/sl_dlss_g.h)
	target_include_directories(edaa SYSTEM PRIVATE ${SL_DIR}/include)
	target_compile_definitions(edaa PRIVATE EDAA_SL=1)
	# Production Streamline DLLs and the DLSS-G library next to EDAA.addon64.
	foreach(SL_DLL sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll)
		add_custom_command(TARGET edaa POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SL_DIR}/bin/x64/${SL_DLL} $<TARGET_FILE_DIR:edaa>)
	endforeach()
else()
	target_compile_definitions(edaa PRIVATE EDAA_SL=0)
	message(STATUS "Building without Streamline (SDK not in third_party/streamline)")
endif()
# C4702 (unreachable code) is reported by the optimizer inside reshade_overlay.hpp.
target_compile_options(edaa PRIVATE /W4 /permissive- /utf-8 /wd4702)
# Static CRT: no VC++ redistributable needed on the user's machine.
set_property(TARGET edaa PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# ReShade loads 64-bit add-ons with the .addon64 extension.
set_target_properties(edaa PROPERTIES OUTPUT_NAME "EDAA" SUFFIX ".addon64")
