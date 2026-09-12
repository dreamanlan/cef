// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_MYAPP_CEFCLIENT_BROWSER_TAB_H_
#define CEF_MYAPP_CEFCLIENT_BROWSER_TAB_H_
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/views/cef_browser_view.h"
#include "myapp/cefclient/browser/client_handler_std.h"

namespace client {

class RootWindowViews;

// Owns one content handler and its browser state. The owning root window
// keeps this delegate alive through the final browser callback.
// Tab migration reassigns the owning window on the main thread.
class Tab : public ClientHandler::Delegate,
            public std::enable_shared_from_this<Tab> {
 public:
  struct State {
    std::string url;
    std::string title;
    CefRefPtr<CefImage> favicon;
    // Serialized PNG data URL of |favicon|, computed once per favicon change
    // so tab snapshots never re-encode the image (main thread only).
    std::string favicon_data_url;
    bool is_loading = false;
    bool can_go_back = false;
    bool can_go_forward = false;
    std::vector<CefDraggableRegion> draggable_regions;
  };

  // Initialization may run on any thread, before the handler is published.
  Tab(RootWindowViews* owner, bool with_controls,
      const std::string& startup_url);
  ~Tab() override;

  Tab(const Tab&) = delete;
  Tab& operator=(const Tab&) = delete;

  CefRefPtr<ClientHandler> GetClientHandler() const;

  // Main-thread access only.
  CefRefPtr<CefBrowser> GetBrowser() const;
  const State& GetState() const;
  // Main-thread access only. Reassign the owner when this tab migrates.
  void SetOwner(RootWindowViews* owner);

  // Replay a main-thread snapshot after activation on the UI thread.
  void ReplayState();

  // UI-thread access only. Release the view when its window is destroyed.
  void SetBrowserView(CefRefPtr<CefBrowserView> browser_view);
  CefRefPtr<CefBrowserView> GetBrowserView() const;

  // ClientHandler::Delegate methods:
  bool UseViews() const override { return true; }
  bool UseAlloyStyle() const override { return use_alloy_style_; }
  RootWindowViews* GetViewsRootWindow() override;
  void OnBrowserCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBrowserClosing(CefRefPtr<CefBrowser> browser) override;
  bool OnBrowserCloseApproved(
      CefRefPtr<CefBrowser> browser) override;
  void OnBrowserClosed(CefRefPtr<CefBrowser> browser) override;
  void OnSetAddress(const std::string& url) override;
  void OnSetTitle(const std::string& title) override;
  void OnSetFavicon(CefRefPtr<CefImage> image) override;
  void OnSetFullscreen(bool fullscreen) override;
  void OnAutoResize(const CefSize& new_size) override;
  void OnContentsBounds(const CefRect& new_bounds) override;
  void OnSetLoadingState(bool is_loading,
                         bool can_go_back,
                         bool can_go_forward) override;
  void OnSetDraggableRegions(
      const std::vector<CefDraggableRegion>& regions) override;
  bool OnSetFocus(cef_focus_source_t source) override;
  void OnTakeFocus(bool next) override;
  void OnBeforeContextMenu(CefRefPtr<CefMenuModel> model) override;
  bool GetRootWindowScreenRect(CefRect& rect) override;

 private:
  // Keep cached state on the main thread; filter window updates on the UI thread.
  void DispatchToOwner(
      std::function<void(ClientHandler::Delegate*)> callback);

  ClientHandler::Delegate* GetOwnerDelegate() const;

  // Non-owning to avoid a cycle. The window owns this Tab.
  // Reassigned on the main thread when this tab migrates to another window.
  RootWindowViews* owner_;
  const bool use_alloy_style_;
  CefRefPtr<ClientHandlerStd> client_handler_;

  // Main-thread state.
  CefRefPtr<CefBrowser> browser_;
  State state_;

  // UI-thread state.
  CefRefPtr<CefBrowserView> browser_view_;
};

}  // namespace client

#endif  // CEF_MYAPP_CEFCLIENT_BROWSER_TAB_H_
