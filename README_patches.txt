================================================================================
CEF 自定义 Patch 管理说明
================================================================================

本项目对 CEF 自身源码做了若干定制修改。由于 CEF 自带的补丁机制只对
Chromium 生效、不能对 CEF 自己的源码自动打补丁，因此这里用脚本来统一管理这些
修改。每类脚本都提供 Python 与 PowerShell 两个等价版本（互相独立、互不调用）：

    apply_cef_custom_patches.py / .ps1        应用补丁
    regenerate_cef_custom_patches.py / .ps1   升级 CEF 后重新生成补丁（修行号漂移）

任选 Python 或 PowerShell 版运行即可，二者行为一致。


--------------------------------------------------------------------------------
一、目录与文件
--------------------------------------------------------------------------------

    <cef 根>/
      apply_cef_custom_patches.py       引擎（Python）
      apply_cef_custom_patches.ps1      引擎（PowerShell）
      README_patches.txt                本说明
      myapp/patch/
        01-resource_request_handler_wrapper.patch
        02-proxy_url_loader_factory.patch
        03-browser_info_manager.patch
        04-BUILD.gn.patch
        05-gn_args.py.patch

myapp/patch/ 下的每个 *.patch 都是标准的 unified diff（用 git diff 生成），
脚本会自动发现并按文件名排序依次套用。


--------------------------------------------------------------------------------
二、两类修改（重要）
--------------------------------------------------------------------------------

脚本管理的修改分两类，处理方式不同：

