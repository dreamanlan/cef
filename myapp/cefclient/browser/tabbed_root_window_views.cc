// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/tabbed_root_window_views.h"

#include <algorithm>
#include <utility>

#if defined(OS_WIN)
#include <windows.h>
#endif

#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "myapp/cefclient/browser/tab.h"
#include "myapp/cefclient/browser/tab_drag_controller.h"
#include "myapp/cefclient/hostclr/HostCLR.h"

namespace client {

TabbedRootWindowViews::TabbedRootWindowViews(bool use_alloy_style)
    : RootWindowViews(use_alloy_style) {}

TabbedRootWindowViews::~TabbedRootWindowViews() {
  REQUIRE_MAIN_THREAD();
}

CefRefPtr<ClientHandler> TabbedRootWindowViews::CreateContentClientHandler(
    bool with_controls, const std::string& url) {
  DCHECK(!tab_);
  with_controls_ = with_controls;
  tab_ = std::make_shared<Tab>(this, with_controls, url);
  tabs_.push_back(tab_);
  return tab_->GetClientHandler();
}

bool TabbedRootWindowViews::HasBrowser(int browser_id) const {
  REQUIRE_MAIN_THREAD();
  for (const auto& tab : tabs_) {
    auto browser = tab->GetBrowser();
    if (browser && browser->GetIdentifier() == browser_id) {
      return true;
    }
  }
  return false;
}

void TabbedRootWindowViews::OnTabBrowserCreated(
    Tab* tab, CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();
  const auto it = std::find_if(
      tabs_.begin(), tabs_.end(),
      [tab](const std::shared_ptr<Tab>& entry) { return entry.get() == tab; });
  DCHECK(it != tabs_.end());
  if (it == tabs_.end()) {
    return;
  }
  // Only seed the compatibility browser. New tabs must be activated explicitly.
  if (!GetBrowser()) {
    SetActiveBrowser(browser);
  }
  PublishTabSnapshot();
}

void TabbedRootWindowViews::OnTabBrowserClosed(
    Tab* tab, CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();
  const auto it = std::find_if(
      tabs_.begin(), tabs_.end(),
      [tab](const std::shared_ptr<Tab>& entry) { return entry.get() == tab; });
  DCHECK(it != tabs_.end());
  if (it == tabs_.end()) {
    return;
  }
  auto active_browser = GetBrowser();
  if (active_browser && active_browser->IsSame(browser)) {
    SetActiveBrowser(nullptr);
  }
  // Tab already detached its handler and retains itself through this callback.
  // Never invoke the base single-browser teardown for an individual tab.
  tabs_.erase(it);
  PublishTabSnapshot();
  if (tabs_.empty()) {
    if (!IsWindowCreated()) {
      // An aborted detached adoption never created its window shell.
      NotifyWindowlessTeardown();
      detach_keep_alive_ = nullptr;
    }
    CefPostTask(TID_UI,
                base::BindOnce(&TabbedRootWindowViews::CloseEmptyWindow, this));
    NotifyAllBrowsersClosed();
  }
}

bool TabbedRootWindowViews::OnTabSnapshotRequested() {
  CEF_REQUIRE_UI_THREAD();
  if (!GetViewsWindow() || all_browsers_closed_) {
    return false;
  }
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::PublishTabSnapshot, this));
  return true;
}

void TabbedRootWindowViews::OnTabStateChanged(Tab* tab) {
  REQUIRE_MAIN_THREAD();
  const auto it = std::find_if(
      tabs_.begin(), tabs_.end(),
      [tab](const std::shared_ptr<Tab>& entry) { return entry.get() == tab; });
  if (it != tabs_.end()) {
    PublishTabSnapshot();
  }
}

void TabbedRootWindowViews::PublishTabSnapshot() {
  REQUIRE_MAIN_THREAD();
  auto list = CefListValue::Create();
  size_t index = 0;
  for (const auto& tab : tabs_) {
    auto browser = tab->GetBrowser();
    if (!browser) {
      continue;
    }
    const auto& state = tab->GetState();
    auto item = CefDictionaryValue::Create();
    item->SetInt("id", browser->GetIdentifier());
    item->SetString("title", state.title);
    item->SetString("url", state.url);
    // Serialized favicon (PNG data URL); empty when none was downloaded.
    item->SetString("favicon", state.favicon_data_url);
    item->SetBool("loading", state.is_loading);
    item->SetBool("canGoBack", state.can_go_back);
    item->SetBool("canGoForward", state.can_go_forward);
    list->SetDictionary(index++, item);
  }
  auto value = CefValue::Create();
  value->SetList(list);
  const std::string json = CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
  CefPostTask(TID_UI, base::BindOnce(
      &TabbedRootWindowViews::DeliverTabSnapshot, this, json));
}

void TabbedRootWindowViews::DeliverTabSnapshot(const std::string& json) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (window) {
    window->SetTabSnapshot(json);
  }
}

bool TabbedRootWindowViews::OnNewTabRequested(const std::string& url) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (!window || !window->SupportsMultipleTabs() || all_browsers_closed_) {
    return false;
  }
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::CreateNewTab, this,
      url.empty() ? std::string("about:blank") : url, new_tab_generation_));
  return true;
}

void TabbedRootWindowViews::CreateNewTab(const std::string& url,
                                        unsigned int generation) {
  REQUIRE_MAIN_THREAD();
  // An empty registry has already authorized permanent browser-side teardown.
  if (tabs_.empty()) {
    return;
  }
  auto tab = std::make_shared<Tab>(this, with_controls_, url);
  tabs_.push_back(tab);
  // Match the initial-tab path (RootWindowViews::CreateClientHandler), which
  // enables favicon downloads for the tab bar snapshot.
  if (tab->GetClientHandler()) {
    tab->GetClientHandler()->set_download_favicon_images(true);
  }
  // Keep a main-thread reference if task submission itself fails.
  if (!CefPostTask(TID_UI,
                   base::BindOnce(&TabbedRootWindowViews::AttachNewTab, this,
                                  tab, url, generation))) {
    FinishNewTab(std::move(tab), nullptr, false);
  }
}

