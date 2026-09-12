# WebAgent 多窗口多 Tab 模型设计文档（开发总纲）

> 状态：M1 / M2 已实现（已回归通过）；M3 已实现（2026-09-13，已回归通过）；M4a / M4b / M5 的 Windows 侧已实现并经多轮实测回归（2026-09-14：撕离、拖回合并、整窗移动、误合并修复、Exit/X 生命周期、重排、favicon、中键）；M4c Mac/Linux 已实现（mac 用 NSEvent 本地监视器、linux 用 X11 JS 喂给式拖环，待各自平台构建回归）。作为多 tab / 多窗口开发的唯一依据。
> 关系：本文件是 `TABBAR_DESIGN.md` 的**后续立项**。`TABBAR_DESIGN.md` 交付「单 browser + HTML tabbar 外壳」（阶段 0-5，为本模型的地基）；本文件在其之上引入「真正的多 tab（每 tab 一个 CefBrowser）+ 撕离 / 合并 / 跨窗口迁移」。
> 说明：文内源码行号为撰写时快照，可能随代码演进偏移，以函数名 / 符号名为准；标注「待核实」处须在对应阶段动手前再次读码确认。

---

## 1. 文档定位与分工

| 文档 | 覆盖范围 | 核心实体 |
|------|----------|----------|
| `TABBAR_DESIGN.md` | 单窗口单 browser + HTML tabbar 外壳（frameless、overlay 系统按钮、cefQuery 桥、alloy 地址栏） | `ViewsWindow`（持 1 个 `browser_view_` + 1 个 `tabbar_view_`） |
| **本文件** | 单窗口内 N 个 tab（每 tab 独立 `CefBrowser`）、tab 新建 / 关闭 / 切换 / 重排、撕离成独立窗口、拖回合并、跨窗口迁移 | `Application` / `TabbedRootWindowViews`（Window）/ `Tab` 三层 |

当前地基（已实现）：内容 tab 使用 `Tab`（实现 `ClientHandler::Delegate`，每 tab 独立 `ClientHandlerStd`，owner 可经 `Tab::SetOwner` 重指）；窗口容器为 `TabbedRootWindowViews`（`tabs_` 主线程注册表 + `view_tabs_` UI 线程关联表，经 `RootWindow::Delegate::CreateDetachedWindow` 接入 `RootWindowManager`）；辅助 HTML tabbar 使用独立 `DefaultClientHandler`，固定 Alloy runtime，通过 owner 指针转发查询与拖动区。C#/DSL 回调能力不等于内容状态 delegate 接口，两种 handler 不可直接互换。

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
RootWindowManager（进程内单例，窗口簿记）
  ├─ set<RootWindow> 生命周期 / 退出判定（MaybeCleanup）
  ├─ CreateRootWindow / CreateRootWindowAsPopup / CreateDetachedWindow（撕离建窗入口）
  └─ Tab 拖拽会话状态 → 由 TabDragController 承担（M4a，§6.5；Windows 先行）
        │
        ▼
Window = TabbedRootWindowViews（接入 RootWindowManager）
  ├─ 1 个 CefWindow（顶层窗口壳，frameless，经 ViewsWindow 持有）
  ├─ 1 个 tabbar_view_（HTML CefBrowserView，本窗口独占，撕离时不迁移、由新窗口另建）
  ├─ native overlay 系统按钮（menu / min / max / close，AddOverlayView）
  ├─ tabs_（主线程：vector<shared_ptr<Tab>>，本窗口当前承载的 tab 与顺序）
  ├─ view_tabs_（UI 线程：view ↔ Tab 关联表，strip 顺序真相源）
  └─ 兼容 browser_（RootWindowViews::SetActiveBrowser，窗口级调用方使用）
        │
        ▼
Tab（一个 web 页面的完整承载）
  ├─ 1 个 CefBrowser
  ├─ 1 个 content CefBrowserView（flex=1，切 tab 时 SetVisible 切换；delegate 为可重指的
  │    ContentBrowserViewDelegate 包装，owner 指向所属 ViewsWindow）
  ├─ 1 个 ClientHandlerStd（继承 ClientHandler，delegate 指向 Tab，见 §5.4；owner 可 SetOwner 重指）
  └─ 状态缓存：url / title / loading / canGoBack / canGoForward / favicon / draggable_regions
