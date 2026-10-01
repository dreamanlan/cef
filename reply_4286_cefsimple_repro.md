# Reply to #4286 (final version)

Thanks for testing. `--use-native` cannot reach the affected code path, which is why it doesn't reproduce:

- `--use-native` disables Views/Chrome-style entirely (`tests/cefsimple/simple_app.cc`: "Views is enabled by default (add `--use-native` to disable)"), so no `BrowserView` is created and `tab_search_bubble_host_` never exists.
- With `cefclient`, `--use-native` additionally forces Alloy style on macOS (`tests/cefclient/browser/main_context_impl.cc`, see issue #3294).

The crash requires **Chrome-style windows created by the Chrome runtime itself** — not the cefclient root window, and not `window.open` popups (those are routed through the cefclient root window machinery). cefsimple never creates such windows. In the application where we originally observed this (a cefclient fork using the Chrome runtime on macOS), the crash occurred when closing such windows and when exiting the application while they were open.

Reproduction on macOS:

```
./cefclient.app/Contents/MacOS/cefclient --use-chrome-style-window --no-sandbox
# 1. Open the Chrome app menu (the "⋮" hamburger menu) and choose
#    "New Window" — this creates a window owned by the Chrome runtime
# 2. Close that window, and/or exit the application while it is open
```

The code-level concern stands regardless of reproduction: `BrowserView::~BrowserView()` resets `tab_search_bubble_host_` assuming the toolbar child views still exist, whereas the sibling `autofill_bubble_handler_` is already reset in `WillDestroyToolbar()` (called from `ToolbarView::~ToolbarView()`) for exactly this kind of lifetime reason. Moving the `tab_search_bubble_host_` reset there makes its lifetime consistent with the other toolbar-dependent members.