1) 静态 patch —— myapp/patch/*.patch
   不引入任何新的 CEF API 的普通源码改动（.cc / .py / BUILD.gn 等）。
   用 `git apply` 套用，幂等。绝大多数修改都属于这一类。
   ★ 新增这类修改时，只需丢一个 .patch 文件进去，脚本无需改动。★

2) API header 修改 —— OnBeforeResourceResponse 回调
   这一条会往 include/cef_resource_request_handler.h 里新增一个 CEF API，
   随后必须运行 CEF 官方生成工具（tools/translator.py 和
   tools/version_manager.py）来生成 C API、更新版本号。
   它【不能】做成静态 patch，因为生成工具会把 `added=next` 改写成一个由环境
   决定的具体版本号（例如 15001，注释还带生成日期），把这种动态结果固化进
   patch 一定会出错或冲突。
   因此这一条作为“专用逻辑”写死在脚本里，一般无需你操心。


--------------------------------------------------------------------------------
三、如何应用 patch
--------------------------------------------------------------------------------

在 cef 根目录执行其一：

    python3 ./apply_cef_custom_patches.py

或：

    pwsh -File ./apply_cef_custom_patches.ps1

脚本是幂等的：已经打过的 patch 会显示 "Already applied" 并跳过，重复运行安全。
成功结束时会打印：

    Patch completed successfully.
    Source changed: True/False        （本次是否真的改动了源码）

可选参数：

    --cef-root <路径>        指定 CEF 源码根（默认取脚本所在目录）
    --force-generate         即使没有引入新 API 也强制跑一次官方生成工具
                             （PowerShell 版对应 -ForceGenerate）


--------------------------------------------------------------------------------
三之二、升级 CEF 后的工作流（修正行号漂移）★
--------------------------------------------------------------------------------

升级 / 重新同步 CEF 源码后，上游文件行号会变化，导致 myapp/patch/*.patch 里
记录的行号（@@ -NNN ...）与新源码对不上，即“行号漂移”。处理步骤：

  1. 升级 / 重新同步 CEF 源码树（此时工作区等于新的上游 HEAD，是干净的）。

  2. 应用补丁：

         python3 ./apply_cef_custom_patches.py
         # 或 pwsh -File ./apply_cef_custom_patches.ps1

     apply 内部用 `git apply --ignore-whitespace`，能容忍一定的行号偏移和
     CRLF/LF 差异，把我们的改动自动定位并打到新源码上。
     ★ 如果某个 patch 此处报错（does not apply），说明上游把该处上下文改得
       太多，git 已无法自动定位——需要人工打开对应文件、手动把改动加回去，
       然后直接进入第 3 步用它重新生成 patch。★

  3. 重新生成补丁，把行号/上下文刷新为新源码的：

         python3 ./regenerate_cef_custom_patches.py
         # 或 pwsh -File ./regenerate_cef_custom_patches.ps1

     它对每个已应用的 patch，用 `git diff` 按该 patch 涉及的文件重新导出，
     覆盖旧 patch。输出示例：

         Unchanged: 01-...patch          （无漂移，内容未变）
         Regenerated: 04-BUILD.gn.patch  （行号已刷新）
         Skipped (not currently applied, won't overwrite): 03-...patch

  4. 检查并提交刷新后的 patch：

         git diff -- myapp/patch

为什么第 2 步能打上、第 3 步还要重新生成？
  - 第 2 步 `git apply` 是“带偏移地”把改动打到文件里，源码结果是对的，
    但它不会去修改 myapp/patch/ 里的 patch 文件本身（里面仍是旧行号）。
  - 第 3 步从工作区反向导出，才把 patch 文件的行号/上下文更新为新源码的，
    这样 patch 文件本身与最新源码保持一致，下次升级 diff 更干净。

regenerate 的安全保护（重要）：
  * 只有当某 patch “当前确实已应用”（其反向 diff 能干净套回）且
    `git diff` 结果非空时，才会覆盖该 patch 文件。
  * 否则一律 Skipped，【绝不】用空内容把 patch 冲掉。所以哪怕你忘了先跑
    apply，regenerate 也不会破坏已有 patch。
  * OnBeforeResourceResponse 那条 API 修改不是静态 patch（见第二节），
    regenerate 不处理它；升级后它由 apply 脚本重新插入并触发官方生成。

注意：regenerate 按“整文件 git diff”导出。请确保被 patch 的文件里【只有】
我们的定制改动、没有混入其它未提交改动，否则那些改动会被一并写进 patch。
正常升级流程（第 1 步干净、第 2 步 apply）天然满足这一点。


--------------------------------------------------------------------------------
四、如何新增一个 patch（最常用）
--------------------------------------------------------------------------------

前提：你已经直接在 CEF 源码里改好了某个文件，改动【不涉及新增 CEF API】。

步骤：

  1. 【必须】切到 cef 仓库根目录（路径基准就是这里，不能在子目录生成）：

         cd <cef 根>

     确认这里就是 git 仓库根：

         git rev-parse --show-toplevel      # 应输出 .../src/cef

  2. 用 git diff 生成 patch，路径写相对 cef 根的路径：

         git diff -- tools/gn_args.py > myapp/patch/06-gn_args_xxx.patch

     命名建议：两位数字序号 + 简短说明，例如 06-xxx.patch。序号只用于决定
     套用顺序，一般各 patch 互不依赖时顺序无所谓。

  3. 验证 patch 与当前源码状态一致（能反向套 = 确实对应已应用状态）：

         git apply --reverse --check myapp/patch/06-gn_args_xxx.patch

     无输出且退出码 0 即正常。

  4. 跑一遍脚本确认纳管成功（会显示 Already applied，因为源码里已经是改后的）：

         python3 ./apply_cef_custom_patches.py

完成。两个脚本都会自动发现新文件，【无需修改任何脚本代码】。


--------------------------------------------------------------------------------
五、为什么 patch 必须相对 cef 根生成
--------------------------------------------------------------------------------

patch 文件里记录的是相对路径（git 会加 a/ b/ 前缀），本身不带基准目录信息。
真正决定基准的是“执行时的工作目录”：

  - 生成：在 cef 根执行 git diff，路径就相对 cef 根。
  - 套用：脚本内部把工作目录固定为 cef 根，再调用 git apply
          （默认 -p1，剥掉 a/ b/ 前缀），剩下的路径相对 cef 根解析。

两头基准一致，才能对得上。所以：

  ★ 一定要在 cef 根用 git diff 生成；不要在子目录生成，不要用绝对路径或
    ../ 相对路径，也不要手写路径前缀。★


--------------------------------------------------------------------------------
六、幂等与错误处理机制
--------------------------------------------------------------------------------

对每个静态 patch，脚本按如下顺序判断：

  1. `git apply --reverse --check` 成功  → 判定“已应用”，跳过。
  2. 否则 `git apply --check` 成功        → 正常套用。
  3. 两者都失败                           → 报错停止（不会静默错位）。

第 3 种情况通常意味着上游 CEF 源码变了、patch 的上下文对不上了，需要人工
重新生成该 patch（改好源码后重跑第四节的步骤）。

注意：这里用 `git apply` 而非模糊匹配的 `patch -F`，就是要在上下文对不齐时
【响亮报错】，而不是猜一个位置硬打进去导致源码语义错误。


--------------------------------------------------------------------------------
七、不由本脚本管理的修改
--------------------------------------------------------------------------------

以下属于 CEF 官方“对 Chromium 打补丁”的机制（patch/ 目录，由 CEF 自己的
patch_updater 处理），【不纳入】本脚本：

    patch/patch.cfg
    patch/patches/*.patch

这些在每次更新 CEF/Chromium 时需要人工合并、合完后再由官方工具重新生成，
不要试图用本脚本接管。


--------------------------------------------------------------------------------
八、当前已纳管的修改一览
--------------------------------------------------------------------------------

  [API] OnBeforeResourceResponse 回调（含官方生成）—— 脚本内专用逻辑
  [静态] 01  resource_request_handler_wrapper.cc  响应头覆盖：捕获并回传修改后的响应头
  [静态] 02  proxy_url_loader_factory.cc          响应头覆盖：无 current_response 时保留 override
  [静态] 03  browser_info_manager.cc              --use-chrome-window 下 window.open 并入 tabstrip
  [静态] 04  BUILD.gn                             myapp 相关构建目标
  [静态] 05  tools/gn_args.py                     构建参数定制

================================================================================
