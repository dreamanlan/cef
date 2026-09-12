// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_MYAPP_CEFCLIENT_BROWSER_TABBED_ROOT_WINDOW_VIEWS_H_
#define CEF_MYAPP_CEFCLIENT_BROWSER_TABBED_ROOT_WINDOW_VIEWS_H_
#pragma once

#include <memory>
#include <set>
#include <vector>

#include "myapp/cefclient/browser/root_window_views.h"

namespace client {

class Tab;

// Owns the main-thread content registry. Additional view creation and
// activation are connected separately from browser lifespan bookkeeping.
class TabbedRootWindowViews : public RootWindowViews {
 public:
  explicit TabbedRootWindowViews(bool use_alloy_style);
  ~TabbedRootWindowViews() override;

  TabbedRootWindowViews(const TabbedRootWindowViews&) = delete;
  TabbedRootWindowViews& operator=(const TabbedRootWindowViews&) = delete;

  // Initialize as a detached top-level window adopting |tab| and its
  // existing browser (MULTITAB_WINDOW_DESIGN.md M3). Called by the
  // RootWindowManager on the main thread; the window shell is created
  // asynchronously on the UI thread afterwards.
  void InitDetached(RootWindow::Delegate* delegate,
                    std::shared_ptr<Tab> tab,
                    const CefBrowserSettings& settings,
                    const CefRect& bounds);

  bool HasBrowser(int browser_id) const override;
  void OnTabBrowserCreated(Tab* tab,
                           CefRefPtr<CefBrowser> browser) override;
  void OnTabBrowserClosed(Tab* tab,
                          CefRefPtr<CefBrowser> browser) override;

  // Main-thread cached state notification.
  void OnTabStateChanged(Tab* tab) override;

  // ViewsWindow::Delegate methods, called on the UI thread.
  bool OnCloseRequested(bool force) override;
  bool OnNewTabRequested(const std::string& url) override;
  bool OnTabSnapshotRequested() override;
  bool OnTabReorderRequested(int browser_id, int before_id) override;
  bool OnTabDetachRequested(int browser_id) override;
  bool OnTabCommandRequested(const std::string& action,
                             int browser_id) override;
  bool OnWindowDragRequested(bool allow_merge) override;
  TabbedRootWindowViews* AsTabbedRootWindow() override { return this; }

  // Tab merge (MULTITAB_WINDOW_DESIGN.md M4a + M4b): move every tab of this
  // window (single- or multi-tab shell) into |target_root| at the drop
  // position. Called by TabDragController on the UI thread when a drag
  // session ends over the target's tab bar.
  bool MergeDraggedTabInto(TabbedRootWindowViews* target_root, int before_id);
  bool OnTabBrowserCloseApproved(Tab* tab,
                                 CefRefPtr<CefBrowser> browser) override;
  void OnBrowserViewCreated(
      CefRefPtr<CefBrowserView> browser_view) override;
  void OnBrowserViewDestroyed(
      CefRefPtr<CefBrowserView> browser_view) override;
  void OnViewsWindowDestroyed(CefRefPtr<ViewsWindow> window) override;

 protected:
  CefRefPtr<ClientHandler> CreateContentClientHandler(
      bool with_controls, const std::string& url) override;

 private:
  // Serialize on main; only immutable JSON crosses to UI.
  void PublishTabSnapshot();
  void DeliverTabSnapshot(const std::string& json);
  void ApplyTabOrder(const std::vector<int>& browser_ids);

  // Bootstrap reference, assigned before publishing the initial handler.
  // Retained until root destruction, including aborted popup creation.
  // Do not mutate this shared_ptr after initialization.
  std::shared_ptr<Tab> tab_;

  // Initialized before publishing the first handler, then read on main only.
  bool with_controls_ = false;
  void CreateNewTab(const std::string& url, unsigned int generation);
  void AttachNewTab(std::shared_ptr<Tab> tab, const std::string& url,
                    unsigned int generation);
  void FinishNewTab(std::shared_ptr<Tab> tab,
                    CefRefPtr<CefBrowser> active_browser,
                    bool browser_created);

  // UI-thread cancellation token. A close request cancels older queued creates,
  // without preventing future requests after a canceled beforeunload.
  unsigned int new_tab_generation_ = 0;

  // UI-thread association. Additional tabs must register before attachment.
  bool RegisterBrowserView(const std::shared_ptr<Tab>& tab,
                           CefRefPtr<CefBrowserView> browser_view);
  void ReleaseViewTab(std::shared_ptr<Tab> tab);

  // Deferred UI teardown and main-thread compatibility state updates.
  void DetachApprovedBrowser(CefRefPtr<CefBrowser> browser);
  void UpdateActiveTab(CefRefPtr<CefBrowser> browser);
  void CloseEmptyWindow();
  std::set<int> approved_close_ids_;
  bool all_browsers_closed_ = false;

  // Tab detach (MULTITAB_WINDOW_DESIGN.md M3): move a tab into a new
  // top-level window that adopts its existing browser view.
  void BeginDetachOnMain(std::shared_ptr<Tab> tab, const CefRect& bounds);
  void ContinueDetachedInit();
  void CreateDetachedViewsWindow();
  void FinishDetachedAdoption();
  void AbortDetachedWindow();

  // Main-thread atomic tail of the merge: registries, owner re-pointing and
  // snapshots, with both roots kept alive across the thread hop. |anchored|
  // mirrors the UI-side placement so both registries keep the same order.
  void CompleteTabMerge(std::vector<std::shared_ptr<Tab>> tabs,
                        std::shared_ptr<Tab> active_tab,
                        scoped_refptr<TabbedRootWindowViews> target_root,
                        int before_id,
                        bool anchored);

  // Detached-adoption state. Seeded by InitDetached on the main thread
  // before the UI continuation is posted.
  CefBrowserSettings detached_settings_;
  CefRect detached_bounds_;
  // Keeps an aborted adoption alive until the final browser callback.
  scoped_refptr<TabbedRootWindowViews> detach_keep_alive_;

  // Keep each associated Tab alive until its UI view reference is cleared.
  // Release these shared references on the main thread, never on the UI thread.
  std::vector<std::shared_ptr<Tab>> view_tabs_;
  bool initial_view_associated_ = false;

  // Seeded during initialization, then accessed only on the main thread.
  // UI callbacks must not iterate or mutate this registry.
  std::vector<std::shared_ptr<Tab>> tabs_;
};

}  // namespace client

#endif  // CEF_MYAPP_CEFCLIENT_BROWSER_TABBED_ROOT_WINDOW_VIEWS_H_