```

**要点**
- **tabbar 归 Window，不归 Tab**：每个 Window 有且仅有一个 tabbar HTML view。撕离一个 tab 到新窗口时，迁移的是该 tab 的**内容 view**，新窗口另建自己的 tabbar view（不搬 tabbar）。
- **Tab 是其内容 handler 的 delegate 接收方**：每 tab 独立 handler 和 delegate，以保留不带 browser 参数的状态回调所属身份（§4.1）。
- **寻址采用每窗口注册表 + 进程唯一 browser_id**，不设独立的全局 Tab 注册表：cefQuery 经发起 tabbar 的 owner 定位窗口，再在窗口内解析；跨窗口识别 view 归属用 `ViewsWindow::GetHostWindowForView`（每 tab 包装 delegate 的 live 集合）。设计早期设想的"Application 全局 Tab 注册表"已由上述机制替代。

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

### 6.1 撕离（detach：tab → 新独立窗口）——已实现（M3）

触发：HTML 层检测到某 tab 页签被拖出 tabbar 区域（垂直位移 ≥ 24px，鼠标按住期间页面持续收到 mousemove，即使光标已离开 strip），发 cefQuery `detachtab {id}`。

实现链路（2026-09-13 定稿，符号见附录）：
1. UI（`TabbedRootWindowViews::OnTabDetachRequested`）：`view_tabs_` 寻址 → 单 tab 拒绝 → 若是 active 先 `SetActiveBrowserView(next)` → `RemoveBrowserView` 摘下 content view（不销毁 browser）→ 移出 `view_tabs_`（不 ReleaseViewTab）。
2. main（`BeginDetachOnMain`）：browser 存活校验 → 移出源 `tabs_` + 快照 → `delegate_->CreateDetachedWindow(tab, bounds)`（`RootWindowManager` 实现：new `TabbedRootWindowViews` → `InitDetached`：接管 Tab、`tab->SetOwner` 新窗口、`SetActiveBrowser`、post UI）。
3. UI（`ContinueDetachedInit` → `CreateDetachedViewsWindow`）：`ViewsWindow::CreateForExistingView`（校验 view / `RepointViewOwner` 重指每 tab 包装 delegate → 构造 ViewsWindow → `CreateTopLevelWindow`），新窗口自带 tabbar。
4. main（`FinishDetachedAdoption`）：`ReplayState` 回放地址 / 标题 / 加载态 / 拖动区 + `PublishTabSnapshot`。
5. 三平台：`TabDragController::Start(this, browser, /*allow_merge=*/true)`（§6.5）接管拖动（M4a 已替换 M3 v1 的 `SC_MOVE` move-loop；`StartDetachedMoveLoop` 已删除）。

abort 路径：各阶段校验失败 / in-flight browser 关闭 → `AbortDetachedWindow`（`detach_keep_alive_` 自持到最终关闭回调，`NotifyWindowlessTeardown` 幂等收尾，避免 manager 双重销毁 DCHECK）。

### 6.2 合并（merge：拖动中的 tab 落入已存在窗口）——M4a 设计定稿

**为什么原方案不可行**：原设计「HTML 层发 cefQuery `mergeTab`」基于"拖动中的页面能看到落点"的假设。实际撕离后拖的是**原生窗口**（move-loop），期间任何窗口的 HTML 都收不到鼠标事件（鼠标被 OS 捕获）；且 M3 v1 的 `SC_MOVE` 模态循环会阻塞浏览器进程 UI 线程的消息泵，cefQuery 回调也无法及时送达。**合并的落点检测必须由 C++ 侧完成**，HTML 只承担"显示插入指示条 + 回报插入位"（前提是拖动循环不阻塞消息泵，见 §6.5）。

**交互定义**：tab 手势拖动的窗口（撕离产生的单 tab 窗口 B，或单 tab 窗口的页签拖动）被拖到某窗口 W（含原窗口 A）的 tabbar 区域上空 → W 的 tabbar 显示插入指示条；松手 → B 的 tab 并入 W 指定位置，B 自动关闭。落在 tabbar 之外（内容区 / 桌面）→ 不合并，B 留在原地（现状）。空白区整窗拖动（M4b）永不合并（`merge:0`，§7）。

**合并时序**（实现为 `MergeDraggedTabInto`（UI）+ `CompleteTabMerge`（main），符号见附录）：
1. 拖环结束（`TabDragController` WM_LBUTTONUP / mouseUp / `windowdragend`，§6.5）：合并经 `CefPostTask` 投递出消息分发后执行（见校验要点末条），确认落点 target W + 缓存的 `beforeId`（HTML 回报；缺失或过期 → 0 = 追加末尾）。
2. UI（源 B 侧，`MergeDraggedTabInto` 前半）：预校验全部可拆条件 → `RemoveBrowserView(allow_active_removal=true)` 整块摘下（含 active；摘后清源 `browser_view_`）→ `view_tabs_.clear()`。
3. UI（目标 W 侧，同函数后半）：`RepointViewOwner` → `RegisterBrowserView`（迁移分支）→ `AddBrowserView` 逐个挂载（Add 失败回滚登记并释放该 browser）→ 全部收养时整块按锚点插入 `view_tabs_`，部分收养追加末尾（`anchored` 标志）→ `SetActiveBrowserView`（源 active 优先，fallback 末位）。
4. main（**单任务原子段** `CompleteTabMerge(adopted, active, target, before_id, anchored)`，源 / 目标 root 以 `scoped_refptr` 保活）：源 `tabs_` 移除（in-flight 死亡的跳过）→ `tab->SetOwner(W)` → 目标 `tabs_` 镜像 UI 侧位置（anchored 锚点插入 / 否则追加）→ 目标 `PublishTabSnapshot` + `SetActiveBrowser` → 源 `tabs_` 空 → `NotifyAllBrowsersClosed` + `CloseEmptyWindow`（B 的 shell 关闭走 `OnViewsWindowDestroyed` → manager 计数）。
   - 原子段设计动机：消除「源已移除但 owner 未换」的中间态窗口，期间 Tab 状态回调可能被错误的 owner 丢弃（find_if 不命中即忽略）。撕离路径保留多段交接是建窗所需；合并路径无建窗，可单段。
5. 全程不关闭被移动的 browser，页面状态 / 导航栈 / C# 上下文连续（§9 既有结论）。

**顺序 / 校验要点**：
- drop 时目标窗口可能已关闭 / tabbar 尚未就绪：`AdoptTabForMerge` 全量校验，失败 → 回退为"窗口留在原地"（不吞 tab）。
- 拆除前预校验全部可拆条件（含 `GetHostWindowForView` 归属），使逐个 `RemoveBrowserView` 不可能中途失败留下半拆状态。
- 拖动中目标窗口增删 tab：`beforeId` 缓存可能过期 → 目标侧校验该 browser_id 仍在自身 `view_tabs_`（经 view 取 browser，UI 线程安全），否则按 0 追加。
- 部分收养（个别 Repoint/Register/Add 失败）：失败 tab 经 `CloseBrowser(false)` 释放、Add 失败回滚 `view_tabs_` 登记；成功集**追加**到目标末尾（不锚点插入），`CompleteTabMerge` 以 anchored 标志镜像，保证 `view_tabs_` 与 `tabs_` 顺序一致。
- 落点为源窗口 B 自身：排除（controller 枚举时跳过 dragged hwnd）。
- 落点为原窗口 A：合法（拖出又拖回 = merge 回去）。
- **迁移不变量（实测教训 2026-09-14）**：视图迁出后源 `ViewsWindow::browser_view_` 必须清空（`RemoveBrowserView(allow_active_removal)` 内置）——否则空壳窗默认关闭路径（`CanClose → TryCloseBrowser`）会误杀已迁走的 browser，连锁产生死页签、幽灵窗、进程不退。
- 合并执行经 `CefPostTask` 投递出 wndproc / 事件分发：跨窗口视图手术不可在消息分发内联执行（嵌套消息泵会楔死 views 框架，实测"重叠后卡住"）。

### 6.3 跨窗口迁移

两条路径统一收敛到 §6.2 原语：
1. **拖出后拖入**（主路径）：detach（§6.1）产生 B → `TabDragController` 接管拖动 → 落到目标 tabbar → §6.2 合并。对用户呈现为"直接把 tab 从窗口 A 拖进窗口 W"。
2. **已存在窗口间的直接移动**（无手势入口，预留 API）：与 §6.2 完全相同的 `PrepareTabForMerge` / `AdoptTabForMerge` / `CompleteTabMerge`，仅触发源不同（未来可来自 C#/DSL 命令或右键菜单"移动到窗口…"）。

不引入独立状态机：拖拽会话状态全部由 `TabDragController`（§6.5）持有；无拖环时的窗口间移动是纯 API 调用。

### 6.4 单 tab 窗口语义
- 窗口只剩 1 个 tab 时：拖动该 tab = **移动窗口**（不分离）。分离会导致源窗口清空后原地重建 + 闪烁，无意义（与 Chrome 一致）。
- 实现（已实现 2026-09-14，方式取代原 CSS 方案）：单 tab 拖动走 JS 手势 → `dragwindow` → `TabDragController`（拖动=移窗，落到它窗 tabbar 可合并，三平台一致）；C++ 侧 `view_tabs_.size() <= 1` 拒绝 detach 兜底保留。原"把页签标 draggable 让 OS 移窗"的 CSS 方案不再需要（它无法提供合并落点）。
- 一个 tab 被移走后源窗口 `vector<Tab>` 空 → 关闭源 Window（`RootWindowManager` 计数驱动退出）。（已实现：`OnTabBrowserClosed` 空分支 + `CloseEmptyWindow`；merge 路径复用。）

### 6.5 TabDragController（拖拽会话控制器，M4a 新增，Windows 先行）

**定位**：进程内单例（UI 线程独占），持有一次拖拽会话的完整状态；撕离建窗后接管拖动（替代 M3 的 `StartDetachedMoveLoop`），并在拖动中检测合并落点。

**为什么必须放弃 `SC_MOVE` 模态循环**：`DefWindowProc(SC_MOVE)` 内部是原生模态循环，**不泵 Chromium 任务**——CEF UI 线程排队的任务（含 cefQuery 回调、`ExecuteJavaScript` 的往返）全部滞留到松手之后。指示条可以显示（renderer 是独立进程，出向 IPC 不受阻），但 HTML 回报的 `beforeId` 永远迟到 → 插入位只能靠 C++ 按等宽假设计算，无法接受。参照 Chrome `TabDragController` 的实际做法：**`SetCapture` + 自管理拖环**——鼠标消息进入我们的 wndproc，窗口随 `SetWindowPos` 跟随，且**消息泵正常运行**（非模态），CEF 任务 / renderer / `ExecuteJavaScript` / cefQuery 全部实时可用。

**状态**（共享声明 `tab_drag_controller.h`，各平台实现；UI thread 单例、泄漏式无析构）：
```
TabDragController
  ├─ source_root_ / dragged_browser_   源 root 指针 + 任一内容 browser（仅用于解析原生窗口）
  ├─ dragged_hwnd_ (Win) / 原生窗口句柄 (Mac/Linux 文件域)   被拖窗口
  ├─ grab_offset_          光标相对窗口原点偏移；撕离起步低于 strip 时重锚到条带垂直中线
  ├─ allow_merge_          手势语义：tab 拖动 true / 空白区整窗移动 false（false 时跳过全部悬停检测）
  ├─ target_window_        当前命中目标（ViewsWindow*）
  └─ last_before_id_       目标 HTML 最近回报的插入位（缓存）