void TabbedRootWindowViews::AttachNewTab(std::shared_ptr<Tab> tab,
                                        const std::string& url,
                                        unsigned int generation) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  CefRefPtr<CefBrowserView> view;
  CefRefPtr<CefBrowser> browser;
  CefRefPtr<CefBrowser> active_browser;
  bool attached = false;
  bool activated = false;
  if (window && window->SupportsMultipleTabs() && !all_browsers_closed_ &&
      generation == new_tab_generation_) {
    view = window->CreateTabBrowserView(tab->GetClientHandler(), url);
    if (view && RegisterBrowserView(tab, view)) {
      // Creation and view callbacks can reenter window teardown.
      if (window == GetViewsWindow() && !all_browsers_closed_ &&
          generation == new_tab_generation_) {
        attached = window->AddBrowserView(view);
      }
      browser = view->GetBrowser();
      if (attached && browser && window == GetViewsWindow() &&
          !all_browsers_closed_ && generation == new_tab_generation_) {
        activated = window->SetActiveBrowserView(view);
      }
      if (activated) {
        active_browser = browser;
      }
    }
  }

  if (view && !activated) {
    // A created browser remains registered until its final close callback.
    // Attached views use the normal approved-close coordinator. Detached
    // views must release their association even if AddBrowserView did so.
    if (browser) {
      browser->GetHost()->CloseBrowser(true);
      if (!view->GetParentView()) {
        OnBrowserViewDestroyed(view);
      }
    } else {
      if (attached && window == GetViewsWindow()) {
        const bool removed = window->RemoveBrowserView(view);
        DCHECK(removed);
      }
      OnBrowserViewDestroyed(view);
    }
  }

  // Release the final local view before handing the Tab back to main. View
  // destruction may enqueue browser callbacks that must precede finalization.
  const bool browser_created = browser != nullptr;
  view = nullptr;
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::FinishNewTab, this, std::move(tab),
      active_browser, browser_created));
}

void TabbedRootWindowViews::FinishNewTab(
    std::shared_ptr<Tab> tab,
    CefRefPtr<CefBrowser> active_browser,
    bool browser_created) {
  REQUIRE_MAIN_THREAD();
  const auto it = std::find(tabs_.begin(), tabs_.end(), tab);
  // A synchronous or queued terminal callback may already have removed it.
  if (it == tabs_.end()) {
    return;
  }
  if (active_browser) {
    UpdateActiveTab(active_browser);
    return;
  }
  if (browser_created || tab->GetBrowser()) {
    return;
  }

  // No browser was published: removing the pending registration and releasing
  // this function's reference on main safely detaches the unused handler.
  tabs_.erase(it);
  if (tabs_.empty()) {
    // The last real browser closed while this pending creation was in flight.
    CefPostTask(TID_UI,
                base::BindOnce(&TabbedRootWindowViews::CloseEmptyWindow, this));
    NotifyAllBrowsersClosed();
  }
}

bool TabbedRootWindowViews::OnTabCommandRequested(
    const std::string& action, int browser_id) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (!window || !window->SupportsMultipleTabs() || all_browsers_closed_ ||
      browser_id <= 0 ||
      (action != "closetab" && action != "selecttab")) {
    return false;
  }

  // Only this window's associated content views are addressable. Never use
  // a process-wide browser lookup for ordinary tab commands.
  CefRefPtr<CefBrowserView> target_view;
  CefRefPtr<CefBrowser> target_browser;
  for (const auto& tab : view_tabs_) {
    auto view = tab->GetBrowserView();
    auto browser = view ? view->GetBrowser() : nullptr;
    if (browser && browser->IsValid() &&
        browser->GetIdentifier() == browser_id) {
      target_view = view;
      target_browser = browser;
      break;
    }
  }
  if (!target_browser) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] command target not found: action=%s browser_id=%d "
              "view_tabs=%d",
              action.c_str(), browser_id,
              static_cast<int>(view_tabs_.size()));
    return false;
  }
  if (approved_close_ids_.count(browser_id)) {
    return action == "closetab";
  }
  if (action == "closetab") {
    // Keep both registries intact until the actual close lifecycle advances.
    // In particular, beforeunload cancellation must leave the tab available.
    target_browser->GetHost()->CloseBrowser(false);
    return true;
  }
  if (!window->SetActiveBrowserView(target_view)) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] selecttab failed (active switch): browser_id=%d",
              browser_id);
    return false;
  }
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::UpdateActiveTab, this, target_browser));
  return true;
}

bool TabbedRootWindowViews::OnTabReorderRequested(int browser_id,
                                                int before_id) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (!window || !window->SupportsMultipleTabs() || all_browsers_closed_ ||
      browser_id <= 0 || before_id < 0 ||
      approved_close_ids_.count(browser_id) ||
      (before_id && approved_close_ids_.count(before_id))) {
    return false;
  }
  auto source = view_tabs_.end();
  auto anchor = view_tabs_.end();
  for (auto it = view_tabs_.begin(); it != view_tabs_.end(); ++it) {
    auto view = (*it)->GetBrowserView();
    auto browser = view ? view->GetBrowser() : nullptr;
    if (!browser || !browser->IsValid()) {
      continue;
    }
    const int id = browser->GetIdentifier();
    if (id == browser_id) {
      source = it;
    }
    if (id == before_id) {
      anchor = it;
    }
  }
  // Both identities must still belong to this window at execution time.
  if (source == view_tabs_.end() ||
      (before_id && anchor == view_tabs_.end())) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] reorder target not found: id=%d before_id=%d "
              "view_tabs=%d",
              browser_id, before_id, static_cast<int>(view_tabs_.size()));
    return false;
  }
  if (source < anchor) {
    std::rotate(source, source + 1, anchor);
  } else if (anchor < source) {
    std::rotate(anchor, source, source + 1);
  }

  // Reordering retains every UI reference and does not reparent any view.
  // Only stable browser identities cross the thread boundary.
  std::vector<int> browser_ids;
  for (const auto& tab : view_tabs_) {
    auto view = tab->GetBrowserView();
    auto browser = view ? view->GetBrowser() : nullptr;
    if (browser && browser->IsValid()) {
      browser_ids.push_back(browser->GetIdentifier());
    }
  }
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::ApplyTabOrder, this, std::move(browser_ids)));
  return true;
}

