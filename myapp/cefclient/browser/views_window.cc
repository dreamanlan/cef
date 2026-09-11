// Copyright (c) 2016 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/views_window.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "include/base/cef_build.h"
#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_i18n_util.h"
#include "include/views/cef_box_layout.h"
#include "include/wrapper/cef_helpers.h"
#include "myapp/cefclient/browser/base_client_handler.h"
#include "myapp/cefclient/browser/default_client_handler.h"
#include "myapp/cefclient/browser/main_context.h"
#include "myapp/cefclient/browser/resource.h"
#include "myapp/cefclient/browser/views_style.h"
#include "myapp/cefclient/common/custom_scheme_common.h"
#include "myapp/cefclient/hostclr/HostCLR.h"
#include "myapp/shared/browser/geometry_util.h"
#include "myapp/shared/common/client_switches.h"

namespace client {

namespace {

// Default window size.
constexpr int kDefaultWidth = 800;
constexpr int kDefaultHeight = 600;

#if defined(OS_MAC)
constexpr int kTitleBarHeight = 35;
#endif

// Control IDs for Views in the top-level Window.
enum ControlIds {
  ID_WINDOW = 1,
  ID_BROWSER_VIEW,
  ID_MENU_BUTTON,

  // HTML tab bar view (enabled by default for NORMAL windows).
  ID_TABBAR_VIEW,
};

// Height model (in DIP) of the HTML tab bar strip for NORMAL windows.
// The single row constant is the tab strip row height and is the sole source
// of the base height; the navigation-toolbar row is an increment added only
// for Alloy-style windows (Chrome-style draws its own toolbar). See §6.5 of
// TABBAR_DESIGN.md.
const int kHtmlTabbarRowHeight = 34;
const int kHtmlTabbarNavRowHeight = 42;

// Initial (pre-report) strip height by window style. HTML reports the real
// height via cefQuery once loaded; this is only the first-frame guidance.
int InitialTabbarHeight(bool use_alloy_style) {
  return use_alloy_style ? (kHtmlTabbarRowHeight + kHtmlTabbarNavRowHeight)
                         : kHtmlTabbarRowHeight;
}

// First-frame guidance persistence: C++ stores only the LAST reported strip
// height per window style (a rendering seed, NOT tab bar state -- that lives
// in the HTML/localStorage). On next launch the window lays out with this
// number before the HTML reports, avoiding a first-frame jump. See §6.5.
std::string TabbarHeightFilePath(bool use_alloy_style) {
  std::string dir = MainContext::Get()->GetAppWorkingDirectory();
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
    dir += '/';
  }
  return dir + (use_alloy_style ? "tabbar_height_alloy.txt"
                                : "tabbar_height_chrome.txt");
}

int LoadTabbarHeightGuidance(bool use_alloy_style) {
  const int fallback = InitialTabbarHeight(use_alloy_style);
  std::ifstream f(TabbarHeightFilePath(use_alloy_style));
  if (!f.is_open()) {
    return fallback;
  }
  int v = 0;
  f >> v;
  if (!f || v < kHtmlTabbarRowHeight || v > 400) {
    return fallback;
  }
  return v;
}

void SaveTabbarHeightGuidance(bool use_alloy_style, int height) {
  std::ofstream f(TabbarHeightFilePath(use_alloy_style),
                  std::ios::out | std::ios::trunc);
  if (f.is_open()) {
    f << height;
  }
}

// Escape a string into a JS double-quoted string literal for ExecuteJavaScript.
std::string JsStringLiteral(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += "\"";
  return out;
}

// Minimal BrowserView delegate for the HTML tab bar strip. It pins the
// preferred height (read from the owning ViewsWindow so height reports from
// the HTML take effect) and, critically, reports the same runtime style as the
// content browser: a mismatch makes the strip render blank (white screen).
class TabbarViewDelegate : public CefBrowserViewDelegate {
 public:
  TabbarViewDelegate(ViewsWindow* owner, bool use_alloy_style)
      : owner_(owner), use_alloy_style_(use_alloy_style) {}

  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    // Width must be > 0: libcef CalculatePreferredSize (view_view.h) discards
    // the whole CefSize when CefSize::IsEmpty() is true (width <= 0 ||
    // height <= 0), which would drop the pinned height and fall back to the
    // web content's large preferred size (tab bar grabbing ~half the window).
    // The actual strip width is stretched to the full window by the box
    // layout's CEF_AXIS_ALIGNMENT_STRETCH cross-axis alignment.
    const int height =
        owner_ ? owner_->GetTabbarHeightDip() : kHtmlTabbarRowHeight;
    return CefSize(1, height);
  }

  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    // MUST match the content browser's runtime style. A docked BrowserView
    // whose style differs from its window host renders blank; this was the
    // root cause of the tab bar white-screen. Alloy-style windows host Alloy
    // browsers, Chrome-style windows host Chrome browsers.
    return use_alloy_style_ ? CEF_RUNTIME_STYLE_ALLOY
                            : CEF_RUNTIME_STYLE_CHROME;
  }

 private:
  ViewsWindow* const owner_;  // Not owned; the window outlives the strip.
  const bool use_alloy_style_;
  IMPLEMENT_REFCOUNTING(TabbarViewDelegate);
};

void AddTestMenuItems(CefRefPtr<CefMenuModel> test_menu) {
  test_menu->AddItem(ID_TESTS_GETSOURCE, "Get Source");
  test_menu->AddItem(ID_TESTS_GETTEXT, "Get Text");
  test_menu->AddItem(ID_TESTS_WINDOW_NEW, "New Window");
  test_menu->AddItem(ID_TESTS_WINDOW_POPUP, "Popup Window");
  test_menu->AddItem(ID_TESTS_WINDOW_DIALOG, "Dialog Window");
  test_menu->AddItem(ID_TESTS_REQUEST, "Request");
  test_menu->AddItem(ID_TESTS_ZOOM_IN, "Zoom In");
  test_menu->AddItem(ID_TESTS_ZOOM_OUT, "Zoom Out");
  test_menu->AddItem(ID_TESTS_ZOOM_RESET, "Zoom Reset");
  test_menu->AddItem(ID_TESTS_TRACING_BEGIN, "Begin Tracing");
  test_menu->AddItem(ID_TESTS_TRACING_END, "End Tracing");
  test_menu->AddItem(ID_TESTS_PRINT, "Print");
  test_menu->AddItem(ID_TESTS_PRINT_TO_PDF, "Print to PDF");
  test_menu->AddItem(ID_TESTS_MUTE_AUDIO, "Mute Audio");
  test_menu->AddItem(ID_TESTS_UNMUTE_AUDIO, "Unmute Audio");
  test_menu->AddItem(ID_TESTS_OTHER_TESTS, "Other Tests");
  test_menu->AddItem(ID_TESTS_DUMP_WITHOUT_CRASHING, "Dump without crashing");
}

