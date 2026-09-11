// Copyright (c) 2016 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_TESTS_CEFCLIENT_BROWSER_VIEWS_WINDOW_H_
#define CEF_TESTS_CEFCLIENT_BROWSER_VIEWS_WINDOW_H_
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "include/base/cef_callback_forward.h"
#include "include/cef_menu_model_delegate.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_button_delegate.h"
#include "include/views/cef_menu_button.h"
#include "include/views/cef_menu_button_delegate.h"
#include "include/views/cef_overlay_controller.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"
#include "myapp/cefclient/browser/image_cache.h"
#include "myapp/cefclient/browser/root_window.h"
#include "myapp/cefclient/browser/views_overlay_browser.h"
#include "myapp/cefclient/browser/views_overlay_controls.h"

namespace client {

class DefaultClientHandler;

// Implements a CefWindow that hosts a single CefBrowserView and optional
// Views-hosted controls. All methods must be called on the browser process UI
// thread.
class ViewsWindow : public CefBrowserViewDelegate,
                    public CefMenuButtonDelegate,
                    public CefMenuModelDelegate,
                    public CefWindowDelegate {
 public:
  // Delegate methods will be called on the browser process UI thread.
  class Delegate {
   public:
    // Returns true if the window should use Alloy style, otherwise Chrome
    // style.
    virtual bool UseAlloyStyle() const = 0;

    // Return true if the window should be created initially hidden.
    virtual bool InitiallyHidden() = 0;

    // Returns the parent for this window.
    virtual CefRefPtr<CefWindow> GetParentWindow() = 0;

    // Return the initial window bounds.
    virtual CefRect GetInitialBounds() = 0;

    // Return the initial window show state.
    virtual cef_show_state_t GetInitialShowState() = 0;

    // Returns the ImageCache.
    virtual scoped_refptr<ImageCache> GetImageCache() = 0;

    // Called when the ViewsWindow is created.
    virtual void OnViewsWindowCreated(CefRefPtr<ViewsWindow> window) = 0;

    // Called when the ViewsWindow is closing.
    virtual void OnViewsWindowClosing(CefRefPtr<ViewsWindow> window) = 0;

    // Called when the ViewsWindow is destroyed. All references to |window|
    // should be released in this callback.
    virtual void OnViewsWindowDestroyed(CefRefPtr<ViewsWindow> window) = 0;

    // Called when the ViewsWindow is activated (becomes the foreground window).
    virtual void OnViewsWindowActivated(CefRefPtr<ViewsWindow> window) = 0;

    // Return the Delegate for the popup window controlled by |client|.
    virtual Delegate* GetDelegateForPopup(CefRefPtr<CefClient> client) = 0;

    // Called to execute a test. See resource.h for |test_id| values.
    virtual void OnTest(int test_id) = 0;

    // Called to exit the application.
    virtual void OnExit() = 0;

   protected:
    virtual ~Delegate() = default;
  };

  // Create a new top-level ViewsWindow hosting a browser with the specified
  // configuration.
  static CefRefPtr<ViewsWindow> Create(
      WindowType type,
      Delegate* delegate,
      CefRefPtr<CefClient> client,
      const CefString& url,
      const CefBrowserSettings& settings,
      CefRefPtr<CefRequestContext> request_context,
      CefRefPtr<CefCommandLine> command_line);

  void Show();
  void Hide();
  void Minimize();
  void Maximize();
  void SetBounds(const CefRect& bounds);
  void SetBrowserSize(const CefSize& size,
                      bool has_position,
                      const CefPoint& position);
  void Close(bool force);
  void SetAddress(const std::string& url);
  void SetTitle(const std::string& title);
  void SetFavicon(CefRefPtr<CefImage> image);
  void SetFullscreen(bool fullscreen);
  void SetAlwaysOnTop(bool on_top);
  void SetLoadingState(bool isLoading, bool canGoBack, bool canGoForward);
  void SetDraggableRegions(const std::vector<CefDraggableRegion>& regions);

  // HTML tab bar bridge (see TABBAR_DESIGN.md §6.5/§8). All UI-thread only.
  // Current pinned strip height (DIP), read by the tab bar view delegate.
  int GetTabbarHeightDip() const { return tabbar_height_dip_; }
  // Apply a height reported by the tab bar HTML (relayouts + persists it as
  // the next-launch first-frame guidance for this window style).
  void SetTabbarHeight(int height_dip);
  // Draggable regions reported by the tab bar HTML (tab strip drag area).
  void SetTabbarDraggableRegions(
      const std::vector<CefDraggableRegion>& regions);
  // Route a tab bar command to the content browser (back/forward/reload/stop/
  // navigate). |url| is only used by "navigate".
  void ExecuteTabbarCommand(const std::string& action, const std::string& url);
  bool OnSetFocus(cef_focus_source_t source);
  void TakeFocus(bool next);
  void OnBeforeContextMenu(CefRefPtr<CefMenuModel> model);

  static bool SupportsWindowRestore(WindowType type);
  bool SupportsWindowRestore() const;
  bool GetWindowRestorePreferences(cef_show_state_t& show_state,
                                   std::optional<CefRect>& dip_bounds);
  void SetTitlebarHeight(const std::optional<float>& height);

  void UpdateDraggableRegions();

  // CefBrowserViewDelegate methods:
  CefRefPtr<CefBrowserViewDelegate> GetDelegateForPopupBrowserView(
      CefRefPtr<CefBrowserView> browser_view,
      const CefBrowserSettings& settings,
      CefRefPtr<CefClient> client,
      bool is_devtools) override;
  bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view,
                                 CefRefPtr<CefBrowserView> popup_browser_view,
                                 bool is_devtools) override;
  ChromeToolbarType GetChromeToolbarType(
      CefRefPtr<CefBrowserView> browser_view) override;
  bool UseFramelessWindowForPictureInPicture(
      CefRefPtr<CefBrowserView> browser_view) override;
#if CEF_API_ADDED(13601)
  bool AllowMoveForPictureInPicture(
      CefRefPtr<CefBrowserView> browser_view) override;
#endif
#if CEF_API_ADDED(14400)
  bool AllowPictureInPictureWithoutUserActivation(
      CefRefPtr<CefBrowserView> browser_view) override;
#endif
  cef_runtime_style_t GetBrowserRuntimeStyle() override;

