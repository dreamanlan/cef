// Copyright (c) 2015 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_ROOT_WINDOW_MANAGER_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_ROOT_WINDOW_MANAGER_H_
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "include/cef_command_line.h"
#include "include/cef_request_context_handler.h"
#include "myapp/cefclient/browser/cef_query_handler.h"
#include "myapp/cefclient/browser/image_cache.h"
#include "myapp/cefclient/browser/root_window.h"
#include "myapp/cefclient/browser/temp_window.h"

namespace client {

// Used to create/manage RootWindow instances. The methods of this class can be
// called from any browser process thread unless otherwise indicated.
class RootWindowManager : public RootWindow::Delegate {
 public:
  // If |terminate_when_all_windows_closed| is true quit the main message loop
  // after all windows have closed.
  explicit RootWindowManager(bool terminate_when_all_windows_closed);

  RootWindowManager(const RootWindowManager&) = delete;
  RootWindowManager& operator=(const RootWindowManager&) = delete;

  // Create a new top-level native window. This method can be called from
  // anywhere.
  scoped_refptr<RootWindow> CreateRootWindow(
      std::unique_ptr<RootWindowConfig> config);

  // Host a Chrome-style browser in a Chrome self-created native (tabstrip)
  // top-level window at |url|, instead of a client RootWindow. The browser is
  // tracked via OtherBrowserCreated/Closed and drives termination through the
  // same counter. This method can be called from anywhere.
  void CreateChromeWindow(const std::string& url);

  // Same as above, but |created_callback| runs with the new browser once Chrome
  // has actually created it (CefBrowserHost::CreateBrowser is asynchronous, so
  // the caller cannot just read a return value). Used by the hot reload flow,
  // which needs the browser to report completion.
  void CreateChromeWindow(
      const std::string& url,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> created_callback);

  // Create a new native popup window.
  // If |with_controls| is true the window will show controls.
  // If |with_osr| is true the window will use off-screen rendering.
  // This method is called from ClientHandler::CreatePopupWindow() to
  // create a new popup or DevTools window. Must be called on the UI thread.
  scoped_refptr<RootWindow> CreateRootWindowAsPopup(
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
      bool adopt_as_tab = false);

  // Abort or close the popup matching the specified identifiers. If |popup_id|
  // is -1 then all popups for |opener_browser_id| will be impacted.
  void AbortOrClosePopup(int opener_browser_id, int popup_id);

  // Returns the RootWindow associated with the specified browser ID. Must be
  // called on the main thread.
  scoped_refptr<RootWindow> GetWindowForBrowser(int browser_id) const;

  // Returns the currently active/foreground RootWindow. May return nullptr.
  // Must be called on the main thread.
  scoped_refptr<RootWindow> GetActiveRootWindow() const;

  // Close all existing windows. If |force| is true onunload handlers will not
  // be executed.
  void CloseAllWindows(bool force);

  bool request_context_per_browser() const {
    return request_context_per_browser_;
  }

  // Track other browsers that are not directly associated with a RootWindow.
  // This may be an overlay browser, a popup created with `--use-default-popup`,
  // or a browser using default Chrome UI. |opener_browser_id| will be > 0 for
  // popup browsers.
  void OtherBrowserCreated(int browser_id,
                           int opener_browser_id,
                           CefRefPtr<CefBrowser> browser);
  void OtherBrowserClosed(int browser_id, int opener_browser_id);

  // Close browsers that have no RootWindow (the default Chrome self-created
  // window, and default-UI popups). CloseAllWindows() only walks
  // |root_windows_| and therefore never sees them.
  void CloseOtherBrowsers(bool force);

  // Temporarily disable termination when all windows are closed.
  // Used for hot reload functionality.
  void SetDisableTermination(bool disable);

  // Execute hot reload flow: close the browsers, wait until they are really
  // gone, copy files, and create a new window.
  void ExecuteHotReload(
      const std::string& url,
      const std::vector<cef_query_handler::FileCopyInfo>& files,
      base::OnceCallback<void()> copy_callback,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);