void AddFileMenuItems(CefRefPtr<CefMenuModel> file_menu) {
  file_menu->AddItem(ID_QUIT, "E&xit");

  // Show the accelerator shortcut text in the menu.
  file_menu->SetAcceleratorAt(file_menu->GetCount() - 1, 'X', false, false,
                              true);
}

CefBrowserViewDelegate::ChromeToolbarType CalculateChromeToolbarType(
    bool use_alloy_style,
    const std::string& toolbar_type,
    bool hide_toolbar) {
  if (use_alloy_style || toolbar_type == "none" || hide_toolbar) {
    return CEF_CTT_NONE;
  }

  if (toolbar_type == "location") {
    return CEF_CTT_LOCATION;
  }

  return CEF_CTT_NORMAL;
}

}  // namespace

// static
CefRefPtr<ViewsWindow> ViewsWindow::Create(
    WindowType type,
    Delegate* delegate,
    CefRefPtr<CefClient> client,
    const CefString& url,
    const CefBrowserSettings& settings,
    CefRefPtr<CefRequestContext> request_context,
    CefRefPtr<CefCommandLine> command_line) {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(delegate);

  // Create a new ViewsWindow.
  CefRefPtr<ViewsWindow> views_window =
      new ViewsWindow(type, delegate, nullptr, command_line);

  // Only create an overlay browser for a primary window.
  if (command_line->HasSwitch(switches::kShowOverlayBrowser)) {
    views_window->with_overlay_browser_ = true;
    views_window->initial_url_ = url;
    views_window->settings_ = settings;
    views_window->request_context_ = request_context;
  }

  const auto expected_browser_runtime_style = views_window->use_alloy_style_
                                                  ? CEF_RUNTIME_STYLE_ALLOY
                                                  : CEF_RUNTIME_STYLE_CHROME;
  const auto expected_window_runtime_style =
      views_window->use_alloy_style_window_ ? CEF_RUNTIME_STYLE_ALLOY
                                            : CEF_RUNTIME_STYLE_CHROME;

  // Create a new BrowserView.
  CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
      client, url, settings, nullptr, request_context, views_window);
  CHECK_EQ(expected_browser_runtime_style, browser_view->GetRuntimeStyle());

  // Associate the BrowserView with the ViewsWindow.
  views_window->SetBrowserView(browser_view);

  // Create a new top-level Window. It will show itself after creation.
  auto window = CefWindow::CreateTopLevelWindow(views_window);
  CHECK_EQ(expected_window_runtime_style, window->GetRuntimeStyle());

  return views_window;
}

void ViewsWindow::Show() {
  CEF_REQUIRE_UI_THREAD();
  if (window_) {
    if (type_ == WindowType::DIALOG) {
      if (use_window_modal_dialog_) {
        // Show as a window modal dialog (IsWindowModalDialog() will return
        // true).
        window_->Show();
      } else {
        CefRefPtr<CefBrowserView> browser_view;
        if (auto parent_window = delegate_->GetParentWindow()) {
          if (auto view = parent_window->GetViewForID(ID_BROWSER_VIEW)) {
            browser_view = view->AsBrowserView();
          }
        }
        CHECK(browser_view);

        // Show as a browser modal dialog (relative to |browser_view|).
        window_->ShowAsBrowserModalDialog(browser_view);
      }
    } else {
      window_->Show();
    }
  }
  MaybeRequestBrowserFocus();
}

void ViewsWindow::Hide() {
  CEF_REQUIRE_UI_THREAD();
  if (window_) {
    window_->Hide();
  }
}

void ViewsWindow::Minimize() {
  CEF_REQUIRE_UI_THREAD();
  if (window_) {
    window_->Minimize();
  }
}

void ViewsWindow::Maximize() {
  CEF_REQUIRE_UI_THREAD();
  if (window_) {
    window_->Maximize();
  }
}

void ViewsWindow::SetBounds(const CefRect& bounds) {
  CEF_REQUIRE_UI_THREAD();
  if (window_) {
    auto window_bounds = bounds;
    ConstrainWindowBounds(window_->GetDisplay()->GetWorkArea(), window_bounds);
    window_->SetBounds(window_bounds);
  }
}

void ViewsWindow::SetBrowserSize(const CefSize& size,
                                 bool has_position,
                                 const CefPoint& position) {
  CEF_REQUIRE_UI_THREAD();
  if (browser_view_) {
    browser_view_->SetSize(size);
  }
  if (window_) {
    window_->SizeToPreferredSize();
    if (has_position) {
      window_->SetPosition(position);
    }
  }
}

void ViewsWindow::Close(bool force) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser_view_) {
    return;
  }

#if defined(OS_MAC)
  if (hide_on_close_) {
    // Don't hide on close if we actually want to close.
    hide_on_close_ = false;
  }
#endif

  CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
  if (browser) {
    // This will result in a call to CefWindow::Close() which will then call
    // ViewsWindow::CanClose().
    browser->GetHost()->CloseBrowser(force);
  }
}

void ViewsWindow::SetAddress(const std::string& url) {
  CEF_REQUIRE_UI_THREAD();
  if (!window_) {
    return;
  }

  // Push the address to the HTML tab bar's own address row (Alloy-style).
  PushToTabbar(
      "window.__tabbarApi&&__tabbarApi.onAddressChanged&&"
      "__tabbarApi.onAddressChanged(" +
      JsStringLiteral(url) + ");");
}

void ViewsWindow::SetTitle(const std::string& title) {
  CEF_REQUIRE_UI_THREAD();
  if (window_) {
    window_->SetTitle(title);
  }
  // Push the document title to the HTML tab bar (active tab label).
  PushToTabbar(
      "window.__tabbarApi&&__tabbarApi.setActiveTitle&&"
      "__tabbarApi.setActiveTitle(" +
      JsStringLiteral(title) + ");");
}

