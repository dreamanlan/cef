// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/tab_drag_controller.h"

#if defined(OS_LINUX)

#include <string>

#include "myapp/cefclient/browser/tabbed_root_window_views.h"
#include "myapp/cefclient/browser/views_window.h"

// Xlib must come last: its macros (Bool/True/False/None/Success) must not
// leak into Chromium/CEF headers included above, and none of those macro
// names may be used below.
#include <X11/Xlib.h>

namespace client {

namespace {

// Session-scoped Xlib state. The connection is kept open for the process
// lifetime once created (open/close churn per drag is pointless), and only
// one session can be active at a time.
Display* g_display = nullptr;
Window g_dragged_window = 0;
int g_grab_x = 0;
int g_grab_y = 0;

// Current pointer position in root (screen) coordinates, physical pixels.
bool RootPoint(int* x, int* y) {
  Window root_return = 0;
  Window child_return = 0;
  int win_x = 0;
  int win_y = 0;
  unsigned int mask = 0;
  return XQueryPointer(g_display, DefaultRootWindow(g_display), &root_return,
                       &child_return, x, y, &win_x, &win_y, &mask) != 0;
}

// A window's origin in root coordinates (XTranslateCoordinates walks the
// whole reparenting chain, WM frame included).
bool WindowRootOrigin(Window window, int* x, int* y) {
  Window child = 0;
  return XTranslateCoordinates(g_display, window,
                               DefaultRootWindow(g_display), 0, 0, x, y,
                               &child) != 0;
}

// The tab bar strip's rect in root coordinates.
bool TabbarRootRect(Window tabbar, int* x, int* y, unsigned int* w,
                    unsigned int* h) {
  Window root_return = 0;
  int discard_x = 0;
  int discard_y = 0;
  unsigned int discard_border = 0;
  unsigned int discard_depth = 0;
  if (!XGetGeometry(g_display, tabbar, &root_return, &discard_x, &discard_y,
                    w, h, &discard_border, &discard_depth)) {
    return false;
  }
  return WindowRootOrigin(tabbar, x, y);
}

}  // namespace

// static
TabDragController& TabDragController::GetInstance() {
  // Leaked on purpose: no exit-time destructor is allowed, and the singleton
  // must outlive every window anyway.
  static auto* const instance = new TabDragController();
  return *instance;
}

bool TabDragController::Start(TabbedRootWindowViews* source_root,
                              CefRefPtr<CefBrowser> browser,
                              bool allow_merge,
                              bool tear_off) {
  CEF_REQUIRE_UI_THREAD();
  if (dragging_ || !source_root || !browser || !browser->GetHost()) {
    return false;
  }
  auto window = source_root->GetViewsWindow();
  if (!window) {
    return false;
  }
  // The CefWindow's own X11 window is the WM's client window: moving it
  // issues a ConfigureRequest that the window manager honors by moving the
  // frame, which is what the user perceives as the window following the
  // cursor.
  const CefWindowHandle handle = window->GetTopLevelNativeHandle();
  if (!handle) {
    return false;
  }
  if (!g_display) {
    g_display = XOpenDisplay(nullptr);
  }
  if (!g_display) {
    return false;
  }
  int cursor_x = 0;
  int cursor_y = 0;
  int window_x = 0;
  int window_y = 0;
  if (!RootPoint(&cursor_x, &cursor_y) ||
      !WindowRootOrigin(handle, &window_x, &window_y)) {
    return false;
  }

  source_root_ = source_root;
  dragged_browser_ = browser;
  target_window_ = nullptr;
  last_before_id_ = 0;
  allow_merge_ = allow_merge;
  dragging_ = true;
  g_dragged_window = handle;
  g_grab_x = cursor_x - window_x;
  g_grab_y = cursor_y - window_y;

  // Chrome-style tear-off anchoring (see the Windows controller): when the
  // session begins with the cursor below the strip (the detach gesture drags
  // the tab down before the new shell exists), re-anchor the grab point to
  // the strip's vertical center. X11 coordinates are top-left origin,
  // physical pixels — same math as Windows.
  if (auto source_window = source_root->GetViewsWindow()) {
    const auto tabbar_browser = source_window->GetTabbarBrowser();
    Window strip_win = 0;
    if (tabbar_browser && tabbar_browser->GetHost() &&
        tabbar_browser->GetHost()->GetWindowHandle()) {
      strip_win = reinterpret_cast<Window>(
          tabbar_browser->GetHost()->GetWindowHandle());
    }
    int sx = 0;
    int sy = 0;
    unsigned int sw = 0;
    unsigned int sh = 0;
    if (strip_win && TabbarRootRect(strip_win, &sx, &sy, &sw, &sh) && sh > 0 &&
        (tear_off || cursor_y > sy + static_cast<int>(sh))) {
      g_grab_y = static_cast<int>(sh) / 2;
      XMoveWindow(g_display, g_dragged_window, cursor_x - g_grab_x,
                  cursor_y - g_grab_y);
      XFlush(g_display);
    }
  }

  UpdateDropTarget();
  return true;
}

void TabDragController::ReportDropHover(CefRefPtr<CefBrowser> tabbar_browser,
                                        int before_id) {
  CEF_REQUIRE_UI_THREAD();
  if (!dragging_ || !target_window_ || !tabbar_browser) {
    return;
  }
  if (tabbar_browser->IsSame(target_window_->GetTabbarBrowser().get())) {
    last_before_id_ = before_id > 0 ? before_id : 0;
  }
}

void TabDragController::NotifyDragMove() {
  CEF_REQUIRE_UI_THREAD();
  if (!dragging_ || !g_display || !g_dragged_window) {
    return;
  }
  int cursor_x = 0;
  int cursor_y = 0;
  if (!RootPoint(&cursor_x, &cursor_y)) {
    return;
  }
  XMoveWindow(g_display, g_dragged_window, cursor_x - g_grab_x,
              cursor_y - g_grab_y);
  XFlush(g_display);
  UpdateDropTarget();
}

void TabDragController::NotifyDragEnd(bool merge) {
  CEF_REQUIRE_UI_THREAD();
  End(merge);
}

void TabDragController::UpdateDropTarget() {
  CEF_REQUIRE_UI_THREAD();
  // Blank-area window moves never merge (Chrome parity).
  if (!allow_merge_) {
    return;
  }
  if (!g_display) {
    return;
  }
  int cursor_x = 0;
  int cursor_y = 0;
  if (!RootPoint(&cursor_x, &cursor_y)) {
    return;
  }

  ViewsWindow* hit = nullptr;
  double relative_x = 0.0;
  for (auto* window : ViewsWindow::GetLiveWindows()) {
    if (!window->SupportsMultipleTabs()) {
      continue;
    }
    const auto browser = window->GetTabbarBrowser();
    if (!browser || !browser->GetHost()) {
      continue;
    }
    // CefBrowserHost::GetWindowHandle() returns the X11 window of the
    // tab bar's browser view.
    const CefWindowHandle tabbar = browser->GetHost()->GetWindowHandle();
    if (!tabbar || window->GetTopLevelNativeHandle() == g_dragged_window) {
      continue;  // Never merge a window into itself.
    }
    int x = 0;
    int y = 0;
    unsigned int w = 0;
    unsigned int h = 0;
    if (!TabbarRootRect(tabbar, &x, &y, &w, &h) || w == 0U) {
      continue;
    }
    if (cursor_x >= x && cursor_x < x + static_cast<int>(w) && cursor_y >= y &&
        cursor_y < y + static_cast<int>(h)) {
      hit = window;
      relative_x = static_cast<double>(cursor_x - x) /
                   static_cast<double>(w);
      break;
    }
  }

  if (hit == target_window_) {
    if (hit) {
      PushDropHover(hit, true, relative_x);
    }
    return;
  }
  if (target_window_) {
    PushDropHover(target_window_, false, 0.0);
  }
  target_window_ = hit;
  last_before_id_ = 0;
  if (hit) {
    PushDropHover(hit, true, relative_x);
  }
}

void TabDragController::PushDropHover(ViewsWindow* window,
                                      bool active,
                                      double relative_x) {
  const auto browser = window->GetTabbarBrowser();
  const auto frame = browser ? browser->GetMainFrame() : nullptr;
  if (!frame) {
    return;
  }
  // The tab bar HTML owns the layout, so the insertion slot is computed there
  // from the normalized cursor position.
  frame->ExecuteJavaScript(
      std::string("window.__tabbarApi&&__tabbarApi.onDropHover&&"
                  "__tabbarApi.onDropHover(") +
          (active ? "true" : "false") + "," +
          std::to_string(relative_x) + ");",
      frame->GetURL(), 0);
}

void TabDragController::End(bool perform_merge) {
  CEF_REQUIRE_UI_THREAD();
  if (!dragging_) {
    return;
  }
  g_dragged_window = 0;
  g_grab_x = 0;
  g_grab_y = 0;
  dragging_ = false;

  TabbedRootWindowViews* source_root = source_root_;
  ViewsWindow* target_window = target_window_;
  const int before_id = last_before_id_;

  source_root_ = nullptr;
  dragged_browser_ = nullptr;
  target_window_ = nullptr;
  last_before_id_ = 0;

  if (target_window) {
    PushDropHover(target_window, false, 0.0);
  }

  if (perform_merge && source_root && target_window) {
    auto* target_root = target_window->delegate()->AsTabbedRootWindow();
    if (target_root) {
      source_root->MergeDraggedTabInto(target_root, before_id);
    }
  }
}

}  // namespace client

#endif  // OS_LINUX
