; USBTestStudio.iss — Inno Setup 6 打包脚本（T11）
; 用法：装 Inno Setup 6（https://jrsoftware.org/isinfo.php）后
;       ISCC.exe packaging\win\USBTestStudio.iss
; 前置：先 cmake --build apps/win/build --config Release 产出主程序。
; 产物：packaging\win\Output\USBTestStudio-<版本>-setup.exe

#define MyAppName "USBTestStudio"
#define MyAppVersion "0.9.0"
#define MyAppPublisher "USB-Labs"
#define MyAppExeName "USBTestStudio.exe"
#define BuildDir "..\..\apps\win\build\Release"

[Setup]
AppId={{8E6F2A57-3C4B-4B9A-9E1D-USBTESTSTUDIO}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
; 产线工具常按工位装到用户目录，允许免管理员安装
PrivilegesRequired=lowest
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-setup
OutputDir=Output
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
Source: "{#BuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{#MyAppName} 工程师控制台"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--console"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
