cmake_minimum_required(VERSION 3.20)
project(USBTestStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(USBTestStudio WIN32
    src/app/main.cpp
    src/framework/win32_rai.cpp
    src/framework/json_mini.cpp
    src/usb/device_enumerator.cpp
    src/usb/hid_port.cpp
    src/usb/serial_port.cpp
    src/usb/msc_scsi.cpp
    src/engine/test_engine.cpp
    src/ui/log_view.cpp
    src/ui/main_window.cpp
)

target_include_directories(USBTestStudio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(USBTestStudio PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# /W4 /utf-8 /permissive-（CMake MSVC 生成器；permissive- 由 MSVC 默认在 C++20 下开启，此处显式声明）
if(MSVC)
    target_compile_options(USBTestStudio PRIVATE /W4 /utf-8 /permissive-)
endif()

target_link_libraries(USBTestStudio PRIVATE setupapi cfgmgr32 hid comctl32 comdlg32 shell32)

# app.manifest：CMake 路径下通过 /MANIFESTINPUT 注入（PerMonitorV2 DPI + Common Controls v6）
if(MSVC)
    target_link_options(USBTestStudio PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
        "/ENTRY:wWinMainCRTStartup"
    )
endif()