```

**生命周期**：
1. `Start(root, browser, allow_merge)`：撕离窗口创建后调用（`CreateDetachedViewsWindow` 末尾 / `OnWindowDragRequested`）；记录 `grab_offset_`、`SetCapture(dragged_hwnd_)`、子类化（subclass）该 hwnd 的 wndproc（保存原 proc，透传一切非拖拽消息）。**光标重锚**（Chrome 式撕离，三平台）：detach 手势需先垂直拖 ≥24px 才触发，接管时光标常已低于 strip 且水平偏移沿用源窗口坐标（新窗的 tab 重排到条带左端）——此时把抓取点重锚为（x=60 CSS px（条带 padding 8 + 最小 tab 宽 90 之半，任何标题长度都在 tab 内）× DPI，y=条带垂直中线）并立即移窗一次；Mac 左下原点故 y = 窗高 − 条高/2。**条带几何必须来自 `GetTabbarHeightDip()`（钉定值）+ 窗口 rect，不可查视图 `GetBoundsInScreen()`——撕离窗是"新生儿"（毫秒级），布局未 settle 时该查询间歇返回整窗（实测 14 次撕离仅 1 次触发）**；成熟窗口（命中检测的对象）则可靠。
2. `WM_MOUSEMOVE`：`SetWindowPos(dragged_hwnd_, cursor - grab_offset_)`；枚举所有 live `ViewsWindow`（复用 `LiveViewsWindows()`，排除自身）。**命中测试**：Windows 上 tabbar browser 的原生 hwnd 覆盖整窗（实测 1692x981），故用 `ViewsWindow::GetTabbarScreenBoundsDip()`（视图屏幕边界，DIP）× `GetDpiForWindow/96` 换算物理矩形再 `PtInRect`；Mac 用 NSView 坐标换算、Linux 用 X11 几何（本就精确）。命中变化时向旧目标推 `__tabbarApi.onDropHover(false, 0)`、向新目标推 `onDropHover(true, relativeX)`（`relativeX = (cursor.x - rect.left) / rect.width`，归一化规避 DPI 换算）。
3. `WM_LBUTTONUP`：`ReleaseCapture` → 有 target 且校验通过 → §6.2 合并（经 `CefPostTask` 投递）；否则就地结束。
4. `WM_CAPTURECHANGED`（Alt-Tab / 系统打断）/ `WM_KEYDOWN(ESC)`（v1）：结束会话不合并，窗口停在当前位置（不做 Chrome 式弹回，后续打磨）。
5. `End`：**先缓存合并输入并重置会话状态，再 `ReleaseCapture()`**——`ReleaseCapture` 会同步派发 `WM_CAPTURECHANGED`，若状态未清，其 `End(false)` 重入会清空合并目标导致拖放合并静默失效（M1_M5 审查发现的实弹 bug）；之后反子类化（`CallWindowProc` 链必须完整归还）、清 indicator。

**HTML 配合接口**（custom_scheme.cc 内置页新增）：
```js
// C++ → HTML：显示/隐藏插入指示条；HTML 用 relativeX 结合自身布局计算插入位
window.__tabbarApi.onDropHover(active, relativeX)
// HTML → C++（cefQuery，仅指示位变化时发送）：回报插入位，controller 缓存
{ "channel":"tabbar", "action":"drophover", "beforeId": 3 }
```
`beforeId` 由 HTML 计算的原因：tab 宽度随标题长度不均，只有 HTML 知道布局；cefQuery 回报为异步，controller 缓存最近值，drop 时校验后使用，从未回报 → 0（追加末尾）。**relativeX 的归一基准（实测教训）**：C++ 按 **tabbar 视图宽度**（= 目标页视口 `window.innerWidth`）归一；HTML 必须按同一基准展开（`px = relativeX × innerWidth`），**不可**按 `#tabs` 元素展开——该元素受行 padding（左 8px/右 140px）与溢出裁剪影响，会系统性偏移约 74px（曾致光标在第 2 个 tab、指示条却在第 1 个前）。

