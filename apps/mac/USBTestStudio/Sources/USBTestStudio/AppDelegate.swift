//
//  AppDelegate.swift
//  USBTestStudio — UI 层（AppKit）
//
//  职责（方案 §2 分层规则）：只做控件装配与事件渲染，禁止阻塞 I/O；
//  TestEngine 委托回调到达时一律 DispatchQueue.main 转发后更新控件。
//
//  T7 首切片（本切片未编译验证，需 macOS，按 Swift 5.9 / AppKit 口径编写）新增：
//    · 文件日志：UTSLog（见 Log.swift）打点启动/窗口创建/报告目录/扫描完成/退出
//      （info，级别语义对齐 Windows C++ 规范）；
//    · 设备发现过滤：设备表上方 NSSearchField 即时过滤（EP-4 设计 §二/§四.1），
//      大小写不敏感匹配 VID:PID/产品名/路径；状态栏显示 "N/M 台"；
//      关键词变化 debug、命中数 info（component: "discovery"）。
//
//  人性化清单（方案 §6）：
//    · 原生 NSWindow / NSTableView / NSTextView / NSProgressIndicator + 自动布局；
//    · 快捷键：F5 扫描、⌘R 运行（对应 Windows Ctrl+R）、⌘S 导出、⌘O 打开计划、⌘. 停止；
//    · 日志等宽字体（monospacedSystemFont 12 = Monospaced12）+ 时间戳 + 级别着色；
//    · 运行中禁用危险操作（打开/扫描/运行），按钮状态机明确。
//

import AppKit
import UniformTypeIdentifiers

final class AppDelegate: NSObject, NSApplicationDelegate {

    // MARK: UI 控件

    private var window: NSWindow!
    private var tableView: NSTableView!
    private var logView: NSTextView!
    private var statusLabel: NSTextField!
    private var progressLabel: NSTextField!
    private var progressIndicator: NSProgressIndicator!

    private var openPlanButton: NSButton!
    private var scanButton: NSButton!
    private var runButton: NSButton!
    private var stopButton: NSButton!
    private var exportButton: NSButton!
    private var clearLogButton: NSButton!

    // MARK: 状态

    private let engine = TestEngine()
    private var devices: [USBDeviceInfo] = []
    // T7 首切片：发现过滤状态（filteredDevices 才是表格数据源；filterQuery 跨扫描保留）
    private var filteredDevices: [USBDeviceInfo] = []
    private var filterQuery = ""
    private var filterField: NSSearchField!
    private var filterDebounce: DispatchWorkItem?
    private var currentPlan: TestPlan?
    private var currentPlanURL: URL?
    private var lastReport: TestReport?
    private var lastReportPath: String?
    private var logFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    // MARK: 生命周期

    func applicationDidFinishLaunching(_ notification: Notification) {
        UTSLog.info("应用启动: USBTestStudio v\(ReportWriter.toolVersion)（macOS / AppKit）")
        engine.delegate = self
        buildUI()
        buildMainMenu()

        log("USBTestStudio v\(ReportWriter.toolVersion) 就绪（macOS 原生产测上位机）")
        log("流程：打开计划(⌘O) → 扫描设备(F5) → 运行测试(⌘R) → 导出报告(⌘S)")
        log("报告目录: \(engine.reportDirectory)")
        UTSLog.info("报告目录: \(engine.reportDirectory)")
        log("提示: HID 回报率测量要求固件持续上报（如移动鼠标轴/使能 sensor 流）")
        scanDevices(self)   // 启动即扫一次
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ notification: Notification) {
        UTSLog.info("应用退出")   // T7 首切片：生命周期收尾打点
    }

    // MARK: UI 装配

