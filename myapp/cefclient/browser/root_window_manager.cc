// Copyright (c) 2015 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/root_window_manager.h"

#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_browser.h"
#include "include/cef_id_mappers.h"
#include "include/cef_task_manager.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "myapp/cefclient/browser/default_client_handler.h"
#include "myapp/cefclient/browser/main_context.h"
#include "myapp/cefclient/browser/tab.h"
#include "myapp/cefclient/browser/tabbed_root_window_views.h"
#include "myapp/cefclient/browser/test_runner.h"
#include "myapp/shared/browser/file_util.h"
#include "myapp/shared/browser/resource_util.h"
#include "myapp/shared/common/client_switches.h"
#include "myapp/cefclient/hostclr/HostCLR.h"

namespace client {

namespace {

// Waits used while a hot reload brings the closed windows back. Named because
// the polling helpers of both window modes share them.
constexpr int kHotReloadTabStepDelayMs = 100;      // between two tabs of a window
constexpr int kHotReloadShellPollMs = 200;         // waiting for a window shell
constexpr int kHotReloadSettlePollMs = 300;        // waiting for restored windows
constexpr int kHotReloadMaxWaitMs = 5000;          // give up after this

// Chrome's tab restore service only records pages that have a real navigation
// and skips its own internal UI pages, so the same urls are excluded here when
// counting what a hot reload will be able to bring back. Plain pointers keep
// this a trivially destructible constant.
bool IsRestorableHotReloadUrl(const std::string& url) {
  if (url.empty()) {
    return false;
  }
  static const char* const kInternalPrefixes[] = {
      "chrome://", "chrome-untrusted://", "devtools://", "about:",
      "view-source:", "chrome-extension://", "webagent://"};
  for (const char* prefix : kInternalPrefixes) {
    if (url.rfind(prefix, 0) == 0) {
      return false;
    }
  }
  return true;
}

class ClientRequestContextHandler : public CefRequestContextHandler {
 public:
  using CreateCallback = RootWindow::Delegate::RequestContextCallback;

  explicit ClientRequestContextHandler(CreateCallback callback)
      : create_callback_(std::move(callback)) {}

  ClientRequestContextHandler(const ClientRequestContextHandler&) = delete;
  ClientRequestContextHandler& operator=(const ClientRequestContextHandler&) =
      delete;

  // CefRequestContextHandler methods:
  void OnRequestContextInitialized(
      CefRefPtr<CefRequestContext> request_context) override {
    CEF_REQUIRE_UI_THREAD();

    // Allow the startup URL to create popups that bypass the popup blocker.
    // For example, via Tests > New Popup from the top menu. This applies for
    // for Chrome style only.
    const auto& startup_url =
        MainContext::Get()->GetMainURL(/*command_line=*/nullptr);
    request_context->SetContentSetting(startup_url, startup_url,
                                       CEF_CONTENT_SETTING_TYPE_POPUPS,
                                       CEF_CONTENT_SETTING_VALUE_ALLOW);

    if (!create_callback_.is_null()) {
      // Execute the callback asynchronously.
      CefPostTask(TID_UI,
                  base::BindOnce(std::move(create_callback_), request_context));
    }
  }

 private:
  CreateCallback create_callback_;