**多 tab 整窗合并（M4b，已实现 2026-09-14）**：Windows 上 tabbar 空白区的原生 drag region 改为 JS 手势（HTML 层 `webkitAppRegion='no-drag'` + mousedown→`dragwindow`），controller 会话覆盖整窗（`OnWindowDragRequested` 接受任意 tab 数），drop 时 `MergeDraggedTabInto` 整块迁移；补偿：空白区双击发 `togglemaximize`（`ViewsWindow::ToggleMaximize`）。已知取舍：Windows 失去空白区拖动的 Aero Snap（Win+方向键仍可用）。**Mac 保留原生 drag region**（平台条件 `isMac`），即 Mac 上整窗合并 v1 不可用，窗口移动保持原生。

**平台差异（M4c-Mac/Linux 均已实现 2026-09-14）**：
- **Mac**：`tab_drag_controller_mac.mm` 用 `NSEvent addLocalMonitorForEventsMatchingMask`（dragged/up/keydown）实现同款非模态拖环：消费 drag 事件、`setFrameOrigin` 跟随、mouseUp 放行保持 renderer 视图跟踪平衡、ESC 取消；落点检测/指示条/合并与 Windows 共用同一套桥与原语。Mac 上撕离跟随、单 tab 拖动=移窗+合并均可用。
- **Linux（X11，JS 喂给式拖环）**：`tab_drag_controller_linux.cc`。X11 无跨窗口捕获 API，但**隐式指针抓取**（button 按下期间）让手势发起页持续收到 mousemove（即使光标在窗外/撕离后仍归属源页面）→ HTML 每帧（16ms 节流）发 `windowdragmove`（仅作触发信号，不带坐标）→ C++ `XQueryPointer` 查物理坐标（规避 DPI 换算跨 IPC）+ `XMoveWindow`（顶层 client window 的 ConfigureRequest，WM 负责移 frame）→ `UpdateDropTarget`（tabbar X window 经 `XTranslateCoordinates`+`XGetGeometry` 取 root 矩形）。结束：mouseup → `windowdragend`（命中即合并）；ESC → `windowdragend {cancel:1}`。已知取舍：JS→IPC→XMove 往返有 ~10-30ms 跟随延迟（Win/Mac 为原生帧率）；Linux 空白区与 Windows 同走 JS 手势（无 drag region），双击最大化同样生效。三平台的合并原语（`MergeDraggedTabInto`/`CompleteTabMerge`）完全共用。