void ViewsWindow::SetFavicon(CefRefPtr<CefImage> image) {
  CEF_REQUIRE_UI_THREAD();

  // Window icons should be 16 DIP in size.
  DCHECK_EQ(std::max(image->GetWidth(), image->GetHeight()), 16U);

  if (window_) {
    window_->SetWindowIcon(image);
  }
}

void ViewsWindow::SetFullscreen(bool fullscreen) {
  CEF_REQUIRE_UI_THREAD();

  // For Chrome style we ignore this notification from
  // ClientHandler::OnFullscreenModeChange(). Chrome style will trigger
  // the fullscreen change internally and then call
  // OnWindowFullscreenTransition().
  if (!use_alloy_style_) {
    return;
  }

  // For Alloy style we need to explicitly trigger the fullscreen change.
  if (window_) {
    // Results in a call to OnWindowFullscreenTransition().
    window_->SetFullscreen(fullscreen);
  }
}

void ViewsWindow::SetAlwaysOnTop(bool on_top) {
  CEF_REQUIRE_UI_THREAD();
  if (window_) {
    window_->SetAlwaysOnTop(on_top);
  }
}

void ViewsWindow::SetLoadingState(bool isLoading,
                                  bool canGoBack,
                                  bool canGoForward) {
  CEF_REQUIRE_UI_THREAD();

  // Push loading / navigation state to the HTML tab bar (reload/stop button,
  // back/forward enablement).
  PushToTabbar(
      "window.__tabbarApi&&__tabbarApi.onLoadingStateChanged&&"
      "__tabbarApi.onLoadingStateChanged({isLoading:" +
      std::string(isLoading ? "true" : "false") +
      ",canGoBack:" + std::string(canGoBack ? "true" : "false") +
      ",canGoForward:" + std::string(canGoForward ? "true" : "false") + "});");
}

void ViewsWindow::SetDraggableRegions(
    const std::vector<CefDraggableRegion>& regions) {
  CEF_REQUIRE_UI_THREAD();
  // Regions reported by the content browser (in content BrowserView coords).
  content_regions_ = regions;
  ApplyDraggableRegions();
}

void ViewsWindow::SetTabbarDraggableRegions(
    const std::vector<CefDraggableRegion>& regions) {
  CEF_REQUIRE_UI_THREAD();
  // Regions reported by the tab bar HTML (in tab bar BrowserView coords).
  tabbar_regions_ = regions;
  ApplyDraggableRegions();
}

void ViewsWindow::ApplyDraggableRegions() {
  CEF_REQUIRE_UI_THREAD();

  if (!window_) {
    return;
  }

  // Two sources, each converted from its OWN view's coordinate space, then
  // merged: CefWindow::SetDraggableRegions replaces the whole set each call,
  // so content and tab bar regions must be combined here (not overwrite each
  // other). See §7.2 of TABBAR_DESIGN.md.
  std::vector<CefDraggableRegion> window_regions;

  if (browser_view_) {
    for (auto region : content_regions_) {
      CefPoint origin(region.bounds.x, region.bounds.y);
      browser_view_->ConvertPointToWindow(origin);
      region.bounds.x = origin.x;
      region.bounds.y = origin.y;
      window_regions.push_back(region);
    }
  }

  if (tabbar_view_) {
    for (auto region : tabbar_regions_) {
      CefPoint origin(region.bounds.x, region.bounds.y);
      tabbar_view_->ConvertPointToWindow(origin);
      region.bounds.x = origin.x;
      region.bounds.y = origin.y;
      window_regions.push_back(region);
    }
  }

  if (overlay_controls_) {
    // Exclude all regions obscured by overlays.
    overlay_controls_->UpdateDraggableRegions(window_regions);
  }

  if (overlay_browser_) {
    // Exclude all regions obscured by overlays.
    overlay_browser_->UpdateDraggableRegions(window_regions);
  }

  window_->SetDraggableRegions(window_regions);
}

bool ViewsWindow::OnSetFocus(cef_focus_source_t source) {
  CEF_REQUIRE_UI_THREAD();

  // No special handling of focus requests originating from the system.
  if (source == FOCUS_SOURCE_SYSTEM) {
    return false;
  }

  RequestBrowserFocus();
  return true;
}

void ViewsWindow::TakeFocus(bool next) {
  CEF_REQUIRE_UI_THREAD();

  if (!window_) {
    return;
  }

  if (chrome_toolbar_type_ == CEF_CTT_NORMAL && toolbar_) {
    // Give focus to the docked Chrome toolbar (the address bar for
    // Chrome-style tab bar windows).
    toolbar_->RequestFocus();
  }
}

void ViewsWindow::OnBeforeContextMenu(CefRefPtr<CefMenuModel> model) {
  CEF_REQUIRE_UI_THREAD();

  views_style::ApplyTo(model);
}

// static
bool ViewsWindow::SupportsWindowRestore(WindowType type) {
  // Only support window restore with normal windows.
  return type == WindowType::NORMAL;
}

bool ViewsWindow::SupportsWindowRestore() const {
  return SupportsWindowRestore(type_);
}

bool ViewsWindow::GetWindowRestorePreferences(
    cef_show_state_t& show_state,
    std::optional<CefRect>& dip_bounds) {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(SupportsWindowRestore());
  if (!window_) {
    return false;
  }

  show_state = CEF_SHOW_STATE_NORMAL;
  if (window_->IsMinimized()) {
    show_state = CEF_SHOW_STATE_MINIMIZED;
  } else if (window_->IsFullscreen()) {
    // On MacOS, IsMaximized() will also return true for fullscreen, so check
    // IsFullscreen() first.
    show_state = CEF_SHOW_STATE_FULLSCREEN;
  } else if (window_->IsMaximized()) {
    show_state = CEF_SHOW_STATE_MAXIMIZED;
  }

  if (show_state == CEF_SHOW_STATE_NORMAL) {
    // Use the current visible bounds.
    dip_bounds = window_->GetBoundsInScreen();
  } else {
    // Use the last known visible bounds.
    dip_bounds = last_visible_bounds_;
  }

  return true;
}