  IMPLEMENT_REFCOUNTING(ClientRequestContextHandler);
};

// Ensure a compatible set of window creation attributes.
void SanityCheckWindowConfig(const bool is_devtools,
                             const bool use_views,
                             bool& use_alloy_style,
                             bool& with_osr) {
  // This configuration is not supported by cefclient architecture and
  // should use default window creation instead.
  CHECK(!(is_devtools && !use_views));

  if (is_devtools && use_alloy_style) {
    LOG(WARNING) << "Alloy style is not supported with Chrome runtime DevTools;"
                    " using Chrome style.";
    use_alloy_style = false;
  }

  if (!use_alloy_style && with_osr) {
    LOG(WARNING) << "Windowless rendering is not supported with Chrome style;"
                    " using windowed rendering.";
    with_osr = false;
  }

  if (use_views && with_osr) {
    LOG(WARNING) << "Windowless rendering is not supported with Views;"
                    " using windowed rendering.";
    with_osr = false;
  }
}

}  // namespace

RootWindowManager::RootWindowManager(bool terminate_when_all_windows_closed)
    : terminate_when_all_windows_closed_(terminate_when_all_windows_closed) {
  CefRefPtr<CefCommandLine> command_line =
      CefCommandLine::GetGlobalCommandLine();
  DCHECK(command_line.get());
  request_context_per_browser_ =
      command_line->HasSwitch(switches::kRequestContextPerBrowser);
  request_context_shared_cache_ =
      command_line->HasSwitch(switches::kRequestContextSharedCache);
}

RootWindowManager::~RootWindowManager() {
  // All root windows should already have been destroyed.
  DCHECK(root_windows_.empty());
}

scoped_refptr<RootWindow> RootWindowManager::CreateRootWindow(
    std::unique_ptr<RootWindowConfig> config) {
  CefBrowserSettings settings;
  MainContext::Get()->PopulateBrowserSettings(&settings);

  SanityCheckWindowConfig(/*is_devtools=*/false, config->use_views,
                          config->use_alloy_style, config->with_osr);

  scoped_refptr<RootWindow> root_window =
      RootWindow::Create(config->use_views, config->use_alloy_style,
                         config->window_type);
  root_window->Init(this, std::move(config), settings);

  // Store a reference to the root window on the main thread.
  OnRootWindowCreated(root_window);

  return root_window;
}

void RootWindowManager::CreateChromeWindow(const std::string& url) {
  CreateChromeWindow(url, base::OnceCallback<void(CefRefPtr<CefBrowser>)>());
}

void RootWindowManager::CreateChromeWindow(
    const std::string& url,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> created_callback) {
  // Host a Chrome-style browser in a Chrome self-created native window. No
  // parent + Chrome runtime style yields a fully styled Chrome UI (tabstrip)
  // top-level window (mirrors cefsimple's --use-native path). The browser has
  // no client RootWindow; DefaultClientHandler joins the C# browser-query
  // system and is tracked as an "other browser", so termination is driven by
  // the same counter (other_browser_ct_) as default Chrome UI windows.
  // CefBrowserHost::CreateBrowser may be called on any browser process thread,
  // and the browser only exists once Chrome reports it through
  // OtherBrowserCreated - hence |created_callback| instead of a return value.
  CefBrowserSettings settings;
  MainContext::Get()->PopulateBrowserSettings(&settings);

  CefWindowInfo window_info;
#if defined(OS_WIN)
  window_info.SetAsPopup(nullptr, "webagent");
#endif
  window_info.runtime_style = CEF_RUNTIME_STYLE_CHROME;

  CefRefPtr<CefClient> client =
      new DefaultClientHandler(/*use_alloy_style=*/false, url);

  if (!created_callback.is_null()) {
    // A pending completion from an earlier call would never run if it is simply
    // overwritten, so answer it first (its timeout task finds nothing to do).
    if (!pending_chrome_window_callback_.is_null()) {
      printf_log(LOG_SEVERITY_WARNING,
                 "RootWindowManager: Replacing a pending chrome window "
                 "completion, answering the previous one as failed");
      std::move(pending_chrome_window_callback_).Run(nullptr);
    }
    pending_chrome_window_callback_ = std::move(created_callback);
    // Always answer the pending completion, even if Chrome never reports the
    // browser, so the hot reload query cannot hang forever.
    const int kCreateTimeoutMs = 10000;
    CefPostDelayedTask(
        TID_UI,
        base::BindOnce(&RootWindowManager::OnChromeWindowCreateTimeout,
                       base::Unretained(this)),
        kCreateTimeoutMs);
  }

  CefBrowserHost::CreateBrowser(window_info, client, url, settings, nullptr,
                                nullptr);
}

void RootWindowManager::OnChromeWindowCreateTimeout() {
  CEF_REQUIRE_UI_THREAD();

  if (!pending_chrome_window_callback_.is_null()) {
    printf_log(LOG_SEVERITY_WARNING,
               "RootWindowManager: Timed out waiting for the Chrome window");
    std::move(pending_chrome_window_callback_).Run(nullptr);
  }
}

void RootWindowManager::OnHotReloadWindowReady(
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
    CefRefPtr<CefBrowser> browser) {
  // Runs when the new window exists, or when the wait timed out (null browser).
  // Only now is it safe to let the app terminate again: right after the old
  // window closed there are no windows at all, and MaybeCleanup() would quit.
  SetDisableTerminationInternal(false);
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Termination re-enabled");

  std::move(completion_callback).Run(browser);
}

void RootWindowManager::RestoreNextChromeWindow(
    size_t window_index,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  CEF_REQUIRE_UI_THREAD();

  if (window_index >= hot_reload_chrome_windows_.size()) {
    FinishHotReloadChromeRestore(std::move(completion_callback));
    return;
  }

  hot_reload_chrome_index_ = window_index;
  // The first browser of the window is reported before this collection flag
  // goes up again, so only the tabs created by IDC_NEW_TAB end up in it.
  hot_reload_collect_new_tabs_ = false;
  hot_reload_chrome_host_ = nullptr;

  const auto& urls = hot_reload_chrome_windows_[window_index].urls;
  printf_log(LOG_SEVERITY_INFO,
             "ExecuteHotReload: rebuilding Chrome window %zu with %zu tab(s)",
             window_index, urls.size());

  CreateChromeWindow(
      urls[0],
      base::BindOnce(&RootWindowManager::OnHotReloadChromeWindowReady,
                     base::Unretained(this), std::move(completion_callback)));
}

void RootWindowManager::OnHotReloadChromeWindowReady(
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
    CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  if (!browser.get()) {
    // The window never came up - continue with the next one instead of giving
    // up on the whole rebuild.
    printf_log(LOG_SEVERITY_WARNING,
               "ExecuteHotReload: Chrome window %zu missing",
               hot_reload_chrome_index_);
    RestoreNextChromeWindow(hot_reload_chrome_index_ + 1,
                            std::move(completion_callback));
    return;
  }

  hot_reload_chrome_host_ = browser;
  hot_reload_result_browsers_.push_back(browser);
  // From here on every browser that shows up is a tab of this window.
  hot_reload_collect_new_tabs_ = true;
  RestoreNextChromeTab(1, 0, false, std::move(completion_callback));
}

void RootWindowManager::RestoreNextChromeTab(
    size_t tab_index,
    int wait_ms,
    bool issued,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  CEF_REQUIRE_UI_THREAD();

  if (hot_reload_chrome_index_ >= hot_reload_chrome_windows_.size()) {
    FinishHotReloadChromeRestore(std::move(completion_callback));
    return;
  }
  const auto& urls = hot_reload_chrome_windows_[hot_reload_chrome_index_].urls;

  if (tab_index >= urls.size()) {
    // This window is complete; the next one resets the collection.
    RestoreNextChromeWindow(hot_reload_chrome_index_ + 1,
                            std::move(completion_callback));
    return;
  }

  // The tab created for |tab_index| is reported through OtherBrowserCreated
  // and collected in creation order, so it lands at [tab_index - 1].
  if (hot_reload_new_tabs_.size() >= tab_index) {
    CefRefPtr<CefBrowser> tab_browser = hot_reload_new_tabs_[tab_index - 1];
    CefRefPtr<CefFrame> frame =
        tab_browser.get() ? tab_browser->GetMainFrame() : nullptr;
    if (frame.get()) {
      frame->LoadURL(urls[tab_index]);
    } else {
      printf_log(LOG_SEVERITY_WARNING,
                 "ExecuteHotReload: rebuilt tab %zu has no main frame",
                 tab_index);
    }
    CefPostDelayedTask(
        TID_UI,
        base::BindOnce(&RootWindowManager::RestoreNextChromeTab,
                       base::Unretained(this), tab_index + 1, 0, false,
                       std::move(completion_callback)),
        kHotReloadTabStepDelayMs);
    return;
  }

  // The command is refused on a window that was just created (its command
  // state is not ready yet), so keep retrying instead of giving up on the
  // first refusal.
  if (!issued) {
    const int new_tab_id = cef_id_for_command_id_name("IDC_NEW_TAB");
    if (new_tab_id >= 0 &&
        hot_reload_chrome_host_->GetHost()->CanExecuteChromeCommand(
            new_tab_id)) {
      printf_log(LOG_SEVERITY_INFO,
                 "ExecuteHotReload: opening tab %zu of window %zu with "
                 "IDC_NEW_TAB (after %dms)",
                 tab_index, hot_reload_chrome_index_, wait_ms);
      hot_reload_chrome_host_->GetHost()->ExecuteChromeCommand(new_tab_id,
                                                                CEF_WOD_UNKNOWN);
      issued = true;
    } else if (wait_ms == 0) {
      printf_log(LOG_SEVERITY_INFO,
                 "ExecuteHotReload: IDC_NEW_TAB not available yet (id=%d), "
                 "retrying",
                 new_tab_id);
    }
  }

  if (wait_ms < kHotReloadMaxWaitMs) {
    CefPostDelayedTask(
        TID_UI,
        base::BindOnce(&RootWindowManager::RestoreNextChromeTab,
                       base::Unretained(this), tab_index,
                       wait_ms + kHotReloadShellPollMs, issued,
                       std::move(completion_callback)),
        kHotReloadShellPollMs);
    return;
  }

  printf_log(LOG_SEVERITY_WARNING,
             issued ? "ExecuteHotReload: rebuilt tab %zu never appeared, page "
                      "is lost"
                    : "ExecuteHotReload: IDC_NEW_TAB never became available, "
                      "page %zu is lost",
             tab_index);
  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(&RootWindowManager::RestoreNextChromeTab,
                     base::Unretained(this), tab_index + 1, 0, false,
                     std::move(completion_callback)),
      kHotReloadTabStepDelayMs);
}