---

## 7. 拖拽手势与 draggable region 语义

- **tab 页签**：多 tab 时标 `no-drag`（JS 捕获 mousedown 做重排 / 撕下：垂直 ≥24px 撕离、水平 ≥10px 乐观重排）；单 tab 拖动同样走 JS 手势 → `dragwindow`（默认 `merge=1`，可拖回合并）——取代早期"标 drag 让 OS 移窗"的 CSS 方案（该方案无法提供合并落点，已废弃）。
- **tabbar 空白区**：多 tab 的 Alloy 窗口（Windows/Linux）改 `no-drag` + JS 手势 → `dragwindow {merge:0}` **整窗只移动、永不合并**（Chrome 语义：仅 tab 手势可合并——2026-09-14 实测修正误合并）；**Chrome style 与非 multitab 窗口保留原生 `drag` region**（它们的 `dragwindow` 请求会被 `SupportsMultipleTabs()` 拒绝，移除 region 会导致无法拖动——实测教训 2026-09-14）；macOS 全部保留原生 region。
- **关闭按钮（x）等交互控件**：即使在 drag 区内也要挖 `no-drag` 小洞（drag 区会吞掉 JS mousedown，同 Chrome）。（已实现：`inCloseBtn` 检查。）
- **撕离窗口的拖动**：M3 v1 用 `SC_MOVE | HTCAPTION` 原生 move-loop（平滑度验证用）；**M4a 替换为 `TabDragController`（`SetCapture` + `SetWindowPos` 自管理拖环，§6.5）**——不用 JS 每帧喂坐标（仍是 C++ 原生路径），同时保证消息泵运行使合并落点检测与 HTML 指示条可用。Chrome 的 `TabDragController` 即此方案。
- 平台差异：`TabDragController` 三平台均已实现（Win：`SetCapture`+wndproc 子类化；Mac：`NSEvent` 本地监视器；Linux：JS 喂给式，见 §6.5）；draggable region 在 Mac/Linux 的 Views 框架行为仍需平台实测（§4.7）。

