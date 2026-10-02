# Chrome runtime: CHECK crash in `TabModel::IsSelected()` when creating a new tab (Chromium 154 / branch 8037)

## Environment

- CEF branch 8037 @ `564dd6c4aaff`, Chromium 154.0.8037.58 (src @ `a654841425914`, 2026-09-21)
- macOS arm64, debug build (the failing check is a hard `CHECK`, so release builds are affected as well)
- Chrome runtime with chrome-style window (tab strip visible)

## Steps to reproduce

1. Create a chrome-style browser window (chrome runtime) with at least one tab.
2. Click the "+" (new tab) button on the tab strip.

## Result

Immediate `EXC_BAD_INSTRUCTION` (SIGILL) in the browser process from `CHECK(ContainsIndex(index))` — the window appears frozen. Crash report:

```
#9   logging::CheckNoreturnError::Check
#11  TabStripModel::IsTabSelected(int)::$_0::operator()() const
#12  tabs_api::TabStripModelAdapterImpl::GetTabStates(...)
#13  tabs_api::events::ToEvent(TabStripModelAdapter const&, tabs::TabInterface*, TabChangeType)
#14  tabs_api::tab_strip_model::TabStripModelEventBridge::OnTabChangedAt(tabs::TabInterface*, TabChangeType)
#15  TabStripModel::NotifyTabChanged(tabs::TabInterface*, TabChangeType)
#16  BrowserUiController::NotifyTabUIChanged(tabs::TabInterface*, TabChangeType)
#17  BrowserUiController::ScheduleUIUpdate(content::WebContents*, unsigned int)
#18  BrowserWebContentsDelegate::LoadingStateChanged(content::WebContents*, bool)
#20  content::WebContentsImpl::LoadingStateChanged(content::LoadingState)
#23  content::FrameTreeNode::TakeNavigationRequest(...)
#26  content::NavigationControllerImpl::LoadURLWithParams(...)
#27  internal::NavigateImpl(NavigateParams*, ...)
#29  chrome::AddAndReturnTabAt(BrowserWindowInterface*, GURL const&, ...)
#30  chrome::NewTab(BrowserWindowInterface*, NewTabTypes)
#31  BrowserTabStripController::CreateNewTab(NewTabTypes)
#32  TabStrip::NewTabButtonPressed(ui::Event const&)
```

## Root cause analysis

Four factors combine:

1. CEF attaches the browser's `BrowserWebContentsDelegate` to a new WebContents **before** the tab is inserted into the `TabStripModel` (`ChromeBrowserDelegate::OnWebContentsCreated()` in `libcef/browser/chrome/chrome_browser_delegate.cc` — this is required so CEF receives `LoadingStateChanged` during initial navigation). Upstream Chrome only attaches the delegate during insertion.

2. Upstream `internal::NavigateImpl()` navigates the new tab **before** inserting it: `LoadURLWithParams()` (`chrome/browser/ui/navigator/browser_navigator.cc:929`) runs synchronously and fires `LoadingStateChanged` → `ScheduleUIUpdate(INVALIDATE_TYPE_LOAD)` before `TabStripModel::AddTab()` (`browser_navigator.cc:994`).

3. Because the delegate is attached, the synchronous load-state notification reaches `BrowserUiController::ScheduleUIUpdate()`, which calls `NotifyTabUIChanged()` for a tab whose index in the model is still `kNoTab`.

4. Since upstream CL `b505afe3b7dbc` ("Remove the index parameter from TabStripModelObserver::OnTabChangedAt", 2026-08-04), the TabStrip API event bridge (`TabStripModelEventBridge::OnTabChangedAt` → `events::ToEvent` → `TabStripModelAdapterImpl::GetTabStates` → `TabModel::IsSelected()`) recomputes the index internally. `TabModel::IsSelected()` (`chrome/browser/ui/tabs/tab_model.cc:250`) passes the result of `GetIndexOfTab()` to `IsTabSelected()` without guarding `kNoTab`, and `IsTabSelected()` starts with `CHECK(ContainsIndex(index))` → crash with index `-1`.

## Suggested fix

We carry the following locally in `chrome_browser_browser.patch`: guard the synchronous load-state notification so it is only sent for tabs that are already in the model.

```diff
--- a/chrome/browser/ui/browser_ui_controller/browser_ui_controller.cc
+++ b/chrome/browser/ui/browser_ui_controller/browser_ui_controller.cc
@@ void BrowserUiController::ScheduleUIUpdate(content::WebContents* source,
     if (changed_flags & content::INVALIDATE_TYPE_LOAD) {
       // Update the loading state synchronously. This is so the throbber will
       // immediately start/stop, which gives a more snappy feel. We want to do
       // this for any tab so they start & stop quickly.
-      NotifyTabUIChanged(tab, TabChangeType::kLoadingOnly);
+      // CEF attaches the WebContentsDelegate before the tab is inserted into
+      // the model (ChromeBrowserDelegate::OnWebContentsCreated), so
+      // LoadingStateChanged can fire synchronously for a tab that the
+      // TabStripModel does not contain yet.
+      if (tab_strip_model_->GetIndexOfTab(tab) != TabStripModel::kNoTab) {
+        NotifyTabUIChanged(tab, TabChangeType::kLoadingOnly);
+      }
```

An alternative (arguably more robust) fix would be guarding `TabModel::IsSelected()` to return `false` when `GetIndexOfTab()` returns `kNoTab`, which would protect all `NotifyTabChanged` observers against any current or future path that synchronously notifies about a tab that is not yet in the model. We carry both guards locally (the `ScheduleUIUpdate` guard in `chrome_browser_browser.patch`, and the `IsSelected` guard as a standalone patch):

```diff
--- a/chrome/browser/ui/tabs/tab_model.cc
+++ b/chrome/browser/ui/tabs/tab_model.cc
@@ bool TabModel::IsSelected() const {
   TabStripModel* tab_strip = GetModelForTabInterface();
   const int index = tab_strip->GetIndexOfTab(this);
+  // The tab may not have been inserted into the tab strip model yet (e.g.
+  // when LoadingStateChanged fires during chrome::Navigate before
+  // TabStripModel::AddTab, which is reachable in CEF because the browser
+  // WebContentsDelegate is attached before insertion; see
+  // ChromeBrowserDelegate::OnWebContentsCreated). A tab that is not in the
+  // model is by definition not selected. Mirrors the guard in IsActivated().
+  if (index == TabStripModel::kNoTab) {
+    return false;
+  }
   return GetModelForTabInterface()->IsTabSelected(index);
 }
```

Note that `IsActivated()` in the same file already guards `owning_model_` for the same reason, so the missing `kNoTab` guard in `IsSelected()` looks like an upstream oversight.

Note: the same notification path is also taken during initial tab insertion (browser creation with a URL), so the guard covers that case as well.