void ViewsWindow::SetTitlebarHeight(const std::optional<float>& height) {
  CEF_REQUIRE_UI_THREAD();
  if (height.has_value()) {
    override_titlebar_height_ = height;
  } else {
    override_titlebar_height_ = default_titlebar_height_;
  }
  NudgeWindow();
}

void ViewsWindow::UpdateDraggableRegions() {
  // Re-apply both content and tab bar regions (view geometry may have moved).
  ApplyDraggableRegions();
}

void ViewsWindow::PushToTabbar(const std::string& js) {
  CEF_REQUIRE_UI_THREAD();
  if (!tabbar_view_) {
    return;
  }
  CefRefPtr<CefBrowser> browser = tabbar_view_->GetBrowser();
  if (!browser) {
    return;
  }
  CefRefPtr<CefFrame> frame = browser->GetMainFrame();
  if (!frame) {
    return;
  }
  frame->ExecuteJavaScript(js, frame->GetURL(), 0);
}

void ViewsWindow::SetTabbarHeight(int height_dip) {
  CEF_REQUIRE_UI_THREAD();
  // Clamp to a sane range so a bad report can't collapse or explode the strip.
  if (height_dip < kHtmlTabbarRowHeight) {
    height_dip = kHtmlTabbarRowHeight;
  } else if (height_dip > 400) {
    height_dip = 400;
  }
  if (height_dip == tabbar_height_dip_) {
    return;
  }
  tabbar_height_dip_ = height_dip;

  // Persist as first-frame guidance for the next launch of this window style.
  SaveTabbarHeightGuidance(use_alloy_style_, height_dip);

  // The strip's preferred size now differs; relayout so the content browser
  // resizes to match.
  if (tabbar_view_) {
    tabbar_view_->InvalidateLayout();
  }
  if (window_) {
    window_->Layout();
  }
}

void ViewsWindow::ExecuteTabbarCommand(const std::string& action,
                                       const std::string& url) {
  CEF_REQUIRE_UI_THREAD();
  CefRefPtr<CefBrowser> browser =
      browser_view_ ? browser_view_->GetBrowser() : nullptr;
  if (!browser) {
    return;
  }
  if (action == "back") {
    browser->GoBack();
  } else if (action == "forward") {
    browser->GoForward();
  } else if (action == "reload") {
    browser->Reload();
  } else if (action == "reload_nocache") {
    browser->ReloadIgnoreCache();
  } else if (action == "stop") {
    browser->StopLoad();
  } else if (action == "navigate") {
    if (!url.empty()) {
      if (CefRefPtr<CefFrame> frame = browser->GetMainFrame()) {
        frame->LoadURL(url);
      }
    }
  }
}

CefRefPtr<CefBrowserViewDelegate> ViewsWindow::GetDelegateForPopupBrowserView(
    CefRefPtr<CefBrowserView> browser_view,
    const CefBrowserSettings& settings,
    CefRefPtr<CefClient> client,
    bool is_devtools) {
  CEF_REQUIRE_UI_THREAD();

  // The popup browser client is created in CefLifeSpanHandler::OnBeforePopup()
  // (e.g. via RootWindowViews::InitAsPopup()). The Delegate (RootWindowViews)
  // knows the association between |client| and itself.
  Delegate* popup_delegate = delegate_->GetDelegateForPopup(client);

  // May be nullptr when using the default popup behavior.
  if (!popup_delegate) {
    return nullptr;
  }

  // Should not be the same RootWindowViews that owns |this|.
  DCHECK(popup_delegate != delegate_);

  // Create a new ViewsWindow for the popup BrowserView.
  return new ViewsWindow(
      is_devtools ? WindowType::DEVTOOLS : WindowType::NORMAL, popup_delegate,
      nullptr, command_line_);
}

bool ViewsWindow::OnPopupBrowserViewCreated(
    CefRefPtr<CefBrowserView> browser_view,
    CefRefPtr<CefBrowserView> popup_browser_view,
    bool is_devtools) {
  CEF_REQUIRE_UI_THREAD();

  // Retrieve the ViewsWindow created in GetDelegateForPopupBrowserView.
  CefRefPtr<ViewsWindow> popup_window =
      static_cast<ViewsWindow*>(static_cast<CefBrowserViewDelegate*>(
          popup_browser_view->GetDelegate().get()));

  // May be nullptr when using the default popup behavior.
  if (!popup_window) {
    return false;
  }

  // Should not be the same ViewsWindow as |this|.
  DCHECK(popup_window != this);

  // Associate the ViewsWindow with the new popup browser.
  popup_window->SetBrowserView(popup_browser_view);

  // Create a new top-level Window for the popup. It will show itself after
  // creation.
  CefWindow::CreateTopLevelWindow(popup_window);

  // We created the Window.
  return true;
}

CefBrowserViewDelegate::ChromeToolbarType ViewsWindow::GetChromeToolbarType(
    CefRefPtr<CefBrowserView> browser_view) {
  return chrome_toolbar_type_;
}

bool ViewsWindow::UseFramelessWindowForPictureInPicture(
    CefRefPtr<CefBrowserView> browser_view) {
  return hide_pip_frame_;
}

#if CEF_API_ADDED(13601)
bool ViewsWindow::AllowMoveForPictureInPicture(
    CefRefPtr<CefBrowserView> browser_view) {
  return move_pip_enabled_;
}
#endif

#if CEF_API_ADDED(14400)
bool ViewsWindow::AllowPictureInPictureWithoutUserActivation(
    CefRefPtr<CefBrowserView> browser_view) {
  return allow_pip_without_user_activation_;
}
#endif

cef_runtime_style_t ViewsWindow::GetBrowserRuntimeStyle() {
  if (use_alloy_style_) {
    return CEF_RUNTIME_STYLE_ALLOY;
  }
  return CEF_RUNTIME_STYLE_DEFAULT;
}

void ViewsWindow::OnButtonPressed(CefRefPtr<CefButton> button) {
  // Nothing to do: no label buttons are delegated to this window any more
  // (the demo toolbar and the legacy Windows titlebar buttons are gone); the
  // hamburger menu button uses OnMenuButtonPressed instead. This override
  // only exists because CefButtonDelegate declares it pure virtual.
}

