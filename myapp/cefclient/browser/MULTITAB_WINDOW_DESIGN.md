# WebAgent 多窗口多 Tab 模型设计文档（开发总纲）

> 状态：设计定稿（尚未开始编码），作为多 tab / 多窗口开发的唯一依据。
> 关系：本文件是 `TABBAR_DESIGN.md` 的**后续立项**。`TABBAR_DESIGN.md` 交付「单 browser + HTML tabbar 外壳」（阶段 0-5，为本模型的地基）；本文件在其之上引入「真正的多 tab（每 tab 一个 CefBrowser）+ 撕离 / 合并 / 跨窗口迁移」。
> 说明：文内源码行号为撰写时快照，可能随代码演进偏移，以函数名 / 符号名为准；标注「待核实」处须在对应阶段动手前再次读码确认。

---

## 1. 文档定位与分工

| 文档 | 覆盖范围 | 核心实体 |
|------|----------|----------|
| `TABBAR_DESIGN.md` | 单窗口单 browser + HTML tabbar 外壳（frameless、overlay 系统按钮、cefQuery 桥、alloy 地址栏） | `ViewsWindow`（持 1 个 `browser_view_` + 1 个 `tabbar_view_`） |
| **本文件** | 单窗口内 N 个 tab（每 tab 独立 `CefBrowser`）、tab 新建 / 关闭 / 切换 / 重排、撕离成独立窗口、拖回合并、跨窗口迁移 | `Application` / `TabbedRootWindowViews`（Window）/ `Tab` 三层 |

当前地基：单内容 browser 使用 `ClientHandlerStd`（继承 `ClientHandler`），delegate 指向 `RootWindowViews`。辅助 HTML tabbar 使用独立 `DefaultClientHandler`，固定 Alloy runtime，通过 owner 指针转发查询与拖动区。C#/DSL 回调能力不等于内容状态 delegate 接口，两种 handler 不可直接互换。下文 Tab / TabbedRootWindowViews 及迁移流程均为后续设计，M1-M5 未开始。

---

## 2. 目标与非目标

### 2.1 目标
1. 一个顶层窗口承载 N 个 tab，每 tab 一个独立 `CefBrowser`、导航栈和业务上下文；不保证每 tab 独占 renderer 进程，进程分配由 Chromium 策略决定。
2. tab 新建 / 关闭 / 切换 / 重排，由 HTML tabbar 驱动。
3. tab 可从窗口「撕离」成独立顶层窗口；可拖回或拖入另一窗口「合并」；支持跨窗口迁移。
4. 单窗口剩最后一个 tab 时，拖动 tab = 移动窗口（不分离，与 Chrome 一致）。
5. 全程保持 HostCLR / DSL / 注入 JS / 自定义 scheme 等既有能力，不因多 tab 而丢失。

### 2.2 非目标（本立项不做）
- tab 冻结 / 丢弃（tab discarding，内存回收）——留作后续优化项，仅在设计中预留位置。
- 书签栏功能（存储 / 持久化）——沿用 `TABBAR_DESIGN.md` 的后置结论。
- 跨进程会话恢复、tab 拖到桌面新建窗口的极端手势细节——先保证窗口间迁移，再打磨。
- OSR / 原生非 Views 窗口 / 默认 Chrome 自建 tabstrip 模式不在本模型范围。`--use-chrome-window` 已删除；当前由 `UseChromeWindowGlobal()` 选择 Chrome 自建模式，该路径不创建 ViewsWindow。本设计仅面向实际进入 Views NORMAL 窗口的路径，模式条件见 `TABBAR_DESIGN.md`。

---

## 3. 实体模型（三层）