---

## 8. 通信桥（tabId 寻址，重构 TABBAR_DESIGN §8）

> 当前与后续设计的边界：现有桥通过 DefaultClientHandler 的 tabbar owner 指针定位 ViewsWindow，不是 GetViewForID 查找；导航仍操作窗口唯一内容 browser。本节为未实施的多 tab 协议草案，不是当前可用 API。实施时 HTML/C++ 必须同步更新，不能直接用草案替代现有调用。

### 8.1 正向（HTML tabbar → C++）
- cefQuery 分流位于 `cef_query_handler.cc::HandleTabbarQuery`，经 tabbar owner 定位 `ViewsWindow`，多 tab 操作经 `RequestNewTab` / `RequestTabCommand` / `RequestTabReorder` / `RequestTabDetach` 转发到 `TabbedRootWindowViews`。
- **已实现**（命名沿用全小写 + `id` / `beforeId` 字段 + browser_id 寻址，不采用早期驼峰草案）：
```jsonc
{ "channel":"tabbar", "action":"newtab", "url":"..." }
{ "channel":"tabbar", "action":"closetab",  "id": 3 }
{ "channel":"tabbar", "action":"selecttab", "id": 2 }
{ "channel":"tabbar", "action":"reordertab", "id": 2, "beforeId": 0 }
{ "channel":"tabbar", "action":"detachtab", "id": 3 }
{ "channel":"tabbar", "action":"navigate", "url":"..." }
{ "channel":"tabbar", "action":"back|forward|reload|reload_nocache|stop" }
{ "channel":"tabbar", "action":"resize", "height": 76 }
{ "channel":"tabbar", "action":"ready" }
```
- **M4a/M4b/M4c 新增**（拖拽会话专用）：
```jsonc
{ "channel":"tabbar", "action":"dragwindow", "merge": 0 }   // 拖环启动：tab 手势缺省 merge=1；空白区整窗移动 merge=0（永不合并）
{ "channel":"tabbar", "action":"drophover", "beforeId": 3 }  // 仅当本窗口为当前 drop 目标时被 controller 接受
{ "channel":"tabbar", "action":"togglemaximize" }            // 空白区双击最大化补偿（Win/Linux，M4b）
{ "channel":"tabbar", "action":"windowdragmove" }            // M4c-Linux：JS 喂帧（16ms 节流，仅触发信号不带坐标）
{ "channel":"tabbar", "action":"windowdragend", "cancel": 1 } // M4c-Linux：拖环结束（cancel=1 为 ESC）
```
- 早期草案中的 `mergeTab` **取消**：合并不是 HTML 发起的（拖动期间 HTML 无鼠标事件，见 §6.2），落点检测与合并触发均在 `TabDragController`。
- 普通操作校验 Tab 属于发起窗口；跨窗口操作（merge）由 controller 校验源 / 目标存活与归属。

### 8.2 反向（C++ → HTML tabbar）
沿用 `window.__tabbarApi` 命名空间。已实现：`onTabsChanged(items, selectedId)`（快照推送，`PublishTabSnapshot` → `SetTabSnapshot`）、`onAddressChanged` / `setActiveTitle` / `setActiveUrl` / `onLoadingStateChanged` / `setState` / `reset`。
- **M4a 新增**：`onDropHover(active, relativeX)`——拖拽会话指示条（§6.5）；active=false 时隐藏，active=true 时按 relativeX 显示插入位并以 `drophover` 回报 `beforeId`。
- `onTabState` 单 tab 精确推送仍属后续打磨；快照现含 id / title / url / favicon(PNG data URL) / loading / canGoBack / canGoForward（M5 已实现，2026-09-14）。favicon 语义：**导航时清旧图标**（`Tab::OnSetAddress`，`ReplayState` 不经过该方法故回放不受影响；无图标页不残留上页图标）；已知接受的窄风险：favicon 下载回调晚于导航到达会覆盖新页图标（无导航代次校验，修复需动 delegate 接口，暂缓）。

### 8.3 业务介入
- 纯浏览器控制（导航 / 前进后退 / tab 操作）走 cefQuery（就近、低延迟）。
- 需 C#/DSL 介入的（每 tab 绑定 agent 上下文、鉴权、持久化）走既有 HostCLR / DSL API 链路，不与 tabbar cefQuery 混用。

---

## 9. 生命周期与 RootWindowManager 集成

