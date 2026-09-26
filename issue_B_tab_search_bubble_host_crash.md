# Chrome runtime: crash on window close — `tab_search_bubble_host_` reset too late in `BrowserView::~BrowserView()` (dangling raw_ptr)

## Environment

- CEF branches 7871 and 8037 (Chromium 152/154)
- macOS (verified; likely all platforms)
- Chrome-style window (chrome runtime, not the cefclient root window)

## Steps to reproduce

1. Open a chrome-style (chrome runtime) window.
2. Close the window, or exit the application.

## Result

Crash during teardown. `BrowserView::~BrowserView()` calls `tab_search_bubble_host_.reset()` after the toolbar child views have already been destroyed. The `WebUIBubbleManager` owned by `tab_search_bubble_host_` holds a `raw_ptr<views::View> anchor_view_` pointing at a toolbar child view (e.g. the tab search button), resulting in a dangling-pointer crash.

## Suggested fix

We carry the following locally in `chrome_runtime_views.patch`: move the reset into `WillDestroyToolbar()`, which runs before the toolbar child views are destroyed.

```diff
--- a/chrome/browser/ui/views/frame/browser_view.cc
+++ b/chrome/browser/ui/views/frame/browser_view.cc
@@ void BrowserView::~BrowserView() {
   // Remove the layout manager to avoid dangling. This needs to be earlier than
   // other cleanups that destroy views referenced in the layout manager.
   SetLayoutManager(nullptr);
 
-  tab_search_bubble_host_.reset();
-
   // Destroy the top controls slide controller first as it depends on the
   // tabstrip model and the browser frame.
   top_controls_slide_controller_.reset();
@@ void BrowserView::WillDestroyToolbar() {
+  // Reset tab search bubble host before destroying toolbar, as its
+  // WebUIBubbleManager holds a raw_ptr<views::View> anchor_view_ that points
+  // to a toolbar child view (e.g. tab_search_button). This must happen before
+  // the toolbar child views are destroyed to avoid a dangling raw_ptr when
+  // BrowserView is destroyed later (e.g. in CEF).
+  tab_search_bubble_host_.reset();
+
   // Reset autofill bubble handler to make sure it does not out-live toolbar,
   // since it is responsible for showing autofill related bubbles from toolbar's
   // child views and it is an observer for avatar toolbar button if any.
```

The essence of the fix is moving `tab_search_bubble_host_.reset()` from the destructor to `WillDestroyToolbar()` so that it executes at the correct time (before the toolbar child views are destroyed).