    private func buildUI() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1060, height: 680),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "USBTestStudio — USB-Labs 产线测试上位机"
        window.contentMinSize = NSSize(width: 900, height: 600)   // ≥900x600
        window.center()

        guard let content = window.contentView else { return }

        // ── 顶部按钮条 ─────────────────────────────────────────────
        openPlanButton = makeButton("打开计划…", key: "o", mask: .command,
                                    action: #selector(openPlan(_:)))
        scanButton = makeButton("扫描设备", key: String(UnicodeScalar(NSF5FunctionKey)!),
                                mask: [], action: #selector(scanDevices(_:)))
        runButton = makeButton("运行测试", key: "r", mask: .command,
                               action: #selector(runTests(_:)))
        stopButton = makeButton("停止", key: ".", mask: .command,
                                action: #selector(stopTests(_:)))
        exportButton = makeButton("导出报告…", key: "s", mask: .command,
                                  action: #selector(exportReport(_:)))
        clearLogButton = makeButton("清除日志", action: #selector(clearLogs(_:)))

        let topBar = NSStackView(views: [openPlanButton, scanButton, runButton,
                                         stopButton, exportButton, clearLogButton])
        topBar.orientation = .horizontal
        topBar.spacing = 10
        topBar.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(topBar)

        // ── 设备过滤（T7 首切片，EP-4 设计 §二 设备发现区）────────────────
        // EN_CHANGE 等价：controlTextDidChange 逐键回调 + 500ms 防抖（见文件末 extension）；
        // 回车确认 / ✕ 清空走 target/action，绕过防抖立即生效。
        filterField = NSSearchField()
        filterField.placeholderString = "过滤设备：VID:PID / 产品名 / 路径（空格分隔多关键词）"
        filterField.target = self
        filterField.action = #selector(filterFieldAction(_:))
        filterField.delegate = self
        filterField.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(filterField)

        // ── 设备表 ─────────────────────────────────────────────────
        tableView = NSTableView()
        tableView.allowsColumnReordering = false
        tableView.allowsMultipleSelection = false
        tableView.usesAlternatingRowBackgroundColors = true
        tableView.gridStyleMask = [.dashedHorizontalGridLineMask]

        for (id, title, width) in [
            ("name",    "设备名称",        200.0),
            ("vidpid",  "VID:PID",          96.0),
            ("speed",   "速度",            150.0),
            ("serial",  "序列号",          130.0),
            ("path",    "IORegistry 路径", 280.0)
        ] {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
            column.title = title
            column.width = width
            column.minWidth = 56
            tableView.addTableColumn(column)
        }

        let tableScroll = NSScrollView()
        tableScroll.documentView = tableView
        tableScroll.hasVerticalScroller = true
        tableScroll.autohidesScrollers = true
        tableScroll.borderType = .bezelBorder
        tableScroll.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(tableScroll)

        // ── 状态行 ─────────────────────────────────────────────────
        progressIndicator = NSProgressIndicator()
        progressIndicator.isIndeterminate = true
        progressIndicator.style = .spinning
        progressIndicator.controlSize = .small
        progressIndicator.displayedWhenStopped = false
        progressIndicator.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(progressIndicator)

        statusLabel = NSTextField(labelWithString: "空闲")
        statusLabel.font = .systemFont(ofSize: 12, weight: .medium)
        statusLabel.lineBreakMode = .byTruncatingMiddle
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(statusLabel)

        progressLabel = NSTextField(labelWithString: "")
        progressLabel.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        progressLabel.textColor = .secondaryLabelColor
        progressLabel.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(progressLabel)

        // ── 日志 ───────────────────────────────────────────────────
        let logScroll = NSScrollView()
        logView = NSTextView(frame: NSRect(x: 0, y: 0, width: 800, height: 300))
        logView.isEditable = false
        logView.richText = true
        logView.autoresizingMask = [.width]
        logView.font = .monospacedSystemFont(ofSize: 12, weight: .regular)   // Monospaced12
        logView.textContainer?.widthTracksTextView = true
        logView.backgroundColor = .textBackgroundColor
        logScroll.documentView = logView
        logScroll.hasVerticalScroller = true
        logScroll.borderType = .bezelBorder
        logScroll.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(logScroll)

        // ── 自动布局 ───────────────────────────────────────────────
        let m: CGFloat = 12
        NSLayoutConstraint.activate([
            topBar.topAnchor.constraint(equalTo: content.safeAreaLayoutGuide.topAnchor, constant: m),
            topBar.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: m),
            topBar.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -m),

            // T7 首切片：过滤框位于按钮条与设备表之间（列表上方）
            filterField.topAnchor.constraint(equalTo: topBar.bottomAnchor, constant: 10),
            filterField.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: m),
            filterField.widthAnchor.constraint(lessThanOrEqualToConstant: 460),

            tableScroll.topAnchor.constraint(equalTo: filterField.bottomAnchor, constant: 8),
            tableScroll.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: m),
            tableScroll.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -m),
            tableScroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 120),
            tableScroll.heightAnchor.constraint(lessThanOrEqualToConstant: 240),

            progressIndicator.centerYAnchor.constraint(equalTo: statusLabel.centerYAnchor),
            progressIndicator.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: m),

            statusLabel.topAnchor.constraint(equalTo: tableScroll.bottomAnchor, constant: 8),
            statusLabel.leadingAnchor.constraint(equalTo: progressIndicator.trailingAnchor, constant: 8),
            statusLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 520),

            progressLabel.centerYAnchor.constraint(equalTo: statusLabel.centerYAnchor),
            progressLabel.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -m),
            progressLabel.leadingAnchor.constraint(greaterThanOrEqualTo: statusLabel.trailingAnchor, constant: 12),

            logScroll.topAnchor.constraint(equalTo: statusLabel.bottomAnchor, constant: 8),
            logScroll.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: m),
            logScroll.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -m),
            logScroll.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -m)
        ])

        tableView.delegate = self
        tableView.dataSource = self

        window.makeFirstResponder(tableView)
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        UTSLog.info("主窗口已创建（初始 1060×680，最小 900×600）")
    }

    /// 应用主菜单（裸 NSApplication 无默认菜单；⌘Q/编辑菜单必须自建才可用）。
    private func buildMainMenu() {
        let mainMenu = NSMenu()

        // App 菜单
        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)
        let appMenu = NSMenu(title: "USBTestStudio")
        appMenu.addItem(withTitle: "关于 USBTestStudio",
                        action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
                        keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "隐藏 USBTestStudio",
                        action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "退出 USBTestStudio",
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu

        // 文件
        let fileItem = NSMenuItem()
        mainMenu.addItem(fileItem)
        let fileMenu = NSMenu(title: "文件")
        fileMenu.addItem(withTitle: "打开测试计划…", action: #selector(openPlan(_:)), keyEquivalent: "o")
        fileMenu.addItem(withTitle: "导出报告…", action: #selector(exportReport(_:)), keyEquivalent: "s")
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "关闭窗口",
                         action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        fileItem.submenu = fileMenu

        // 编辑（日志可复制）
        let editItem = NSMenuItem()
        mainMenu.addItem(editItem)
        let editMenu = NSMenu(title: "编辑")
        editMenu.addItem(withTitle: "拷贝", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "全选", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = editMenu

        // 测试
        let testItem = NSMenuItem()
        mainMenu.addItem(testItem)
        let testMenu = NSMenu(title: "测试")
        testMenu.addItem(withTitle: "扫描设备",
                         action: #selector(scanDevices(_:)),
                         keyEquivalent: String(UnicodeScalar(NSF5FunctionKey)!))
        testMenu.addItem(withTitle: "运行测试", action: #selector(runTests(_:)), keyEquivalent: "r")
        testMenu.addItem(withTitle: "停止", action: #selector(stopTests(_:)), keyEquivalent: ".")
        testItem.submenu = testMenu

        NSApp.mainMenu = mainMenu
    }

    // MARK: 动作

    @objc func openPlan(_ sender: Any?) {
        guard !engine.isRunning else { return }
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.json]   // UTType.json（macOS 12+）
        panel.message = "选择测试计划 JSON（字段与 tools/usbtest YAML 计划同构）"
        panel.beginSheetModal(for: window) { [weak self] response in
            guard let self, response == .OK, let url = panel.url else { return }
            self.ingestPlan(at: url)
        }
    }

    private func ingestPlan(at url: URL) {
        do {
            let plan = try engine.loadPlan(from: url)
            currentPlan = plan
            currentPlanURL = url
            log("计划已加载: \(plan.name)（\(plan.steps.count) 步）← \(url.lastPathComponent)")
            log("  device: vid=\(plan.device.vid.map(hex4) ?? "任意") pid=\(plan.device.pid.map(hex4) ?? "任意")" +
                " port=\(plan.device.port ?? "auto") baud=\(plan.device.baud ?? 115200)")
            for (i, step) in plan.steps.enumerated() {
                log("  步骤\(i + 1): [\(step.type)] \(step.displayName)")
            }
            setStatus("计划就绪：\(plan.name)（\(plan.steps.count) 步）")
        } catch {
            currentPlan = nil
            log("计划解析失败: \(error.localizedDescription)", color: .systemRed)
            alert("计划解析失败：\(error.localizedDescription)\n\n字段规范见 README（与 tools/usbtest 同构）")
        }
    }

    @objc func scanDevices(_ sender: Any?) {
        guard !engine.isRunning else { return }
        setStatus("扫描中…")
        progressIndicator.startAnimation(nil)
        // IOKit 扫描放后台，UI 线程零阻塞（方案 §3）
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let list = DeviceScanner.scan()
            DispatchQueue.main.async {
                self?.applyScanResult(list)
                self?.progressIndicator.stopAnimation(nil)
            }
        }
    }

    private func applyScanResult(_ list: [USBDeviceInfo]) {
        devices = list
        applyFilter(query: filterQuery)   // 重扫后按当前关键词重新收敛（保留过滤状态）
        UTSLog.info("扫描完成: \(list.count) 台 USB 设备", component: "discovery")
        for d in list {
            let sn = d.serialNumber.isEmpty ? "-" : d.serialNumber
            log("  · \(d.name) [\(d.vidPidText)] \(d.speedLabel) sn=\(sn)")
        }
    }

    // MARK: 发现过滤（T7 首切片）

    /// 重算过滤结果并渲染（调用方保证主线程）。
    /// 状态栏口径：无关键词 → "共 N 台"；有关键词 → "显示 N/M 台（过滤: …）"。
    private func applyFilter(query: String) {
        filterQuery = query
        filteredDevices = DeviceScanner.filtered(devices, by: query)
        tableView.reloadData()
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty {
            setStatus(devices.isEmpty ? "空闲" : "共 \(devices.count) 台 USB 设备")
        } else {
            setStatus("显示 \(filteredDevices.count)/\(devices.count) 台（过滤: \"\(trimmed)\"）")
            // 命中数 info（空关键词不重复打点：扫描完成 info 已含总台数）
            UTSLog.info("过滤命中 \(filteredDevices.count)/\(devices.count) 台（关键词: \"\(trimmed)\"）",
                        component: "discovery")
        }
    }

    /// 回车确认 / ✕ 清空：绕过防抖立即生效。
    @objc func filterFieldAction(_ sender: NSSearchField) {
        filterDebounce?.cancel()
        applyFilter(query: sender.stringValue)
    }

    @objc func runTests(_ sender: Any?) {
        guard let plan = currentPlan else {
            alert("请先打开测试计划（JSON）。字段规范见 README。")
            return
        }
        guard !engine.isRunning else { return }
        log("启动计划: \(plan.name)")
        setControls(running: true)
        engine.run(plan: plan)   // 回调经 TestEngineDelegate → DispatchQueue.main
    }

    @objc func stopTests(_ sender: Any?) {
        engine.cancel()
    }

    @objc func exportReport(_ sender: Any?) {
        guard let report = lastReport else {
            alert("当前没有可导出的报告（先运行一次测试）。")
            return
        }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "report_\(report.plan)_\(report.dutSn).json"
        panel.beginSheetModal(for: window) { [weak self] response in
            guard let self, response == .OK, let url = panel.url else { return }
            do {
                let data = try ReportWriter.encode(report)
                try data.write(to: url, options: .atomic)
                self.log("报告已导出: \(url.path)")
                self.setStatus("报告已导出")
            } catch {
                self.log("导出失败: \(error.localizedDescription)", color: .systemRed)
                self.alert("导出失败: \(error.localizedDescription)")
            }
        }
    }

    @objc func clearLogs(_ sender: Any?) {
        logView.textStorage?.setAttributedString(NSAttributedString(string: ""))
    }

    // MARK: 日志 / 状态

    /// 追加一行带时间戳日志；等宽 12pt；可指定颜色（FAIL 红 / PASS 绿）。
    private func log(_ line: String, color: NSColor = .labelColor) {
        let text = "[\(logFormatter.string(from: Date()))] \(line)\n"
        let attributed = NSAttributedString(string: text, attributes: [
            .font: NSFont.monospacedSystemFont(ofSize: 12, weight: .regular),
            .foregroundColor: color
        ])
        logView.textStorage?.append(attributed)
        let length = (logView.string as NSString).length
        logView.scrollRange(toVisible: NSRange(location: max(0, length - 1), length: 0))
    }

    private func setStatus(_ text: String) {
        statusLabel.stringValue = text
    }

    private func setControls(running: Bool) {
        openPlanButton.isEnabled = !running
        scanButton.isEnabled = !running
        runButton.isEnabled = !running
        exportButton.isEnabled = !running
        stopButton.isEnabled = running
        if running {
            progressIndicator.startAnimation(nil)
            setStatus("测试运行中…")
            progressLabel.stringValue = ""
        } else {
            progressIndicator.stopAnimation(nil)
        }
    }

    private func alert(_ message: String) {
        let a = NSAlert()
        a.messageText = "USBTestStudio"
        a.informativeText = message
        a.alertStyle = .warning
        a.beginSheetModal(for: window)
    }

    private func makeButton(_ title: String, key: String = "", mask: NSEvent.ModifierFlags = [],
                            action: Selector) -> NSButton {
        let button = NSButton(title: title, target: self, action: action)
        button.bezelStyle = .rounded
        button.controlSize = .regular
        if !key.isEmpty {
            button.keyEquivalent = key
            button.keyEquivalentModifierMask = mask
        }
        return button
    }

    private func hex4(_ v: Int) -> String { String(format: "0x%04X", v) }
}

// MARK: - 设备表数据源/委托（cell-based，结构简单可审计；T7 首切片起数据源为 filteredDevices）

extension AppDelegate: NSTableViewDataSource, NSTableViewDelegate {

    func numberOfRows(in tableView: NSTableView) -> Int { filteredDevices.count }

    func tableView(_ tableView: NSTableView, objectValueFor tableColumn: NSTableColumn?,
                   row: Int) -> Any? {
        guard row >= 0, row < filteredDevices.count else { return nil }
        let d = filteredDevices[row]
        switch tableColumn?.identifier.rawValue {
        case "name":   return d.name
        case "vidpid": return d.vidPidText
        case "speed":  return String(format: "%@ (%.0f Mbps)", d.speedLabel, d.speedMbps)
        case "serial": return d.serialNumber.isEmpty ? "-" : d.serialNumber
        case "path":   return d.path
        default:       return nil
        }
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        let row = tableView.selectedRow
        guard row >= 0, row < filteredDevices.count else { return }
        let d = filteredDevices[row]
        log("选中行 \(row): \(d.name) [\(d.vidPidText)] \(d.path)")
        log("提示: 引擎按计划 device 字段(vid/pid/name_prefix)自动匹配 DUT，表格选中仅用于查看")
    }
}

// MARK: - 发现过滤委托（T7 首切片）：controlTextDidChange = Windows EN_CHANGE 等价

extension AppDelegate: NSControlTextEditingDelegate {

    /// 逐键回调：debug 留痕（逐操作语义）+ 500ms 防抖后应用（快速输入只应用最后一次）。
    func controlTextDidChange(_ obj: Notification) {
        guard (obj.object as? NSSearchField) === filterField else { return }
        let query = filterField.stringValue
        UTSLog.debug("过滤关键词变化: \"\(query)\"", component: "discovery")
        filterDebounce?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.applyFilter(query: query) }
        filterDebounce = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: work)
    }
}

