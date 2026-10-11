# Writes OUT, a header including the per-shader headers produced by fxc.
# Usage: cmake -DOUT=<header> -DHEADERS=<a.h,b.h> -P combine.cmake
string(REPLACE "," ";" headers "${HEADERS}")
set(content "// Generated: compiled shaders, see addon/shaders/*.hlsl\n#pragma once\n")
foreach(h ${headers})
	string(APPEND content "#include \"${h}\"\n")
endforeach()
file(WRITE ${OUT} "${content}")