void RootWindowManager::FinishHotReloadChromeRestore(
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  CEF_REQUIRE_UI_THREAD();

  hot_reload_chrome_windows_.clear();
  hot_reload_chrome_host_ = nullptr;
  hot_reload_new_tabs_.clear();
  hot_reload_collect_new_tabs_ = false;

  // Report the page the reload was asked for when it is among the rebuilt
  // ones, so the completion hook sees the page that started the reload.
  CefRefPtr<CefBrowser> result;
  for (const auto& browser : hot_reload_result_browsers_) {
    if (!browser.get()) {
      continue;
    }
    if (!result.get()) {
      result = browser;
    }
    CefRefPtr<CefFrame> frame = browser->GetMainFrame();
    const std::string page_url =
        frame.get() ? frame->GetURL().ToString() : std::string();
    if (!hot_reload_url_.empty() && page_url == hot_reload_url_) {
      result = browser;
      break;
    }
  }
  hot_reload_result_browsers_.clear();

  SetDisableTerminationInternal(false);
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Termination re-enabled");

  std::move(completion_callback).Run(result);
}

void RootWindowManager::RestoreNextHotReloadViewsWindow(
    size_t window_index,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  REQUIRE_MAIN_THREAD();

  if (window_index >= hot_reload_views_windows_.size()) {
    FinishHotReloadViewsRestore(0, std::move(completion_callback));
    return;
  }

  hot_reload_views_index_ = window_index;

  // Keep the startup defaults - controls / osr / bounds all come from the
  // command line - and override only the url, so the new window matches the one
  // it replaces.
  auto config = std::make_unique<RootWindowConfig>();
  config->url = hot_reload_views_windows_[window_index].urls[0];
  hot_reload_views_root_ = CreateRootWindow(std::move(config));
  if (window_index == 0) {
    hot_reload_views_first_root_ = hot_reload_views_root_;
  }

  // The first tab is the window's own browser, created together with the
  // window; the remaining ones are added by the step below.
  RestoreNextHotReloadViewsTab(1, 0, std::move(completion_callback));
}

void RootWindowManager::RestoreNextHotReloadViewsTab(
    size_t tab_index,
    int wait_ms,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  CEF_REQUIRE_UI_THREAD();

  if (hot_reload_views_index_ >= hot_reload_views_windows_.size()) {
    FinishHotReloadViewsRestore(0, std::move(completion_callback));
    return;
  }
  const auto& snap = hot_reload_views_windows_[hot_reload_views_index_];

  if (tab_index >= snap.urls.size()) {
    // The tabs created here end up with the last one in front, so the tab that
    // was active before the reload has to be selected again.
    ActivateHotReloadViewsTab(0, std::move(completion_callback));
    return;
  }

  RootWindowViews* views = hot_reload_views_root_.get()
                               ? hot_reload_views_root_->AsRootWindowViews()
                               : nullptr;
  TabbedRootWindowViews* tabbed = views ? views->AsTabbedRootWindow() : nullptr;

  if (!tabbed || !tabbed->OnNewTabRequested(snap.urls[tab_index])) {
    // The window shell is built asynchronously, so it may not exist yet.
    if (wait_ms < kHotReloadMaxWaitMs) {
      CefPostDelayedTask(
          TID_UI,
          base::BindOnce(&RootWindowManager::RestoreNextHotReloadViewsTab,
                         base::Unretained(this), tab_index,
                         wait_ms + kHotReloadShellPollMs,
                         std::move(completion_callback)),
          kHotReloadShellPollMs);
      return;
    }
    printf_log(LOG_SEVERITY_WARNING,
               "ExecuteHotReload: tab %zu of window %zu was not accepted",
               tab_index, hot_reload_views_index_);
  }

  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(&RootWindowManager::RestoreNextHotReloadViewsTab,
                     base::Unretained(this), tab_index + 1, 0,
                     std::move(completion_callback)),
      kHotReloadTabStepDelayMs);
}

void RootWindowManager::ActivateHotReloadViewsTab(
    int wait_ms,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  CEF_REQUIRE_UI_THREAD();

  if (hot_reload_views_index_ >= hot_reload_views_windows_.size()) {
    FinishHotReloadViewsRestore(0, std::move(completion_callback));
    return;
  }
  const auto& snap = hot_reload_views_windows_[hot_reload_views_index_];

  RootWindowViews* views = hot_reload_views_root_.get()
                               ? hot_reload_views_root_->AsRootWindowViews()
                               : nullptr;
  TabbedRootWindowViews* tabbed = views ? views->AsTabbedRootWindow() : nullptr;

  if (!tabbed || snap.urls.size() <= 1) {
    // Nothing to select in a window that cannot hold more than one tab.
    RestoreNextHotReloadViewsWindow(hot_reload_views_index_ + 1,
                                    std::move(completion_callback));
    return;
  }

  CefRefPtr<CefBrowser> target;
  if (snap.active_index >= 0) {
    const auto& tabs = tabbed->GetTabs();
    if (snap.active_index < static_cast<int>(tabs.size())) {
      target = tabs[snap.active_index]->GetBrowser();
    }
  }

  if (!target.get()) {
    // The tabs are still being created.
    if (wait_ms < kHotReloadMaxWaitMs) {
      CefPostDelayedTask(
          TID_UI,
          base::BindOnce(&RootWindowManager::ActivateHotReloadViewsTab,
                         base::Unretained(this),
                         wait_ms + kHotReloadSettlePollMs,
                         std::move(completion_callback)),
          kHotReloadSettlePollMs);
      return;
    }
    printf_log(LOG_SEVERITY_WARNING,
               "ExecuteHotReload: active tab of window %zu has no browser yet",
               hot_reload_views_index_);
  } else {
    tabbed->OnTabCommandRequested("selecttab", target->GetIdentifier());
  }

  RestoreNextHotReloadViewsWindow(hot_reload_views_index_ + 1,
                                  std::move(completion_callback));
}

void RootWindowManager::FinishHotReloadViewsRestore(
    int wait_ms,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  CEF_REQUIRE_UI_THREAD();

  CefRefPtr<CefBrowser> result =
      hot_reload_views_first_root_ ? hot_reload_views_first_root_->GetBrowser()
                                   : nullptr;

  if (!result.get() && wait_ms < kHotReloadMaxWaitMs) {
    // The browser of the first rebuilt window is created asynchronously.
    CefPostDelayedTask(
        TID_UI,
        base::BindOnce(&RootWindowManager::FinishHotReloadViewsRestore,
                       base::Unretained(this),
                       wait_ms + kHotReloadSettlePollMs,
                       std::move(completion_callback)),
        kHotReloadSettlePollMs);
    return;
  }
  if (!result.get()) {
    printf_log(LOG_SEVERITY_WARNING,
               "ExecuteHotReload: rebuilt window has no browser yet");
  }

  hot_reload_views_windows_.clear();
  hot_reload_views_root_ = nullptr;
  hot_reload_views_first_root_ = nullptr;

  if (!hot_reload_chrome_windows_.empty()) {
    // Chrome-created windows were closed as well - rebuild those before
    // completing the reload.
    if (result.get()) {
      hot_reload_result_browsers_.push_back(result);
    }
    RestoreNextChromeWindow(0, std::move(completion_callback));
    return;
  }

  SetDisableTerminationInternal(false);
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Termination re-enabled");

  std::move(completion_callback).Run(result);
}

