cmake_minimum_required(VERSION 3.28)

project(JPEGView
    VERSION 2.0.0
    LANGUAGES C CXX
    DESCRIPTION "Ultra-fast, modern, lightweight image viewer and editor for Windows"
)

# Standard C++23 (ISO C++23)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable Unicode & Windows 10/11 Targeting
add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    STRICT
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    _WIN32_IE=0x0A00
)

if(MSVC)
    add_compile_options(
        /MP
        /W4
        /permissive-
        /Zc:inline
        /Zc:throwingNew
        /Zc:preprocessor
        /utf-8
        /EHsc
    )
    # Enable AVX2 on x64 architectures for enhanced vectorization
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        add_compile_options(/arch:AVX2)
    endif()

    # Enable Interprocedural Optimization (Link-Time Code Generation) in Release
    include(CheckIPOSupported OPTIONAL)
    if(COMMAND check_ipo_supported)
        check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
        if(ipo_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        endif()
    endif()
endif()

# Include WTL headers
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/WTL-sf/Include")
    include_directories(SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/deps/WTL-sf/Include")
endif()

add_subdirectory(src/WICLoader)
add_subdirectory(src/JPEGView)
