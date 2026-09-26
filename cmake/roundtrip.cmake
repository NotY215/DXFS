# Round-trip test: generate pseudo-random digits, pack, unpack, diff.
# Invoked by ctest with -DDXTOOL=... -DWORKDIR=...

if(NOT DEFINED DXTOOL)
    message(FATAL_ERROR "DXTOOL not set")
endif()
if(NOT DEFINED WORKDIR)
    set(WORKDIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

file(MAKE_DIRECTORY "${WORKDIR}")

# 1) generate a deterministic digit source (xorshift in CMake script)
set(seed 88172645463325252)
set(text "")
set(state ${seed})
foreach(i RANGE 4999)
    math(EXPR a "${state} >> 12")
    math(EXPR state "${state} ^ ${a}")
    math(EXPR a "${state} << 25")
    math(EXPR state "${state} ^ ${a}")
    math(EXPR a "${state} >> 27")
    math(EXPR state "${state} ^ ${a}")
    math(EXPR d "${state} % 10")
    string(APPEND text "${d}")
endforeach()

file(WRITE "${WORKDIR}/rt_in.txt" "${text}")

# 2) pack
execute_process(
        COMMAND "${DXTOOL}" pack "${WORKDIR}/rt_in.txt" "${WORKDIR}/rt.dx"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "pack failed (${rc}): ${out}")
endif()
message(STATUS "pack: ${out}")

# 3) unpack
execute_process(
        COMMAND "${DXTOOL}" unpack "${WORKDIR}/rt.dx" "${WORKDIR}/rt_out.txt"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "unpack failed (${rc}): ${out}")
endif()

# 4) compare
file(READ "${WORKDIR}/rt_in.txt"  a)
file(READ "${WORKDIR}/rt_out.txt" b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "round-trip mismatch")
endif()

# 5) check
execute_process(
        COMMAND "${DXTOOL}" check "${WORKDIR}/rt.dx"
        RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "check failed")
endif()

message(STATUS "roundtrip_packed OK")