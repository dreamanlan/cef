# WebAgent HTML Tabbar 设计文档

> 状态：基本单 tab UI 已实现（用户确认）；真正的多 tab 尚未开始。下文区分当前源码、历史问题与后续目标；不代表各平台均已验收。
> 启用方式：**默认启用**（已移除 `--use-html-tabbar` 开关），对 Views 普通顶层窗口生效
> 模式选择：`--use-chrome-window` 已删除。`UseChromeWindowGlobal()` 当前返回 `!use_windowless_rendering_ && !use_alloy_style_ && !use_chrome_style_window_`，命中时默认使用 Chrome 自建 tabstrip 窗口，不创建本方案的 ViewsWindow。HTML tabbar 仅在实际进入 Views 路径且 `type_ == WindowType::NORMAL` 时启用；`--use-alloy-style` / `--use-chrome-style-window` 会排除默认 Chrome 自建模式，OSR 及平台 native 限制仍须遵守。
> 文档目的：把 tabbar 从「需求 → 设计 → 实现」完整梳理，作为后续实现与查阅的唯一依据。
> 说明：文内引用的源码行号为撰写时快照，可能随代码演进偏移，以函数名/符号名为准。

---

## 1. 背景与目标

WebAgent 需要一个「带 tabbar 的浏览器窗口」，用于承载多个 web 页面与相关工具区。要求：

- **两种 runtime style 下都有效**：`chrome-style`（Chrome runtime）与 `alloy-style`（Alloy runtime）。
- **使用无系统标题栏（frameless）窗口**：要显示自定义 HTML，只能用系统窗口的 client 区域，隐藏原生标题栏。
- **tabbar 用 HTML 渲染**：tab 页签、（在需要时）地址栏/工具栏等，都由 HTML 页面绘制，运行在独立的 `CefBrowserView` 中。
- **保留 native 的系统按钮与菜单**：菜单、最小化/最大化/关闭按钮仍用 native 控件，但要「浮」在 HTML 之上、与 tab 同处标题栏那一行。

非目标（本期不做）：
- 书签栏的**功能**实现（存储/增删改查/持久化）。alloy 下书签栏**只在 UI 区域归属上纳入 tabbar**，功能后置。
- 真正的多 tab 及撕离 / 合并不在本期实现；后续设计见 `MULTITAB_WINDOW_DESIGN.md`。跨窗口迁移不等于跨进程迁移。

---

## 2. 需求

### 2.1 功能需求
1. 窗口顶部显示自定义 tabbar（HTML），包含 tab 页签行。
2. 当前页签新建 / 关闭 / 切换仅改变 HTML 界面；C++ 仍只有一个内容 browser。真正的多 browser tab 管理见 `MULTITAB_WINDOW_DESIGN.md`，尚未开始。
3. `chrome-style`：当前尝试将 `browser_view_->GetChromeToolbar()` 插在 tabbar 与内容之间；返回空时跳过。不将 Chrome popup 必然提供完整工具栏 / 书签栏作为当前保证。
4. `alloy-style`：CEF 不提供产品级地址栏/工具栏/书签栏，由 tabbar（HTML）承担地址栏 + 导航工具栏（前进/后退/刷新/停止/URL），书签栏 UI 区域也归 tabbar（功能后置）。
5. HTML tabbar 与 C++ 双向交互：UI 操作驱动浏览器（导航/tab 操作），浏览器状态回推 HTML（URL 同步、加载态）。

### 2.2 约束
- frameless 对 Views 的 NORMAL 窗口默认启用，包括归为 NORMAL 的普通 window.open 弹窗；不能笼统声称所有 popup 都排除。DEVTOOLS / DIALOG 及非 Views 路径不启用 HTML tabbar。
- HTML tabbar 窗口已通过门控禁用 demo 地址栏 / 工具栏、location-bar overlay 和 Windows 旧自定义标题栏；相关源码已于 2026-09-11 物理删除（见 §9 阶段5 注）。Alloy 使用 HTML 导航栏，Chrome 内容模式保留获取 Chrome 工具栏的路径。
- native 系统按钮/菜单必须完美覆盖在 HTML 之上（不采用 HTML 非矩形渲染方案）。
- 窗口缩放、页面内容变化时，tabbar 不得消失或错位。
- 保持零新增编译警告/错误基线。构建由用户执行。

---

## 3. 窗口层级模型

自上而下的区域划分（这是需求与设计的公共语言）：