void TabbedRootWindowViews::ApplyTabOrder(
    const std::vector<int>& browser_ids) {
  REQUIRE_MAIN_THREAD();
  std::vector<std::shared_ptr<Tab>> ordered;
  for (int browser_id : browser_ids) {
    const auto it = std::find_if(
        tabs_.begin(), tabs_.end(),
        [browser_id](const std::shared_ptr<Tab>& tab) {
          auto browser = tab->GetBrowser();
          return browser && browser->GetIdentifier() == browser_id;
        });
    if (it != tabs_.end()) {
      ordered.push_back(*it);
    }
  }
  // Preserve slots belonging to pending creates and unmatched closing tabs.
  // A tab removed by a final callback is never reinserted from a UI snapshot.
  size_t next = 0;
  for (auto& tab : tabs_) {
    if (std::find(ordered.begin(), ordered.end(), tab) != ordered.end()) {
      tab = ordered[next++];
    }
  }
  PublishTabSnapshot();
}

bool TabbedRootWindowViews::OnWindowDragRequested(bool allow_merge) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (!window || !window->SupportsMultipleTabs() || all_browsers_closed_ ||
      view_tabs_.empty()) {
    printf_log(LOG_SEVERITY_WARNING,
              "[drag] root rejected: window=%d view_tabs=%d closed=%d",
              window ? 1 : 0, static_cast<int>(view_tabs_.size()),
              all_browsers_closed_ ? 1 : 0);
    return false;
  }
  // Any content browser resolves the same top-level shell, so the session
  // covers whole-window drags too (single-tab shells from the tab gesture,
  // multi-tab shells from the strip blank area, M4b). Prefer the active
  // view's browser so the drop activates the tab the user was looking at.
  CefRefPtr<CefBrowser> browser;
  const CefRefPtr<CefBrowserView> active_view = window->GetActiveBrowserView();
  for (const auto& tab : view_tabs_) {
    const CefRefPtr<CefBrowserView> view = tab->GetBrowserView();
    const auto entry_browser = view ? view->GetBrowser() : nullptr;
    if (entry_browser && entry_browser->IsValid() &&
        (!browser || view.get() == active_view.get())) {
      browser = entry_browser;
    }
  }
  if (!browser) {
    return false;
  }
#if defined(OS_WIN) || defined(OS_MAC) || defined(OS_LINUX)
  return TabDragController::GetInstance().Start(this, browser, allow_merge);
#else
  return false;
#endif
}

bool TabbedRootWindowViews::OnTabDetachRequested(int browser_id) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (!window || !window->SupportsMultipleTabs() || all_browsers_closed_ ||
      browser_id <= 0 || approved_close_ids_.count(browser_id)) {
    return false;
  }

  // Resolve the tab and view in this window. Only this window's associated
  // content views are addressable; never use a process-wide browser lookup.
  std::shared_ptr<Tab> tab;
  CefRefPtr<CefBrowserView> view;
  CefRefPtr<CefBrowserView> next_view;
  for (const auto& entry : view_tabs_) {
    auto entry_view = entry->GetBrowserView();
    auto entry_browser = entry_view ? entry_view->GetBrowser() : nullptr;
    if (!entry_browser || !entry_browser->IsValid()) {
      continue;
    }
    if (entry_browser->GetIdentifier() == browser_id) {
      tab = entry;
      view = entry_view;
    } else if (!next_view &&
               !approved_close_ids_.count(entry_browser->GetIdentifier())) {
      next_view = entry_view;
    }
  }
  if (!tab || !view) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] detach target not found: browser_id=%d view_tabs=%d",
              browser_id, static_cast<int>(view_tabs_.size()));
    return false;
  }
  // A window with a single tab moves as a whole; it never detaches.
  if (view_tabs_.size() <= 1U) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] detach rejected: single tab (browser_id=%d)",
              browser_id);
    return false;
  }

  // Switch away from the active view before unparenting it.
  const bool was_active = window->GetActiveBrowserView().get() == view.get();
  if (was_active && (!next_view || !window->SetActiveBrowserView(next_view))) {
    return false;
  }
  // Unparent the content view without closing its browser.
  if (!window->RemoveBrowserView(view)) {
    if (was_active) {
      window->SetActiveBrowserView(view);
    }
    return false;
  }

  // Drop the UI association without releasing the tab; ownership moves to
  // the detached window created on the main thread.
  const auto it = std::find_if(
      view_tabs_.begin(), view_tabs_.end(),
      [&view](const std::shared_ptr<Tab>& entry) {
        return entry->GetBrowserView() == view;
      });
  DCHECK(it != view_tabs_.end());
  if (it != view_tabs_.end()) {
    view_tabs_.erase(it);
  }

  // Size the detached window like the source, offset like a cascade.
  const CefRect source_bounds = window->GetWindowBounds();
  CefRect bounds = source_bounds;
  if (!bounds.IsEmpty()) {
    bounds.x += 20;
    bounds.y += 20;
  }

  if (was_active) {
    CefRefPtr<CefBrowser> next_browser =
        next_view ? next_view->GetBrowser() : nullptr;
    MAIN_POST_CLOSURE(base::BindOnce(
        &TabbedRootWindowViews::UpdateActiveTab, this, next_browser));
  }
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::BeginDetachOnMain, this, tab, bounds));
  return true;
}

void TabbedRootWindowViews::BeginDetachOnMain(std::shared_ptr<Tab> tab,
                                              const CefRect& bounds) {
  REQUIRE_MAIN_THREAD();
  // Abort when the browser closed while the request was in flight; its
  // final callback has already removed the tab from this window.
  if (!tab->GetBrowser()) {
    return;
  }
  const auto it = std::find(tabs_.begin(), tabs_.end(), tab);
  if (it == tabs_.end()) {
    return;
  }
  tabs_.erase(it);
  PublishTabSnapshot();

  // Hand the tab to a new top-level window via the manager.
  auto detached = delegate_->CreateDetachedWindow(tab, bounds);
  if (!detached) {
    // Adoption refused. Close the orphaned browser so its final callback
    // can release the tab; the view is already unparented and unregistered.
    tab->GetBrowser()->GetHost()->CloseBrowser(false);
  }
}