```
Application（进程内单例，语义层）
  ├─ 全局 Tab 注册表：tabId -> Tab*（用于 cefQuery tabId 寻址、跨窗口迁移查找）
  ├─ tab 拖拽会话协调器（撕离 / 合并 / 迁移的状态机）
  └─ 持有 N 个 Window
        │
        ▼
Window = TabbedRootWindowViews（接入 RootWindowManager）
  ├─ 1 个 CefWindow（顶层窗口壳，frameless）
  ├─ 1 个 tabbar_view_（HTML CefBrowserView，本窗口独占，撕离时不迁移、由新窗口另建）
  ├─ native overlay 系统按钮（menu / min / max / close，AddOverlayView）
  ├─ vector<Tab>（本窗口当前承载的 tab）
  └─ active_index（当前显示的 tab）
        │
        ▼
Tab（一个 web 页面的完整承载）
  ├─ 1 个 CefBrowser
  ├─ 1 个 content CefBrowserView（flex=1，切 tab 时 SetVisible 切换）
  ├─ 1 个 ClientHandlerStd（继承 ClientHandler，delegate 指向 Tab，见 §5.4）
  └─ 状态缓存：url / title / loading / canGoBack / canGoForward / favicon
```

**要点**
- **tabbar 归 Window，不归 Tab**：每个 Window 有且仅有一个 tabbar HTML view。撕离一个 tab 到新窗口时，迁移的是该 tab 的**内容 view**，新窗口另建自己的 tabbar view（不搬 tabbar）。
- **Tab 是其内容 handler 的 delegate 接收方**：每 tab 独立 handler 和 delegate，以保留不带 browser 参数的状态回调所属身份（§4.1）。
- **Application 是 tabId 的全局真相源**：cefQuery 只带 tabId，由 Application/Window 解析到具体 Tab，避免旧 stage4「窗口唯一 content browser」的假设（§7）。

---

## 4. 关键约束与代码证据（已核实，沿用）

### 4.1 一个 ClientHandler 天生服务单个 browser
`ClientHandler::Delegate` 的 `OnSetAddress` / `OnSetTitle` / `OnSetLoadingState` **不带 browser 参数**（`client_handler.h:62-82`），只有 `OnBrowserCreated` / `Closing` / `Closed` 带。→ 一个 handler 无法区分状态来自哪个 browser。**故每 tab 必须独立 handler**（或每 tab 独立 delegate 接收方 + 独立 handler 实例）。

### 4.2 RootWindowViews 强单 browser 假设
`RootWindowViews` 持单个 `browser_`，`OnBrowserCreated` 有 `DCHECK(!browser_)`，`OnBrowserClosed` 直接 `DetachDelegate + 销毁 handler + NotifyDestroyed`（`root_window_views.h:122` 及 .cc）。→ 多 tab 必须**打破此假设**：新增 `TabbedRootWindowViews` 或改造为持 `vector<Tab>`。

### 4.3 CefBrowserView 跨窗口迁移（参考范例，内容 tab 待验证）
`views_overlay_browser.cc` 的 pop-out/pop-in 是视图迁移的参考，不是本项目内容 tab 在所有 runtime / 平台下可无损迁移的证明。M3 需验证相同 browser 的持续渲染、焦点、Chrome toolbar 归属、窗口回调和关闭流程；不预先保证无需 CEF 适配。

### 4.4 内容 handler 与辅助 tabbar handler 不同
`RootWindowViews::CreateClientHandler` 当前创建 `ClientHandlerStd(this, ...)`。`ClientHandler::Delegate` 提供内容状态与生命周期通知。tabbar 创建处的 `DefaultClientHandler(true)` 只是辅助 browser 的范例，其 `SetTabbarOwnerWindow` 不是内容状态 delegate 接口。不能从该范例推导“每 tab 直接用 DefaultClientHandler 即可”。

### 4.5 原生 Views 收不到单 tab 拖拽手势
`CefViewDelegate` 只有 `GetPreferredSize` / `OnLayoutChanged` / `OnFocus` / `OnThemeChanged`，无 mouse/drag 事件（`cef_view_delegate.h`）。→ tab 页签的拖拽 / 重排 / 撕下手势必须在 **HTML 层**用 JS 捕获，再驱动 C++；这是 HTML tabbar 的根本理由。

### 4.6 RootWindowManager 生命周期
`RootWindowManager` 用 `set<RootWindow>` + `terminate_when_all_windows_closed_` 管理；撕离出的新窗口必须接入这套（`CreateViewsWindow` 见 `root_window_views.cc:553` 附近）。退出判定 `MaybeCleanup`：`root_windows_.empty() && other_browser_ct_==0`。

