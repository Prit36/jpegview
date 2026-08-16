cmake_minimum_required(VERSION 3.31...4.0)

project(JPEGViewBenchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    STRICT
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    add_compile_options(
        /MP
        /W4
        /permissive-
        /utf-8
        /EHsc
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Gw>
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        add_compile_options(/arch:AVX2)
    endif()
endif()

# Include ATL & WTL headers
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/atlmfc/atlmfc/include")
    include_directories(SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/atlmfc/atlmfc/include")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/WTL-sf/Include")
    include_directories(SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/WTL-sf/Include")
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/JPEGView
)

add_executable(JPEGViewBenchmark
    BenchmarkMain.cpp
)

target_link_libraries(JPEGViewBenchmark PRIVATE
    shlwapi
    Dbghelp
    ole32
    uuid
)

set_target_properties(JPEGViewBenchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.cache/bin"
)
