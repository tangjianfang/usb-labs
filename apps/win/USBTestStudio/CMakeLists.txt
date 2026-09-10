cmake_minimum_required(VERSION 3.20)
project(USBTestStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 公共头文件路径：本库源码 + vendored spdlog（日志规范见 src/app/log.h 文件头）
set(UTS_COMMON_INC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
)

add_executable(USBTestStudio WIN32
    src/app/main.cpp
    src/framework/win32_rai.cpp
    src/framework/json_mini.cpp
    src/usb/device_enumerator.cpp
    src/usb/hid_port.cpp
    src/usb/serial_port.cpp
    src/usb/winusb_port.cpp
    src/usb/msc_scsi.cpp
    src/engine/test_engine.cpp
    src/ui/log_view.cpp
    src/ui/main_window.cpp
    src/ui/console_window.cpp
    src/ui/session_pane.cpp
)

target_include_directories(USBTestStudio PRIVATE ${UTS_COMMON_INC})

target_compile_definitions(USBTestStudio PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    SPDLOG_WCHAR_FILENAMES   # wchar 路径：LOCALAPPDATA 含中文用户名时 fopen/窄路径会炸
)

# /W4 /utf-8 /permissive-（CMake MSVC 生成器；permissive- 由 MSVC 默认在 C++20 下开启，此处显式声明）
if(MSVC)
    target_compile_options(USBTestStudio PRIVATE /W4 /utf-8 /permissive-)
endif()

target_link_libraries(USBTestStudio PRIVATE setupapi cfgmgr32 hid winusb comctl32 comdlg32 shell32)

# app.manifest：CMake 路径下通过 /MANIFESTINPUT 注入（PerMonitorV2 DPI + Common Controls v6）
if(MSVC)
    target_link_options(USBTestStudio PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
        "/ENTRY:wWinMainCRTStartup"
    )
endif()

# EP-4 S1/S5 通道层离线自测（控制台；MockEchoPort/MockHidPort/MockWinUsbPort/MockMscPort
# 注入模板 + SerialPort/HidPort/WinUsbPort/MscScsi 显式实例化）
add_executable(channel_selftest
    src/app/channel_selftest.cpp
    src/framework/win32_rai.cpp
    src/usb/hid_port.cpp
    src/usb/serial_port.cpp
    src/usb/winusb_port.cpp
    src/usb/msc_scsi.cpp
)
target_include_directories(channel_selftest PRIVATE ${UTS_COMMON_INC})
target_compile_definitions(channel_selftest PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    SPDLOG_WCHAR_FILENAMES   # wchar 路径：LOCALAPPDATA 含中文用户名时 fopen/窄路径会炸
)
if(MSVC)
    target_compile_options(channel_selftest PRIVATE /W4 /utf-8 /permissive-)
endif()
target_link_libraries(channel_selftest PRIVATE setupapi cfgmgr32 hid winusb)

# EP-4 S2/S5 设备发现离线自测（控制台；目录模型+即时过滤+COM 枚举+MSC 盘行+
# 目录组装/会话工厂，无 USB 设备亦可跑；catalog_build 引入四通道成员 → 链端口实现）
add_executable(discovery_selftest
    src/app/discovery_selftest.cpp
    src/framework/win32_rai.cpp
    src/usb/hid_port.cpp
    src/usb/serial_port.cpp
    src/usb/winusb_port.cpp
    src/usb/msc_scsi.cpp
)
target_include_directories(discovery_selftest PRIVATE ${UTS_COMMON_INC})
target_compile_definitions(discovery_selftest PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    SPDLOG_WCHAR_FILENAMES   # wchar 路径：LOCALAPPDATA 含中文用户名时 fopen/窄路径会炸
)
if(MSVC)
    target_compile_options(discovery_selftest PRIVATE /W4 /utf-8 /permissive-)
endif()
target_link_libraries(discovery_selftest PRIVATE setupapi cfgmgr32 hid winusb)

# EP-4 S3 会话台前半离线自测（控制台；收发编解码+会话核心纯逻辑，无端口实现依赖）
add_executable(session_selftest
    src/app/session_selftest.cpp
)
target_include_directories(session_selftest PRIVATE ${UTS_COMMON_INC})
target_compile_definitions(session_selftest PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    SPDLOG_WCHAR_FILENAMES   # wchar 路径：LOCALAPPDATA 含中文用户名时 fopen/窄路径会炸
)
if(MSVC)
    target_compile_options(session_selftest PRIVATE /W4 /utf-8 /permissive-)
endif()

# EP-4 S4 解析面板前半离线自测（控制台；HID 报告解码纯逻辑——键码表/修饰键位图/
# 鼠标按钮与位移/消费页用量/Report ID 剥离，码表口径见 hid_parser.h 文件头）
add_executable(parser_selftest
    src/app/parser_selftest.cpp
)
target_include_directories(parser_selftest PRIVATE ${UTS_COMMON_INC})
target_compile_definitions(parser_selftest PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    SPDLOG_WCHAR_FILENAMES   # wchar 路径：LOCALAPPDATA 含中文用户名时 fopen/窄路径会炸
)
if(MSVC)
    target_compile_options(parser_selftest PRIVATE /W4 /utf-8 /permissive-)
endif()

# DevStudio MS0 shell 模块离线自测（控制台；token 单源/设置/视角/面板注册/
# 命令+模糊匹配/工程模型/模板库/日志视图模型/崩溃管理——纯逻辑+VM 层，逐步追加）
add_executable(shell_selftest
    src/shell/shell_selftest.cpp
    src/framework/win32_rai.cpp
    src/framework/json_mini.cpp
)
target_include_directories(shell_selftest PRIVATE ${UTS_COMMON_INC})
target_compile_definitions(shell_selftest PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    SPDLOG_WCHAR_FILENAMES   # wchar 路径：LOCALAPPDATA 含中文用户名时 fopen/窄路径会炸
)
if(MSVC)
    target_compile_options(shell_selftest PRIVATE /W4 /utf-8 /permissive-)
endif()
target_link_libraries(shell_selftest PRIVATE setupapi shlwapi dbghelp)