### 4.7 draggable region 链路（拖动窗口 / tab 语义）
当前 HTML tabbar 路由：CSS `-webkit-app-region` → `DefaultClientHandler::OnDraggableRegionsChanged` → owner 的 `ViewsWindow::SetTabbarDraggableRegions`。内容区路由经 `ClientHandler` / `RootWindowViews::OnSetDraggableRegions`。两源最终由 `ViewsWindow::ApplyDraggableRegions` 各自换算坐标后合并，调用 `CefWindow::SetDraggableRegions`。多 tab 需适配为当前 active tab 的内容拖动区；各平台行为仍需实测。

---

## 5. 架构设计（定案）

### 5.1 每 tab 独立 browser + 独立 handler（后续设计）
- 每 Tab 持 1 个 `CefBrowser` + 1 个 content `CefBrowserView` + 1 个内容 handler，沿用 `ClientHandlerStd` / `ClientHandler` 体系。辅助 tabbar 继续使用独立 `DefaultClientHandler`。
- Tab 实现内容 handler 的 `ClientHandler::Delegate`，接收状态及生命周期回调；更新自身缓存后，向当前所属 Window 上报。
- 窗口相关 delegate 责任由 Tab 转发给所属 Window；M1 需核对完整 delegate 接口、关闭流程和 RootWindowManager 计数，不是只替换创建类名。

### 5.2 Window = TabbedRootWindowViews
- 持 `CefWindow` + `tabbar_view_` + `vector<Tab>` + `active_index`，接入 `RootWindowManager`。
- 负责：布局（tabbar 行 + 当前 active tab 内容 view）、tab 增删切换、tabbar 与内容的双向桥转接、撕离 / 合并时的 view reparent 与 Tab 所属 Window 更新（内容 handler delegate 不变）。
- 复用 `TABBAR_DESIGN.md` 的 frameless + overlay 系统按钮 + cross-axis STRETCH 结论。

### 5.3 布局与切换
- 窗口竖向 box：`[tabbar_view_（动态高度，flex=0）] + [active tab 的 content view（flex=1）]`，cross-axis = `STRETCH`（沿用阶段1 修复）。
- **切 tab = SetVisible**：所有已创建 tab 的 content view 都 `AddChildView` 进内容区，仅 active 的 `SetVisible(true)` 且 flex=1，其余 `SetVisible(false)`。好处：tab 保持「温」态（browser 不销毁，切换快，符合 Chrome 行为）；代价：内存随 tab 数增长（后续用 tab discarding 优化，本期不做）。
- 默认保留 SetVisible 切换设计。add/remove child view 本身不等于销毁 browser，不能据此声称省内存或必然丢失页面状态；隐藏 view 的布局、焦点与后台行为在 M2 验证。

### 5.4 handler delegate 归属
- 内容 handler 的 delegate 始终指向所属 Tab，不指向 Window。状态路由为 handler → Tab → 当前所属 Window。
- 迁移时保留 browser、handler 与 Tab 的关系，更新 Tab 的所属 Window 及窗口内容视图归属。这不等于当前 ViewsWindow / BrowserView delegate 已支持迁移；其窗口绑定回调和拆卸行为须在 M1/M3 适配验证。
- Tab 需活到 browser 关闭回调完成，或按现有约定 `DetachDelegate()`；不能在异步关闭完成前销毁 delegate 接收方。

---

## 6. 撕离 / 合并 / 跨窗口迁移（核心难点）

### 6.1 撕离（detach：tab → 新独立窗口）
触发：HTML 层检测到某 tab 页签被拖出 tabbar 区域（垂直位移超阈值），发 cefQuery `detachTab {tabId}`。

