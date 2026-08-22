set(JPEGVIEW_SOURCES
    AboutDlg.cpp
    ApplyFilterAVX.cpp
    AVIFWrapper.cpp
    BasicProcessing.cpp
    BatchCopyDlg.cpp
    Clipboard.cpp
    CropCtl.cpp
    CropSizeDlg.cpp
    D2DRenderer.cpp
    dcraw_mod.cpp
    DesktopWallpaper.cpp
    DirectoryWatcher.cpp
    EXIFDisplay.cpp
    EXIFDisplayCtl.cpp
    EXIFHelpers.cpp
    EXIFReader.cpp
    FileExtensionsRegistry.cpp
    FileList.cpp
    FileOpenDialog.cpp
    GUIControls.cpp
    HashCompareLPCTSTR.cpp
    HEIFWrapper.cpp
    HelpDisplay.cpp
    HelpDisplayCtl.cpp
    HelpDlg.cpp
    Helpers.cpp
    HelpersGUI.cpp
    HistogramCorr.cpp
    ICCProfileTransform.cpp
    ImageLoadThread.cpp
    ImageProcessingPanel.cpp
    ImageProcPanelCtl.cpp
    InfoButtonPanel.cpp
    InfoButtonPanelCtl.cpp
    JPEGImage.cpp
    JPEGLosslessTransform.cpp
    JPEGProvider.cpp
    JPEGView.cpp
    JPEGView.rc
    JXLWrapper.cpp
    KeyMap.cpp
    LocalDensityCorr.cpp
    MainDlg.cpp
    ManageOpenWithDlg.cpp
    MultiMonitorSupport.cpp
    NavigationPanel.cpp
    NavigationPanelCtl.cpp
    NLS.cpp
    PaintMemDCMgr.cpp
    Panel.cpp
    PanelController.cpp
    PanelMgr.cpp
    ParameterDB.cpp
    ParallelJPEGV2.cpp
    PNGWrapper.cpp
    PrintDlg.cpp
    PrintImage.cpp
    ProcessingThreadPool.cpp
    PSDWrapper.cpp
    QOIWrapper.cpp
    RAWWrapper.cpp
    ReaderBMP.cpp
    ReaderTGA.cpp
    ResizeDlg.cpp
    ResizeFilter.cpp
    RotationPanel.cpp
    RotationPanelCtl.cpp
    SaveImage.cpp
    SettingsProvider.cpp
    stdafx.cpp
    TiltCorrectionPanel.cpp
    TiltCorrectionPanelCtl.cpp
    TJPEGWrapper.cpp
    Tooltip.cpp
    TransformPanel.cpp
    TransformPanelCtl.cpp
    UnsharpMaskPanel.cpp
    UnsharpMaskPanelCtl.cpp
    UserCommand.cpp
    WEBPWrapper.cpp
    WndButtonPanel.cpp
    WndButtonPanelCtl.cpp
    WorkThread.cpp
    XMMImage.cpp
    ZoomNavigator.cpp
    ZoomNavigatorCtl.cpp
)

add_executable(JPEGView WIN32 ${JPEGVIEW_SOURCES})

# Target modern include directories
target_include_directories(JPEGView PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libjpeg-turbo/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../extras/third_party/libjpeg-turbo/src
    ${CMAKE_CURRENT_SOURCE_DIR}/libpng-apng/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libjxl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libheif/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libwebp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libavif/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lcms2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libraw/include
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(LIB_ARCH_DIR "lib64")
else()
    set(LIB_ARCH_DIR "lib")
endif()

target_link_directories(JPEGView PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libjpeg-turbo/${LIB_ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libpng-apng/${LIB_ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libjxl/${LIB_ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libheif/${LIB_ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libwebp/${LIB_ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libavif/${LIB_ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lcms2/${LIB_ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libraw/${LIB_ARCH_DIR}
    "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

target_link_libraries(JPEGView PRIVATE
    WICLoader
    turbojpeg-static
    libpng16
    zlib
    jxl_dec
    jxl_threads
    heif
    libwebp
    libwebpdemux
    avif
    lcms2
    libraw
    gdiplus
    d2d1
    dwrite
    d3d11
    dxgi
    dwmapi
    uxtheme
    shlwapi
    comctl32
    Dbghelp
    ole32
    oleaut32
    uuid
    delayimp
)

if(MSVC)
    target_link_options(JPEGView PRIVATE
        "/DELAYLOAD:avif.dll"
        "/DELAYLOAD:heif.dll"
        "/DELAYLOAD:libraw.dll"
        "/DELAYLOAD:jxl_dec.dll"
        "/DELAYLOAD:jxl_threads.dll"
        "/DELAYLOAD:lcms2.dll"
        "/DELAYLOAD:d3d11.dll"
        "/DELAYLOAD:d2d1.dll"
        "/DELAYLOAD:dwrite.dll"
    )
endif()

set_target_properties(JPEGView PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(RUNTIME_DLL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin/x64/Release")
else()
    set(RUNTIME_DLL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin/x86/Release")
endif()

file(GLOB RUNTIME_DLLS "${RUNTIME_DLL_DIR}/*.dll")
foreach(DLL_PATH ${RUNTIME_DLLS})
    get_filename_component(DLL_NAME "${DLL_PATH}" NAME)
    if(NOT DLL_NAME STREQUAL "WICLoader.dll")
        add_custom_command(TARGET JPEGView POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_PATH}"
                "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${DLL_NAME}"
            COMMENT "Copying runtime dependency ${DLL_NAME}"
        )
    endif()
endforeach()

# Copy default configuration INI
add_custom_command(TARGET JPEGView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Config/JPEGView.ini"
        "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/JPEGView.ini"
    COMMENT "Copying JPEGView.ini"
)

