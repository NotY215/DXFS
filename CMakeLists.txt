cmake_minimum_required(VERSION 3.20)
project(dxfs LANGUAGES CXX)

# ------------------------------------------------------------------ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ------------------------------------------------------------------ default to Release
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------ output layout
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY                  ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG            ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE          ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO   ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL       ${CMAKE_BINARY_DIR}/bin)

# ------------------------------------------------------------------ compiler flags
if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive- /MP /Zc:__cplusplus /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)

    set(CMAKE_CXX_FLAGS_RELEASE        "/O2 /DNDEBUG /GL")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /DNDEBUG /Zi")
    set(CMAKE_CXX_FLAGS_MINSIZEREL     "/O2 /DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG          "/Od /Zi /RTC1")

    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/LTCG /OPT:REF /OPT:ICF")
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# ==================================================================
#  dxfmt — the format library
# ==================================================================
add_library(dxfmt STATIC
        src/format.cpp
        src/blockstore.cpp
)
target_include_directories(dxfmt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ==================================================================
#  dxtool — CLI
# ==================================================================
add_executable(dxtool src/dxtool.cpp)
target_link_libraries(dxtool PRIVATE dxfmt)

# ==================================================================
#  dxstudio — Win32 GUI editor
# ==================================================================
if(WIN32)
    add_executable(dxstudio WIN32 src/dxstudio.cpp)
    target_link_libraries(dxstudio PRIVATE
            dxfmt
            comdlg32
            gdi32
            user32
            shell32
    )
endif()

# ==================================================================
#  dxedit — POSIX terminal editor (skipped on Windows)
# ==================================================================
if(NOT WIN32)
    add_executable(dxedit src/dxedit.cpp)
    target_link_libraries(dxedit PRIVATE dxfmt)
endif()

# ==================================================================
#  VS-specific
# ==================================================================
if(MSVC)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            PROPERTY VS_STARTUP_PROJECT dxstudio)
endif()

# ==================================================================
#  install
# ==================================================================
install(TARGETS dxtool RUNTIME DESTINATION bin)
if(WIN32)
    install(TARGETS dxstudio RUNTIME DESTINATION bin)
else()
    install(TARGETS dxedit RUNTIME DESTINATION bin)
endif()

# ==================================================================
#  tests
# ==================================================================
enable_testing()

add_test(NAME roundtrip_packed
        COMMAND ${CMAKE_COMMAND}
        -DDXTOOL=$<TARGET_FILE:dxtool>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/roundtrip.cmake)

add_test(NAME pi_constant
        COMMAND ${CMAKE_COMMAND}
        -DDXTOOL=$<TARGET_FILE:dxtool>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pi_test.cmake)