 private:
  // Browsers without a RootWindow, keyed by browser id. Held so hot reload can
  // close them: the default Chrome self-created window lives here and would
  // otherwise survive CloseAllWindows(), which only walks |root_windows_|.
  std::map<int, CefRefPtr<CefBrowser>> other_browsers_;

  // Completion for a CreateChromeWindow() call still waiting for Chrome to
  // create the browser (CefBrowserHost::CreateBrowser is asynchronous).
  base::OnceCallback<void(CefRefPtr<CefBrowser>)> pending_chrome_window_callback_;

  // The url the reload was asked for, kept to pick the browser that the
  // completion callback reports.
  std::string hot_reload_url_;

  // Hot reload, Windows we own (everything but the Chrome window mode): those
  // are rebuilt here instead of being restored by Chrome - one window per
  // closed window, with all of its tabs in strip order and the tab that was
  // active. The window bounds and the show state come back from the
  // preferences each window saves when it closes, so they are not recorded.
  struct HotReloadWindowPages {
    std::vector<std::string> urls;
    int active_index = 0;
  };

  // Same for the Windows we own: the windows that were closed, and the ones
  // being rebuilt.
  std::vector<HotReloadWindowPages> hot_reload_views_windows_;
  scoped_refptr<RootWindow> hot_reload_views_root_;
  scoped_refptr<RootWindow> hot_reload_views_first_root_;
  size_t hot_reload_views_index_ = 0;

  // Chrome-created windows to rebuild: the pages of the default Chrome window
  // mode, and the Chrome windows pages open in the other modes (window.open
  // targets, "new window"). They are not recorded by Chrome's own restore
  // service (CEF hosts them without the tabstrip feature Chrome requires), so
  // they are rebuilt here as well: one window per closed window, one
  // IDC_NEW_TAB command plus a LoadURL for every further tab.
  std::vector<HotReloadWindowPages> hot_reload_chrome_windows_;
  size_t hot_reload_chrome_index_ = 0;
  CefRefPtr<CefBrowser> hot_reload_chrome_host_;
  std::vector<CefRefPtr<CefBrowser>> hot_reload_new_tabs_;
  bool hot_reload_collect_new_tabs_ = false;
  // One browser per rebuilt window, either kind, for the completion callback.
  std::vector<CefRefPtr<CefBrowser>> hot_reload_result_browsers_;

  // Gives up on a pending CreateChromeWindow() completion and reports it with a
  // null browser, so the hot reload query is always answered.
  void OnChromeWindowCreateTimeout();

  // Wraps the hot reload completion for the Chrome window path: the window is
  // created asynchronously, so termination may only be re-enabled once the new
  // browser exists (otherwise the app sees "no windows" in between and quits).
  void OnHotReloadWindowReady(
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
      CefRefPtr<CefBrowser> browser);

  void RestoreNextHotReloadViewsWindow(
      size_t window_index,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);
  void RestoreNextHotReloadViewsTab(
      size_t tab_index,
      int wait_ms,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);
  void ActivateHotReloadViewsTab(
      int wait_ms,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);
  void FinishHotReloadViewsRestore(
      int wait_ms,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);

  // Rebuild of the Chrome-created windows: one CreateChromeWindow per window,
  // then IDC_NEW_TAB + LoadURL for each further tab. The tab browsers are
  // collected through OtherBrowserCreated while the rebuild runs.
  void RestoreNextChromeWindow(
      size_t window_index,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);
  void OnHotReloadChromeWindowReady(
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
      CefRefPtr<CefBrowser> browser);
  void RestoreNextChromeTab(
      size_t tab_index,
      int wait_ms,
      bool issued,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);
  void FinishHotReloadChromeRestore(
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);

  // Internal method to update disable_termination_ flag with logging.
  // All modifications to disable_termination_ should go through this method.
  void SetDisableTerminationInternal(bool disable);

