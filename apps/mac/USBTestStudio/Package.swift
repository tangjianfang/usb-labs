// swift-tools-version:5.9
// USBTestStudio — USB-Labs macOS 产线测试上位机（AppKit + IOKit，零第三方依赖）
import PackageDescription

let package = Package(
    name: "USBTestStudio",
    platforms: [
        // 最低 macOS 13（Ventura）：monospacedSystemFont / allowedContentTypes 等均可用
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "USBTestStudio",
            path: "Sources/USBTestStudio"
        )
    ]
)