void TabbedRootWindowViews::InitDetached(RootWindow::Delegate* delegate,
                                         std::shared_ptr<Tab> tab,
                                         const CefBrowserSettings& settings,
                                         const CefRect& bounds) {
  REQUIRE_MAIN_THREAD();
  DCHECK(delegate && tab && tab->GetBrowser());
  DCHECK(!initialized_);

  delegate_ = delegate;
  config_ = std::make_unique<RootWindowConfig>(
      CefCommandLine::GetGlobalCommandLine());
  config_->use_views = true;
  config_->use_alloy_style = IsAlloyStyle();
  config_->with_controls = true;
  config_->window_type = WindowType::NORMAL;

  detached_settings_ = settings;
  detached_bounds_ = bounds;

  with_controls_ = config_->with_controls;
  // Adoption keeps the source Tab's handler and reassigns its owner.
  tab_ = tab;
  tabs_.push_back(tab);
  tab->SetOwner(this);
  client_handler_ = tab->GetClientHandler();
  SetActiveBrowser(tab->GetBrowser());
  initialized_ = true;

  // Continue on the UI thread.
  CefPostTask(TID_UI,
              base::BindOnce(&TabbedRootWindowViews::ContinueDetachedInit,
                             this));
}

void TabbedRootWindowViews::ContinueDetachedInit() {
  CEF_REQUIRE_UI_THREAD();
  if (GetViewsWindow()) {
    return;  // Unexpected: a window shell already exists.
  }
  const auto view = tab_ ? tab_->GetBrowserView() : nullptr;
  if (!view || !view->IsValid()) {
    AbortDetachedWindow();
    return;
  }

  SetInitialBounds(detached_bounds_);
  image_cache_ = delegate_->GetImageCache();

  // The shared ImageCache already holds the default window images loaded
  // by the source window; proceed directly to window creation.
  CreateDetachedViewsWindow();
}

void TabbedRootWindowViews::CreateDetachedViewsWindow() {
  CEF_REQUIRE_UI_THREAD();
  const auto view = tab_ ? tab_->GetBrowserView() : nullptr;
  const auto browser = view ? view->GetBrowser() : nullptr;
  if (!view || !view->IsValid() || !browser) {
    AbortDetachedWindow();
    return;
  }

  const auto views_window = ViewsWindow::CreateForExistingView(
      WindowType::NORMAL, this, view, detached_settings_,
      browser->GetHost()->GetRequestContext(),
      CefCommandLine::GetGlobalCommandLine());
  if (!views_window) {
    AbortDetachedWindow();
    return;
  }

  // The re-hosted browser needs a compositor kick to produce its first frame
  // in the new window: the native view moved between top-level widgets, and
  // without a resize/move notification the renderer may not push a new frame,
  // leaving the content area white until an unrelated event repaints it.
  browser->GetHost()->NotifyMoveOrResizeStarted();

  // Replay the adopted tab's cached state into the new window shell.
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::FinishDetachedAdoption, this));
#if defined(OS_WIN) || defined(OS_MAC) || defined(OS_LINUX)
  // Hand the still-held mouse button to the non-modal drag session: the
  // window follows the cursor and can merge into another window's tab bar
  // (MULTITAB_WINDOW_DESIGN.md M4a). Tear-off is a tab gesture: merging is
  // always allowed, and the grab point is always re-anchored onto the strip.
  if (!TabDragController::GetInstance().Start(this, browser,
                                              /*allow_merge=*/true,
                                              /*tear_off=*/true)) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] drag session could not start after detach");
  }
#endif
}

void TabbedRootWindowViews::FinishDetachedAdoption() {
  REQUIRE_MAIN_THREAD();
  const auto browser = tab_ ? tab_->GetBrowser() : nullptr;
  if (!browser) {
    return;  // Closed in flight; the final callback owns cleanup.
  }
  SetActiveBrowser(browser);
  tab_->ReplayState();
  PublishTabSnapshot();
}