- 每个 Window（含撕离新建的）都是 `RootWindow`，接入 `RootWindowManager` 的 `set<RootWindow>`；退出由 `MaybeCleanup`（`root_windows_.empty() && other_browser_ct_==0`）驱动，无需另造退出逻辑。
- **菜单 Exit / 窗口 X 语义（Chrome 对齐，2026-09-14 实测定稿）**：Exit = 对全部窗口发优雅关闭（`CloseAllWindows(false)`，每窗经 tab 接管链关完所有 tab）→ 空窗自关 → `MaybeCleanup` 退进程；窗口 X = 仅本窗走同链路，其它窗不动。**Exit 不可用 force**：强制关闭跳过 `CanClose` 即跳过浏览器关闭，窗口没了进程还在（实测）。OS 级 force 由 `OnCloseRequested(force)` 早退兜底（不降级为关单 tab）。
- Tab 关闭：请求关闭 CefBrowser，处理 beforeunload 可能取消关闭的情况；收到最终关闭回调后再完成注册表移除、delegate 解绑和释放。不能将请求关闭等同于同步销毁；窗口清空后的关闭也需与此流程协调。
- 迁移目标：不销毁 browser（reparent 而非重建），保持页面状态、导航栈和 C# 上下文连续；在 M3/M4 验证，不作为现有能力保证。
- DEVTOOLS / DIALOG 不纳入本模型。普通 window.open 在现有 Views 路径可归为 NORMAL，不能笼统排除所有 popup；实施时需明确其建窗、建 tab 与 opener 关闭策略，不改变本模型以外的窗口路径。

---

## 10. 分阶段实施计划

> 前置：`TABBAR_DESIGN.md` 阶段 0-5（单 browser 外壳 + 桥）需先构建验证通过。本计划在其之上推进。每阶段可独立构建验证。

| 阶段 | 内容 | 目标 / 验证点 |
|------|------|---------------|
| M1（已实现） | 引入 `Tab` + `TabbedRootWindowViews`，打破 `RootWindowViews` 单 browser 假设；单 tab 跑通（1 个 Window 1 个 Tab） | 现有单页行为不变；DCHECK 不触发；C# 回调正常 |
| M2（已实现） | 单窗口多 tab：`newtab`/`closetab`/`selecttab`/`reordertab`（cefQuery，browser_id 寻址）+ N 个 browser + SetVisible 切换 + 反向状态推送 | 可建多 tab、切换、关闭、重排（含鼠标拖拽重排）；页签标题 / URL / 加载态同步 |
| M3（已实现，待构建回归） | 撕离：tab 拖出成独立窗口（view reparent + 新 Window + Tab 所属 Window 更新 + `SC_MOVE` move-loop） | 拖出成独立窗口、继续渲染、可独立关闭；拖动平滑 |
| M4a | **合并**：`TabDragController`（`SetCapture` 自管理拖环替代 `SC_MOVE`）+ 落点检测 + `onDropHover`/`drophover` 指示条 + `PrepareTabForMerge`/`AdoptTabForMerge`/`CompleteTabMerge` 原语 | 撕离窗口拖入另一窗口（含拖回原窗口）指定位置合并；源窗口空自动关闭；状态连续不重载；指示条实时 |
| M4b（已实现 2026-09-14） | 多 tab 整窗合并（拖动多 tab 窗口落到它窗 tabbar 合并全部 tab）：Windows/Linux 走 JS 手势 + controller（空白区 drag region 改 no-drag，双击发 togglemaximize 补偿）；Mac 保留原生 drag region，整窗合并不做 | 拖整窗到它窗 tabbar 合并全部 tab；源窗关闭、原 active 保持 |
| M4c（已实现 2026-09-14，待各平台回归） | Mac（NSEvent 本地监视器）/ Linux（X11 JS 喂给式拖环）的 `TabDragController` 等价物，§6.5 | 各平台撕离跟随、单 tab 拖动=移窗+合并可用；Linux 跟随有 10-30ms IPC 延迟 |
| M5（已实现 2026-09-14，待回归） | 收尾：favicon / title / active 精确同步（快照含 PNG data URL favicon）、单 tab 拖动=移窗语义（dragwindow）、中键关闭；tab discarding 仅预留位置，Mac/Linux draggable 待平台构建联调 | favicon/中键可见；单 tab 拖动移窗；边界稳态 |

顺序原则：先立骨架（M1）→ 单窗口多 tab 可见（M2）→ 撕离（M3）→ 合并（M4）→ 打磨与跨平台（M5）。tab discarding、书签功能在 M5 之后单独立项。

---

## 11. 待决 / 风险