时序：
1. Window A 从 `vector<Tab>` 移出该 Tab；若它是 active，先切到相邻 tab（或若是最后一个，见 6.4）。
2. `RemoveChildView` 把该 tab 的 content view 从 Window A 的内容区摘下（**不销毁 browser**）。
3. 新建 Window B = `TabbedRootWindowViews`（`CreateTopLevelWindow`，接入 `RootWindowManager`），新建 B 自己的 tabbar view。
4. Window B `AddChildView` 接管该 content view，把 Tab 加入 B 的 `vector<Tab>` 并置为 active。
5. 更新 Tab 的所属 Window 为 B，内容 handler 的 delegate 仍指向原 Tab；同步适配内容 view 的窗口绑定回调（§5.4）。
6. 两窗口各自 `Layout()` + 刷新各自 tabbar（`onTabsChanged`）。
7. Window B 定位到鼠标处并进入原生窗口 move-loop（§7），使「拖出」动作平滑接管为「拖动新窗口」。

以 §4.3 的 overlay 流程为参考；上述时序是设计目标，待 M3 验证，不是已跑通的内容 tab 迁移流程。

### 6.2 合并（merge：tab 拖入已存在窗口）
触发：拖动中的 tab（或独立窗口）落到目标 Window C 的 tabbar 区域，HTML 层发 cefQuery `mergeTab {sourceTabId, targetWindow, index}`。

时序：与撕离对称——从源 Window `RemoveChildView` + 源 `vector<Tab>` 移除；目标 Window C `AddChildView` + 插入 `vector<Tab>` 指定 index + 更新 Tab 所属 Window 为 C；若源 Window 因此清空则关闭源 Window（6.4）；两侧 `Layout()` + 刷新 tabbar。

### 6.3 跨窗口迁移
即「先 detach 再 merge」的组合，或直接在两个已存在窗口间移动一个 tab。统一走 §6.1/§6.2 的 view reparent + Tab 所属 Window 更新原语，Application 的拖拽会话协调器负责判定落点是「新窗口」「已有窗口 tabbar」还是「原窗口」。

### 6.4 单 tab 窗口语义
- 窗口只剩 1 个 tab 时：拖动该 tab = **移动窗口**（不分离）。分离会导致源窗口清空后原地重建 + 闪烁，无意义（与 Chrome 一致）。
- 实现：单 tab 时把 tab 页签区域也标为 draggable（`-webkit-app-region: drag`），JS 不再对其做 detach 手势；tab 数变化时 JS 动态改 CSS，CEF 自动重回调 `OnDraggableRegionsChanged` 更新。
- 一个 tab 被移走后源窗口 `vector<Tab>` 空 → 关闭源 Window（`RootWindowManager` 计数驱动退出）。

---

## 7. 拖拽手势与 draggable region 语义

- **tab 页签**：多 tab 时标 `no-drag`（JS 捕获 mousedown 做重排 / 撕下）；单 tab 时标 `drag`（移动窗口）。
- **tabbar 空白区**：始终 `drag`（移动窗口）。
- **关闭按钮（x）等交互控件**：即使在 drag 区内也要挖 `no-drag` 小洞（drag 区会吞掉 JS mousedown，同 Chrome）。
- **撕离的窗口拖动**：用原生 OS 窗口 move-loop（Windows `WM_SYSCOMMAND` / `SC_MOVE` 或 move-loop API），**不**用 JS 每帧喂坐标——平滑度好、reparent 只做一次。这是撕离流畅度的关键，属阶段 M3 的实测点。
- 平台差异：Windows Views 路径 draggable 已现成；Mac/Linux 靠 libcef Views 框架，需实测（4.7）。

---

## 8. 通信桥（tabId 寻址，重构 TABBAR_DESIGN §8）

> 当前与后续设计的边界：现有桥通过 DefaultClientHandler 的 tabbar owner 指针定位 ViewsWindow，不是 GetViewForID 查找；导航仍操作窗口唯一内容 browser。本节为未实施的多 tab 协议草案，不是当前可用 API。实施时 HTML/C++ 必须同步更新，不能直接用草案替代现有调用。