bool TabbedRootWindowViews::MergeDraggedTabInto(TabbedRootWindowViews* target_root,
                                                int before_id) {
  CEF_REQUIRE_UI_THREAD();
  printf_log(LOG_SEVERITY_WARNING,
            "[tabs] merge requested: tabs=%d target=%p before_id=%d",
            static_cast<int>(view_tabs_.size()),
            static_cast<void*>(target_root), before_id);
  auto window = GetViewsWindow();
  auto target_window =
      target_root ? target_root->GetViewsWindow() : nullptr;
  if (!window || !window->SupportsMultipleTabs() || all_browsers_closed_ ||
      !target_window || !target_window->SupportsMultipleTabs() ||
      target_root == this || view_tabs_.empty()) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] merge rejected: window=%d windowTabs=%d closed=%d "
              "targetWindow=%d targetTabs=%d self=%d ownTabs=%d",
              window ? 1 : 0,
              window && window->SupportsMultipleTabs() ? 1 : 0,
              all_browsers_closed_ ? 1 : 0, target_window ? 1 : 0,
              target_window && target_window->SupportsMultipleTabs() ? 1 : 0,
              target_root == this ? 1 : 0,
              static_cast<int>(view_tabs_.size()));
    return false;
  }
  // Whole-window merge (M4a single tab + M4b multi tab): every tab of this
  // shell moves, in strip order. Snapshot the membership before touching it.
  const std::vector<std::shared_ptr<Tab>> tabs = view_tabs_;
  const CefRefPtr<CefBrowserView> active_view = window->GetActiveBrowserView();
  std::shared_ptr<Tab> active_tab;
  for (const auto& tab : tabs) {
    const CefRefPtr<CefBrowserView> view = tab->GetBrowserView();
    const auto browser = view ? view->GetBrowser() : nullptr;
    if (!tab || !view || !view->IsValid() || !browser || !browser->IsValid()) {
      printf_log(LOG_SEVERITY_WARNING,
                "[tabs] merge rejected: invalid tab/view/browser");
      return false;
    }
    // Pre-check the removable conditions so the unparent loop below cannot
    // fail halfway and leave a partially disassembled strip (M1_M5 review,
    // merge item 2).
    if (ViewsWindow::GetHostWindowForView(view).get() != window.get()) {
      printf_log(LOG_SEVERITY_WARNING,
                "[tabs] merge rejected: view owner is not this window");
      return false;
    }
    // Prefer the active tab; fall back to the first one.
    if (view.get() == active_view.get() || !active_tab) {
      active_tab = tab;
    }
  }

  // Resolve the drop position against the target's current membership; a
  // stale or missing |before_id| appends at the end. Browser identity comes
  // from the view (UI thread); Tab::GetBrowser() is main-thread only
  // (M1_M5 review, merge item 1).
  size_t anchor_index = target_root->view_tabs_.size();
  if (before_id > 0) {
    for (size_t i = 0; i < target_root->view_tabs_.size(); ++i) {
      const CefRefPtr<CefBrowserView> entry_view =
          target_root->view_tabs_[i]->GetBrowserView();
      const auto entry_browser =
          entry_view ? entry_view->GetBrowser() : nullptr;
      if (entry_browser &&
          entry_browser->GetIdentifier() == before_id) {
        anchor_index = i;
        break;
      }
    }
  }

  // 1. Unparent every view from this shell without closing its browser. The
  // active view is included: the shell is about to close (merge override).
  for (const auto& tab : tabs) {
    if (!window->RemoveBrowserView(tab->GetBrowserView(),
                                   /*allow_active_removal=*/true)) {
      printf_log(LOG_SEVERITY_WARNING,
                "[tabs] merge rejected: RemoveBrowserView failed");
      return false;
    }
  }
  view_tabs_.clear();

  // 2. Adopt on the target: re-point each per-tab delegate, register through
  // the migration branch and attach (hidden) in strip order. A refused
  // adoption releases that browser through its final lifespan callback, as
  // the detach abort path does; CompleteTabMerge only moves adopted tabs.
  // A registration that cannot be attached is rolled back so the target's
  // strip bookkeeping matches what CompleteTabMerge will migrate (M1_M5
  // review, merge item 3).
  std::vector<std::shared_ptr<Tab>> adopted;
  adopted.reserve(tabs.size());
  for (const auto& tab : tabs) {
    const CefRefPtr<CefBrowserView> view = tab->GetBrowserView();
    // A refused adoption force-closes the browser: the source shell is
    // already disassembled, so there is no "cancel the merge" path back, and
    // a graceful close can stall forever on a beforeunload handler (the
    // window then never completes its close, MaybeCleanup never fires and
    // the process refuses to exit).
    // The close of a views-hosted browser is view-driven (see
    // DetachApprovedBrowser): the pending close only completes when the view
    // is destroyed. The Tab still holds that view reference, so it must be
    // dropped here - otherwise the browser, its page and this source window
    // all stay alive forever (the tab's final close callback is what empties
    // tabs_ and closes this shell).
    if (!ViewsWindow::RepointViewOwner(view, target_window.get())) {
      printf_log(LOG_SEVERITY_WARNING,
                "[tabs] merge adoption failed: RepointViewOwner (view=%d)",
                view && view->IsValid() ? 1 : 0);
      view->GetBrowser()->GetHost()->CloseBrowser(true);
      ReleaseViewTab(tab);
      continue;
    }
    if (!target_root->RegisterBrowserView(tab, view)) {
      printf_log(LOG_SEVERITY_WARNING,
                "[tabs] merge adoption failed: RegisterBrowserView");
      // Give the view back to this window so its destruction callbacks run
      // on the window that still owns the tab.
      ViewsWindow::RepointViewOwner(view, window.get());
      view->GetBrowser()->GetHost()->CloseBrowser(true);
      ReleaseViewTab(tab);
      continue;
    }
    if (!target_window->AddBrowserView(view)) {
      printf_log(LOG_SEVERITY_WARNING,
                "[tabs] merge adoption failed: AddBrowserView");
      const auto it = std::find(target_root->view_tabs_.begin(),
                                target_root->view_tabs_.end(), tab);
      if (it != target_root->view_tabs_.end()) {
        target_root->view_tabs_.erase(it);
      }
      ViewsWindow::RepointViewOwner(view, window.get());
      view->GetBrowser()->GetHost()->CloseBrowser(true);
      ReleaseViewTab(tab);
      continue;
    }
    adopted.push_back(tab);
  }
  if (adopted.empty()) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] merge rejected: nothing was adopted");
    return false;
  }
  // The appended block sits at the back; move it in front of the anchor when
  // the whole block was adopted (partial adoption keeps the append order,
  // mirrored by the |anchored| flag in CompleteTabMerge: M1_M5 review,
  // merge item 4).
  const bool anchored = adopted.size() == tabs.size();
  if (anchored &&
      anchor_index + adopted.size() < target_root->view_tabs_.size()) {
    const size_t count = adopted.size();
    const std::vector<std::shared_ptr<Tab>> block(
        target_root->view_tabs_.end() - count,
        target_root->view_tabs_.end());
    target_root->view_tabs_.erase(target_root->view_tabs_.end() - count,
                                  target_root->view_tabs_.end());
    target_root->view_tabs_.insert(
        target_root->view_tabs_.begin() +
            std::min(anchor_index, target_root->view_tabs_.size()),
        block.begin(), block.end());
  }
  // Activate the tab the user was looking at in the dragged window.
  const std::shared_ptr<Tab> focus_tab =
      std::find(adopted.begin(), adopted.end(), active_tab) != adopted.end()
          ? active_tab
          : adopted.back();
  if (!target_window->SetActiveBrowserView(focus_tab->GetBrowserView())) {
    target_window->SetActiveBrowserView(
        target_root->view_tabs_.back()->GetBrowserView());
  }

  // 3. Atomic main-thread tail; both roots stay alive across the hop.
  printf_log(LOG_SEVERITY_WARNING,
            "[tabs] merge ok: adopted=%d/%d anchored=%d before_id=%d",
            static_cast<int>(adopted.size()), static_cast<int>(tabs.size()),
            anchored ? 1 : 0, before_id);
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::CompleteTabMerge, this, adopted, focus_tab,
      scoped_refptr<TabbedRootWindowViews>(target_root), before_id,
      anchored));
  return true;
}