void RootWindowManager::CloseOtherBrowsers(bool force) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::CloseOtherBrowsers,
                                     base::Unretained(this), force));
    return;
  }

  if (other_browsers_.empty()) {
    return;
  }

  // Copy: OtherBrowserClosed (from OnBeforeClose) mutates |other_browsers_|.
  auto browsers = other_browsers_;
  for (auto& entry : browsers) {
    if (entry.second.get()) {
      printf_log(LOG_SEVERITY_INFO,
                 "CloseOtherBrowsers: closing browser %d", entry.first);
      entry.second->GetHost()->CloseBrowser(force);
    }
  }
}

scoped_refptr<RootWindow> RootWindowManager::CreateDetachedWindow(
    const std::shared_ptr<Tab>& tab,
    const CefRect& bounds) {
  REQUIRE_MAIN_THREAD();

  // The tab's browser must still be alive after the in-flight handoff.
  if (!tab || !tab->GetBrowser()) {
    return nullptr;
  }

  CefBrowserSettings settings;
  MainContext::Get()->PopulateBrowserSettings(&settings);

  auto* root_window = new TabbedRootWindowViews(tab->UseAlloyStyle());
  root_window->InitDetached(this, tab, settings, bounds);

  // Store a reference to the root window on the main thread.
  OnRootWindowCreated(root_window);

  return root_window;
}

scoped_refptr<RootWindow> RootWindowManager::CreateRootWindowAsPopup(
    bool use_views,
    bool use_alloy_style,
    bool with_controls,
    bool with_osr,
    int opener_browser_id,
    int popup_id,
    bool is_devtools,
    const CefPopupFeatures& popupFeatures,
    CefWindowInfo& windowInfo,
    CefRefPtr<CefClient>& client,
    CefBrowserSettings& settings,
    bool adopt_as_tab) {
  CEF_REQUIRE_UI_THREAD();

  if (MainContext::Get()->UseDefaultPopup() || (is_devtools && !use_views)) {
    // Use default window creation for the popup. A new |client| instance is
    // required by cefclient architecture if the type is not already
    // DefaultClientHandler.
    if (!DefaultClientHandler::GetForClient(client)) {
      client = new DefaultClientHandler(use_alloy_style);
    }
    return nullptr;
  }

  CHECK_GT(opener_browser_id, 0);
  CHECK(popup_id > 0 || is_devtools);

  SanityCheckWindowConfig(is_devtools, use_views, use_alloy_style, with_osr);

  if (!temp_window_ && !use_views) {
    // TempWindow must be created on the UI thread. It is only used with
    // native (non-Views) parent windows.
    temp_window_.reset(new TempWindow());
  }

  MainContext::Get()->PopulateBrowserSettings(&settings);

  scoped_refptr<RootWindow> root_window =
      RootWindow::Create(use_views, use_alloy_style,
                         is_devtools ? WindowType::DEVTOOLS
                                     : WindowType::NORMAL);
  if (!is_devtools) {
    root_window->SetPopupId(opener_browser_id, popup_id);
    // Remember the disposition decision: the popup root later publishes it to
    // the opening window when the popup browser view is created.
    root_window->set_popup_adopt_as_tab(adopt_as_tab);
  }
  root_window->InitAsPopup(this, with_controls, with_osr, popupFeatures,
                           windowInfo, client, settings);

  // Store a reference to the root window on the main thread.
  OnRootWindowCreated(root_window);

  return root_window;
}

void RootWindowManager::AbortOrClosePopup(int opener_browser_id, int popup_id) {
  CEF_REQUIRE_UI_THREAD();
  // Continue on the main thread.
  OnAbortOrClosePopup(opener_browser_id, popup_id);
}

scoped_refptr<RootWindow> RootWindowManager::GetWindowForBrowser(
    int browser_id) const {
  REQUIRE_MAIN_THREAD();

  for (auto root_window : root_windows_) {
    if (root_window->HasBrowser(browser_id)) {
      return root_window;
    }
  }
  return nullptr;
}

scoped_refptr<RootWindow> RootWindowManager::GetActiveRootWindow() const {
  REQUIRE_MAIN_THREAD();
  return active_root_window_;
}

void RootWindowManager::CloseAllWindows(bool force) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::CloseAllWindows,
                                     base::Unretained(this), force));
    return;
  }

  if (root_windows_.empty()) {
    return;
  }

  // Use a copy of |root_windows_| because the original set may be modified
  // in OnRootWindowDestroyed while iterating.
  RootWindowSet root_windows = root_windows_;

  for (auto root_window : root_windows) {
    root_window->Close(force);
  }
}

void RootWindowManager::OtherBrowserCreated(int browser_id,
                                            int opener_browser_id,
                                            CefRefPtr<CefBrowser> browser) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::OtherBrowserCreated,
                                     base::Unretained(this), browser_id,
                                     opener_browser_id, browser));
    return;
  }

  other_browser_ct_++;
  // Hold the browser so hot reload can close it later (CloseAllWindows only
  // walks |root_windows_|, which never contains these).
  if (browser.get()) {
    other_browsers_[browser_id] = browser;
    // Tabs created by the hot reload rebuild (IDC_NEW_TAB), collected in
    // creation order so each one can be loaded with its page.
    if (hot_reload_collect_new_tabs_) {
      hot_reload_new_tabs_.push_back(browser);
    }
  }
  printf_log(LOG_SEVERITY_INFO,
            "RootWindowManager: Other browser created, count = %d",
            other_browser_ct_);

  // Track ownership of popup browsers that don't have a RootWindow.
  if (opener_browser_id > 0) {
    other_browser_owners_[opener_browser_id].insert(browser_id);
  }

  // A window we were asked to create asynchronously (see the
  // CreateChromeWindow overload) is ready now. Top-level windows have no
  // opener, so this cannot be claimed by an unrelated popup.
  if (opener_browser_id <= 0 && !pending_chrome_window_callback_.is_null()) {
    printf_log(LOG_SEVERITY_INFO,
              "RootWindowManager: Pending chrome window created (browser %d)",
              browser_id);
    std::move(pending_chrome_window_callback_).Run(browser);
  }
}

