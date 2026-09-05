//
//  main.swift
//  USBTestStudio — 入口
//
//  裸 NSApplication 装配：Swift Package executableTarget 可直接 `swift run` 启动 GUI。
//  线程口径（与 apps/方案-上位机软件工程实施.md 一致）：
//    · UI 线程只做控件更新，禁止阻塞 I/O；
//    · TestEngine 在独立串行队列执行计划，所有回调经 DispatchQueue.main 回 UI 渲染；
//    · Swift 5.9 严格并发（Sendable 严格检查）非本工程强制要求。
//

import AppKit

let app = NSApplication.shared
let appDelegate = AppDelegate()          // main.swift 顶层常量，在 app.run() 期间全程存活
app.delegate = appDelegate
app.setActivationPolicy(.regular)        // Dock 常驻，普通 GUI 应用形态
app.run()
