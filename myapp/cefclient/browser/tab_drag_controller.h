// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_MYAPP_CEFCLIENT_BROWSER_TAB_DRAG_CONTROLLER_H_
#define CEF_MYAPP_CEFCLIENT_BROWSER_TAB_DRAG_CONTROLLER_H_
#pragma once

// Must come BEFORE the platform guard below: OS_WIN/OS_MAC/OS_LINUX are
// defined by this header in the CEF source build (they are NOT compiler
// command-line defines here). Including it inside the guard would be a
// chicken-and-egg hole: the guard evaluates first and skips everything.
#include "include/base/cef_build.h"

#if defined(OS_WIN) || defined(OS_MAC) || defined(OS_LINUX)

#include <string>

#if defined(OS_WIN)
#include <windows.h>
#endif

#include "include/base/cef_scoped_refptr.h"
#include "include/cef_browser.h"

namespace client {

class TabbedRootWindowViews;
class ViewsWindow;

// Owns one tab tear-off / merge drag session (MULTITAB_WINDOW_DESIGN.md 6.5).
//
// Replaces the SC_MOVE modal loop used by M3: a native modal loop does not
// pump Chromium tasks, so cefQuery / ExecuteJavaScript round-trips needed for
// the merge drop-target indicator would stall until the mouse is released.
// This controller instead takes over event delivery and pumps messages
// normally while following the cursor:
//  - Windows: SetCapture + GWLP_WNDPROC subclassing (tab_drag_controller_win.cc)
//  - macOS:   NSEvent local monitor (tab_drag_controller_mac.mm)
//  - Linux:   JS-fed loop (tab_drag_controller_linux.cc). X11 has no
//             cross-window capture, but the implicit pointer grab keeps
//             delivering mousemove to the gesture's page even outside the
//             window, so the HTML signals each move ("windowdragmove") and
//             the controller queries the native cursor (XQueryPointer) and
//             moves the window (XMoveWindow via the WM's ConfigureRequest).
//
// UI thread only (the browser process UI thread owns the native windows).
class TabDragController {
 public:
  static TabDragController& GetInstance();

  // Begin dragging the shell of |source_root| around the cursor. |browser| is
  // any content browser hosted by |source_root| (used only to resolve the
  // native window); the session covers the whole window, so both one-tab
  // shells (torn-off windows) and multi-tab whole-window drags (M4b) are
  // supported. |allow_merge| mirrors Chrome's gesture semantics: tab drags
  // may merge into another window's tab bar, blank-area window moves may
  // not. |tear_off| marks the detach path, whose grab point is always
  // re-anchored onto the strip (the detach threshold is only 24 CSS px, so
  // the cursor may still be inside the strip band when the shell appears).
  // Returns false when a session is already active or the required
  // native handles are unavailable.
  bool Start(TabbedRootWindowViews* source_root,
             CefRefPtr<CefBrowser> browser,
             bool allow_merge,
             bool tear_off = false);

  // Drop-hover position reported by the target tab bar HTML (action
  // "drophover"). |tabbar_browser| identifies the reporting tab bar; it is
  // ignored unless it belongs to the current drop target.
  void ReportDropHover(CefRefPtr<CefBrowser> tabbar_browser, int before_id);

#if defined(OS_LINUX)
  // JS-fed loop callbacks (actions "windowdragmove" / "windowdragend").
  // Each move signal triggers a native cursor query + window move; the end
  // signal finishes the session, merging when a target is hovered and
  // |merge| is true (cancel paths pass false).
  void NotifyDragMove();
  void NotifyDragEnd(bool merge);
#endif

#if defined(OS_WIN)
  bool is_dragging() const { return dragged_hwnd_ != nullptr; }
#elif defined(OS_MAC) || defined(OS_LINUX)
  bool is_dragging() const { return dragging_; }
#endif

 private:
  TabDragController() = default;
  ~TabDragController() = default;

  // Hit-test every other live window's tab bar; push indicator updates to the
  // HTML when the hovered target changes.
  void UpdateDropTarget();

  // Stop the session. |perform_merge| merges into the current target when one
  // is hovered and validates.
  void End(bool perform_merge);

  void PushDropHover(ViewsWindow* window, bool active, double relative_x);

  TabbedRootWindowViews* source_root_ = nullptr;  // Not owned.
  CefRefPtr<CefBrowser> dragged_browser_;
  ViewsWindow* target_window_ = nullptr;  // Not owned; current hover target.
  int last_before_id_ = 0;
  bool allow_merge_ = false;

#if defined(OS_WIN)
  // Subclassed window procedure and its dispatch helper.
  static LRESULT CALLBACK DragWndProc(HWND hwnd,
                                      UINT message,
                                      WPARAM wparam,
                                      LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  HWND dragged_hwnd_ = nullptr;
  WNDPROC original_wndproc_ = nullptr;
  POINT grab_offset_ = {0, 0};
#elif defined(OS_MAC) || defined(OS_LINUX)
  bool dragging_ = false;
  // Native drag state (monitor token / NSWindow on macOS; Display*, X11
  // window and grab offset on Linux) lives in the platform .mm/.cc as
  // file-scope storage; only one session can be active at a time.
#endif

  TabDragController(const TabDragController&) = delete;
  TabDragController& operator=(const TabDragController&) = delete;
};

}  // namespace client

#endif  // OS_WIN || OS_MAC || OS_LINUX

#endif  // CEF_MYAPP_CEFCLIENT_BROWSER_TAB_DRAG_CONTROLLER_H_