void TabbedRootWindowViews::CompleteTabMerge(
    std::vector<std::shared_ptr<Tab>> tabs,
    std::shared_ptr<Tab> active_tab,
    scoped_refptr<TabbedRootWindowViews> target_root,
    int before_id,
    bool anchored) {
  REQUIRE_MAIN_THREAD();
  if (tabs.empty() || !target_root) {
    return;
  }

  // Source side: drop the moved tabs; a tab whose browser died in flight is
  // left to its final close callback instead of migrating.
  std::vector<std::shared_ptr<Tab>> live;
  live.reserve(tabs.size());
  for (auto& tab : tabs) {
    const auto it = std::find(tabs_.begin(), tabs_.end(), tab);
    if (it != tabs_.end()) {
      tabs_.erase(it);
    }
    if (tab->GetBrowser()) {
      live.push_back(tab);
    }
  }

  // Ownership handoff: state callbacks now report to the target window.
  for (const auto& tab : live) {
    tab->SetOwner(target_root.get());
  }

  // Target side: insert the block before the anchor tab (append when absent
  // or when the UI side could not anchor because of a partial adoption;
  // view_tabs_ and tabs_ must stay in the same order: M1_M5 review, item 4).
  size_t insert_at = target_root->tabs_.size();
  if (anchored && before_id > 0) {
    for (size_t i = 0; i < target_root->tabs_.size(); ++i) {
      const auto entry_browser = target_root->tabs_[i]->GetBrowser();
      if (entry_browser &&
          entry_browser->GetIdentifier() == before_id) {
        insert_at = i;
        break;
      }
    }
  }
  target_root->tabs_.insert(target_root->tabs_.begin() + insert_at,
                            live.begin(), live.end());
  if (active_tab && active_tab->GetBrowser()) {
    target_root->SetActiveBrowser(active_tab->GetBrowser());
  }
  target_root->PublishTabSnapshot();

  // The merge emptied this shell: tear it down through the same path a closed
  // last tab takes, without touching the moved browsers.
  if (tabs_.empty() && !all_browsers_closed_) {
    NotifyAllBrowsersClosed();
    CefPostTask(TID_UI, base::BindOnce(
        &TabbedRootWindowViews::CloseEmptyWindow, this));
  }
}

bool TabbedRootWindowViews::AdoptPopupAsTab(
    CefRefPtr<ViewsWindow> popup_window,
    CefRefPtr<CefBrowserView> popup_view) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (!window || !window->SupportsMultipleTabs() || all_browsers_closed_ ||
      !popup_window || !popup_view || !popup_view->IsValid()) {
    return false;
  }
  // The pre-created popup root owns the bootstrap Tab that is paired with the
  // popup browser's client handler. A Tab cannot be re-created for that
  // browser - it has to be transferred, exactly like a tab merge moves Tabs.
  auto* popup_delegate = popup_window->delegate();
  auto* popup_root =
      popup_delegate ? popup_delegate->AsTabbedRootWindow() : nullptr;
  if (!popup_root || popup_root == this || !popup_root->tab_ ||
      popup_root->tab_->GetBrowserView()) {
    return false;
  }
  const std::shared_ptr<Tab> tab = popup_root->tab_;
  if (!ViewsWindow::RepointViewOwner(popup_view, window.get())) {
    return false;
  }
  if (!RegisterBrowserView(tab, popup_view) ||
      !window->AddBrowserView(popup_view)) {
    // Roll the registration back so the caller can open the popup as a
    // regular window instead.
    const auto it = std::find(view_tabs_.begin(), view_tabs_.end(), tab);
    if (it != view_tabs_.end()) {
      view_tabs_.erase(it);
    }
    if (tab->GetBrowserView() == popup_view) {
      tab->SetBrowserView(nullptr);
    }
    ViewsWindow::RepointViewOwner(popup_view, popup_window.get());
    return false;
  }
  window->SetActiveBrowserView(popup_view);
  // The view moved from no widget into this window's widget; kick the
  // compositor so the first frame is produced (see CreateDetachedViewsWindow).
  if (auto browser = popup_view->GetBrowser()) {
    browser->GetHost()->NotifyMoveOrResizeStarted();
  }
  MAIN_POST_CLOSURE(base::BindOnce(
      &TabbedRootWindowViews::CompletePopupTabAdoption,
      scoped_refptr<TabbedRootWindowViews>(this),
      scoped_refptr<TabbedRootWindowViews>(popup_root), tab,
      popup_view->GetBrowser()));
  printf_log(LOG_SEVERITY_INFO, "[tabs] popup adopted as a new tab");
  return true;
}

void TabbedRootWindowViews::CompletePopupTabAdoption(
    scoped_refptr<TabbedRootWindowViews> popup_root,
    std::shared_ptr<Tab> tab,
    CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();
  if (!popup_root || !tab) {
    return;
  }
  // Detach the bootstrap tab from the popup root's registry.
  const auto it =
      std::find(popup_root->tabs_.begin(), popup_root->tabs_.end(), tab);
  if (it != popup_root->tabs_.end()) {
    popup_root->tabs_.erase(it);
  }
  // Ownership handoff: state callbacks now report to this window.
  tab->SetOwner(this);
  tabs_.push_back(tab);
  if (browser) {
    SetActiveBrowser(browser);
  }
  PublishTabSnapshot();
  // The popup root never created its window shell. Finish its teardown
  // accounting; the browser itself moved here and stays alive.
  popup_root->NotifyWindowlessTeardown();
  popup_root->NotifyAllBrowsersClosed();
}

void TabbedRootWindowViews::AbortDetachedWindow() {
  CEF_REQUIRE_UI_THREAD();
  // The window shell was never created. Keep this object alive on the main
  // thread until the adopted browser's final close callback completes.
  MAIN_POST_CLOSURE(base::BindOnce(
      [](scoped_refptr<TabbedRootWindowViews> self) {
        REQUIRE_MAIN_THREAD();
        self->detach_keep_alive_ = self;
        const auto browser =
            self->tab_ ? self->tab_->GetBrowser() : nullptr;
        if (browser) {
          browser->GetHost()->CloseBrowser(false);
        } else {
          // The final callback already ran; finish the teardown accounting.
          if (self->tabs_.empty()) {
            self->NotifyWindowlessTeardown();
          }
          self->detach_keep_alive_ = nullptr;
        }
      },
      scoped_refptr<TabbedRootWindowViews>(this)));
}