void RootWindowManager::OtherBrowserClosed(int browser_id,
                                           int opener_browser_id) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::OtherBrowserClosed,
                                     base::Unretained(this), browser_id,
                                     opener_browser_id));
    return;
  }

  DCHECK_GT(other_browser_ct_, 0);
  other_browser_ct_--;
  other_browsers_.erase(browser_id);
  printf_log(LOG_SEVERITY_INFO,
            "RootWindowManager: Other browser closed, count = %d",
            other_browser_ct_);

  // Track ownership of popup browsers that don't have a RootWindow.
  if (opener_browser_id > 0) {
    DCHECK(other_browser_owners_.contains(opener_browser_id));
    auto& child_set = other_browser_owners_[opener_browser_id];
    DCHECK(child_set.contains(browser_id));
    child_set.erase(browser_id);
    if (child_set.empty()) {
      other_browser_owners_.erase(opener_browser_id);
    }
  }

  MaybeCleanup();
}

void RootWindowManager::OnRootWindowCreated(
    scoped_refptr<RootWindow> root_window) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::OnRootWindowCreated,
                                     base::Unretained(this), root_window));
    return;
  }

  root_windows_.insert(root_window);

  if (root_windows_.size() == 1U) {
    // The first root window should be considered the active window.
    OnRootWindowActivated(root_window.get());
  }
}

void RootWindowManager::OnAbortOrClosePopup(int opener_browser_id,
                                            int popup_id) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::OnAbortOrClosePopup,
                                     base::Unretained(this), opener_browser_id,
                                     popup_id));
    return;
  }

  // Use a copy of |root_windows_| because the original set may be modified
  // in OnRootWindowDestroyed while iterating.
  RootWindowSet root_windows = root_windows_;

  // Close or destroy the associated RootWindow(s). This may be a specific popup
  // (|popup_id| > 0), or all popups if the opener is closing (|popup_id| < 0).
  for (auto root_window : root_windows) {
    if (root_window->IsPopupIdMatch(opener_browser_id, popup_id)) {
      const bool window_created = root_window->IsWindowCreated();
      LOG(INFO) << (window_created ? "Closing" : "Aborting") << " popup "
                << root_window->popup_id() << " of browser "
                << opener_browser_id;
      if (window_created) {
        // Close the window in the usual way. Will result in a call to
        // OnRootWindowDestroyed.
        root_window->Close(/*force=*/false);
      } else {
        // The window was not created, so destroy directly.
        OnRootWindowDestroyed(root_window.get());
      }
    }
  }

  // Close all other associated popups if the opener is closing. These popups
  // don't have a RootWindow (e.g. when running with `--use-default-popup`).
  if (popup_id < 0 && other_browser_owners_.contains(opener_browser_id)) {
    // Use a copy as the original set may be modified in OtherBrowserClosed
    // while iterating.
    auto set = other_browser_owners_[opener_browser_id];
    for (auto browser_id : set) {
      if (auto browser = CefBrowserHost::GetBrowserByIdentifier(browser_id)) {
        LOG(INFO) << "Closing popup browser " << browser_id << " of browser "
                  << opener_browser_id;
        browser->GetHost()->CloseBrowser(/*force=*/false);
      }
    }
  }
}

CefRefPtr<CefRequestContext> RootWindowManager::GetRequestContext() {
  REQUIRE_MAIN_THREAD();
  return CreateRequestContext(RequestContextCallback());
}

void RootWindowManager::GetRequestContext(RequestContextCallback callback) {
  DCHECK(!callback.is_null());

  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(
        base::IgnoreResult(&RootWindowManager::CreateRequestContext),
        base::Unretained(this), std::move(callback)));
  } else {
    CreateRequestContext(std::move(callback));
  }
}

CefRefPtr<CefRequestContext> RootWindowManager::CreateRequestContext(
    RequestContextCallback callback) {
  REQUIRE_MAIN_THREAD();

  if (request_context_per_browser_) {
    // Synchronous use of non-global request contexts is not safe.
    CHECK(!callback.is_null());

    // Create a new request context for each browser.
    CefRequestContextSettings settings;

    CefRefPtr<CefCommandLine> command_line =
        CefCommandLine::GetGlobalCommandLine();
    if (command_line->HasSwitch(switches::kCachePath)) {
      if (request_context_shared_cache_) {
        // Give each browser the same cache path. The resulting context objects
        // will share the same storage internally.
        CefString(&settings.cache_path) =
            command_line->GetSwitchValue(switches::kCachePath);
      } else {
        // Give each browser a unique cache path. This will create completely
        // isolated context objects.
        std::stringstream ss;
        ss << command_line->GetSwitchValue(switches::kCachePath).ToString()
           << file_util::kPathSep << time(nullptr);
        CefString(&settings.cache_path) = ss.str();
      }
    }

    return CefRequestContext::CreateContext(
        settings, new ClientRequestContextHandler(std::move(callback)));
  }

  // All browsers will share the global request context.
  if (!shared_request_context_) {
    shared_request_context_ = CefRequestContext::CreateContext(
        CefRequestContext::GetGlobalContext(),
        new ClientRequestContextHandler(std::move(callback)));
  } else if (!callback.is_null()) {
    // Execute the callback on the UI thread.
    CefPostTask(TID_UI,
                base::BindOnce(std::move(callback), shared_request_context_));
  }

  return shared_request_context_;
}

scoped_refptr<ImageCache> RootWindowManager::GetImageCache() {
  CEF_REQUIRE_UI_THREAD();

  if (!image_cache_) {
    image_cache_ = new ImageCache;
  }
  return image_cache_;
}

void RootWindowManager::OnTest(RootWindow* root_window, int test_id) {
  REQUIRE_MAIN_THREAD();

  test_runner::RunTest(root_window->GetBrowser(), test_id);
}

void RootWindowManager::OnExit(RootWindow* root_window) {
  REQUIRE_MAIN_THREAD();

  // Exit must go through the graceful close chain: a forced close skips
  // CanClose and therefore never closes the CefBrowsers, leaving the windows
  // gone but the browser process alive (zombie). The non-forced request is
  // intercepted by the multi-tab DoClose takeover, which closes every tab of
  // every window; empty windows then close themselves and MaybeCleanup()
  // exits the process.
  CloseAllWindows(false);
}

void RootWindowManager::OnRootWindowDestroyed(RootWindow* root_window) {
  REQUIRE_MAIN_THREAD();

  RootWindowSet::iterator it = root_windows_.find(root_window);
  DCHECK(it != root_windows_.end());
  if (it != root_windows_.end()) {
    root_windows_.erase(it);
  }

  if (root_window == active_root_window_) {
    active_root_window_ = nullptr;
  }

  MaybeCleanup();
}

void RootWindowManager::OnRootWindowActivated(RootWindow* root_window) {
  REQUIRE_MAIN_THREAD();

  if (root_window == active_root_window_) {
    return;
  }

  active_root_window_ = root_window;
}