void ViewsWindow::OnMenuButtonPressed(
    CefRefPtr<CefMenuButton> menu_button,
    const CefPoint& screen_point,
    CefRefPtr<CefMenuButtonPressedLock> button_pressed_lock) {
  CEF_REQUIRE_UI_THREAD();

  DCHECK(with_html_tabbar_);
  DCHECK_EQ(ID_MENU_BUTTON, menu_button->GetID());

  const auto button_bounds = menu_button->GetBoundsInScreen();

  auto point = screen_point;
  if (with_html_tabbar_) {
    // Align the menu correctly under the button.
    if (CefIsRTL()) {
      point.x += button_bounds.width - 4;
    } else {
      point.x -= button_bounds.width - 4;
    }
  }

  if (use_bottom_controls_) {
    const auto display_bounds =
        menu_button->GetWindow()->GetDisplay()->GetWorkArea();
    const int available_height = display_bounds.y + display_bounds.height -
                                 button_bounds.y - button_bounds.height;

    // Approximation of the menu height.
    const int menu_height =
        static_cast<int>(button_menu_model_->GetCount()) * button_bounds.height;
    if (menu_height > available_height) {
      // The menu will go upwards, so place it above the button.
      point.y -= button_bounds.height - 8;
    }
  }

  menu_button->ShowMenu(button_menu_model_, point, CEF_MENU_ANCHOR_TOPLEFT);
}

void ViewsWindow::ExecuteCommand(CefRefPtr<CefMenuModel> menu_model,
                                 int command_id,
                                 cef_event_flags_t event_flags) {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(with_html_tabbar_);

  if (command_id == ID_QUIT) {
    delegate_->OnExit();
  } else if (command_id >= ID_TESTS_FIRST && command_id <= ID_TESTS_LAST) {
    delegate_->OnTest(command_id);
  } else {
    NOTREACHED();
  }
}

void ViewsWindow::OnWindowFullscreenTransition(CefRefPtr<CefWindow> window,
                                               bool is_completed) {
#if defined(OS_MAC)
  // On MacOS we get two asynchronous callbacks, and we want to change the UI on
  // |is_completed=false| (e.g. when the fullscreen transition begins).
  const bool should_change = !is_completed;
#else
  // On other platforms we only get a single synchronous callback with
  // |is_completed=true|.
  DCHECK(is_completed);
  const bool should_change = true;
#endif

  // With Alloy style we need to explicitly exit browser fullscreen when
  // exiting window fullscreen. Chrome style handles this internally.
  if (use_alloy_style_ && should_change && !window->IsFullscreen()) {
    CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
    if (browser && browser->GetHost()->IsFullscreen()) {
      // Will not cause a resize because the fullscreen transition has already
      // begun.
      browser->GetHost()->ExitFullscreen(/*will_cause_resize=*/false);
    }
  }

#if defined(OS_MAC)
  // Continue hide logic from CanClose.
  if (is_completed && hide_after_fullscreen_exit_) {
    hide_after_fullscreen_exit_ = false;
    window->Hide();
  }
#endif
}

void ViewsWindow::OnThemeColorsChanged(CefRefPtr<CefWindow> window,
                                       bool chrome_theme) {
  // Apply color overrides to the current theme.
  views_style::ApplyTo(window);
}

cef_runtime_style_t ViewsWindow::GetWindowRuntimeStyle() {
  if (use_alloy_style_window_) {
    return CEF_RUNTIME_STYLE_ALLOY;
  }
  return CEF_RUNTIME_STYLE_DEFAULT;
}

#if defined(OS_LINUX)
bool ViewsWindow::GetLinuxWindowProperties(
    CefRefPtr<CefWindow> window,
    CefLinuxWindowProperties& properties) {
  CefString(&properties.wayland_app_id) =
      CefString(&properties.wm_class_class) =
          CefString(&properties.wm_class_name) =
              CefString(&properties.wm_role_name) = "cef";

  return true;
}
#endif

void ViewsWindow::OnWindowCreated(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(browser_view_);
  DCHECK(!window_);
  DCHECK(window);

  window_ = window;
  window_->SetID(ID_WINDOW);

  // Apply color overrides to the current native/OS theme. This is only
  // necessary until the CefBrowserView is added to the CefWindow, at which time
  // the Chrome theme will be applied (triggering a call to OnThemeColorsChanged
  // with |chrome_theme=true|).
  views_style::ApplyTo(window_);
  window_->ThemeChanged();

  delegate_->OnViewsWindowCreated(this);

  if (type_ == WindowType::NORMAL || type_ == WindowType::DEVTOOLS) {
    const CefRect bounds = delegate_->GetInitialBounds();
    if (bounds.IsEmpty()) {
      // Size the Window and center it at the default size.
      window_->CenterWindow(CefSize(kDefaultWidth, kDefaultHeight));
    } else if (SupportsWindowRestore()) {
      // Remember the bounds from the previous application run in case the user
      // does not move or resize the window during this application run.
      last_visible_bounds_ = bounds;
    }
  }

  if (with_html_tabbar_) {
    // Gated shell: use a vertical box layout so the HTML tab bar strip can be
    // docked above the content BrowserView. The tab bar BrowserView itself is
    // created and inserted in OnWindowChanged (after the content view is added).
    CefBoxLayoutSettings settings;
    settings.horizontal = false;
    // Stretch children to the full window width; otherwise the tab bar strip
    // (preferred width 0) collapses to zero width and disappears on resize /
    // content relayout (issue 1).
    settings.cross_axis_alignment = CEF_AXIS_ALIGNMENT_STRETCH;
    CefRefPtr<CefBoxLayout> box_layout = window_->SetToBoxLayout(settings);
    window_->AddChildView(browser_view_);
    box_layout->SetFlexForView(browser_view_, 1);

    // Choose a reasonable minimum window size.
    minimum_window_size_ = CefSize(100, 100);
  } else {
    // Add the BrowserView as the only child of the Window.
    window_->AddChildView(browser_view_);

    // Choose a reasonable minimum window size.
    minimum_window_size_ = CefSize(100, 100);
  }

  if (!delegate_->InitiallyHidden()) {
    // Show the Window.
    Show();
  }
}

void ViewsWindow::OnWindowClosing(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(window_);

  delegate_->OnViewsWindowClosing(this);
}