- **迁移归属的正确性**：内容 handler delegate 保持指向 Tab，迁移时更新 Tab 所属 Window，同时检查 BrowserView 窗口回调绑定。不能将状态推给旧窗口或在移除 view 时误关闭 browser；这是 M3/M4 的首要验证点。（M3 已按此实现，待回归。）
- **子类化 CEF 内部 hwnd**（M4a）：`TabDragController` 需 subclass 被拖窗口的顶层 hwnd（`GWLP_WNDPROC`）。必须完整透传 `CallWindowProc` 原链、正确处理反子类化时机（`WM_CAPTURECHANGED` / 窗口销毁）；CEF 内部 wndproc 无公开契约，升级 CEF（如 152）后需回归。备选：改用 `SetWindowsHookEx(WH_MOUSE)` + 仅对被拖 hwnd 过滤，侵入更小但粒度更粗。
- **拖环期间的消息泵语义**：`SetCapture` 拖环中 CEF 任务正常运行是 `ExecuteJavaScript` / cefQuery 往返的前提，需实测（指示条实时性、`drophover` 回报延迟）。若个别消息路径仍被吞，`beforeId` 兜底 0（追加末尾）保证功能不失效。
- **drop 竞态**：目标窗口在 drop 前关闭 / tabbar 未就绪 / `beforeId` 过期（拖动中目标增删 tab）——`AdoptTabForMerge` 全量校验 + 失败回退"窗口留在原地"；`beforeId` 校验失败按 0 追加。
- **原生窗口拖动平滑度**（M3）：reparent 后接管拖动的时机与手感（M4a 换为 `SetWindowPos` 逐帧跟随，Chrome 同款，需实测 WM_MOUSEMOVE 频率下的开销）。
- **tabbar 资源开销**：每窗口新增一个辅助 browser，但不保证一对一新增 renderer 进程。进程数和内存需实测；不为未验证的开销直接改变全局进程隔离策略。
- **内存随 tab 数增长**：SetVisible 保活策略下 browser 不销毁；tab discarding 留作后续。
- **Mac/Linux**：`TabDragController` 代码已实现（§6.5）但未在真实平台构建回归；draggable region 行为（4.7）待实测。
- **构建环境**：本地无法编译，全部改动须用户构建验证；保持零新增警告 / 错误基线。

---

## 附录：关键源码位置速查（撰写时快照）

| 符号 / 功能 | 位置 |
|-------------|------|
| `ClientHandler::Delegate`（状态回调不带 browser） | `client_handler.h:62-82` |
| `Tab`（每 tab delegate + `SetOwner` 迁移重指 + `ReplayState`） | `tab.h` / `tab.cc` |
| `TabbedRootWindowViews`（`tabs_` / `view_tabs_` 双注册表） | `tabbed_root_window_views.h` / `.cc` |
| 撕离链路（`OnTabDetachRequested` / `BeginDetachOnMain` / `InitDetached` / `ContinueDetachedInit` / `CreateDetachedViewsWindow` / `FinishDetachedAdoption` / `AbortDetachedWindow`） | `tabbed_root_window_views.cc` |
| `ViewsWindow::CreateForExistingView`（接纳已存在 view 建窗）/ `AddBrowserView` / `RemoveBrowserView` / `SetActiveBrowserView` | `views_window.cc` |
| 每 tab 视图包装 delegate（`ContentBrowserViewDelegate` / `GetContentDelegate` / `RepointViewOwner` / `GetHostWindowForView`） | `views_window.cc` |
| `RootWindow::Delegate::CreateDetachedWindow` / `RootWindowManager::CreateDetachedWindow` | `root_window.h` / `root_window_manager.cc` |
| `StartDetachedMoveLoop`（M3 v1 的 `SC_MOVE` 拖动，M4a 由 `TabDragController` 替代） | 已删除（M4a） |
| `TabDragController`（共享声明 `tab_drag_controller.h`；Win `tab_drag_controller_win.cc` / Mac `tab_drag_controller_mac.mm` / Linux `tab_drag_controller_linux.cc`） | 已实现（2026-09-14） |
| `PrepareTabForMerge` / `AdoptTabForMerge` / `CompleteTabMerge`（M4a 合并原语） | 已实现（源/目标侧收敛为 `MergeDraggedTabInto` + `CompleteTabMerge`，`tabbed_root_window_views.cc`） |
| cefQuery 分流（`newtab`/`closetab`/`selecttab`/`reordertab`/`detachtab`/`dragwindow`/`drophover`/`togglemaximize`/`windowdragmove`/`windowdragend`） | `cef_query_handler.cc` `HandleTabbarQuery` |
| tabbar 内置页（鼠标拖拽手势 / `onTabsChanged` / `onDropHover`(M4a)） | `custom_scheme.cc` |
| `RootWindowManager` 退出判定 | `root_window_manager.cc` `MaybeCleanup` |
| CefBrowserView 跨窗口转移范例 | `views_overlay_browser.cc`（`CreateTopLevelWindow` / `AddChildView` / `RemoveChildView`） |
| draggable region 链路 | `cef_drag_handler.h:76` / `root_window_views.cc:463-475` / `cef_window.h:316` |
| message router（renderer/browser） | `client_renderer.cc:209` / `base_client_handler.cc:124` |
| cefQuery handler 注册 | `test_runner.cc`（`CreateMessageHandlers`）/ `cef_query_handler.cc` |
| tabbar view 创建 / 布局 | `views_window.cc`（`tabbar_view_` / cross-axis STRETCH） |