### 8.1 正向（HTML tabbar → C++）
- 现有 cefQuery 分流位于 `cef_query_handler.cc::HandleTabbarQuery`。多 tab 需将该路由适配到 Window/Tab 模型；是否拆出 `tabbar_handler.cc/.h` 在实施时确定，并非现有文件或必需前提。保留 resize 等窗口级操作。
- request（后续 JSON 草案）：现有 HTML 使用 `newtab` / `closetab` / `selecttab` + `id`；下列驼峰 action 与 `tabId` 需同步迁移，或在实施时明确兼容策略。`back|forward|reload|stop` 表示四种可选 action，不是字面请求值；现有 `reload_nocache` 也需保留。
```jsonc
{ "channel":"tabbar", "action":"newTab", "url":"..." }
{ "channel":"tabbar", "action":"closeTab",   "tabId": 3 }
{ "channel":"tabbar", "action":"activateTab", "tabId": 2 }
{ "channel":"tabbar", "action":"reorderTab",  "tabId": 2, "toIndex": 0 }
{ "channel":"tabbar", "action":"detachTab",   "tabId": 3 }
{ "channel":"tabbar", "action":"mergeTab", "sourceTabId": 3, "targetWindow": 5, "index": 1 }
{ "channel":"tabbar", "action":"navigate", "tabId": 2, "url":"https://..." }
{ "channel":"tabbar", "action":"back|forward|reload|stop", "tabId": 2 }
```
- 后续 handler 定位：先根据发起 tabbar browser 的所属关系定位 Window，再按 `tabId` 解析 Tab。现有 owner 类型是 ViewsWindow，不能假定已可指向 TabbedRootWindowViews；具体映射在 M1/M2 适配。普通操作校验 Tab 属于发起窗口；跨窗口操作由 Application 协调，校验源 / 目标存活与归属。

### 8.2 反向（C++ → HTML tabbar，后续扩展）
沿用当前 `window.__tabbarApi` 命名空间。下列 `onTabsChanged` / `onTabState` 是待新增接口，当前内置页尚未提供；现有地址栏、标题和加载态接口见 `TABBAR_DESIGN.md` §8。
- 每个 Tab 的状态变化（`OnSetAddress` / `OnSetTitle` / `OnSetLoadingState` / favicon）→ Tab 更新缓存 → 上报所属 Window → Window 对其 `tabbar_view_` 执行 `ExecuteJavaScript` 推送：
```js
window.__tabbarApi.onTabsChanged([{ id, title, url, active, loading }, ...]);
window.__tabbarApi.onTabState(tabId, { url, title, loading, canGoBack, canGoForward, favicon });
```
- active tab 的地址栏 / 加载态同步走 `onTabState`（active 项）；非 active tab 仅更新页签标题 / 图标。

### 8.3 业务介入
- 纯浏览器控制（导航 / 前进后退 / tab 操作）走 cefQuery（就近、低延迟）。
- 需 C#/DSL 介入的（每 tab 绑定 agent 上下文、鉴权、持久化）走既有 HostCLR / DSL API 链路，不与 tabbar cefQuery 混用。

---

## 9. 生命周期与 RootWindowManager 集成

- 每个 Window（含撕离新建的）都是 `RootWindow`，接入 `RootWindowManager` 的 `set<RootWindow>`；退出由 `MaybeCleanup`（`root_windows_.empty() && other_browser_ct_==0`）驱动，无需另造退出逻辑。
- Tab 关闭：请求关闭 CefBrowser，处理 beforeunload 可能取消关闭的情况；收到最终关闭回调后再完成注册表移除、delegate 解绑和释放。不能将请求关闭等同于同步销毁；窗口清空后的关闭也需与此流程协调。
- 迁移目标：不销毁 browser（reparent 而非重建），保持页面状态、导航栈和 C# 上下文连续；在 M3/M4 验证，不作为现有能力保证。
- DEVTOOLS / DIALOG 不纳入本模型。普通 window.open 在现有 Views 路径可归为 NORMAL，不能笼统排除所有 popup；实施时需明确其建窗、建 tab 与 opener 关闭策略，不改变本模型以外的窗口路径。

---

## 10. 分阶段实施计划

> 前置：`TABBAR_DESIGN.md` 阶段 0-5（单 browser 外壳 + 桥）需先构建验证通过。本计划在其之上推进。每阶段可独立构建验证。