bool TabbedRootWindowViews::OnCloseRequested(bool force) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (force) {
    // A forced close (menu Exit, shutdown) must never degrade to per-tab
    // closes; decline the takeover so the default close path runs.
    return false;
  }
  if (!window || !window->SupportsMultipleTabs() || all_browsers_closed_) {
    return false;
  }

  ++new_tab_generation_;
  // Snapshot browser handles before calling into CEF. Do not hold UI copies of
  // the shared Tab registry across callbacks that can release its entries.
  std::vector<CefRefPtr<CefBrowser>> browsers;
  for (const auto& tab : view_tabs_) {
    auto view = tab->GetBrowserView();
    auto browser = view ? view->GetBrowser() : nullptr;
    if (browser && !approved_close_ids_.count(browser->GetIdentifier())) {
      browsers.push_back(browser);
    }
  }
  if (browsers.empty()) {
    // Nothing left to close ourselves. Decline so CanClose proceeds with the
    // default path (TryCloseBrowser / window teardown) and finishes the
    // already-approved browsers; taking over here would cancel the close
    // notification sent by their completing DoClose (see CefLifeSpanHandler).
    printf_log(LOG_SEVERITY_INFO,
              "[tabs] close requested: force=%d view_tabs=%d closing=0 - "
              "declining, nothing to close",
              force ? 1 : 0, static_cast<int>(view_tabs_.size()));
    return false;
  }
  printf_log(LOG_SEVERITY_INFO,
            "[tabs] close requested: force=%d view_tabs=%d closing=%d",
            force ? 1 : 0, static_cast<int>(view_tabs_.size()),
            static_cast<int>(browsers.size()));
  for (const auto& browser : browsers) {
    browser->GetHost()->CloseBrowser(force);
  }
  // Do not latch requests: a canceled beforeunload must be retryable.
  return true;
}

bool TabbedRootWindowViews::OnTabBrowserCloseApproved(
    Tab* tab, CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  auto window = GetViewsWindow();
  if (!window || !window->SupportsMultipleTabs() || !browser) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] close approval rejected (window state): browser_id=%d",
              browser ? browser->GetIdentifier() : 0);
    return false;
  }
  const auto it = std::find_if(
      view_tabs_.begin(), view_tabs_.end(),
      [tab](const std::shared_ptr<Tab>& entry) { return entry.get() == tab; });
  if (it == view_tabs_.end()) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] close approval rejected (tab not in view_tabs): "
              "browser_id=%d view_tabs=%d",
              browser->GetIdentifier(), static_cast<int>(view_tabs_.size()));
    return false;
  }
  auto view = (*it)->GetBrowserView();
  auto view_browser = view ? view->GetBrowser() : nullptr;
  if (!view_browser || !view_browser->IsSame(browser)) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] close approval rejected (view/browser mismatch): "
              "browser_id=%d",
              browser->GetIdentifier());
    return false;
  }
  if (approved_close_ids_.insert(browser->GetIdentifier()).second) {
    printf_log(LOG_SEVERITY_INFO,
              "[tabs] close approved, detaching view: browser_id=%d",
              browser->GetIdentifier());
    // DoClose must unwind before releasing the view and continuing teardown.
    CefPostTask(
        TID_UI,
        base::BindOnce(&TabbedRootWindowViews::DetachApprovedBrowser, this,
                       browser));
    return true;
  }
  // A repeated DoClose for an already-approved browser must decline so the
  // default close proceeds; this is how the completion CloseBrowser() call
  // in DetachApprovedBrowser unwinds (see CefLifeSpanHandler::DoClose).
  return false;
}

void TabbedRootWindowViews::DetachApprovedBrowser(
    CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  const int browser_id = browser->GetIdentifier();
  printf_log(LOG_SEVERITY_INFO,
            "[tabs] detach approved browser: browser_id=%d", browser_id);
  auto window = GetViewsWindow();
  if (!window || !window->SupportsMultipleTabs()) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] detach approved failed (window state): browser_id=%d",
              browser_id);
    // Complete via the default path; the latched approval makes the next
    // DoClose decline so the browser cannot stall.
    browser->GetHost()->CloseBrowser(true);
    return;
  }

  CefRefPtr<CefBrowserView> closing_view;
  CefRefPtr<CefBrowserView> next_view;
  std::shared_ptr<Tab> closing_tab;
  for (const auto& tab : view_tabs_) {
    auto view = tab->GetBrowserView();
    auto view_browser = view ? view->GetBrowser() : nullptr;
    if (!view_browser) {
      continue;
    }
    if (view_browser->IsSame(browser)) {
      closing_view = view;
      closing_tab = tab;
    } else if (!next_view &&
               !approved_close_ids_.count(view_browser->GetIdentifier())) {
      next_view = view;
    }
  }
  if (!closing_view) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] detach approved failed (closing view not found): "
              "browser_id=%d view_tabs=%d",
              browser_id, static_cast<int>(view_tabs_.size()));
    browser->GetHost()->CloseBrowser(true);
    return;
  }

  const bool was_active = window->GetActiveBrowserView() == closing_view;
  if (was_active && !window->SetActiveBrowserView(next_view)) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] detach approved failed (active switch): browser_id=%d",
              browser_id);
    browser->GetHost()->CloseBrowser(true);
    return;
  }
  if (!window->RemoveBrowserView(closing_view)) {
    printf_log(LOG_SEVERITY_WARNING,
              "[tabs] detach approved failed (remove view): browser_id=%d"
              " - completing close anyway, teardown cleans the view",
              browser_id);
    // Never leave the browser in a partially closed state: a stalled close
    // blocks the whole window. Completing the close destroys the browser and
    // its view, which removes any leftover child from the window.
    browser->GetHost()->CloseBrowser(true);
    return;
  }

  if (was_active) {
    CefRefPtr<CefBrowser> next_browser =
        next_view ? next_view->GetBrowser() : nullptr;
    MAIN_POST_CLOSURE(base::BindOnce(
        &TabbedRootWindowViews::UpdateActiveTab, this, next_browser));
  }
  // RemoveBrowserView does not notify the association owner. Clear it before
  // releasing our final local view reference; actual destruction may reenter.
  OnBrowserViewDestroyed(closing_view);
  closing_view = nullptr;
  // DoClose() returned true for this browser, so the close must not be
  // re-requested: another CloseBrowser() would trigger a second DoClose that
  // declines and sends a close notification, cascading into CanClose and
  // closing every other tab of this window. The browser lifecycle is
  // view-driven instead: dropping the last view reference destroys the view
  // and completes the close (the Tab releases its view reference on the main
  // thread, see ReleaseViewTab). A delayed check on the main-thread Tab
  // state falls back to the explicit completion if the browser unexpectedly
  // survives the view destruction.
  printf_log(LOG_SEVERITY_INFO,
            "[tabs] completing approved close via view release: browser_id=%d",
            browser_id);
  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(
          [](scoped_refptr<TabbedRootWindowViews> self,
             std::shared_ptr<Tab> tab, CefRefPtr<CefBrowser> browser) {
            CEF_REQUIRE_UI_THREAD();
            MAIN_POST_CLOSURE(base::BindOnce(
                [](CefRefPtr<CefBrowser> browser,
                   std::shared_ptr<Tab> tab) {
                  REQUIRE_MAIN_THREAD();
                  // A null Tab browser means the final close callback already
                  // ran; anything else at this point is a stalled close.
                  if (tab && tab->GetBrowser() && browser &&
                      browser->IsValid()) {
                    printf_log(
                        LOG_SEVERITY_WARNING,
                        "[tabs] browser survived view destruction, forcing "
                        "close: browser_id=%d",
                        browser->GetIdentifier());
                    CefPostTask(
                        TID_UI,
                        base::BindOnce(
                            [](CefRefPtr<CefBrowser> browser) {
                              browser->GetHost()->CloseBrowser(true);
                            },
                            browser));
                  }
                },
                browser, tab));
          },
          scoped_refptr<TabbedRootWindowViews>(this), closing_tab, browser),
      500);
}