```
┌───────────────────────────────────────────────┐
│ [1] 系统标题栏（frameless 时隐藏）              │
├───────────────────────────────────────────────┤
│ [2] 顶层菜单栏（如有，平台相关）                │
├───────────────────────────────────────────────┤
│ [3] 自定义 tabbar 行（HTML）                    │
│     tab 页签 ...            [菜单][－][□][×]     │ ← native 按钮 overlay 浮在右上
├───────────────────────────────────────────────┤
│ [4] 其它区域（工具区，风格相关）                │
│     chrome-style: 可用时插入 Chrome toolbar  │
│     alloy-style : HTML 绘制地址栏/导航(/书签)   │
├───────────────────────────────────────────────┤
│ [5] web 内容浏览器（flex=1）                    │
└───────────────────────────────────────────────┘
```

**区域归属**：HTML 在 Alloy 内容模式绘制 [3]+[4]；Chrome 内容模式只绘制 [3]，[4] 由可获取的 Chrome 工具栏承担。native 系统按钮不由 HTML 绘制。
- `chrome-style`：HTML 只画 [3] tab 行，[4] 使用可获取的 Chrome toolbar；获取为空时跳过。
- `alloy-style`：HTML 画 [3] tab 行 + [4] 地址栏/导航工具栏（书签栏 UI 预留），高度随显示项动态变化。
- [3] 中的菜单按钮（无顶层菜单时）、min/max/close（无系统标题栏时）用 native 控件 + `AddOverlayView` 浮在 HTML 右上角。

---

## 4. 两种 runtime style 的能力边界（代码证据）

### 4.1 chrome-style
- `CalculateChromeToolbarType`（`views_window.cc:145` 附近）在非 alloy 且允许 toolbar 时返回 `CEF_CTT_NORMAL`/`CEF_CTT_LOCATION`。
- 产品级工具栏来自 `browser_view_->GetChromeToolbar()`（含地址栏、菜单、扩展、安全指示等），由 Chrome runtime 提供。→ [4] 区完全由 chrome 负责，tabbar 不介入。

### 4.2 alloy-style（关键结论：无产品级工具栏）
- `CalculateChromeToolbarType`：`if (use_alloy_style || toolbar_type=="none" || hide_toolbar) return CEF_CTT_NONE;`（`views_window.cc:145`）→ alloy 直接判定无 chrome toolbar，`GetChromeToolbar()` 拿不到产品级实现。
- 旧 demo 路径（`CreateLocationBar` / `AddControls` 等）已于 2026-09-11 物理删除；当前 Alloy 地址栏 / 导航按钮由内置页的 `#navrow` 提供。
- **书签栏：完全不存在**（`cefclient_mac.mm` 的 `IDC_SHOW_BOOKMARK_MANAGER` 只是 Mac 应用菜单项转发 chrome command，非书签栏 UI）。

→ 因此 alloy 下 [4] 区必须由我们自绘（HTML tabbar 承担），并去掉上述 demo 级 native 地址栏/工具栏。

---

## 5. 关键技术调研结论（带代码证据）

### 5.1 frameless / 自定义标题栏基础设施
- `ViewsWindow::IsFrameless()` 当前返回 `frameless_`；构造时设为 `(hide_frame || with_html_tabbar_) && is_normal_type`，不再依赖已移除的 `--use-html-tabbar`。
- `CanResize` 恒 `true`（`:428-432`）。
- Windows 仍保留 `title_bar_` 旧标题栏及拖动区实现，但 HTML tabbar 窗口已门控禁用该路径；当前拖动区合并见 `ApplyDraggableRegions` 和 §7.2。
- 跨平台（Mac/Linux）通过 `ViewsOverlayControls` 用 overlay 浮层实现同类效果。

### 5.2 native 控件浮在 HTML 之上 —— `AddOverlayView`（选定方案）
- `ViewsOverlayControls`（`views_overlay_controls.cc`）已用 `window->AddOverlayView(view, docking_mode, can_activate=false)` 把 window buttons 面板（`:109`）、menu button（`:118`）、location bar（`:129`）以**独立浮层**贴在内容之上，按 docking mode 定位（Win 右上 / Mac 左上）。
- overlay 是独立合成层，天然盖在主内容（HTML view）之上 → **无需 HTML 非矩形渲染**（该路复杂且脆，放弃）。
- 拖拽区现成模式：`UpdateDraggableRegions`（`views_overlay_controls.cc:181`）把 overlay 区标 non-draggable；HTML 空白区标 draggable。
- 落地：HTML 用 CSS 在右上角留出等宽 padding，overlay 按钮视觉上「嵌」进 tab 行。