  // Internal method for hot reload flow: copy files and create new window.
  void OnCopyFilesAndCreateWindow(
      const std::string& url,
      const std::vector<cef_query_handler::FileCopyInfo>& files,
      base::OnceCallback<void()> copy_callback,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback);

  // Internal method for hot reload flow: poll until the browsers that were asked
  // to close are actually gone, then copy files. Closing a browser also takes
  // its renderer process with it, so there is no separate "kill the renderers"
  // step - it is only used as a fallback when the wait times out, to make sure
  // no renderer survives holding the files.
  void WaitForBrowsersClosed(
      const std::string& url,
      const std::vector<cef_query_handler::FileCopyInfo>& files,
      base::OnceCallback<void()> copy_callback,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
      int elapsed_ms,
      int poll_interval_ms,
      int max_wait_time_ms);

  // Internal method for hot reload flow: check file lock and copy files.
  void CheckFileLockAndCopy(
      const std::string& url,
      const std::vector<cef_query_handler::FileCopyInfo>& files,
      base::OnceCallback<void()> copy_callback,
      base::OnceCallback<void(CefRefPtr<CefBrowser>)> completion_callback,
      int elapsed_ms,
      int poll_interval_ms,
      int max_wait_time_ms);

  // Allow deletion via std::unique_ptr only.
  friend std::default_delete<RootWindowManager>;

  ~RootWindowManager() override;

  void OnRootWindowCreated(scoped_refptr<RootWindow> root_window);
  void OnAbortOrClosePopup(int opener_browser_id, int popup_id);

  // RootWindow::Delegate methods.
  CefRefPtr<CefRequestContext> GetRequestContext() override;
  void GetRequestContext(RequestContextCallback callback) override;
  scoped_refptr<ImageCache> GetImageCache() override;
  void OnTest(RootWindow* root_window, int test_id) override;
  void OnExit(RootWindow* root_window) override;
  void OnRootWindowDestroyed(RootWindow* root_window) override;
  void OnRootWindowActivated(RootWindow* root_window) override;
  scoped_refptr<RootWindow> CreateDetachedWindow(
      const std::shared_ptr<Tab>& tab,
      const CefRect& bounds) override;

  // |callback| may be nullptr. Must be called on the main thread.
  CefRefPtr<CefRequestContext> CreateRequestContext(
      RequestContextCallback callback);

  void MaybeCleanup();
  void CleanupOnUIThread();

  // Helper function to count renderer processes using CefTaskManager.
  // Returns the number of renderer processes, or -1 on error.
  // Must be called on UI thread.
  static int CefCountRenderProcess();

  // Helper function to terminate renderer processes using CefTaskManager.
  // Returns the number of terminated renderer processes, or -1 on error.
  // Must be called on UI thread.
  static int CefTerminateRenderProcess();

  const bool terminate_when_all_windows_closed_;
  bool disable_termination_ = false;
  bool request_context_per_browser_;
  bool request_context_shared_cache_;

  // Existing root windows. Only accessed on the main thread.
  using RootWindowSet = std::set<scoped_refptr<RootWindow>>;
  RootWindowSet root_windows_;

  // Count of browsers that are not directly associated with a RootWindow. Only
  // accessed on the main thread.
  int other_browser_ct_ = 0;

  // Map of owner browser ID to popup browser IDs for popups that don't have a
  // RootWindow. Only accessed on the main thread.
  using BrowserIdSet = std::set<int>;
  using BrowserOwnerMap = std::map<int, BrowserIdSet>;
  BrowserOwnerMap other_browser_owners_;

  // The currently active/foreground RootWindow. Only accessed on the main
  // thread.
  scoped_refptr<RootWindow> active_root_window_;

  // Singleton window used as the temporary parent for popup browsers.
  std::unique_ptr<TempWindow> temp_window_;

  CefRefPtr<CefRequestContext> shared_request_context_;

  scoped_refptr<ImageCache> image_cache_;
};

}  // namespace client

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_ROOT_WINDOW_MANAGER_H_
