// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/tab_drag_controller.h"

#if defined(OS_WIN)

#include <string>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "myapp/cefclient/browser/tabbed_root_window_views.h"
#include "myapp/cefclient/browser/views_window.h"
#include "myapp/cefclient/hostclr/HostCLR.h"

namespace client {

// static
TabDragController& TabDragController::GetInstance() {
  // Leaked on purpose: no exit-time destructor is allowed, and the singleton
  // must outlive every window anyway (same pattern as LiveViewsWindows()).
  static auto* const instance = new TabDragController();
  return *instance;
}

bool TabDragController::Start(TabbedRootWindowViews* source_root,
                              CefRefPtr<CefBrowser> browser,
                              bool allow_merge,
                              bool tear_off) {
  CEF_REQUIRE_UI_THREAD();
  if (dragged_hwnd_ || !source_root || !browser || !browser->GetHost()) {
    return false;
  }
  const HWND content_hwnd = browser->GetHost()->GetWindowHandle();
  const HWND root_hwnd =
      content_hwnd ? ::GetAncestor(content_hwnd, GA_ROOT) : nullptr;
  if (!root_hwnd) {
    return false;
  }

  POINT cursor{};
  RECT bounds{};
  if (!::GetCursorPos(&cursor) || !::GetWindowRect(root_hwnd, &bounds)) {
    return false;
  }

  source_root_ = source_root;
  dragged_browser_ = browser;
  dragged_hwnd_ = root_hwnd;
  allow_merge_ = allow_merge;
  grab_offset_.x = cursor.x - bounds.left;
  grab_offset_.y = cursor.y - bounds.top;

  // Chrome-style tear-off anchoring: the detach gesture drags the tab down
  // 24px+ before the new shell exists, so the cursor starts below the strip
  // and would end up hovering the page area. When that happens, re-anchor
  // the grab point to the strip's vertical center so the cursor keeps
  // "holding the tab". Tab gestures that start inside the strip keep their
  // natural offset.
  // Strip geometry here must NOT come from the tab bar view's screen
  // bounds: this query targets a window created milliseconds ago, whose
  // layout has not settled — GetBoundsInScreen() intermittently returns
  // whole-window bounds at that point (observed: 14 tear-offs, only the
  // first anchored). The pinned height (GetTabbarHeightDip) plus the
  // window's own rect is deterministic.
  if (auto source_window = source_root->GetViewsWindow()) {
    const UINT dpi = ::GetDpiForWindow(root_hwnd);
    const double scale = dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
    const int strip_height =
        static_cast<int>(source_window->GetTabbarHeightDip() * scale);
    // The detach path always re-anchors: its 24 CSS px threshold can leave
    // the cursor inside the strip band when the shell materializes (fast
    // creation), so the position check would silently skip it. Other paths
    // keep the natural offset while the cursor is on the strip.
    if (strip_height > 0 &&
        (tear_off || cursor.y > bounds.top + strip_height)) {
      grab_offset_.y = strip_height / 2;
      // Horizontal re-anchor too: the torn tab is re-laid-out to the strip's
      // left inset in the new window, so the source-window x offset no
      // longer points at it (it would land on the blank strip area to the
      // right of the single tab). Anchor near the tab's left-center: row
      // padding (8 CSS px) plus roughly half of the minimum tab width
      // (90 CSS px) stays inside the tab for any title length.
      grab_offset_.x = static_cast<int>(60 * scale);
      printf_log(LOG_SEVERITY_WARNING,
                "[drag] tear-off reanchor: grab=(%d,%d) strip_h=%d",
                grab_offset_.x, grab_offset_.y, strip_height);
      ::SetWindowPos(root_hwnd, nullptr,
                     cursor.x - grab_offset_.x, cursor.y - grab_offset_.y,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  target_window_ = nullptr;
  last_before_id_ = 0;

  // Redirect mouse input to the dragged window and take over its wndproc.
  // The message pump keeps running normally (no modal loop), so cefQuery and
  // ExecuteJavaScript round-trips with the tab bar HTML stay live.
  ::SetCapture(dragged_hwnd_);
  original_wndproc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtr(
      dragged_hwnd_, GWLP_WNDPROC,
      reinterpret_cast<LONG_PTR>(&TabDragController::DragWndProc)));

  printf_log(LOG_SEVERITY_WARNING,
            "[drag] session start: hwnd=%p root=%p", dragged_hwnd_,
            static_cast<void*>(source_root_));
  UpdateDropTarget();
  return true;
}

void TabDragController::ReportDropHover(CefRefPtr<CefBrowser> tabbar_browser,
                                        int before_id) {
  CEF_REQUIRE_UI_THREAD();
  if (!dragged_hwnd_ || !target_window_ || !tabbar_browser) {
    return;
  }
  if (tabbar_browser->IsSame(target_window_->GetTabbarBrowser().get())) {
    last_before_id_ = before_id > 0 ? before_id : 0;
  }
}

// static
LRESULT CALLBACK TabDragController::DragWndProc(HWND hwnd,
                                                UINT message,
                                                WPARAM wparam,
                                                LPARAM lparam) {
  return GetInstance().HandleMessage(hwnd, message, wparam, lparam);
}

LRESULT TabDragController::HandleMessage(HWND hwnd,
                                         UINT message,
                                         WPARAM wparam,
                                         LPARAM lparam) {
  if (!original_wndproc_ || hwnd != dragged_hwnd_) {
    // Not ours (e.g. a message raced with End()); pass it through unchanged.
    return ::DefWindowProc(hwnd, message, wparam, lparam);
  }

  switch (message) {
    case WM_MOUSEMOVE: {
      POINT cursor{};
      if (::GetCursorPos(&cursor)) {
        ::SetWindowPos(dragged_hwnd_, nullptr,
                       cursor.x - grab_offset_.x, cursor.y - grab_offset_.y,
                       0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateDropTarget();
      }
      return 0;
    }
    case WM_LBUTTONUP:
      End(true);
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        End(false);
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
      // The system revoked our capture (Alt-Tab, another window, etc.).
      if (reinterpret_cast<HWND>(lparam) != dragged_hwnd_) {
        End(false);
      }
      return 0;
    case WM_DESTROY:
      End(false);
      return 0;
    default:
      break;
  }
  return ::CallWindowProc(original_wndproc_, hwnd, message, wparam, lparam);
}

void TabDragController::UpdateDropTarget() {
  CEF_REQUIRE_UI_THREAD();
  // Blank-area window moves never merge (Chrome parity): no hover tracking,
  // no indicator, and the drop tail below skips the merge.
  if (!allow_merge_) {
    return;
  }
  POINT cursor{};
  if (!::GetCursorPos(&cursor)) {
    return;
  }

  ViewsWindow* hit = nullptr;
  double relative_x = 0.0;
  for (auto* window : ViewsWindow::GetLiveWindows()) {
    if (!window->SupportsMultipleTabs()) {
      continue;
    }
    if (window->GetTopLevelNativeHandle() == dragged_hwnd_) {
      continue;  // Never merge a window into itself.
    }
    // The tab bar browser's native hwnd covers the whole window on Windows
    // (observed 1692x981), so derive the strip rect from the view's screen
    // bounds (DIP) scaled by the window's DPI (verified at runtime:
    // 76 DIP x 1.5 scale resolved to the correct band).
    const HWND root_hwnd = window->GetTopLevelNativeHandle();
    if (!root_hwnd) {
      continue;
    }
    const UINT dpi = ::GetDpiForWindow(root_hwnd);
    const double scale = dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
    const CefRect bounds_dip = window->GetTabbarScreenBoundsDip();
    if (bounds_dip.IsEmpty()) {
      continue;
    }
    const RECT rect{
        static_cast<LONG>(bounds_dip.x * scale),
        static_cast<LONG>(bounds_dip.y * scale),
        static_cast<LONG>((bounds_dip.x + bounds_dip.width) * scale),
        static_cast<LONG>((bounds_dip.y + bounds_dip.height) * scale)};
    if (::PtInRect(&rect, cursor)) {
      hit = window;
      const int width = rect.right - rect.left;
      // Diagnostic: confirms the hit rect is the tab strip band, not the
      // whole window.
      printf_log(LOG_SEVERITY_WARNING,
                "[drag] drop target: window=%p rect=%dx%d",
                static_cast<void*>(hit), width, rect.bottom - rect.top);
      relative_x = width > 0
                       ? static_cast<double>(cursor.x - rect.left) / width
                       : 0.0;
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
  if (!dragged_hwnd_) {
    return;
  }
  const HWND hwnd = dragged_hwnd_;
  const WNDPROC proc = original_wndproc_;

  // Cache the merge inputs and reset the session BEFORE releasing capture:
  // ReleaseCapture() synchronously dispatches WM_CAPTURECHANGED through our
  // subclassed wndproc, whose End(false) re-entry must find the session
  // already closed (dragged_hwnd_ == nullptr) or it would wipe the merge
  // state this tail still needs (M1_M5 review, platform item 2).
  TabbedRootWindowViews* source_root = source_root_;
  ViewsWindow* target_window = target_window_;
  const int before_id = last_before_id_;

  dragged_hwnd_ = nullptr;
  original_wndproc_ = nullptr;
  source_root_ = nullptr;
  dragged_browser_ = nullptr;
  target_window_ = nullptr;
  last_before_id_ = 0;

  if (::GetCapture() == hwnd) {
    ::ReleaseCapture();
  }
  // Restore the original wndproc before running any merge logic that might
  // destroy the subclassed window.
  if (proc) {
    ::SetWindowLongPtr(hwnd, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR>(proc));
  }

  printf_log(LOG_SEVERITY_WARNING,
            "[drag] session end: merge=%d target=%p before_id=%d",
            perform_merge ? 1 : 0, static_cast<void*>(target_window),
            before_id);
  if (target_window) {
    PushDropHover(target_window, false, 0.0);
  }

  if (perform_merge && source_root && target_window) {
    auto* target_root = target_window->delegate()->AsTabbedRootWindow();
    if (target_root) {
      // Defer the merge out of the wndproc: it performs view surgery
      // (unparent/reparent/layout across two windows) and running that
      // inside the WM_LBUTTONUP dispatch can wedge the views framework
      // through nested message pumping. Both roots are kept alive across
      // the hop; MergeDraggedTabInto revalidates everything.
      CefPostTask(TID_UI,
                  base::BindOnce(
                      [](scoped_refptr<TabbedRootWindowViews> source,
                         scoped_refptr<TabbedRootWindowViews> target,
                         int before_id) {
                        source->MergeDraggedTabInto(target.get(), before_id);
                      },
                      scoped_refptr<TabbedRootWindowViews>(source_root),
                      scoped_refptr<TabbedRootWindowViews>(target_root),
                      before_id));
    }
  }
}

}  // namespace client

#endif  // OS_WIN