| 阶段 | 内容 | 目标 / 验证点 |
|------|------|---------------|
| M1 | 引入 `Tab` + `TabbedRootWindowViews`，打破 `RootWindowViews` 单 browser 假设；单 tab 跑通（1 个 Window 1 个 Tab） | 现有单页行为不变；DCHECK 不触发；C# 回调正常 |
| M2 | 单窗口多 tab：`newTab`/`closeTab`/`activateTab`/`reorderTab`（cefQuery，tabId 寻址）+ N 个 browser + SetVisible 切换 + 反向状态推送 | 可建多 tab、切换、关闭、重排；页签标题 / URL / 加载态同步 |
| M3 | 撕离：tab 拖出成独立窗口（view reparent + 新 Window + Tab 所属 Window 更新 + 原生 move-loop 拖动） | 拖出成独立窗口、继续渲染、可独立关闭；拖动平滑（实测原生窗口拖动） |
| M4 | 合并 / 跨窗口迁移：tab 拖入已存在窗口 tabbar；源窗口清空则关闭 | 拖入另一窗口指定位置；源窗口空自动关闭；状态连续不重载 |
| M5 | 收尾：favicon / title / active 精确同步、单 tab 拖动=移窗语义、Mac/Linux draggable 联调；预留 tab discarding 位置 | 三平台一致；单 tab 拖动移窗；边界稳态 |

顺序原则：先立骨架（M1）→ 单窗口多 tab 可见（M2）→ 撕离（M3）→ 合并（M4）→ 打磨与跨平台（M5）。tab discarding、书签功能在 M5 之后单独立项。

---

## 11. 待决 / 风险

- **迁移归属的正确性**：内容 handler delegate 保持指向 Tab，迁移时更新 Tab 所属 Window，同时检查 BrowserView 窗口回调绑定。不能将状态推给旧窗口或在移除 view 时误关闭 browser；这是 M3/M4 的首要验证点。
- **原生窗口拖动平滑度**（M3）：reparent 后接管为 OS move-loop 的时机与手感，Windows 优先，Mac/Linux 后测。
- **tabbar 资源开销**：每窗口新增一个辅助 browser，但不保证一对一新增 renderer 进程。进程数和内存需实测；不为未验证的开销直接改变全局进程隔离策略。
- **内存随 tab 数增长**：SetVisible 保活策略下 browser 不销毁；tab discarding 留作后续。
- **Mac/Linux draggable**：libcef Views 路径需实测（4.7）。
- **构建环境**：本地无法编译，全部改动须用户构建验证；保持零新增警告 / 错误基线。

---

## 附录：关键源码位置速查（撰写时快照）

| 符号 / 功能 | 位置 |
|-------------|------|
| `ClientHandler::Delegate`（状态回调不带 browser） | `client_handler.h:62-82` |
| `RootWindowViews` 单 browser 假设 | `root_window_views.h:122` 及 .cc |
| `CreateViewsWindow` | `root_window_views.cc:553` 附近 |
| CefBrowserView 跨窗口转移范例 | `views_overlay_browser.cc`（`CreateTopLevelWindow` / `AddChildView` / `RemoveChildView`） |
| 内容 / 辅助 tabbar handler 创建 | `root_window_views.cc::CreateClientHandler` / `views_window.cc::OnWindowChanged` |
| `RootWindowManager` 退出判定 | `root_window_manager.cc` `MaybeCleanup` |
| draggable region 链路 | `cef_drag_handler.h:76` / `root_window_views.cc:463-475` / `cef_window.h:316` |
| message router（renderer/browser） | `client_renderer.cc:209` / `base_client_handler.cc:124` |
| cefQuery handler 注册 | `test_runner.cc`（`CreateMessageHandlers`）/ `cef_query_handler.cc` |
| tabbar 内置页 / 自定义 scheme | `custom_scheme.cc`（`webagent://tabbar/`） |
| tabbar view 创建 / 布局 | `views_window.cc`（`tabbar_view_` / cross-axis STRETCH） |
