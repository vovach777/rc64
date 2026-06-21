cmake_minimum_required(VERSION 3.10)
project(rc64 VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Release flags by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
add_compile_options(
    -Wall -Wextra
    -D_POSIX_C_SOURCE=199309L
)

# macOS: need _DARWIN_C_SOURCE for snprintf
if(APPLE)
    add_compile_options(-D_DARWIN_C_SOURCE=1)
endif()

# Release: -O3
set(CMAKE_C_FLAGS_RELEASE "-O3")

# --- Encoder ---
add_executable(rc_encode rc_encode.c)
target_link_libraries(rc_encode m)

# --- Decoder ---
add_executable(rc_decode rc_decode.c)
target_link_libraries(rc_decode m)

# --- Test data generator ---
add_executable(gen_data gen_data.c)

# --- Diagnostics (encodes without writing, reports branch stats) ---
add_executable(rc_diag rc_diag.c)
target_link_libraries(rc_diag m)

# --- Roundtrip test ---
add_custom_target(roundtrip
    COMMAND sh ${CMAKE_SOURCE_DIR}/roundtrip.sh ${CMAKE_BINARY_DIR}
    DEPENDS rc_encode rc_decode gen_data
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running roundtrip tests on all datasets"
)