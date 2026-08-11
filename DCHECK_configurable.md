# DCHECK_IS_CONFIGURABLE 机制说明

## 概述

`dcheck_is_configurable=true` 是 Chromium 的 GN 编译参数，使 libcef 内部的 DCHECK 默认不 crash，而是以 `LOGGING_ERROR` 级别记日志。

## 启用方式

在 `rebuild.bat` 的 `GN_DEFINES` 中加入 `dcheck_is_configurable=true`：

```bat
set GN_DEFINES=is_official_build=true chrome_pgo_phase=0 use_siso=false symbol_level=2 dcheck_is_configurable=true
```

rebuild.bat 路径：`E:\GitHub\CEF_SRC_BUILD\chromium_git\rebuild.bat`

## 核心代码位置

### 1. GN 参数定义

文件：`build/config/dcheck_always_on.gni`

```gn
declare_args() {
  dcheck_is_configurable = false
}

declare_args() {
  # dcheck_is_configurable=true 会强制 dcheck_always_on=true
  # 即使 is_official_build=true，DCHECK 代码也会被编译进来
  dcheck_always_on =
      (build_with_chromium && !is_official_build) || dcheck_is_configurable
}
```

### 2. LOGGING_DCHECK 声明

文件：`base/logging.h:508-512`

```cpp
#if BUILDFLAG(DCHECK_IS_CONFIGURABLE)
BASE_EXPORT extern LogSeverity LOGGING_DCHECK;   // 运行时可变
#else
constexpr LogSeverity LOGGING_DCHECK = LOGGING_FATAL;  // 编译期固定 = FATAL
#endif
```

### 3. LOGGING_DCHECK 定义初始值

文件：`base/logging.cc:507-512`

```cpp
#if BUILDFLAG(DCHECK_IS_CONFIGURABLE)
// 默认为 ERROR，避免在运行时明确选择行为前触发 crash
BASE_EXPORT logging::LogSeverity LOGGING_DCHECK = LOGGING_ERROR;
#endif
```

### 4. ~CheckError() 判断是否 crash

文件：`base/check.cc:339-357`

```cpp
CheckError::~CheckError() {
  const bool is_fatal = log_message_->severity() == LOGGING_FATAL;
  log_message_.reset();
  if (is_fatal) {
    base::ImmediateCrash();   // 只有 FATAL 才 crash
  }
}
```

## 效果

| | dcheck_is_configurable=false (默认) | dcheck_is_configurable=true |
|---|---|---|
| DCHECK 代码 | release 不编译 (空宏) | 编译进来 |
| LOGGING_DCHECK | constexpr LOGGING_FATAL | extern, 初始值 LOGGING_ERROR |
| DCHECK 触发 | 不触发 (release) | LOGGING_ERROR 级别, 写 debug.log, 不 crash |
| CHECK 触发 | crash (LOGGING_FATAL) | 仍 crash (LOGGING_FATAL) |

## 副作用

- 二进制增大 (DCHECK 条件判断都编进去了)
- 运行时有轻微开销 (每个 DCHECK 条件会被求值)
- DCHECK 消息写入 `debug.log` 文件

## 补充说明

### 运行时改为 crash

如果需要运行时把 DCHECK 改回 crash，可通过 Chromium 的 FeatureList 启用 `DcheckIsFatal` feature：

文件：`base/feature_list.cc:695-700`

```cpp
if (FeatureList::IsEnabled(kDCheckIsFatalFeature) ||
    CommandLine::ForCurrentProcess()->HasSwitch(
        "gtest_internal_run_death_test")) {
  logging::LOGGING_DCHECK = logging::LOGGING_FATAL;
} else {
  logging::LOGGING_DCHECK = logging::LOGGING_ERROR;
}
```

cefclient 目前不初始化 FeatureList，所以保持默认的 `LOGGING_ERROR` (不 crash)。

### 符号不导出

`is_official_build=true` 时 `COMPONENT_BUILD` 未定义，`BASE_EXPORT` 是空宏。
`SetLogMessageHandler` 和 `LOGGING_DCHECK` 符号不从 libcef.dll 导出，无法通过
`GetProcAddress` 访问。CEF 也没有提供日志回调 API (`formatted_log_handler` 只在
CEF 库加载前有效)。因此无法实时回调到 C#，只能通过读 `debug.log` 获取 DCHECK 消息。

## 日志文件路径

### CEF 的日志配置

cefclient 在 `cefclient_win.cc:292-293` 设置 `settings.log_severity = LOGSEVERITY_INFO`，
不设置 `settings.log_file`。因此走 `base/logging.cc:285-296` 的默认路径：

- Windows: exe 所在目录 + `debug.log`
- POSIX: 当前目录 + `debug.log`

注意：`chrome_debug.log` 是 Chrome 浏览器 (`chrome/common/logging_chrome.cc:576`) 的默认
文件名，CEF 不用它。CEF 走的是 `base/logging.cc` 的默认 `debug.log`。

### 多进程行为

- browser 进程: 打开 `debug.log`，写入日志
- renderer/其他子进程: browser 启动子进程时通过句柄共享 (`--enable-logging=handle` +
  `--log-file=<handle>`)，所有进程写入同一个 `debug.log`
- 如果 browser 没开 `--enable-logging`，子进程各自独立或写 stderr

### cefclient 的实际日志文件

exe 路径: `E:\GitHub\CEF_SRC_BUILD\cefclientdbg\cefclient.exe`
日志文件: `E:\GitHub\CEF_SRC_BUILD\cefclientdbg\debug.log`

日志级别 `LOGSEVERITY_INFO` 会输出 INFO 及以上所有级别。DCHECK 在
`dcheck_is_configurable=true` 编译后是 `LOGGING_ERROR`，会被写入 `debug.log`。

## CEF配置dcheck_always_on

修改脚本：cef/tools/gn_args.py