void ViewsWindow::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(window_);

  delegate_->OnViewsWindowDestroyed(this);

  browser_view_ = nullptr;
  if (tabbar_client_) {
    tabbar_client_->SetTabbarOwnerWindow(nullptr);
    tabbar_client_ = nullptr;
  }
  tabbar_view_ = nullptr;
  button_menu_model_ = nullptr;
  menu_button_ = nullptr;
  window_ = nullptr;
}

void ViewsWindow::OnWindowActivationChanged(CefRefPtr<CefWindow> window,
                                            bool active) {
  if (!active) {
    return;
  }

  delegate_->OnViewsWindowActivated(this);
}

void ViewsWindow::OnWindowBoundsChanged(CefRefPtr<CefWindow> window,
                                        const CefRect& new_bounds) {
  if (SupportsWindowRestore() && !window->IsMinimized() &&
      !window->IsMaximized() && !window->IsFullscreen()) {
    // Track the last visible bounds for window restore purposes.
    last_visible_bounds_ = new_bounds;
  }
}

bool ViewsWindow::CanClose(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();

  CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();

#if defined(OS_MAC)
  // On MacOS we might hide the window instead of closing it.
  if (hide_on_close_ && browser && !browser->GetHost()->IsReadyToBeClosed()) {
    if (window->IsFullscreen()) {
      // Need to exit fullscreen mode before hiding the window.
      // Execution continues in OnWindowFullscreenTransition.
      hide_after_fullscreen_exit_ = true;
      window->SetFullscreen(false);
    } else {
      window->Hide();
    }
    return false;
  }
#endif

  // Allow the window to close if the browser says it's OK.
  if (browser) {
    return browser->GetHost()->TryCloseBrowser();
  }
  return true;
}

CefRefPtr<CefWindow> ViewsWindow::GetParentWindow(CefRefPtr<CefWindow> window,
                                                  bool* is_menu,
                                                  bool* can_activate_menu) {
  CEF_REQUIRE_UI_THREAD();
  return delegate_->GetParentWindow();
}

bool ViewsWindow::IsWindowModalDialog(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  DCHECK(delegate_->GetParentWindow());
  return use_window_modal_dialog_;
}

CefRect ViewsWindow::GetInitialBounds(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  const CefRect bounds = delegate_->GetInitialBounds();
  if (frameless_ && bounds.IsEmpty()) {
    // Need to provide a size for frameless windows that will be centered.
    return CefRect(0, 0, kDefaultWidth, kDefaultHeight);
  }
  return bounds;
}

cef_show_state_t ViewsWindow::GetInitialShowState(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  return delegate_->GetInitialShowState();
}

bool ViewsWindow::IsFrameless(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  return frameless_;
}

bool ViewsWindow::WithStandardWindowButtons(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  return with_standard_buttons_;
}

bool ViewsWindow::GetTitlebarHeight(CefRefPtr<CefWindow> window,
                                    float* titlebar_height) {
  CEF_REQUIRE_UI_THREAD();
#if defined(OS_MAC)
  if (override_titlebar_height_.has_value()) {
    *titlebar_height = override_titlebar_height_.value();
    return true;
  }
#endif

  return false;
}

cef_state_t ViewsWindow::AcceptsFirstMouse(CefRefPtr<CefWindow> window) {
  if (accepts_first_mouse_) {
    return STATE_ENABLED;
  }
  return STATE_DEFAULT;
}

bool ViewsWindow::CanResize(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  // Only allow resize of normal and DevTools windows.
  return type_ == WindowType::NORMAL || type_ == WindowType::DEVTOOLS;
}

bool ViewsWindow::CanMaximize(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  return CanResize(window);
}

bool ViewsWindow::CanMinimize(CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  return CanResize(window);
}

bool ViewsWindow::OnAccelerator(CefRefPtr<CefWindow> window, int command_id) {
  CEF_REQUIRE_UI_THREAD();

  if (command_id == ID_QUIT) {
    delegate_->OnExit();
    return true;
  } else if (overlay_browser_) {
    return overlay_browser_->OnAccelerator(window, command_id);
  }

  return false;
}

CefSize ViewsWindow::GetPreferredSize(CefRefPtr<CefView> view) {
  CEF_REQUIRE_UI_THREAD();

  if (view->GetID() == ID_WINDOW && type_ == WindowType::DIALOG) {
    // Preferred size for a browser modal dialog. The dialog will be shrunk to
    // fit inside the parent browser view if necessary.
    return CefSize(kDefaultWidth, kDefaultHeight);
  }

  return CefSize();
}

CefSize ViewsWindow::GetMinimumSize(CefRefPtr<CefView> view) {
  CEF_REQUIRE_UI_THREAD();

  if (view->GetID() == ID_WINDOW) {
    return minimum_window_size_;
  }

  return CefSize();
}