void RootWindowManager::MaybeCleanup() {
  REQUIRE_MAIN_THREAD();

  printf_log(LOG_SEVERITY_INFO, "MaybeCleanup called: terminate_when_all_windows_closed_=%d, disable_termination_=%d, root_windows_.empty()=%d, other_browser_ct_=%d",
            terminate_when_all_windows_closed_, disable_termination_, root_windows_.empty(), other_browser_ct_);
  if (terminate_when_all_windows_closed_ && !disable_termination_ &&
      root_windows_.empty() && other_browser_ct_ == 0) {
    // All windows and browsers have closed. Clean up on the UI thread.
    printf_log(LOG_SEVERITY_INFO, "MaybeCleanup: Triggering cleanup");
    CefPostTask(TID_UI, base::BindOnce(&RootWindowManager::CleanupOnUIThread,
                                       base::Unretained(this)));
  } else {
    printf_log(LOG_SEVERITY_INFO, "MaybeCleanup: Cleanup not triggered");
  }
}

void RootWindowManager::SetDisableTermination(bool disable) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::SetDisableTermination,
                                     base::Unretained(this), disable));
    return;
  }
  SetDisableTerminationInternal(disable);
}

void RootWindowManager::SetDisableTerminationInternal(bool disable) {
  disable_termination_ = disable;
  printf_log(LOG_SEVERITY_INFO, "SetDisableTerminationInternal: disable_termination_ = %s", disable ? "true" : "false");
}

void RootWindowManager::ExecuteHotReload(
    const std::string& url,
    const std::vector<cef_query_handler::FileCopyInfo>& files,
    base::OnceCallback<void()> copy_callback,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::ExecuteHotReload,
                                     base::Unretained(this), url, files,
                                     std::move(copy_callback),
                                     std::move(completion_callback)));
    return;
  }
  CEF_REQUIRE_UI_THREAD();
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Starting hot reload flow...");

  // Step 1: Disable termination
  SetDisableTerminationInternal(true);

  // Snapshot the windows this process owns before closing them: they are
  // rebuilt here, so the window count, the tab urls in strip order and the
  // active tab all have to be recorded. The bounds and the show state are not -
  // each window saves them on its way out and the new one reads them back.
  hot_reload_url_ = url;
  hot_reload_views_windows_.clear();
  for (auto root_window : root_windows_) {
    // Popup windows opened by the pages (window.open) hold real pages, so they
    // are rebuilt as well - as ordinary top-level windows, which is the price
    // of the rebuild. DevTools windows are dropped by the url check below.
    HotReloadWindowPages snap;
    CefRefPtr<CefBrowser> active_browser = root_window->GetBrowser();
    RootWindowViews* views = root_window->AsRootWindowViews();
    TabbedRootWindowViews* tabbed = views ? views->AsTabbedRootWindow() : nullptr;
    if (tabbed) {
      for (const auto& tab : tabbed->GetTabs()) {
        CefRefPtr<CefBrowser> browser = tab->GetBrowser();
        CefRefPtr<CefFrame> frame =
            browser.get() ? browser->GetMainFrame() : nullptr;
        const std::string page_url =
            frame.get() ? frame->GetURL().ToString() : std::string();
        if (!IsRestorableHotReloadUrl(page_url)) {
          continue;
        }
        if (browser.get() && active_browser.get() &&
            browser->IsSame(active_browser)) {
          snap.active_index = static_cast<int>(snap.urls.size());
        }
        snap.urls.push_back(page_url);
      }
    } else if (active_browser.get()) {
      CefRefPtr<CefFrame> frame = active_browser->GetMainFrame();
      const std::string page_url =
          frame.get() ? frame->GetURL().ToString() : std::string();
      if (IsRestorableHotReloadUrl(page_url)) {
        snap.urls.push_back(page_url);
      }
    }
    if (!snap.urls.empty()) {
      hot_reload_views_windows_.push_back(std::move(snap));
    }
  }

  // Step 2: Close all windows
  if (!root_windows_.empty()) {
    printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Closing %zu windows...",
              root_windows_.size());
    RootWindowSet root_windows = root_windows_;
    for (auto root_window : root_windows) {
      // A forced close on a multi-tab window declines the tab takeover and only
      // runs the shell path, which leaves every tab browser behind as a zombie
      // (the window can no longer be closed and the process will not exit).
      // Close the tab browsers instead - the window follows on its own once its
      // last tab is gone.
      RootWindowViews* close_views = root_window->AsRootWindowViews();
      TabbedRootWindowViews* close_tabbed =
          close_views ? close_views->AsTabbedRootWindow() : nullptr;
      bool closed_tab_browsers = false;
      if (close_tabbed) {
        for (const auto& tab : close_tabbed->GetTabs()) {
          CefRefPtr<CefBrowser> browser = tab->GetBrowser();
          if (browser.get() && browser->GetHost()) {
            browser->GetHost()->CloseBrowser(true);
            closed_tab_browsers = true;
          }
        }
      }
      if (!closed_tab_browsers) {
        root_window->Close(true);
      }
    }
  }

  // Snapshot the Chrome-created browsers before closing them - the pages of the
  // default Chrome window mode, and the Chrome windows the pages open in the
  // other modes (window.open targets, "new window"). They are not recorded by
  // Chrome's own restore service (CEF hosts them without the tabstrip feature
  // that service requires), so they have to be rebuilt here as well.
  // Browsers of the same window share its window handle, and |other_browsers_|
  // is keyed by browser id, which follows the tab order.
  hot_reload_chrome_windows_.clear();
  {
    std::map<CefWindowHandle, size_t> window_index;
    int page_ct = 0;
    for (const auto& entry : other_browsers_) {
      CefRefPtr<CefBrowser> browser = entry.second;
      if (!browser.get() || !browser->GetHost()) {
        continue;
      }
      CefRefPtr<CefFrame> frame = browser->GetMainFrame();
      const std::string page_url =
          frame.get() ? frame->GetURL().ToString() : std::string();
      if (!IsRestorableHotReloadUrl(page_url)) {
        continue;
      }
      page_ct++;
      const CefWindowHandle handle = browser->GetHost()->GetWindowHandle();
      auto it = window_index.find(handle);
      if (it == window_index.end()) {
        it = window_index
                 .emplace(handle, hot_reload_chrome_windows_.size())
                 .first;
        hot_reload_chrome_windows_.push_back(HotReloadWindowPages());
      }
      hot_reload_chrome_windows_[it->second].urls.push_back(page_url);
    }
    printf_log(LOG_SEVERITY_INFO,
               "ExecuteHotReload: %d page(s) in %zu window(s) to rebuild",
               page_ct, hot_reload_chrome_windows_.size());
  }

  // Close every Chrome-created browser. CloseAllWindows() only walks
  // |root_windows_| and never sees these, and a renderer that survives here
  // keeps the files locked and the agent host running.
  printf_log(LOG_SEVERITY_INFO,
             "ExecuteHotReload: Closing %zu other browser(s)...",
             other_browsers_.size());
  CloseOtherBrowsers(true);

  printf_log(LOG_SEVERITY_INFO,
            "ExecuteHotReload: Close requested, root_windows_.empty()=%d, other_browser_ct_=%d, files count=%zu",
            root_windows_.empty(), other_browser_ct_, files.size());

  // Step 3: Wait until the browsers are actually gone, then copy files. Closing
  // a browser terminates its renderer, so there is no separate "kill the
  // renderers" step - it is only a fallback if the wait times out.
  const int kPollIntervalMs = 200;
  const int kMaxWaitTimeMs = 10000; // Maximum 10 seconds wait
  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(&RootWindowManager::WaitForBrowsersClosed,
                     base::Unretained(this), url, files, std::move(copy_callback),
                     std::move(completion_callback), 0,
                     kPollIntervalMs, kMaxWaitTimeMs),
      kPollIntervalMs);
}

