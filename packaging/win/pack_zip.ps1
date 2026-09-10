# pack_zip.ps1 — 无 Inno Setup 时的兜底打包（T11）：构建产物 + 说明 → zip
# 用法：powershell -File packaging\win\pack_zip.ps1 [-BuildDir apps\win\build\Release]
param(
    [string]$BuildDir = "",
    [string]$Version  = "0.9.0"
)

$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = Join-Path $PSScriptRoot "..\..\apps\win\build\Release"
}

$exe = Join-Path $BuildDir "USBDevStudio.exe"
if (-not (Test-Path $exe)) {
    throw "未找到 $exe —— 先执行 cmake --build apps/win/build --config Release"
}

$stage = Join-Path $env:TEMP ("USBTestStudio-" + $Version)
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item $stage -ItemType Directory | Out-Null
Copy-Item $exe $stage

$readme = @"
USBTestStudio $Version
=====================
USB 工程师通信控制台 + 产测工具（EP-4）。

用法：
  USBDevStudio.exe               DevStudio 工作站（默认；四视角 IDE 壳）
  USBDevStudio.exe --console    工程师通信控制台（设备发现/会话收发/解析）
  USBDevStudio.exe --legacy     旧产测模式（读同目录 plan.json）

日志：%LOCALAPPDATA%\USBTestStudio\logs\（滚动 5MB x 3）
"@
$readme | Out-File (Join-Path $stage "README.txt") -Encoding utf8

$out = Join-Path $PSScriptRoot ("USBTestStudio-" + $Version + "-win-x64.zip")
if (Test-Path $out) { Remove-Item $out -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $out
Remove-Item $stage -Recurse -Force
Write-Host "打包完成：$out"