void ViewsWindow::OnWindowChanged(CefRefPtr<CefView> view, bool added) {
  const int view_id = view->GetID();
  if (view_id != ID_BROWSER_VIEW) {
    return;
  }

  if (added) {
    if (with_html_tabbar_ && !with_standard_buttons_) {
      // Frameless HTML tab bar window: float the native controls (hamburger
      // menu + min/max/close) over the top-right of the tab strip row (issue
      // 2), matching the Windows custom titlebar layout. The address bar is
      // provided by the HTML tab bar, so no location bar overlay is created.
      if (!button_menu_model_) {
        // Build the menu model here so the hamburger menu has content.
        CreateMenuModel();
      }
      overlay_controls_ = new ViewsOverlayControls(
          /*with_window_buttons=*/true, use_bottom_controls_);
      overlay_controls_->Initialize(window_, CreateMenuButton(),
                                    /*menu_in_panel=*/true);
    }

    if (with_overlay_browser_) {
      overlay_browser_ = new ViewsOverlayBrowser(this);

      // Use default behavior for the overlay browser. A new |client| instance
      // is still required by cefclient architecture.
      CefRefPtr<CefClient> client =
          new DefaultClientHandler(/*use_alloy_style=*/true);

      overlay_browser_->Initialize(window_, client, initial_url_, settings_,
                                   request_context_);
      request_context_ = nullptr;
    }

    if (with_html_tabbar_ && !tabbar_view_) {
      // Dock a fixed height HTML tab bar strip at the top of the content box.
      // A separate client instance is required by the cefclient architecture;
      // DefaultClientHandler joins the same browser query/callback system and
      // (see default_client_handler) forwards page draggable regions back to
      // this window when a tab bar owner is set below.
      // The tab bar is a docked helper BrowserView, not the main view of a
      // Chrome window. A Chrome-runtime-style docked BrowserView cannot render
      // standalone and shows a white screen, so it MUST use Alloy style
      // regardless of the window style (same as the overlay browser above).
      tabbar_client_ = new DefaultClientHandler(/*use_alloy_style=*/true);
      tabbar_client_->SetTabbarOwnerWindow(this);
      // Seed the pinned height from the last reported value for this style so
      // the first frame lays out close to the final size (§6.5).
      tabbar_height_dip_ = LoadTabbarHeightGuidance(use_alloy_style_);
      // nav=1 shows the HTML navigation/address row (Alloy-style, where the
      // page owns the toolbar); nav=0 hides it (Chrome-style, where Chrome
      // draws its own toolbar). See §6.5 / §8 of TABBAR_DESIGN.md.
      const std::string tabbar_url =
          std::string(custom_scheme::kCustomSchemeName) + "://tabbar/" +
          (use_alloy_style_ ? "?nav=1" : "?nav=0");
      CefBrowserSettings tabbar_settings;
      tabbar_view_ = CefBrowserView::CreateBrowserView(
          tabbar_client_, tabbar_url, tabbar_settings, /*extra_info=*/nullptr,
          /*request_context=*/nullptr,
          new TabbarViewDelegate(this, /*use_alloy_style=*/true));
      tabbar_view_->SetID(ID_TABBAR_VIEW);
      // Insert above the content BrowserView (which is the only existing child
      // in the no-controls path, or below the controls otherwise).
      window_->AddChildViewAt(tabbar_view_, 0);

      // Chrome-style tab bar windows use the browser's own Chrome toolbar as
      // their address bar: dock it just below the tab strip (index 1) and above
      // the content BrowserView. Alloy-style windows resolve to CEF_CTT_NONE
      // (the HTML tab bar owns the address row) so this is skipped. A
      // Views-hosted browser is forced TYPE_POPUP and may return a null Chrome
      // toolbar; guard against it so we degrade gracefully instead of crashing.
      // Diagnostics for the chrome toolbar dock path ([tabbar] log lines
      // in debug.log). Low volume: a handful of lines per NORMAL window plus
      // a process-wide capped count from OnLayoutChanged.
      // The CEF_CTT_LOCATION case exists because TYPE_POPUP browsers do not
      // support the kFeatureToolbar window feature (only kFeatureLocationBar);
      // --show-chrome-toolbar=location selects that variant.
      printf_log(LOG_SEVERITY_INFO,
                 "[tabbar] dock check: chrome_toolbar_type_=%d "
                 "use_alloy_style_=%d child_count=%d",
                 static_cast<int>(chrome_toolbar_type_),
                 use_alloy_style_ ? 1 : 0,
                 static_cast<int>(window_->GetChildViewCount()));
      if ((chrome_toolbar_type_ == CEF_CTT_NORMAL ||
           chrome_toolbar_type_ == CEF_CTT_LOCATION) &&
          !toolbar_) {
        toolbar_ = browser_view_->GetChromeToolbar();
        printf_log(LOG_SEVERITY_INFO, "[tabbar] GetChromeToolbar=%s",
                   toolbar_ ? "non-null" : "null");
        if (toolbar_) {
          const CefSize tb_pref = toolbar_->GetPreferredSize();
          printf_log(LOG_SEVERITY_INFO,
                     "[tabbar] toolbar pref size=%dx%d visible=%d",
                     tb_pref.width, tb_pref.height,
                     toolbar_->IsVisible() ? 1 : 0);
          // CEF forces TYPE_POPUP + trusted_source=true for Views-hosted
          // Chrome browsers (chrome_browser_host_impl.cc: "Don't show title
          // bar or address"), which makes BrowserView::IsToolbarVisible()
          // false and leaves the toolbar hidden at creation. This window's
          // layout owns the toolbar as its address bar, so re-show it
          // explicitly (it is reparented out of the BrowserView below and
          // is no longer managed by Chrome's own visibility logic).
          toolbar_->SetVisible(true);
          window_->AddChildViewAt(toolbar_, 1);
          CefRefPtr<CefView> tb_parent = toolbar_->GetParentView();
          printf_log(LOG_SEVERITY_INFO,
                     "[tabbar] after dock: visible=%d parent_id=%d "
                     "child_count=%d",
                     toolbar_->IsVisible() ? 1 : 0,
                     tb_parent ? tb_parent->GetID() : -2,
                     static_cast<int>(window_->GetChildViewCount()));
        }
      }
      window_->Layout();
      for (int i = 0; i < static_cast<int>(window_->GetChildViewCount());
           ++i) {
        CefRefPtr<CefView> child = window_->GetChildViewAt(i);
        if (!child) {
          continue;
        }
        const CefRect cb = child->GetBounds();
        printf_log(LOG_SEVERITY_INFO,
                   "[tabbar] child[%d] id=%d visible=%d bounds=%d,%d %dx%d",
                   i, child->GetID(), child->IsVisible() ? 1 : 0, cb.x, cb.y,
                   cb.width, cb.height);
      }
    }
  } else {
    // Remove any controls that may include the Chrome toolbar before removing
    // the BrowserView.
    if (overlay_controls_) {
      overlay_controls_->Destroy();
      overlay_controls_ = nullptr;
      toolbar_ = nullptr;
    } else if (toolbar_) {
      toolbar_ = nullptr;
    }
    if (overlay_browser_) {
      overlay_browser_->Destroy();
      overlay_browser_ = nullptr;
    }

    if (tabbar_view_) {
      // Remove the tab bar strip before the content BrowserView is removed.
      if (tabbar_client_) {
        // Drop the back-pointer so a late OnDraggableRegionsChanged cannot
        // reach a torn-down window.
        tabbar_client_->SetTabbarOwnerWindow(nullptr);
        tabbar_client_ = nullptr;
      }
      window_->RemoveChildView(tabbar_view_);
      tabbar_view_ = nullptr;
    }
  }
}

