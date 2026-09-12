// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/tab.h"

#include <utility>

#include "include/base/cef_callback.h"
#include "include/cef_parser.h"
#include "myapp/cefclient/browser/root_window_views.h"

namespace client {

Tab::Tab(RootWindowViews* owner, bool with_controls,
         const std::string& startup_url)
    : owner_(owner),
      use_alloy_style_(owner->IsAlloyStyle()) {
  DCHECK(owner_);
  state_.url = startup_url;
  client_handler_ = new ClientHandlerStd(this, with_controls, startup_url);
}

Tab::~Tab() {
  REQUIRE_MAIN_THREAD();
  // Aborted popup creation may never deliver OnBrowserClosed().
  // The handler can outlive the owning window, so detach before releasing it.
  if (client_handler_ && client_handler_->delegate() == this) {
    client_handler_->DetachDelegate();
  }
}

CefRefPtr<ClientHandler> Tab::GetClientHandler() const {
  return client_handler_;
}

CefRefPtr<CefBrowser> Tab::GetBrowser() const {
  REQUIRE_MAIN_THREAD();
  return browser_;
}

const Tab::State& Tab::GetState() const {
  REQUIRE_MAIN_THREAD();
  return state_;
}

void Tab::ReplayState() {
  REQUIRE_MAIN_THREAD();
  const State state = state_;
  DispatchToOwner([state](ClientHandler::Delegate* owner) {
    owner->OnSetLoadingState(state.is_loading, state.can_go_back,
                             state.can_go_forward);
    owner->OnSetAddress(state.url);
    owner->OnSetTitle(state.title);
    owner->OnSetFavicon(state.favicon);
    owner->OnSetDraggableRegions(state.draggable_regions);
  });
}

void Tab::SetBrowserView(CefRefPtr<CefBrowserView> browser_view) {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(!browser_view || !browser_view_);
  browser_view_ = browser_view;
}

CefRefPtr<CefBrowserView> Tab::GetBrowserView() const {
  CEF_REQUIRE_UI_THREAD();
  return browser_view_;
}

void Tab::SetOwner(RootWindowViews* owner) {
  REQUIRE_MAIN_THREAD();
  DCHECK(owner);
  owner_ = owner;
}

ClientHandler::Delegate* Tab::GetOwnerDelegate() const {
  return static_cast<ClientHandler::Delegate*>(owner_);
}

void Tab::DispatchToOwner(
    std::function<void(ClientHandler::Delegate*)> callback) {
  REQUIRE_MAIN_THREAD();
  if (!browser_) {
    return;
  }
  // Capture browser identity instead of extending the Tab lifetime across threads.
  CefPostTask(
      TID_UI,
      base::BindOnce(
          [](scoped_refptr<RootWindowViews> owner,
             CefRefPtr<CefBrowser> browser,
             std::function<void(ClientHandler::Delegate*)> callback) {
            if (owner->IsActiveTabBrowser(browser)) {
              callback(static_cast<ClientHandler::Delegate*>(owner.get()));
            }
            // Preserve the normal-path owner handoff; shutdown delivery is not guaranteed.
            if (!CURRENTLY_ON_MAIN_THREAD()) {
              MAIN_POST_CLOSURE(base::BindOnce(
                  [](scoped_refptr<RootWindowViews> owner) {
                    REQUIRE_MAIN_THREAD();
                  },
                  std::move(owner)));
            }
          },
          scoped_refptr<RootWindowViews>(owner_), browser_,
          std::move(callback)));
}

RootWindowViews* Tab::GetViewsRootWindow() {
  CEF_REQUIRE_UI_THREAD();
  return owner_;
}

void Tab::OnBrowserCreated(CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();
  DCHECK(!browser_);
  browser_ = browser;
  owner_->OnTabBrowserCreated(this, browser);
}

void Tab::OnBrowserClosing(CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();
  owner_->OnTabBrowserClosing(this, browser);
}

bool Tab::OnBrowserCloseApproved(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  return owner_->OnTabBrowserCloseApproved(this, browser);
}

void Tab::OnBrowserClosed(CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();
  // Retain both objects so removing this Tab from its owner's container
  // cannot destroy the delegate while its final callback is still running.
  scoped_refptr<RootWindowViews> owner = owner_;
  std::shared_ptr<Tab> self = shared_from_this();
  if (browser_) {
    DCHECK_EQ(browser_->GetIdentifier(), browser->GetIdentifier());
    browser_ = nullptr;
  }

  // Detach this tab's handler before the owner can remove the tab or
  // complete the window's browser-side teardown.
  if (client_handler_ && client_handler_->delegate() == this) {
    client_handler_->DetachDelegate();
  }
  owner_->OnTabBrowserClosed(this, browser);
  client_handler_ = nullptr;
}

void Tab::OnSetAddress(const std::string& url) {
  REQUIRE_MAIN_THREAD();
  // A real navigation invalidates the previous page's icon: without this a
  // page without favicon declarations would keep showing the prior page's
  // icon forever. ReplayState() bypasses this method, so replayed state is
  // not wiped (M1_M5 review, M5 item 1).
  if (url != state_.url &&
      (state_.favicon || !state_.favicon_data_url.empty())) {
    state_.favicon = nullptr;
    state_.favicon_data_url.clear();
  }
  state_.url = url;
  owner_->OnTabStateChanged(this);
  DispatchToOwner([url](ClientHandler::Delegate* owner) {
    owner->OnSetAddress(url);
  });
}

void Tab::OnSetTitle(const std::string& title) {
  REQUIRE_MAIN_THREAD();
  state_.title = title;
  owner_->OnTabStateChanged(this);
  DispatchToOwner([title](ClientHandler::Delegate* owner) {
    owner->OnSetTitle(title);
  });
}

void Tab::OnSetFavicon(CefRefPtr<CefImage> image) {
  REQUIRE_MAIN_THREAD();
  state_.favicon = image;
  state_.favicon_data_url.clear();
  if (image && !image->IsEmpty()) {
    // Serialize once as a PNG data URL; tab snapshots embed the string.
    int pixel_width = 0;
    int pixel_height = 0;
    const auto png =
        image->GetAsPNG(1.0f, true, pixel_width, pixel_height);
    const size_t size = png ? png->GetSize() : 0U;
    if (size > 0U) {
      std::vector<uint8_t> bytes(size);
      if (png->GetData(bytes.data(), size, 0U)) {
        state_.favicon_data_url = "data:image/png;base64," +
            CefBase64Encode(bytes.data(), bytes.size()).ToString();
      }
    }
  }
  owner_->OnTabStateChanged(this);
  DispatchToOwner([image](ClientHandler::Delegate* owner) {
    owner->OnSetFavicon(image);
  });
}

void Tab::OnSetFullscreen(bool fullscreen) {
  REQUIRE_MAIN_THREAD();
  if (!use_alloy_style_) {
    GetOwnerDelegate()->OnSetFullscreen(fullscreen);
    return;
  }
  DispatchToOwner([fullscreen](ClientHandler::Delegate* owner) {
    owner->OnSetFullscreen(fullscreen);
  });
}

void Tab::OnAutoResize(const CefSize& new_size) {
  REQUIRE_MAIN_THREAD();
  if (!use_alloy_style_) {
    GetOwnerDelegate()->OnAutoResize(new_size);
    return;
  }
  DispatchToOwner([new_size](ClientHandler::Delegate* owner) {
    owner->OnAutoResize(new_size);
  });
}

void Tab::OnContentsBounds(const CefRect& new_bounds) {
  REQUIRE_MAIN_THREAD();
  if (!use_alloy_style_) {
    GetOwnerDelegate()->OnContentsBounds(new_bounds);
    return;
  }
  DispatchToOwner([new_bounds](ClientHandler::Delegate* owner) {
    owner->OnContentsBounds(new_bounds);
  });
}

void Tab::OnSetLoadingState(bool is_loading,
                            bool can_go_back,
                            bool can_go_forward) {
  REQUIRE_MAIN_THREAD();
  state_.is_loading = is_loading;
  state_.can_go_back = can_go_back;
  state_.can_go_forward = can_go_forward;
  owner_->OnTabStateChanged(this);
  DispatchToOwner(
      [is_loading, can_go_back, can_go_forward](ClientHandler::Delegate* owner) {
        owner->OnSetLoadingState(is_loading, can_go_back, can_go_forward);
      });
}

void Tab::OnSetDraggableRegions(
    const std::vector<CefDraggableRegion>& regions) {
  REQUIRE_MAIN_THREAD();
  state_.draggable_regions = regions;
  DispatchToOwner([regions](ClientHandler::Delegate* owner) {
    owner->OnSetDraggableRegions(regions);
  });
}

bool Tab::OnSetFocus(cef_focus_source_t source) {
  CEF_REQUIRE_UI_THREAD();
  if (use_alloy_style_ && !owner_->IsActiveTabBrowserView(browser_view_)) {
    return true;
  }
  return GetOwnerDelegate()->OnSetFocus(source);
}

void Tab::OnTakeFocus(bool next) {
  REQUIRE_MAIN_THREAD();
  if (!use_alloy_style_) {
    GetOwnerDelegate()->OnTakeFocus(next);
    return;
  }
  DispatchToOwner([next](ClientHandler::Delegate* owner) {
    owner->OnTakeFocus(next);
  });
}

void Tab::OnBeforeContextMenu(CefRefPtr<CefMenuModel> model) {
  CEF_REQUIRE_UI_THREAD();
  GetOwnerDelegate()->OnBeforeContextMenu(model);
}

bool Tab::GetRootWindowScreenRect(CefRect& rect) {
  CEF_REQUIRE_UI_THREAD();
  return GetOwnerDelegate()->GetRootWindowScreenRect(rect);
}

}  // namespace client
