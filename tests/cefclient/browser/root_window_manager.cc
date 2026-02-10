// Copyright (c) 2015 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/root_window_manager.h"

#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "include/base/cef_callback.h"
#include "include/base/cef_logging.h"
#include "include/cef_task_manager.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "tests/cefclient/browser/default_client_handler.h"
#include "tests/cefclient/browser/main_context.h"
#include "tests/cefclient/browser/test_runner.h"
#include "tests/shared/browser/file_util.h"
#include "tests/shared/browser/resource_util.h"
#include "tests/shared/common/client_switches.h"
#include "tests/cefclient/hostclr/HostCLR.h"

namespace client {

namespace {

class ClientRequestContextHandler : public CefRequestContextHandler {
 public:
  using CreateCallback = RootWindow::Delegate::RequestContextCallback;

  explicit ClientRequestContextHandler(CreateCallback callback)
      : create_callback_(std::move(callback)) {}

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
  DISALLOW_COPY_AND_ASSIGN(ClientRequestContextHandler);
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
      RootWindow::Create(config->use_views, config->use_alloy_style);
  root_window->Init(this, std::move(config), settings);

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
    CefBrowserSettings& settings) {
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
      RootWindow::Create(use_views, use_alloy_style);
  if (!is_devtools) {
    root_window->SetPopupId(opener_browser_id, popup_id);
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
    CefRefPtr<CefBrowser> browser = root_window->GetBrowser();
    if (browser.get() && browser->GetIdentifier() == browser_id) {
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
                                            int opener_browser_id) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::OtherBrowserCreated,
                                     base::Unretained(this), browser_id,
                                     opener_browser_id));
    return;
  }

  other_browser_ct_++;
  printf_log(LOG_SEVERITY_INFO,
            "RootWindowManager: Other browser created, count = %d",
            other_browser_ct_);

