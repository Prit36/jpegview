cmake_minimum_required(VERSION 3.31...4.0)
project(proftool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(proftool
    proftool.cpp
    atlstub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/JPEGView/ParallelJPEGV2.cpp
)

target_include_directories(proftool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/JPEGView
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/JPEGView/libjpeg-turbo/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../extras/third_party/libjpeg-turbo/src
)

if(MSVC)
    target_compile_definitions(proftool PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(proftool PRIVATE /Ox /Oi /Ot /Ob3 /fp:fast /GS- /Gy /GL /arch:AVX2 /Zi)
    target_link_options(proftool PRIVATE /LTCG /OPT:REF /OPT:ICF)
endif()

find_library(TURBOJPEG_LIB turbojpeg-static
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/JPEGView/libjpeg-turbo/lib64
        ${CMAKE_SOURCE_DIR}/src/JPEGView/libjpeg-turbo/lib64
        ${CMAKE_SOURCE_DIR}/../src/JPEGView/libjpeg-turbo/lib64)
if(NOT TURBOJPEG_LIB)
    # Profiling helper is optional - skip instead of breaking the whole configure
    message(STATUS "proftool disabled: turbojpeg-static library not found")
    return()
endif()
target_link_libraries(proftool PRIVATE ${TURBOJPEG_LIB})

set_target_properties(proftool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