void ViewsWindow::OnLayoutChanged(CefRefPtr<CefView> view,
                                  const CefRect& new_bounds) {
  const int view_id = view->GetID();
  if (view_id != ID_BROWSER_VIEW) {
    return;
  }

  // Track the toolbar's final bounds across the first few layout passes
  // (e.g. after window Show). Process-wide capped at 6 lines to avoid noise.
  static int layout_dbg_count = 0;
  if (layout_dbg_count < 6 && toolbar_) {
    ++layout_dbg_count;
    const CefRect tb = toolbar_->GetBounds();
    printf_log(LOG_SEVERITY_INFO,
               "[tabbar] layout#%d: toolbar=%d,%d %dx%d visible=%d",
               layout_dbg_count, tb.x, tb.y, tb.width, tb.height,
               toolbar_->IsVisible() ? 1 : 0);
  }

  if (overlay_controls_) {
    overlay_controls_->UpdateControls();
  }

  if (overlay_browser_) {
    // TODO: Consider modifying insets based on toolbar visibility.
    CefInsets window_insets(200, 200, 200, 200);
    overlay_browser_->UpdateBounds(window_insets);
  }
}

void ViewsWindow::OnThemeChanged(CefRefPtr<CefView> view) {
  // Apply colors when the theme changes.
  views_style::OnThemeChanged(view);
}

ViewsWindow::~ViewsWindow() = default;

ViewsWindow::ViewsWindow(WindowType type,
                         Delegate* delegate,
                         CefRefPtr<CefBrowserView> browser_view,
                         CefRefPtr<CefCommandLine> command_line)
    : type_(type),
      delegate_(delegate),
      use_alloy_style_(delegate->UseAlloyStyle()),
      command_line_(command_line) {
  DCHECK(delegate_);

  if (browser_view) {
    SetBrowserView(browser_view);
  }

  use_alloy_style_window_ =
      use_alloy_style_ &&
      !command_line_->HasSwitch(switches::kUseChromeStyleWindow);

  const bool is_normal_type = type_ == WindowType::NORMAL;

  // HTML tab bar strip (docked at the top of the content box) for all NORMAL
  // windows. Computed first because it feeds the frameless decision below.
  with_html_tabbar_ = is_normal_type;

  // Keep the browser's native Chrome toolbar for Chrome-style tab bar windows
  // (that is their address bar); Alloy-style resolves to CEF_CTT_NONE anyway
  // (the HTML owns the toolbar). Non-tab-bar windows keep original behavior.
  const bool hide_toolbar = !with_html_tabbar_;
  const bool show_window_buttons =
      command_line->HasSwitch(switches::kShowWindowButtons);
  accepts_first_mouse_ = command_line->HasSwitch(switches::kAcceptsFirstMouse);

  // Without a window frame. Only NORMAL windows host the HTML tab bar and are
  // frameless so the tab strip can occupy the title bar area (issue 2);
  // native window buttons are provided as an overlay floating over the
  // top-right of the tab strip (see OnWindowChanged). DevTools and dialog
  // windows always keep a frame for dragging.
  frameless_ = with_html_tabbar_;

  // If window has frame or flag passed explicitly
  with_standard_buttons_ = !frameless_ || show_window_buttons;

#if defined(OS_MAC)
  if (frameless_ && with_standard_buttons_) {
    default_titlebar_height_ = kTitleBarHeight;
    override_titlebar_height_ = kTitleBarHeight;
  }

  hide_on_close_ = command_line->HasSwitch(switches::kHideWindowOnClose);
#endif

  const std::string& toolbar_type =
      command_line->GetSwitchValue(switches::kShowChromeToolbar);
  chrome_toolbar_type_ = CalculateChromeToolbarType(use_alloy_style_,
                                                    toolbar_type, hide_toolbar);

  use_bottom_controls_ = command_line->HasSwitch(switches::kUseBottomControls);

  use_window_modal_dialog_ =
      command_line->HasSwitch(switches::kUseWindowModalDialog);
  hide_pip_frame_ = command_line->HasSwitch(switches::kHidePipFrame);
  move_pip_enabled_ = command_line->HasSwitch(switches::kMovePipEnabled);
  allow_pip_without_user_activation_ =
      command_line->HasSwitch(switches::kPipNoUserActivationEnabled);
}

void ViewsWindow::SetBrowserView(CefRefPtr<CefBrowserView> browser_view) {
  DCHECK(!browser_view_);
  DCHECK(browser_view);
  DCHECK(browser_view->IsValid());
  DCHECK(!browser_view->IsAttached());
  browser_view_ = browser_view;
  browser_view_->SetID(ID_BROWSER_VIEW);
}

void ViewsWindow::CreateMenuModel() {
  // Create the menu button model.
  // Flatten the menu: show test items directly, then separator + Exit.
  button_menu_model_ = CefMenuModel::CreateMenuModel(this);
  views_style::ApplyTo(button_menu_model_);
  AddTestMenuItems(button_menu_model_);
  button_menu_model_->AddSeparator();
  AddFileMenuItems(button_menu_model_);
}

CefRefPtr<CefMenuButton> ViewsWindow::CreateMenuButton() {
  // Create the menu button.
  DCHECK(!menu_button_);
  menu_button_ = CefMenuButton::CreateMenuButton(this, CefString());
  menu_button_->SetID(ID_MENU_BUTTON);
  menu_button_->SetImage(
      CEF_BUTTON_STATE_NORMAL,
      delegate_->GetImageCache()->GetCachedImage("menu_icon"));
  menu_button_->SetInkDropEnabled(true);
  // Override the default minimum size.
  menu_button_->SetMinimumSize(CefSize(0, 0));
  return menu_button_;
}

#if !defined(OS_MAC)
void ViewsWindow::NudgeWindow() {
  NOTIMPLEMENTED();
}
#endif

void ViewsWindow::MaybeRequestBrowserFocus() {
  if (browser_view_) {
    // BaseClientHandler has some state that we need to query.
    if (auto handler =
            BaseClientHandler::GetForBrowser(browser_view_->GetBrowser());
        handler->ShouldRequestFocus()) {
      RequestBrowserFocus();
    }
  }
}

void ViewsWindow::RequestBrowserFocus() {
  if (window_->IsMinimized()) {
    return;
  }

  // Maybe give keyboard focus to the overlay BrowserView.
  if (overlay_browser_ && overlay_browser_->RequestFocus()) {
    return;
  }

  // Give keyboard focus to the main BrowserView.
  if (browser_view_) {
    browser_view_->RequestFocus();
  }
}

}  // namespace client