// MARK: - 引擎委托（引擎队列触发 → 主线程渲染）

extension AppDelegate: TestEngineDelegate {

    func engineDidLog(_ engine: TestEngine, message: String) {
        DispatchQueue.main.async {
            // 级别着色：PASS 绿 / FAIL 红 / 警告橙
            let color: NSColor
            if message.contains("FAIL") { color = .systemRed }
            else if message.contains("PASS") { color = .systemGreen }
            else if message.contains("警告") { color = .systemOrange }
            else { color = .labelColor }
            self.log(message, color: color)
        }
    }

    func engineDidUpdateProgress(_ engine: TestEngine, completed: Int, total: Int, currentStep: String) {
        DispatchQueue.main.async {
            self.progressLabel.stringValue = "步骤 \(completed)/\(total)：\(currentStep)"
        }
    }

    func engineDidFinish(_ engine: TestEngine, report: TestReport, jsonPath: String?) {
        DispatchQueue.main.async {
            self.lastReport = report
            self.lastReportPath = jsonPath
            self.setControls(running: false)
            let passed = report.steps.filter(\.pass).count
            self.setStatus("判定: \(report.verdict)（\(passed)/\(report.steps.count) 步通过） sn=\(report.dutSn)")
            self.progressLabel.stringValue = jsonPath.map { "报告: \($0)" } ?? "报告未落盘"
            // 产线音效（系统内置命名音，缺省环境下安全降级为无声）
            NSSound(named: NSSound.Name(report.verdict == "PASS" ? "Glass" : "Basso"))?.play()
            // 判定后重扫，反映设备当前状态
            self.scanDevices(self)
        }
    }
}