  // CefButtonDelegate methods:
  void OnButtonPressed(CefRefPtr<CefButton> button) override;

  // CefMenuButtonDelegate methods:
  void OnMenuButtonPressed(
      CefRefPtr<CefMenuButton> menu_button,
      const CefPoint& screen_point,
      CefRefPtr<CefMenuButtonPressedLock> button_pressed_lock) override;

  // CefMenuModelDelegate methods:
  void ExecuteCommand(CefRefPtr<CefMenuModel> menu_model,
                      int command_id,
                      cef_event_flags_t event_flags) override;

  // CefWindowDelegate methods:
#if defined(OS_LINUX)
  virtual bool GetLinuxWindowProperties(
      CefRefPtr<CefWindow> window,
      CefLinuxWindowProperties& properties) override;
#endif
  void OnWindowCreated(CefRefPtr<CefWindow> window) override;
  void OnWindowClosing(CefRefPtr<CefWindow> window) override;
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;
  void OnWindowActivationChanged(CefRefPtr<CefWindow> window,
                                 bool active) override;
  void OnWindowBoundsChanged(CefRefPtr<CefWindow> window,
                             const CefRect& new_bounds) override;
  CefRefPtr<CefWindow> GetParentWindow(CefRefPtr<CefWindow> window,
                                       bool* is_menu,
                                       bool* can_activate_menu) override;
  bool IsWindowModalDialog(CefRefPtr<CefWindow> window) override;
  CefRect GetInitialBounds(CefRefPtr<CefWindow> window) override;
  cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow> window) override;
  bool IsFrameless(CefRefPtr<CefWindow> window) override;
  bool WithStandardWindowButtons(CefRefPtr<CefWindow> window) override;
  bool GetTitlebarHeight(CefRefPtr<CefWindow> window,
                         float* titlebar_height) override;
  cef_state_t AcceptsFirstMouse(CefRefPtr<CefWindow> window) override;
  bool CanResize(CefRefPtr<CefWindow> window) override;
  bool CanMaximize(CefRefPtr<CefWindow> window) override;
  bool CanMinimize(CefRefPtr<CefWindow> window) override;
  bool CanClose(CefRefPtr<CefWindow> window) override;
  bool OnAccelerator(CefRefPtr<CefWindow> window, int command_id) override;
  void OnWindowFullscreenTransition(CefRefPtr<CefWindow> window,
                                    bool is_completed) override;
  void OnThemeColorsChanged(CefRefPtr<CefWindow> window,
                            bool chrome_theme) override;
  cef_runtime_style_t GetWindowRuntimeStyle() override;

  // CefViewDelegate methods:
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override;
  CefSize GetMinimumSize(CefRefPtr<CefView> view) override;
  void OnWindowChanged(CefRefPtr<CefView> view, bool added) override;
  void OnLayoutChanged(CefRefPtr<CefView> view,
                       const CefRect& new_bounds) override;
  void OnThemeChanged(CefRefPtr<CefView> view) override;

 private:
  // |delegate| is guaranteed to outlive this object.
  // |browser_view| may be nullptr, in which case SetBrowserView() will be
  // called.
  ViewsWindow(WindowType type,
              Delegate* delegate,
              CefRefPtr<CefBrowserView> browser_view,
              CefRefPtr<CefCommandLine> command_line);

  // Out-of-line destructor so the CefRefPtr<DefaultClientHandler> member is
  // destroyed in the .cc (where DefaultClientHandler is a complete type),
  // instead of being instantiated here against a forward declaration.
  ~ViewsWindow() override;

  void SetBrowserView(CefRefPtr<CefBrowserView> browser_view);

  // Create the menu model shown by the hamburger menu button.
  void CreateMenuModel();
  CefRefPtr<CefMenuButton> CreateMenuButton();

  // Merge content + tab bar draggable regions (each converted from its own
  // view's coordinates) and push the result to the window.
  void ApplyDraggableRegions();

  // Execute |js| on the tab bar HTML main frame (no-op if the strip is absent
  // or its browser is not ready). Used to push content state to the tab bar.
  void PushToTabbar(const std::string& js);

  void NudgeWindow();

  void MaybeRequestBrowserFocus();
  void RequestBrowserFocus();

  const WindowType type_;
  Delegate* const delegate_;  // Not owned by this object.
  const bool use_alloy_style_;
  bool use_alloy_style_window_;
  CefRefPtr<CefBrowserView> browser_view_;
  CefRefPtr<CefCommandLine> command_line_;
  bool frameless_;
  bool with_standard_buttons_;
  ChromeToolbarType chrome_toolbar_type_;
  bool use_window_modal_dialog_;
  bool use_bottom_controls_;
  bool hide_pip_frame_;
  bool move_pip_enabled_;
  bool allow_pip_without_user_activation_;
  bool accepts_first_mouse_;
  CefRefPtr<CefWindow> window_;

  CefRefPtr<CefMenuModel> button_menu_model_;
  // The browser's own Chrome toolbar, docked below the tab bar strip for
  // Chrome-style NORMAL windows (that is their address bar). Null otherwise.
  CefRefPtr<CefView> toolbar_;
  CefRefPtr<CefMenuButton> menu_button_;
  std::optional<CefRect> last_visible_bounds_;

  CefSize minimum_window_size_;

  CefRefPtr<ViewsOverlayControls> overlay_controls_;

  // Overlay browser view state.
  bool with_overlay_browser_ = false;
  std::string initial_url_;
  CefBrowserSettings settings_;
  CefRefPtr<CefRequestContext> request_context_;
  CefRefPtr<ViewsOverlayBrowser> overlay_browser_;

  // HTML tab bar state (enabled by default for NORMAL windows). When enabled,
  // a fixed height BrowserView is docked at the top of the content box and
  // loads the custom scheme tab bar page; the content browser fills the rest.
  bool with_html_tabbar_ = false;
  CefRefPtr<CefBrowserView> tabbar_view_;
  // The tab bar strip's client handler; holds a back-pointer to this window
  // for draggable-region forwarding. Cleared on teardown to avoid dangling.
  CefRefPtr<DefaultClientHandler> tabbar_client_;
  // Current pinned strip height (DIP). Seeded from persisted per-style
  // guidance, then updated by HTML height reports.
  int tabbar_height_dip_ = 34;
  // Last draggable regions reported by the tab bar HTML (in tab bar view
  // coordinates); merged with content_regions_ when applied to the window.
  std::vector<CefDraggableRegion> tabbar_regions_;

  std::optional<float> default_titlebar_height_;
  std::optional<float> override_titlebar_height_;

#if defined(OS_MAC)
  bool hide_on_close_ = false;
  bool hide_after_fullscreen_exit_ = false;
#endif

  std::vector<CefDraggableRegion> content_regions_;

  IMPLEMENT_REFCOUNTING(ViewsWindow);
};

}  // namespace client

#endif  // CEF_TESTS_CEFCLIENT_BROWSER_VIEWS_WINDOW_H_