void RootWindowManager::WaitForBrowsersClosed(
    const std::string& url,
    const std::vector<cef_query_handler::FileCopyInfo>& files,
    base::OnceCallback<void()> copy_callback,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
    int elapsed_ms,
    int poll_interval_ms,
    int max_wait_time_ms) {
  CEF_REQUIRE_UI_THREAD();

  // A window owned by a RootWindow is gone once that RootWindow is destroyed.
  // The Chrome-created browsers are tracked in the "other browsers" set, which
  // has to drain in every mode now that the reload closes and rebuilds them.
  const bool windows_pending = !root_windows_.empty();
  const bool other_browsers_pending = !other_browsers_.empty();

  if (!windows_pending && !other_browsers_pending) {
    printf_log(LOG_SEVERITY_INFO,
               "WaitForBrowsersClosed: all browsers closed after %dms",
               elapsed_ms);
    CheckFileLockAndCopy(url, files, std::move(copy_callback),
                         std::move(completion_callback), 0, poll_interval_ms,
                         max_wait_time_ms);
    return;
  }

  if (elapsed_ms >= max_wait_time_ms) {
    printf_log(LOG_SEVERITY_WARNING,
               "WaitForBrowsersClosed: timed out after %dms with %zu window(s) "
               "and %zu other browser(s) left; terminating leftover renderers",
               elapsed_ms, root_windows_.size(), other_browsers_.size());
    // A window whose close is still waiting on a beforeunload handler never
    // made it into the restore service as a whole window; force it now so the
    // flow can go on.
    CloseOtherBrowsers(true);
    // Fallback: a renderer that outlived its browser still holds the files.
    CefTerminateRenderProcess();
    CheckFileLockAndCopy(url, files, std::move(copy_callback),
                         std::move(completion_callback), 0, poll_interval_ms,
                         max_wait_time_ms);
    return;
  }

  printf_log(LOG_SEVERITY_INFO,
             "WaitForBrowsersClosed: waiting, root_windows=%zu, "
             "other_browsers=%zu, elapsed=%dms",
             root_windows_.size(), other_browsers_.size(), elapsed_ms);

  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(&RootWindowManager::WaitForBrowsersClosed,
                     base::Unretained(this), url, files, std::move(copy_callback),
                     std::move(completion_callback), elapsed_ms + poll_interval_ms,
                     poll_interval_ms, max_wait_time_ms),
      poll_interval_ms);
}

void RootWindowManager::CheckFileLockAndCopy(
    const std::string& url,
    const std::vector<cef_query_handler::FileCopyInfo>& files,
    base::OnceCallback<void()> copy_callback,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
    int elapsed_ms,
    int poll_interval_ms,
    int max_wait_time_ms) {
  CEF_REQUIRE_UI_THREAD();

  // Try to detect if any destination file is still locked
  // This is a best-effort detection on Windows by attempting to open with exclusive access
  bool is_locked = false;
  std::string locked_file;

#ifdef _WIN32
  for (const auto& file : files) {
    HANDLE hFile = CreateFileA(file.dest.c_str(),
                               GENERIC_READ,
                               0,  // No sharing, exclusive access
                               NULL,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        is_locked = true;
        locked_file = file.dest;
        printf_log(LOG_SEVERITY_INFO,
                  "CheckFileLockAndCopy: File %s is locked (error=%d)",
                  file.dest.c_str(), error);
        break;
      } else {
        // File doesn't exist or other error, not a lock issue
        printf_log(LOG_SEVERITY_INFO,
                  "CheckFileLockAndCopy: Cannot open file %s (error=%d), not a lock issue",
                  file.dest.c_str(), error);
      }
    } else {
      // File is not locked, close the handle
      CloseHandle(hFile);
      printf_log(LOG_SEVERITY_INFO,
                "CheckFileLockAndCopy: File %s is not locked",
                file.dest.c_str());
    }
  }
#else
  // On non-Windows platforms, assume not locked
  printf_log(LOG_SEVERITY_INFO,
            "CheckFileLockAndCopy: File lock detection not supported on this platform");
#endif

  if (!is_locked) {
    // All files are not locked, proceed with copy
    printf_log(LOG_SEVERITY_INFO,
              "CheckFileLockAndCopy: All files are not locked, proceeding with file copy");
    OnCopyFilesAndCreateWindow(url, files, std::move(copy_callback),
                               std::move(completion_callback));
  } else if (elapsed_ms < max_wait_time_ms) {
    // Some file is locked, wait and retry
    printf_log(LOG_SEVERITY_INFO,
              "CheckFileLockAndCopy: File %s is locked, will check again in %dms (elapsed=%dms)",
              locked_file.c_str(), poll_interval_ms, elapsed_ms);
    CefPostDelayedTask(
        TID_UI,
        base::BindOnce(&RootWindowManager::CheckFileLockAndCopy,
                       base::Unretained(this), url, files, std::move(copy_callback),
                       std::move(completion_callback), elapsed_ms + poll_interval_ms,
                       poll_interval_ms, max_wait_time_ms),
        poll_interval_ms);
  } else {
    // Timeout waiting for file unlock, but proceed anyway
    printf_log(LOG_SEVERITY_ERROR,
              "CheckFileLockAndCopy: Timeout waiting for file unlock (file=%s, waited %dms), proceeding with copy anyway",
              locked_file.c_str(), max_wait_time_ms);
    OnCopyFilesAndCreateWindow(url, files, std::move(copy_callback),
                               std::move(completion_callback));
  }
}

// Hot reload completion for the RootWindow path: the browser is created
// asynchronously inside the RootWindow, so it is read a moment after the window
// exists. A null browser means the window never came up; the caller reports it
// as a failure.
static void CompleteHotReloadWithRootWindow(
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
    scoped_refptr<RootWindow> root_window) {
  std::move(completion_callback)
      .Run(root_window ? root_window->GetBrowser() : nullptr);
}

