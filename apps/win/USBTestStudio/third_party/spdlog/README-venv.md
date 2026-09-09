# spdlog（vendored）

- 版本：v1.14.1（https://github.com/gabime/spdlog tag v1.14.1，MIT License，见 LICENSE）
- 抽取范围：仅 `include/spdlog/**`（header-only 用法，不编 SPDLOG_COMPILED_LIB）；
  上游的 bench/test/example 不随库分发。
- 接线：CMakeLists.txt 各靶 `target_include_directories` 加 `third_party/spdlog/include`；
  统一门面在 `src/app/log.h`（模块命名 logger + 规范格式 + 滚动文件 sink）。
- 升级方式：下载新 tag 同样抽取 include/ 与 LICENSE 覆盖本目录，API 变化看
  spdlog 官方 Changelog 再改 log.h 与调用点。