void TabbedRootWindowViews::UpdateActiveTab(CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();
  if (!browser) {
    SetActiveBrowser(nullptr);
    PublishTabSnapshot();
    return;
  }
  for (const auto& tab : tabs_) {
    auto candidate = tab->GetBrowser();
    if (candidate && candidate->IsSame(browser)) {
      SetActiveBrowser(candidate);
      tab->ReplayState();
      PublishTabSnapshot();
      return;
    }
  }
}

void TabbedRootWindowViews::CloseEmptyWindow() {
  CEF_REQUIRE_UI_THREAD();
  all_browsers_closed_ = true;
  auto window = GetViewsWindow();
  if (window && window->SupportsMultipleTabs()) {
    // Only the main-thread final browser callback authorizes shell teardown.
    window->Close(false);
  }
}

bool TabbedRootWindowViews::RegisterBrowserView(
    const std::shared_ptr<Tab>& tab,
    CefRefPtr<CefBrowserView> browser_view) {
  CEF_REQUIRE_UI_THREAD();
  if (!tab || !browser_view || !browser_view->IsValid()) {
    return false;
  }
  for (const auto& entry : view_tabs_) {
    if (entry->GetBrowserView() == browser_view) {
      return entry == tab;
    }
  }
  if (tab->GetBrowserView()) {
    if (tab->GetBrowserView() != browser_view) {
      return false;
    }
    // A migrating tab keeps its view; register it with this window without
    // re-assigning the tab's view reference.
    view_tabs_.push_back(tab);
    return true;
  }
  view_tabs_.push_back(tab);
  tab->SetBrowserView(browser_view);
  return true;
}

void TabbedRootWindowViews::ReleaseViewTab(std::shared_ptr<Tab> tab) {
  CEF_REQUIRE_UI_THREAD();
  tab->SetBrowserView(nullptr);
  // Keep the owner alive while returning the last UI-held Tab reference.
  // As elsewhere, this handoff assumes an operational main message loop.
  MAIN_POST_CLOSURE(base::BindOnce(
      [](scoped_refptr<TabbedRootWindowViews> owner,
         std::shared_ptr<Tab> tab) {
        REQUIRE_MAIN_THREAD();
        tab.reset();
      },
      scoped_refptr<TabbedRootWindowViews>(this), std::move(tab)));
}

void TabbedRootWindowViews::OnBrowserViewCreated(
    CefRefPtr<CefBrowserView> browser_view) {
  CEF_REQUIRE_UI_THREAD();
  for (const auto& tab : view_tabs_) {
    if (tab->GetBrowserView() == browser_view) {
      return;
    }
  }
  // Only the initial view may use the immutable bootstrap reference.
  // New-tab creation must register its own Tab before AddBrowserView().
  if (!initial_view_associated_) {
    initial_view_associated_ = RegisterBrowserView(tab_, browser_view);
    DCHECK(initial_view_associated_);
    return;
  }
  DCHECK(false) << "Content view was not registered before attachment";
}

void TabbedRootWindowViews::OnBrowserViewDestroyed(
    CefRefPtr<CefBrowserView> browser_view) {
  CEF_REQUIRE_UI_THREAD();
  const auto it = std::find_if(
      view_tabs_.begin(), view_tabs_.end(),
      [&browser_view](const std::shared_ptr<Tab>& tab) {
        return tab->GetBrowserView() == browser_view;
      });
  if (it != view_tabs_.end()) {
    auto tab = std::move(*it);
    view_tabs_.erase(it);
    ReleaseViewTab(std::move(tab));
  }
}

void TabbedRootWindowViews::OnViewsWindowDestroyed(
    CefRefPtr<ViewsWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  ++new_tab_generation_;
  approved_close_ids_.clear();
  // Empty the UI registry before releasing views or posting root destruction.
  std::vector<std::shared_ptr<Tab>> tabs;
  tabs.swap(view_tabs_);
  for (auto& tab : tabs) {
    ReleaseViewTab(std::move(tab));
  }
  RootWindowViews::OnViewsWindowDestroyed(window);
}

}  // namespace client
