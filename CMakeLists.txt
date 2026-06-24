cmake_minimum_required(VERSION 3.10)
project(rc64 VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
#set(CMAKE_C_STANDARD_REQUIRED ON)

# Release flags by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
add_compile_options(
    -Wall -Wextra
#    -D_POSIX_C_SOURCE=199309L
)

# macOS: need _DARWIN_C_SOURCE for snprintf
if(APPLE)
    add_compile_options(-D_DARWIN_C_SOURCE=1)
endif()

# Release: -O3
set(CMAKE_C_FLAGS_RELEASE "-O3")

# Math library: separate 'libm' on Unix, part of libc on Windows/macOS.
# find_library leaves the variable empty when 'm' does not exist (e.g. MSVC),
# so we only link it where it is actually present.
find_library(MATH_LIBRARY m)

# --- Encoder ---
add_executable(rc_encode rc_encode.c)
if(MATH_LIBRARY)
    target_link_libraries(rc_encode ${MATH_LIBRARY})
endif()

# --- Decoder ---
add_executable(rc_decode rc_decode.c)
if(MATH_LIBRARY)
    target_link_libraries(rc_decode ${MATH_LIBRARY})
endif()

# --- Test data generator ---
add_executable(gen_data gen_data.c)

# --- Diagnostics (encodes without writing, reports branch stats) ---
add_executable(rc_diag rc_diag.c)
if(MATH_LIBRARY)
    target_link_libraries(rc_diag ${MATH_LIBRARY})
endif()

# --- 32-bit engine (RC_TOTAL_BITS=12, 16-bit words, 256-symbol alphabet) ---
add_executable(rc_encode32 rc_encode32.c)
if(MATH_LIBRARY)
    target_link_libraries(rc_encode32 ${MATH_LIBRARY})
endif()

add_executable(rc_decode32 rc_decode32.c)
if(MATH_LIBRARY)
    target_link_libraries(rc_decode32 ${MATH_LIBRARY})
endif()

# --- Roundtrip test ---
add_custom_target(roundtrip
    COMMAND sh ${CMAKE_SOURCE_DIR}/roundtrip.sh ${CMAKE_BINARY_DIR}
    DEPENDS rc_encode rc_decode gen_data
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running roundtrip tests on all datasets (64-bit engine)"
)

add_custom_target(roundtrip32
    COMMAND sh ${CMAKE_SOURCE_DIR}/roundtrip32.sh ${CMAKE_BINARY_DIR}
    DEPENDS rc_encode32 rc_decode32 gen_data
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running roundtrip tests on all datasets (32-bit engine, 12-bit total)"
)