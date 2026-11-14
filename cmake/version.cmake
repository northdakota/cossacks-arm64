# Writes OUT (a header with COSSACKS_VERSION: git describe of SRC) when its content changes, so that
# a build always carries the commit it was built from without rebuilding when nothing changed.
execute_process(COMMAND git describe --always --dirty --abbrev=10
                WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT version)
    set(version "unknown")
endif()
set(content "#pragma once\n#define COSSACKS_VERSION \"${version}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT "${old}" STREQUAL "${content}")
    file(WRITE "${OUT}" "${content}")
endif()