### 5.3 HTML ↔ C++ 交互桥（两条链路均已就绪）
- **cefQuery 链路**（选定用于浏览器控制）：renderer 侧 `CefMessageRouterRendererSide`（`client_renderer.cc:209`）↔ browser 侧 `CefMessageRouterBrowserSide`（`base_client_handler.cc:124`）→ `OnQuery` handler（`cef_query_handler.cc`，已含超时/取消/pending 追踪的健壮实现）。
- **c#/dsl API 链路**（用于业务介入）：renderer 侧已有 `JsBridgeV8Handler`（`client_renderer.cc:220`），即已打通的 js ↔ c#/dsl 桥。
- **反向（native → HTML）**：现有 `SetAddress`/`SetLoadingState` 回调改为 `frame->ExecuteJavaScript(...)` 推送给 HTML tabbar，实现 URL 同步、加载态刷新。
- **策略**：纯浏览器控制（导航/前进后退/刷新/停止/URL 同步）走 cefQuery（就近、低延迟）；需要业务介入的（书签持久化、鉴权、扩展）走 c#/dsl API。

---

## 6. 架构设计（定案）

### 6.1 组成
- **1 个内容 `browser_view_`**：承载 web 页面，flex=1。`RootWindowViews::CreateClientHandler` 创建 `ClientHandlerStd`（字段类型 `ClientHandler`），其 delegate 当前指向 `RootWindowViews`。
- **1 个 tabbar `CefBrowserView`**（`tabbar_view_`，`ID_TABBAR_VIEW`）：加载 `webagent://tabbar/?nav=1` / `?nav=0`，由内容 style 决定导航行显隐。创建时 `DefaultClientHandler` 与 `TabbarViewDelegate` 均固定传入 `use_alloy_style=true`，辅助 tabbar browser 始终用 Alloy runtime，不要照搬源码中“与内容 runtime 一致”的旧注释。
- **native overlay 控件**：menu button、min/max/close，用 `AddOverlayView` 浮在 tabbar 右上角。

### 6.2 frameless 启用
- `with_html_tabbar_ = is_normal_type`；`IsFrameless()` 返回 `frameless_`（见 §5.1）。
- 范围为 Views NORMAL 窗口，含普通 window.open 弹窗；非 Views / DEVTOOLS / DIALOG 不在此范围。

### 6.3 布局
- Alloy 内容模式：`[tabbar_view_(动态高度)] + [browser_view_(flex=1)]`。
- Chrome 内容模式：在两者之间插入可获取的 Chrome toolbar；获取为空时跳过。
- 竖向 box 使用 cross-axis STRETCH；tabbar 首选尺寸为 `CefSize(1, height)`，避免宽为 0 时整个尺寸被当作空值。系统按钮由 overlay 承载，不占布局流。

### 6.4 平台差异
- 优先走 `ViewsOverlayControls`（`AddOverlayView`）的**跨平台**方案承载系统按钮，保证 Win/Mac/Linux 一致。
- Windows 已有的 `title_bar_` 基础设施作为参考/回退，不作为主路径以免平台分叉。

### 6.5 tabbar 高度模型（当前实现）

- 基准行 `kHtmlTabbarRowHeight = 34`，导航行 `kHtmlTabbarNavRowHeight = 42`。无有效缓存时，Chrome 内容模式初值 34 DIP，Alloy 内容模式 76 DIP。
- HTML 以 `.root` 的实际总高为准，`Math.ceil` 取整；两种模式都上报 `resize`。`ResizeObserver` 直接观测 `.root`，加载时也主动上报；用 `lastH` 去重，未实现 requestAnimationFrame 或定时器防抖。
- 高度由 `ViewsWindow::tabbar_height_dip_` 保存；`TabbarViewDelegate` 通过 owner 读取。`SetTabbarHeight` 将值限制在 34–400 DIP，变化时保存缓存、`InvalidateLayout()` 并调用窗口 `Layout()`。
- 首帧引导数字按内容 style 分开保存：`GetAppWorkingDirectory()` 下的 `tabbar_height_alloy.txt` / `tabbar_height_chrome.txt`，不是 CEF cache 目录。缓存缺失、解析失败或越界时回退到初值。
- C++ 只保存布局高度，不管理工具栏显隐状态。工具栏 / 书签栏状态的 HTML `localStorage` 持久化是后续设计，不是当前内置页已实现的能力。

---

## 7. 历史问题与当前实现

本节保留旧版白条、布局和拖动问题的要点，不表示当前仍存在这些故障。用户确认基本单 tab UI 已实现；完整回归仍需按平台验证。

