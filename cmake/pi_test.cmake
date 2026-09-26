# Verify the constant codec reproduces the first digits of pi exactly.

if(NOT DEFINED DXTOOL)
    message(FATAL_ERROR "DXTOOL not set")
endif()
if(NOT DEFINED WORKDIR)
    set(WORKDIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

file(MAKE_DIRECTORY "${WORKDIR}")

# 1) generate 1000 digits of pi
execute_process(
        COMMAND "${DXTOOL}" gen pi 1000 "${WORKDIR}/pi.dx"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gen pi failed: ${out}")
endif()
message(STATUS "gen: ${out}")

# 2) info — confirm it is the constant codec and tiny
execute_process(
        COMMAND "${DXTOOL}" info "${WORKDIR}/pi.dx"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE info
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "info failed")
endif()
message(STATUS "info:\n${info}")

if(NOT info MATCHES "codec     : constant")
    message(FATAL_ERROR "expected constant codec")
endif()
if(NOT info MATCHES "file size : 33 bytes")
    message(FATAL_ERROR "expected 33-byte file")
endif()

# 3) unpack and check the leading digits
execute_process(
        COMMAND "${DXTOOL}" unpack "${WORKDIR}/pi.dx" "${WORKDIR}/pi.txt"
        RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "unpack failed")
endif()

file(READ "${WORKDIR}/pi.txt" digits)
string(SUBSTRING "${digits}" 0 20 head)
if(NOT head STREQUAL "31415926535897932384")
    message(FATAL_ERROR "pi digits wrong: got '${head}'")
endif()

message(STATUS "pi_constant OK")