  // Track ownership of popup browsers that don't have a RootWindow.
  if (opener_browser_id > 0) {
    other_browser_owners_[opener_browser_id].insert(browser_id);
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
  printf_log(LOG_SEVERITY_INFO,
            "RootWindowManager: Other browser closed, count = %d",
            other_browser_ct_);

  // Track ownership of popup browsers that don't have a RootWindow.
  if (opener_browser_id > 0) {
    DCHECK(other_browser_owners_.find(opener_browser_id) !=
           other_browser_owners_.end());
    auto& child_set = other_browser_owners_[opener_browser_id];
    DCHECK(child_set.find(browser_id) != child_set.end());
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
  if (popup_id < 0 && other_browser_owners_.find(opener_browser_id) !=
                          other_browser_owners_.end()) {
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
    base::OnceCallback<void(scoped_refptr<RootWindow>)> completion_callback,
    bool custom_process_killer) {
  if (!CURRENTLY_ON_MAIN_THREAD()) {
    // Execute this method on the main thread.
    MAIN_POST_CLOSURE(base::BindOnce(&RootWindowManager::ExecuteHotReload,
                                     base::Unretained(this), url, files,
                                     std::move(copy_callback),
                                     std::move(completion_callback),
                                     custom_process_killer));
    return;
  }
  CEF_REQUIRE_UI_THREAD();
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Starting hot reload flow...");

  // Step 1: Disable termination
  SetDisableTerminationInternal(true);

  // Step 2: Close all windows
  if (!root_windows_.empty()) {
    printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Closing %zu windows...",
              root_windows_.size());
    RootWindowSet root_windows = root_windows_;
    for (auto root_window : root_windows) {
      root_window->Close(true);
    }
  }

  printf_log(LOG_SEVERITY_INFO,
            "ExecuteHotReload: All windows closed, root_windows_.empty()=%d, other_browser_ct_=%d, custom_process_killer=%s, files count=%zu",
            root_windows_.empty(), other_browser_ct_,
            custom_process_killer ? "true" : "false", files.size());

  // Step 3: Wait for all renderer processes to terminate, then copy files
  // Instead of a fixed delay, we'll poll and wait for renderer processes to exit
  const int kPollIntervalMs = 200;
  const int kMaxWaitTimeMs = 10000; // Maximum 10 seconds wait
  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(&RootWindowManager::TerminateRendererProcessesAndCopy,
                     base::Unretained(this), url, files, std::move(copy_callback),
                     std::move(completion_callback), 0,
                     kPollIntervalMs, kMaxWaitTimeMs, custom_process_killer),
      kPollIntervalMs * (custom_process_killer ? 5 : 1));
}

void RootWindowManager::TerminateRendererProcessesAndCopy(
    const std::string& url,
    const std::vector<cef_query_handler::FileCopyInfo>& files,
    base::OnceCallback<void()> copy_callback,
    base::OnceCallback<void(scoped_refptr<RootWindow>)> completion_callback,
    int elapsed_ms,
    int poll_interval_ms,
    int max_wait_time_ms,
    bool custom_process_killer) {
  CEF_REQUIRE_UI_THREAD();

  printf_log(LOG_SEVERITY_INFO,
            "TerminateRendererProcessesAndCopy: other_browser_ct_=%d, elapsed=%dms",
            other_browser_ct_, elapsed_ms);

  // First, count renderer processes
  int renderer_count = 0;
  if (custom_process_killer) {
    renderer_count = CountRenderProcess();
    printf_log(LOG_SEVERITY_INFO,
              "TerminateRendererProcessesAndCopy: CountRenderProcess returned %d",
              renderer_count);
  } else {
    renderer_count = CefCountRenderProcess();
    printf_log(LOG_SEVERITY_INFO,
              "TerminateRendererProcessesAndCopy: CefCountRenderProcess returned %d",
              renderer_count);
  }

  if (renderer_count < 0) {
    printf_log(LOG_SEVERITY_ERROR,
              "TerminateRendererProcessesAndCopy: Failed to count renderer processes, proceeding with copy");
    CheckFileLockAndCopy(url, files, std::move(copy_callback),
                         std::move(completion_callback), 0, poll_interval_ms,
                         max_wait_time_ms);
    return;
  }

  // If no renderer processes, proceed with copy
  if (renderer_count == 0) {
    printf_log(LOG_SEVERITY_INFO,
              "TerminateRendererProcessesAndCopy: No renderer processes found, proceeding with copy");
    CheckFileLockAndCopy(url, files, std::move(copy_callback),
                         std::move(completion_callback), 0, poll_interval_ms,
                         max_wait_time_ms);
    return;
  }

  // Terminate renderer processes
  int terminated_count = 0;
  if (custom_process_killer) {
    terminated_count = TerminateRenderProcess();
    printf_log(LOG_SEVERITY_INFO,
              "TerminateRendererProcessesAndCopy: Terminated %d/%d renderer process(es)",
              terminated_count, renderer_count);
  } else {
    terminated_count = CefTerminateRenderProcess();
    printf_log(LOG_SEVERITY_INFO,
              "TerminateRendererProcessesAndCopy: CefTerminateRenderProcess terminated %d/%d renderer process(es)",
              terminated_count, renderer_count);
  }

  // Check if termination was successful
  if (terminated_count == 0 && renderer_count > 0) {
    printf_log(LOG_SEVERITY_WARNING,
              "TerminateRendererProcessesAndCopy: Failed to terminate any renderer processes, looping back");
  } else {
    printf_log(LOG_SEVERITY_INFO,
              "TerminateRendererProcessesAndCopy: Looping back to check renderer process count again");
  }

  // Loop back to count again
  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(&RootWindowManager::TerminateRendererProcessesAndCopy,
                     base::Unretained(this), url, files, std::move(copy_callback),
                     std::move(completion_callback), elapsed_ms + poll_interval_ms,
                     poll_interval_ms, max_wait_time_ms, custom_process_killer),
      poll_interval_ms);
}

void RootWindowManager::CheckFileLockAndCopy(
    const std::string& url,
    const std::vector<cef_query_handler::FileCopyInfo>& files,
    base::OnceCallback<void()> copy_callback,
    base::OnceCallback<void(scoped_refptr<RootWindow>)> completion_callback,
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

void RootWindowManager::OnCopyFilesAndCreateWindow(
    const std::string& url,
    const std::vector<cef_query_handler::FileCopyInfo>& files,
    base::OnceCallback<void()> copy_callback,
    base::OnceCallback<void(scoped_refptr<RootWindow>)> completion_callback) {
  CEF_REQUIRE_UI_THREAD();
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Copying files...");

  // Execute the copy callback
  if (copy_callback) {
    std::move(copy_callback).Run();
  }

  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Creating new window...");

  // Create new window
  auto config = std::make_unique<RootWindowConfig>();
  config->with_controls = true;
  config->with_osr = false;
  config->url = url;

  scoped_refptr<RootWindow> root_window = CreateRootWindow(std::move(config));
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: New window created with URL: %s",
            (url.empty() ? "[default]" : url.c_str()));

  // Step 4: Re-enable termination
  SetDisableTerminationInternal(false);
  printf_log(LOG_SEVERITY_INFO, "ExecuteHotReload: Termination re-enabled");

  // Step 5: Call completion callback
  if (completion_callback && root_window.get()) {
    // Delay to ensure browser is fully initialized
    const int kReloadDelayMs = 500;
    CefPostDelayedTask(
        TID_UI,
        base::BindOnce(std::move(completion_callback), root_window),
        kReloadDelayMs);
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
