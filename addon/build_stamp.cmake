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
