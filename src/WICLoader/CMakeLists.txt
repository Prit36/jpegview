add_library(WICLoader SHARED
    WICLoader.cpp
    dllmain.cpp
    stdafx.cpp
    WICLoader.rc
)

target_include_directories(WICLoader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(WICLoader PRIVATE
    WICLOADER_EXPORTS
    _USRDLL
)

target_link_libraries(WICLoader PRIVATE
    WindowsCodecs
    ole32
    oleaut32
    uuid
)

set_target_properties(WICLoader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