- 旧版竖向 box 的 cross-axis 未显式设置 STRETCH，叠加 tabbar 首选宽为 0，导致重排后不可见。当前使用 STRETCH 与 `CefSize(1, height)`。
- 当前 NORMAL Views 窗口已 frameless，内置页不再是空白占位；页签界面仍不等于多 browser 管理。

### 7.1 tabbar 白屏：runtime 历史修复

旧版 tabbar 未正确指定 runtime，曾出现 Chrome-style BrowserView 不能加入 Alloy-style Window 的错误。“与内容 runtime 一致”是早期修复口径，不是当前创建配置。

当前 `OnWindowChanged` 创建辅助 tabbar 时，`DefaultClientHandler` 与 `TabbarViewDelegate` 均传入 `use_alloy_style=true`，即始终使用 Alloy runtime。类定义附近仍有旧注释，应以调用点为准；本轮不修改源码。

### 7.2 frameless 拖动：已落地的路由

历史问题包括 tabbar browser 未成功创建、缺少拖动区转发和坐标原点不匹配。当前实现：

- `DefaultClientHandler` 已继承 `CefDragHandler`，并提供 `GetDragHandler` / `OnDraggableRegionsChanged`。tabbar 创建时 `SetTabbarOwnerWindow(this)`；无 owner 的其他实例不转发。
- 回调转到 `ViewsWindow::SetTabbarDraggableRegions`。内容与 tabbar 区域分别缓存，`ApplyDraggableRegions` 按各自 view 原点转换后合并，再提交窗口，避免两个来源互相覆盖。
- 移除 tabbar 前清空 owner 弱指针，避免延迟回调访问已拆除窗口。
- tabbar 仍是同一 CefWindow 内 docked 的子 View，不是独立 popup。使用 `DefaultClientHandler` 复用非 RootWindow browser 的查询 / 回调体系，不为辅助 tabbar 引入内容 `ClientHandler::Delegate` 的完整窗口责任。

---

## 8. HTML ↔ C++ 当前接口

`window.cefQuery` 的 request 是 JSON 字符串，固定 `channel: "tabbar"`。

- 已接通：`resize` + `height`；`navigate` + `url`；`back` / `forward` / `reload` / `reload_nocache` / `stop`。
- 内置 HTML 发送的页签操作是全小写 `newtab` / `closetab` / `selecttab`，标识字段为 `id`，不是 `newTab` / `closeTab` / `activateTab` + `tabId`。当前 C++ 只返回成功，不创建、关闭或切换内容 browser；其他未识别 action 也落入成功分支，不能以回包判断功能已实现。
- 原生分流位于 `cef_query_handler.cc::HandleTabbarQuery`：从发起 browser 的 client 取 `DefaultClientHandler::GetTabbarOwnerWindow()`，而非用 RootWindowManager 或 view ID 反查。无 owner 时返回失败；有 owner 时调用 `SetTabbarHeight` / `ExecuteTabbarCommand`。该通道不转 C#/DSL。
- 反向接口为 `window.__tabbarApi`：C++ 经 `PushToTabbar` 调用 `onAddressChanged(url)` / `setActiveTitle(title)` / `onLoadingStateChanged({isLoading, canGoBack, canGoForward})`。HTML 另提供 `setActiveUrl` / `setState` / `reset`；当前没有 `onTabsChanged` / `onTabState`。
- 多 tab 的 `tabId` 寻址和新接口属于第二份文档的后续设计，需 HTML/C++ 同步改造；书签持久化等业务接口仍后置。

---

## 9. 分阶段实施状态

保留原阶段 0-5 的分工，以当前源码同步状态。用户已确认基本单 tab UI 显示；下表不代表全部功能或三平台回归验收通过。

| 阶段 | 当前实现 | 验证边界 |
|------|------|------|
| 0 | 已移除 HTML tabbar 开关，Views NORMAL 窗口默认启用 | 含普通 window.open 弹窗；非 Views / DEVTOOLS / DIALOG 排除 |
| 1 | 竖向 box 使用 cross-axis STRETCH，首选宽为 1 | 缩放、内容变化后保持满宽 |
| 2 | NORMAL Views 窗口 frameless，系统按钮使用 native overlay；拖动区路由已接通 | 按钮与 HTML 预留区对齐、各平台拖动需实测 |
| 3 | tabbar 固定 Alloy runtime；内置 HTML 提供页签界面，nav 参数控制导航行 | HTML 页签增删选不是多 browser 管理 |
| 4 | 复用 DefaultClientHandler 的 owner 指针；cef_query_handler 分流导航 / resize；__tabbarApi 反向推送；两源拖动区合并、工作目录高度缓存 | 当前仍是单内容 browser 路由；不新建专用 handler 类 |
| 5 | Alloy 使用 HTML 导航栏；Chrome 保留可获取的 Chrome toolbar；demo UI / 旧 Windows 标题栏在 tabbar 窗口门控失活 | 源码已于 2026-09-11 物理删除（demo 工具栏/地址栏、Win 自定义标题栏、location-bar overlay、menu_bar_、CefTextfieldDelegate 等；汉堡菜单/上下文菜单/命令 ID/views_menu_bar 文件/switch 定义保留）；书签功能、工具栏状态持久化仍后置 |

