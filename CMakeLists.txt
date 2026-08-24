cmake_minimum_required(VERSION 3.31...4.0)

project(JPEGView
    VERSION 2.3.0
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
        /openmp
        $<$<CONFIG:Release>:/Ox>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/Ob3>
        $<$<CONFIG:Release>:/fp:fast>
        $<$<CONFIG:Release>:/GA>
        $<$<CONFIG:Release>:/GT>
        $<$<CONFIG:Release>:/GS->
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Gw>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/Oy>
        $<$<CONFIG:Release>:/GF>
        $<$<CONFIG:Release>:/Qpar>
    )
    # Enable AVX2 on x64 architectures for enhanced vectorization
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        add_compile_options(/arch:AVX2)
    endif()

    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
else()
    # GCC / Clang / MinGW maximum performance flags (-O3, -Ofast, -flto, loop unrolling)
    add_compile_options(
        -Wall
        -Wextra
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-Ofast>
        $<$<CONFIG:Release>:-flto>
        $<$<CONFIG:Release>:-funroll-loops>
        $<$<CONFIG:Release>:-fomit-frame-pointer>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
        $<$<CONFIG:Release>:-fno-stack-protector>
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        add_compile_options(-mavx2 -mfma)
    endif()
    add_link_options(
        $<$<CONFIG:Release>:-flto>
        $<$<CONFIG:Release>:-Wl,--gc-sections>
    )
endif()

# Include ATL headers
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/atlmfc/atlmfc/include")
    include_directories(SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/deps/atlmfc/atlmfc/include")
endif()

# Include WTL headers
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/WTL-sf/Include")
    include_directories(SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/deps/WTL-sf/Include")
endif()

add_subdirectory(src/WICLoader)
add_subdirectory(src/JPEGView)
