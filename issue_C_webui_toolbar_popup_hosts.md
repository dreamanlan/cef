# Chrome runtime: `WebUIToolbarWebView` is created for CEF hosts whose Browser type is forced to TYPE_POPUP

## Environment

- CEF branches 7871 and 8037 (Chromium 152/154)
- Chrome runtime with `CefBrowserView`

## Problem

CEF hosts using `CefBrowserView` force the Browser type to `TYPE_POPUP` (see `chrome_browser_host_impl.cc`). `BrowserWindowFeatures` only registers `InitialWebUIManager` for `TYPE_NORMAL` browsers. When the WebUI toolbar is enabled, `WebUIToolbarWebView` is nevertheless created for the CEF host, ending up in a partially initialized state: it crashes without a defensive null check, and even with one (upstream CL `6fd0481bbcf93`, M154, added an `InitialWebUIManager::From` null check in `OnPageInitialized`) the toolbar fails to paint on first show.

## Suggested fix

We carry the following locally in `chrome_runtime_views.patch`: align the creation guard with the upstream `BrowserWindowFeatures` registration guard so the WebUI toolbar is only instantiated when its infrastructure is guaranteed to exist.

```diff
--- a/chrome/browser/ui/views/toolbar/toolbar_view.cc
+++ b/chrome/browser/ui/views/toolbar/toolbar_view.cc
@@ void ToolbarView::Init() {
+  // WebUIToolbarWebView depends on InitialWebUIManager, which is only
+  // registered on BrowserWindowFeatures for TYPE_NORMAL browsers. CEF hosts
+  // using CefBrowserView force the Browser type to TYPE_POPUP (see
+  // chrome_browser_host_impl.cc), so InitialWebUIManager is never created for
+  // them. Align the creation guard here with the upstream
+  // BrowserWindowFeatures registration guard so the new WebUI toolbar is
+  // only instantiated when the surrounding infrastructure is guaranteed to
+  // exist.
+  const bool is_normal_browser =
+      browser_->GetType() == BrowserWindowInterface::TYPE_NORMAL;
   if (is_normal_browser &&
       base::FeatureList::IsEnabled(
           features::kWebUIToolbarProcessOverheadExperiment)) {
```

## Note for branch 8037

`browser_->type()` (`Browser::TYPE_NORMAL`) no longer compiles in this context — `browser_` is a `BrowserWindowInterface*` and the check must be written as `browser_->GetType() == BrowserWindowInterface::TYPE_NORMAL` (matching the upstream guard style in `browser_window_features.cc`).