原推进顺序为 0 → 1 → 2 → 3 → 4 → 5。frameless 与系统按钮同阶段落地，避免中间态窗口无法操作。真正多 tab 见 `MULTITAB_WINDOW_DESIGN.md`，M1-M5 尚未开始。

---

## 9.1 关联修复：Chrome 自建窗口的 popup 路径

此模式不创建 ViewsWindow，与 HTML tabbar 路径互斥。`--use-chrome-window` 已删除，当前由 `UseChromeWindowGlobal()` 决定是否使用默认 Chrome 自建窗口（条件见文档首部）。

- **历史问题**：初始窗口由 Chrome 自建，但 popup 曾落入原生 RootWindow 宿主路径，与初始窗口外观不一致。旧开关下的对比不再作为当前启动说明。
- **当前实现**：`DefaultClientHandler::OnBeforePopup` 先处理 picture-in-picture 特例；其余 popup 在 `UseChromeWindowGlobal()` 为 true 时设置 `windowInfo.runtime_style = CEF_RUNTIME_STYLE_CHROME`，必要时创建 `DefaultClientHandler(false)`，返回 false 交给 Chrome 创建，不创建 RootWindow。非该模式继续走 `CreateRootWindowAsPopup`。
- **关闭语义**：`OnBeforeClose` 在默认 Chrome 窗口模式下跳过 opener 子窗口级联关闭，避免将已独立的 Chrome tab / 窗口一并关闭。
- **验证点**：默认 Chrome 模式的 window.open 外观与关闭独立性，以及显式选择 Alloy / Chrome-style-window 后的非默认路径回归。本轮仅核对源码和修改文档，未构建或运行验证。

---

## 10. 待决 / 风险

- **跨平台一致性**：HTML tabbar 窗口走 overlay 系统按钮，不使用旧 Windows title_bar_ 路径；各平台对齐、拖动仍需实测。
- **overlay 与 HTML padding**：按钮区宽度及 docking 位置需与 HTML 留白匹配，特别是导航行高度变化时。
- **高度单位**：当前将 CSS px 总高取整后直接作为 DIP，未做缩放换算；页面 zoom / 系统 DPI 组合需验证。
- **高度与查询路由**：实现以 §6.5 / §8 为准：工作目录的两个 txt 缓存、owner 指针寻址、lastH 去重。不是 cachePath JSON、RootWindowManager 反查或 requestAnimationFrame 防抖。
- **多 tab**：当前页签 id 仅属于 HTML 界面，不是 C++ Tab 注册表标识；真正多 browser 管理及迁移属于第二份文档的后续工作。
- **构建与验收**：构建由用户执行；本次文档同步不声称零新增警告或全平台通过。

---

## 附录：关键源码位置速查（撰写时快照）

| 符号 / 功能 | 位置 |
|-------------|------|
| `TabbarViewDelegate`（尺寸，当前首选宽=1） | `views_window.cc:76-86` |
| tabbar 启用判定 `with_html_tabbar_`（旧为 `--use-html-tabbar`，现默认启用） | `views_window.cc:1350` |
| tabbar 创建 / 销毁 | `views_window.cc:1183-1199` / `:1223-1227` |
| `IsFrameless` | `views_window.cc:422-426` |
| 拖拽区（Win 自定义标题栏） | `views_window.cc:391-447` |
| `CalculateChromeToolbarType` | `views_window.cc:145` |
| `CreateLocationBar` / `OnKeyEvent` | 已随 2026-09-11 旧 UI 清理删除（历史：`views_window.cc:1405-1419` / `:737`） |
| `AddOverlayView` 用法 | `views_overlay_controls.cc:109/118/129` |
| overlay 拖拽区 | `views_overlay_controls.cc:181` |
| message router（renderer/browser） | `client_renderer.cc:209` / `base_client_handler.cc:124` |
| js↔c#/dsl 桥 | `client_renderer.cc:220`（`JsBridgeV8Handler`） |
| cefQuery handler | `cef_query_handler.cc` |
| tabbar 内置页构建 | `custom_scheme.cc`（`webagent://tabbar/`） |
