// Copyright (c) 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/tab_drag_controller.h"

#if defined(OS_MAC)

#import <Cocoa/Cocoa.h>

#include "myapp/cefclient/browser/tabbed_root_window_views.h"
#include "myapp/cefclient/browser/views_window.h"

namespace client {

namespace {

// Escape key code (kVK_Escape) for drag cancellation.
const unsigned short kEscapeKeyCode = 53;

// File-scope native drag state; only one session can be active at a time.
id g_monitor = nil;
NSWindow* g_dragged_window = nil;
NSPoint g_grab_offset = NSMakePoint(0, 0);

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
  NSView* content_view = nil;
  if (browser->GetHost()->GetWindowHandle()) {
    // CefBrowserHost::GetWindowHandle() returns the NSView* on macOS.
    content_view =
        reinterpret_cast<NSView*>(browser->GetHost()->GetWindowHandle());
  }
  NSWindow* window = content_view ? [content_view window] : nil;
  if (!window) {
    return false;
  }

  const NSPoint cursor = [NSEvent mouseLocation];
  const NSRect frame = [window frame];

  source_root_ = source_root;
  dragged_browser_ = browser;
  target_window_ = nullptr;
  last_before_id_ = 0;
  allow_merge_ = allow_merge;
  dragging_ = true;
  g_dragged_window = window;
  g_grab_offset =
      NSMakePoint(cursor.x - frame.origin.x, cursor.y - frame.origin.y);

  // Chrome-style tear-off anchoring (see the Windows controller): when the
  // session begins with the cursor below the strip (the detach gesture drags
  // the tab down before the new shell exists), re-anchor the grab point to
  // the strip's vertical center so the cursor keeps "holding the tab".
  // AppKit screen coordinates are bottom-left origin, and the strip sits at
  // the TOP of the window, hence frame_height - strip_height / 2. The strip
  // height comes from the pinned value, NOT the freshly created tab bar
  // view's bounds: the window is milliseconds old and its layout has not
  // settled (see the Windows controller for the observed intermittency).
  // Points equal DIPs on macOS, so no scaling is needed.
  if (auto source_window = source_root->GetViewsWindow()) {
    const int strip_height = source_window->GetTabbarHeightDip();
    const CGFloat frame_height = [window frame].size.height;
    const CGFloat frame_top = frame.origin.y + frame_height;
    // The detach path always re-anchors (see the Windows controller for the
    // fast-creation timing analysis).
    if (strip_height > 0 &&
        (tear_off || cursor.y < frame_top - strip_height)) {
      g_grab_offset.y = frame_height - strip_height / 2.0;
      // Horizontal re-anchor too: the torn tab is re-laid-out to the strip's
      // left inset, so the source x offset would land on the blank strip
      // right of the tab.
      g_grab_offset.x = 60.0;
      [window setFrameOrigin:NSMakePoint(cursor.x - g_grab_offset.x,
                                         cursor.y - g_grab_offset.y)];
    }
  }

  // A local event monitor sees every event dispatched to this application
  // before delivery, so we can consume drag events (keeping them away from
  // the renderer view) while the normal run loop keeps pumping Chromium
  // tasks. This is the macOS equivalent of the Windows SetCapture drag loop.
  TabDragController* controller = this;
  g_monitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSLeftMouseDraggedMask |
                                            NSLeftMouseUpMask |
                                            NSKeyDownMask)
                                        handler:^NSEvent*(NSEvent* event) {
                                          if (!controller->is_dragging()) {
                                            return event;
                                          }
                                          const NSEventType type =
                                              [event type];
                                          if (type == NSLeftMouseDragged) {
                                            const NSPoint loc =
                                                [NSEvent mouseLocation];
                                            if (g_dragged_window) {
                                              [g_dragged_window
                                                  setFrameOrigin:NSMakePoint(
                                                      loc.x - g_grab_offset.x,
                                                      loc.y -
                                                          g_grab_offset.y)];
                                            }
                                            controller->UpdateDropTarget();
                                            // Consume: the renderer view
                                            // must not keep the gesture.
                                            return nil;
                                          }
                                          if (type == NSLeftMouseUp) {
                                            // End before the event continues,
                                            // then let the mouse-up through
                                            // so the renderer view's tracking
                                            // stays balanced.
                                            controller->End(true);
                                            return event;
                                          }
                                          if (type == NSKeyDown &&
                                              [event keyCode] ==
                                                  kEscapeKeyCode) {
                                            controller->End(false);
                                            return event;
                                          }
                                          return event;
                                        }];
  if (!g_monitor) {
    dragging_ = false;
    g_dragged_window = nil;
    source_root_ = nullptr;
    dragged_browser_ = nullptr;
    return false;
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

void TabDragController::UpdateDropTarget() {
  CEF_REQUIRE_UI_THREAD();
  // Blank-area window moves never merge (Chrome parity).
  if (!allow_merge_) {
    return;
  }
  const NSPoint cursor = [NSEvent mouseLocation];

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
    NSView* tabbar_view = nil;
    if (browser->GetHost()->GetWindowHandle()) {
      tabbar_view = reinterpret_cast<NSView*>(
          browser->GetHost()->GetWindowHandle());
    }
    if (!tabbar_view) {
      continue;
    }
    NSWindow* tabbar_ns_window = [tabbar_view window];
    if (!tabbar_ns_window || tabbar_ns_window == g_dragged_window) {
      continue;  // Never merge a window into itself.
    }
    // Strip bounds in screen coordinates (points, bottom-left origin).
    const NSRect rect = [tabbar_ns_window convertRectToScreen:[tabbar_view
        convertRect:[tabbar_view bounds] toView:nil]];
    if (NSPointInRect(cursor, rect)) {
      hit = window;
      relative_x =
          rect.size.width > 0
              ? (cursor.x - rect.origin.x) / rect.size.width
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
  if (!dragging_) {
    return;
  }
  if (g_monitor) {
    [NSEvent removeMonitor:g_monitor];
    g_monitor = nil;
  }
  g_dragged_window = nil;
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
      // Defer the merge out of the event-monitor dispatch: view surgery
      // (unparent/reparent/layout across two windows) inside an event
      // handler can wedge the views framework through nested dispatch.
      // Both roots are kept alive across the hop; MergeDraggedTabInto
      // revalidates everything.
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

#endif  // OS_MAC