void RootWindowManager::OnCopyFilesAndCreateWindow(
    const std::string& url,
    const std::vector<cef_query_handler::FileCopyInfo>& files,
    base::OnceCallback<void()> copy_callback,
    base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback) {
  CEF_REQUIRE_UI_THREAD();
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Copying files...");

  // Execute the copy callback
  if (copy_callback) {
    std::move(copy_callback).Run();
  }

  // Rebuild what was closed: the windows this process owns first, then the
  // Chrome-created ones. Whichever kind the session started with is the kind
  // that gets recreated here, so the reload does not change the window style.
  if (!hot_reload_views_windows_.empty()) {
    printf_log(LOG_SEVERITY_INFO,
               "ExecuteHotReload: Rebuilding %zu window(s)",
               hot_reload_views_windows_.size());
    RestoreNextHotReloadViewsWindow(0, std::move(completion_callback));
  } else if (!hot_reload_chrome_windows_.empty()) {
    RestoreNextChromeWindow(0, std::move(completion_callback));
  } else if (MainContext::Get()->UseChromeWindowGlobal()) {
    printf_log(LOG_SEVERITY_INFO,
               "ExecuteHotReload: Creating new Chrome window with URL: %s",
               (url.empty() ? "[default]" : url.c_str()));

    // Completion runs once Chrome reports the browser (see CreateChromeWindow).
    // Termination stays disabled until then.
    CreateChromeWindow(
        url,
        base::BindOnce(&RootWindowManager::OnHotReloadWindowReady,
                       base::Unretained(this), std::move(completion_callback)));
  } else {
    printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Creating new window...");

    // Keep the startup defaults - controls / osr / bounds all come from the
    // command line - and override only the url, so the new window matches the
    // old one instead of gaining an extra controls bar.
    auto config = std::make_unique<RootWindowConfig>();
    config->url = url;

    scoped_refptr<RootWindow> root_window = CreateRootWindow(std::move(config));
    printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: New window created with URL: %s",
              (url.empty() ? "[default]" : url.c_str()));

    // Step 5 (RootWindow path): the browser is created asynchronously inside the
    // RootWindow, so report it a moment later.
    if (!completion_callback.is_null()) {
      const int kReloadDelayMs = 500;
      CefPostDelayedTask(
          TID_UI,
          base::BindOnce(&CompleteHotReloadWithRootWindow,
                         std::move(completion_callback), root_window),
          kReloadDelayMs);
    }

    // Step 4: Re-enable termination. Safe here - CreateRootWindow registers the
    // window synchronously, so the app never sees "no windows" in between.
    SetDisableTerminationInternal(false);
    printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Termination re-enabled");
  }
}

void RootWindowManager::CleanupOnUIThread() {
  CEF_REQUIRE_UI_THREAD();

  if (temp_window_) {
    // TempWindow must be destroyed on the UI thread.
    temp_window_.reset(nullptr);
  }

  if (image_cache_) {
    image_cache_ = nullptr;
  }

  // Quit the main message loop.
  MainMessageLoop::Get()->Quit();
}

// Count renderer processes using CefTaskManager
// Returns the number of renderer processes, or -1 on error
// NOTE: Must be called on UI thread
int RootWindowManager::CefCountRenderProcess() {
  CEF_REQUIRE_UI_THREAD();

  int renderer_count = 0;

  // Get the global task manager (nullptr if not on UI thread)
  CefRefPtr<CefTaskManager> task_manager = CefTaskManager::GetTaskManager();
  if (!task_manager) {
    printf_log(LOG_SEVERITY_ERROR,
               "CefCountRenderProcess: Failed to get task manager (must be on UI thread)");
    return -1;
  }

  // Get all task IDs
  CefTaskManager::TaskIdList task_ids;
  if (!task_manager->GetTaskIdsList(task_ids)) {
    printf_log(LOG_SEVERITY_ERROR,
               "CefCountRenderProcess: Failed to get task ID list");
    return -1;
  }

  // Count renderer processes
  for (const auto& task_id : task_ids) {
    CefTaskInfo task_info;

    if (!task_manager->GetTaskInfo(task_id, task_info)) {
      continue;
    }

    // Check if this is a renderer process
    if (task_info.type == CEF_TASK_TYPE_RENDERER) {
      printf_log(LOG_SEVERITY_INFO,
                 "CefCountRenderProcess: Found renderer process task_id=%lld, title='%s', is_killable=%d, memory=%lld",
                 task_info.id,
                 CefString(&task_info.title).ToString().c_str(),
                 task_info.is_killable,
                 task_info.memory);

      // Only count killable renderer processes
      if (task_info.is_killable) {
        renderer_count++;
      }
    }
  }

  printf_log(LOG_SEVERITY_INFO,
             "CefCountRenderProcess: Found %d killable renderer process(es)",
             renderer_count);

  return renderer_count;
}

// Terminate renderer processes using CefTaskManager
// Returns the number of terminated renderer processes, or -1 on error
// NOTE: Must be called on UI thread
int RootWindowManager::CefTerminateRenderProcess() {
  CEF_REQUIRE_UI_THREAD();

  int terminated_count = 0;

  // Get the global task manager (nullptr if not on UI thread)
  CefRefPtr<CefTaskManager> task_manager = CefTaskManager::GetTaskManager();
  if (!task_manager) {
    printf_log(LOG_SEVERITY_ERROR,
               "CefTerminateRenderProcess: Failed to get task manager (must be on UI thread)");
    return -1;
  }

  // Get all task IDs
  CefTaskManager::TaskIdList task_ids;
  if (!task_manager->GetTaskIdsList(task_ids)) {
    printf_log(LOG_SEVERITY_ERROR,
               "CefTerminateRenderProcess: Failed to get task ID list");
    return -1;
  }

  // Find and terminate renderer processes
  for (const auto& task_id : task_ids) {
    CefTaskInfo task_info;

    if (!task_manager->GetTaskInfo(task_id, task_info)) {
      continue;
    }

    // Check if this is a renderer process
    if (task_info.type == CEF_TASK_TYPE_RENDERER) {
      printf_log(LOG_SEVERITY_INFO,
                 "CefTerminateRenderProcess: Checking renderer process task_id=%lld, title='%s', is_killable=%d",
                 task_info.id,
                 CefString(&task_info.title).ToString().c_str(),
                 task_info.is_killable);

      // Only try to kill processes that are marked as killable
      if (!task_info.is_killable) {
        printf_log(LOG_SEVERITY_WARNING,
                   "CefTerminateRenderProcess: Skipping task_id=%lld - not killable",
                   task_info.id);
        continue;
      }

      printf_log(LOG_SEVERITY_INFO,
                 "CefTerminateRenderProcess: Terminating renderer process TaskId=%llu", task_id);

      // Kill the task (renderer process)
      if (task_manager->KillTask(task_id)) {
        terminated_count++;
        printf_log(LOG_SEVERITY_INFO,
                   "CefTerminateRenderProcess: Successfully terminated renderer process task_id=%d", task_id);
      } else {
        printf_log(LOG_SEVERITY_ERROR,
                   "CefTerminateRenderProcess: Failed to terminate renderer process task_id=%d (KillTask returned false)", task_id);
      }
    }
  }

  printf_log(LOG_SEVERITY_INFO,
             "CefTerminateRenderProcess: Terminated %d renderer process(es)", terminated_count);

  return terminated_count;
}

}  // namespace client
