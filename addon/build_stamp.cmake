# Writes OUT with the version and the current date and time, so every build
# shows what it is and when it was made (__DATE__/__TIME__ only change when
# main.cpp itself is recompiled).
# Version: VERSION if given (release workflow: the tag), else
# "git describe --tags --dirty --always" (e.g. 0.1.0-3-gabc1234-dirty).
string(TIMESTAMP stamp "%Y-%m-%d %H:%M:%S")

set(version "${VERSION}")
if(version STREQUAL "")
	find_package(Git QUIET)
	if(GIT_FOUND)
		execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
			WORKING_DIRECTORY ${SOURCE_DIR}
			OUTPUT_VARIABLE version
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET)
	endif()
endif()
if(version STREQUAL "")
	set(version "dev")
endif()
string(REGEX REPLACE "^v" "" version "${version}")

file(WRITE ${OUT} "#pragma once\n#define EDAA_BUILD_STAMP \"${stamp}\"\n#define EDAA_VERSION \"${version}\"\n")

# Numeric version for the DLL version resource (edaa.rc, shown by ReShade):
# "0.3.1-7-g3251ddf-dirty" -> 0.3.1.7 (4th number: commits since the tag).
set(numbers 0 0 0 0)
if(version MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9]+))?")
	set(numbers ${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${CMAKE_MATCH_3} 0)
	if(CMAKE_MATCH_5)
		list(POP_BACK numbers)
		list(APPEND numbers ${CMAKE_MATCH_5})
	endif()
endif()
string(REPLACE ";" "," numbers "${numbers}")
set(rc "#define EDAA_VERSION_NUMBERS ${numbers}\n#define EDAA_VERSION_TEXT \"${version}\"\n")

# Rewritten only when it changes: the resource is not recompiled at every build.
get_filename_component(out_dir "${OUT}" DIRECTORY)
set(rc_out "${out_dir}/version.h")
if(EXISTS "${rc_out}")
	file(READ "${rc_out}" previous)
endif()
if(NOT previous STREQUAL rc)
	file(WRITE "${rc_out}" "${rc}")
endif()
