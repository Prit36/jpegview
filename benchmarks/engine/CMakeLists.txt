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
        $<$<CONFIG:Release>:/Ox>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Ot>
        $<$<CONFIG:Release>:/Ob3>
        $<$<CONFIG:Release>:/fp:fast>
        $<$<CONFIG:Release>:/GA>
        $<$<CONFIG:Release>:/GS->
        $<$<CONFIG:Release>:/Gy>
        $<$<CONFIG:Release>:/Gw>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/Oy>
        $<$<CONFIG:Release>:/GF>
        $<$<CONFIG:Release>:/Qpar>
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        add_compile_options(/arch:AVX2)
    endif()
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
else()
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
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        add_compile_options(-mavx2 -mfma)
    endif()
    add_link_options(
        $<$<CONFIG:Release>:-flto>
        $<$<CONFIG:Release>:-Wl,--gc-sections>
    )
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/JPEGView/libjpeg-turbo/include
)

add_executable(JPEGViewBenchmark
    BenchmarkMain.cpp
)

target_link_directories(JPEGViewBenchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/JPEGView/libjpeg-turbo/lib64
)

target_link_libraries(JPEGViewBenchmark PRIVATE
    turbojpeg-static
    shlwapi
    Dbghelp
    ole32
    uuid
)

set_target_properties(JPEGViewBenchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.cache/bin"
)
