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
class TabbedRootWindowViews;

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

    // Called after associating the content BrowserView, before attaching it
    // to the top-level window. Also called for popup content views.
    virtual void OnBrowserViewCreated(
        CefRefPtr<CefBrowserView> browser_view) {}

    // Called before the content browser's final lifespan callback.
    // Release tab-owned view references without completing window teardown.
    virtual void OnBrowserViewDestroyed(
        CefRefPtr<CefBrowserView> browser_view) {}

    // Return true when a tab container takes ownership of this close request.
    // Called on the UI thread for normal Alloy windows only.
    // The container must eventually close the empty window after all tabs finish.
    virtual bool OnCloseRequested(bool force) { return false; }

    // Return true only when a new-tab creation request has been accepted.
    virtual bool OnNewTabRequested(const std::string& url) { return false; }
    virtual bool OnTabSnapshotRequested() { return false; }

    // Resolve content browser identity inside the owning tab container.
    // Acceptance does not imply completion of an asynchronous close.
    // A zero anchor appends; otherwise insert before a browser in this window.
    virtual bool OnTabReorderRequested(int browser_id, int before_id) {
      return false;
    }

    // Return true only when a detach-to-new-window request was accepted.
    virtual bool OnTabDetachRequested(int browser_id) { return false; }

    virtual bool OnTabCommandRequested(const std::string& action,
                                       int browser_id) {
      return false;
    }

    // Downcast accessor without RTTI: returns the tab container implementing
    // this delegate, or nullptr for plain single-browser roots.
    virtual TabbedRootWindowViews* AsTabbedRootWindow() { return nullptr; }

    // Start a shell drag from the tab strip of the window owned by this
    // delegate. |allow_merge| carries the gesture semantics: tab drags may
    // merge into another window's tab bar, blank-area window moves may not.
    // Returns true when a drag session was started.
    virtual bool OnWindowDragRequested(bool allow_merge) { return false; }

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

  // Create a new top-level ViewsWindow hosting the existing |browser_view|
  // (tab detach, MULTITAB_WINDOW_DESIGN.md M3). The view must be alive,
  // unparented, and use a live per-tab content delegate, which is re-pointed
  // at the new window. Returns nullptr when the view cannot be adopted.
  static CefRefPtr<ViewsWindow> CreateForExistingView(
      WindowType type,
      Delegate* delegate,
      CefRefPtr<CefBrowserView> browser_view,
      const CefBrowserSettings& settings,
      CefRefPtr<CefRequestContext> request_context,
      CefRefPtr<CefCommandLine> command_line);

  void Show();
  void Hide();
  void Minimize();
  void Maximize();
  void SetBounds(const CefRect& bounds);
  // Current top-level window bounds in DIP screen coordinates (empty when
  // the window has not been created).
  CefRect GetWindowBounds() const;
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

  // Create an unparented content view using this window's browser settings.
  // Only available for multi-tab windows ready to accept content.
  // The caller owns association, attachment and failure cleanup.
  CefRefPtr<CefBrowserView> CreateTabBrowserView(
      CefRefPtr<CefClient> client, const CefString& url);

  // Attach an unparented content view as an inactive tab in a ready NORMAL window.
  // The view must use this delegate. Activation and state replay are separate.
  bool AddBrowserView(CefRefPtr<CefBrowserView> browser_view);

  // Detach an inactive content view and its docked toolbar without closing it.
  // The caller must first switch or clear the active view and retain ownership.
  // This does not emit browser destruction notifications.
  // |allow_active_removal| is reserved for the merge path, where a one-tab
  // source window hands its only (active) view to the target window and then
  // closes its shell; ordinary tab operations must keep it false.
  bool RemoveBrowserView(CefRefPtr<CefBrowserView> browser_view,
                         bool allow_active_removal = false);

  // Returns the live window that owns |browser_view|'s delegate, that is the
  // window which created it; nullptr when the view was not created by a live
  // window in this process. Used to accept a browser view adopted from another
  // window (see MULTITAB_WINDOW_DESIGN.md, M3).
  static CefRefPtr<ViewsWindow> GetHostWindowForView(
      CefRefPtr<CefBrowserView> browser_view);  // Re-point the per-tab delegate of |browser_view| at |owner| (nullptr to
  // detach it). Returns false when the view was not created with a per-tab
  // delegate, for example a popup content view (MULTITAB_WINDOW_DESIGN.md, M3).
  static bool RepointViewOwner(CefRefPtr<CefBrowserView> browser_view,
                               ViewsWindow* owner);

  // Switch between content views already attached to a NORMAL window.
  // The caller must replay the selected tab's cached window state afterward.
  bool SetActiveBrowserView(CefRefPtr<CefBrowserView> browser_view,
                            bool request_focus = true);
  CefRefPtr<CefBrowserView> GetActiveBrowserView() const;

  // HTML tab bar bridge (see TABBAR_DESIGN.md §6.5/§8). All UI-thread only.
  // Current pinned strip height (DIP), read by the tab bar view delegate.
  int GetTabbarHeightDip() const { return tabbar_height_dip_; }
  // Multi-tab operations are restricted to normal Alloy Views windows.
  bool SupportsMultipleTabs() const {
    return type_ == WindowType::NORMAL && use_alloy_style_;
  }
  // Apply a height reported by the tab bar HTML (relayouts + persists it as
  // the next-launch first-frame guidance for this window style).
  void SetTabbarHeight(int height_dip);
  // Draggable regions reported by the tab bar HTML (tab strip drag area).
  void SetTabbarDraggableRegions(
      const std::vector<CefDraggableRegion>& regions);
  // Route a tab bar command to the content browser (back/forward/reload/stop/
  // navigate). |url| is only used by "navigate".
  void ExecuteTabbarCommand(const std::string& action, const std::string& url);
  // Request creation through the container, not through the active browser.
  bool RequestNewTab(const std::string& url);
  bool RequestTabCommand(const std::string& action, int browser_id);
  bool RequestTabReorder(int browser_id, int before_id);
  bool RequestTabDetach(int browser_id);
  // Start a native drag session that moves this window's shell; when
  // |allow_merge| is true (tab gesture) the drop may merge this window's
  // tabs into another window's tab bar (MULTITAB_WINDOW_DESIGN.md, M4a/M4b).
  bool RequestWindowDrag(bool allow_merge);
  // Report the drop-hover insertion slot computed by the tab bar HTML while a
  // TabDragController session hovers this window's tab bar.
  void ReportTabDropHover(CefRefPtr<CefBrowser> tabbar_browser, int before_id);
  // Toggle maximize/restore. Compensates the double-click affordance lost
  // when the strip blank area switched from a native drag region to the
  // JS-driven whole-window drag (M4b, Windows).
  void ToggleMaximize();
  // The tab bar strip browser owned by this window, when present.
  CefRefPtr<CefBrowser> GetTabbarBrowser() const;
  // The tab bar strip's bounds in screen coordinates (DIP). Empty before the
  // strip is attached. Used by the drag controller for drop-target hit
  // testing: the tab bar browser's native hwnd covers the whole window on
  // Windows, so the view bounds are the accurate source (verified at runtime:
  // 76 DIP strip resolved correctly, 2026-09-14).
  CefRect GetTabbarScreenBoundsDip() const;
  // The top-level native window handle of this shell (HWND on Windows, the
  // X11 window on Linux; null before creation / after teardown). Used by the
  // Linux drag controller; kept platform-neutral for uniform declaration.
  CefWindowHandle GetTopLevelNativeHandle() const;
  // Linux JS-fed drag loop callbacks (actions "windowdragmove" /
  // "windowdragend"); no-ops on platforms with native capture.
  void NotifyTabDragMove();
  void NotifyTabDragEnd(bool cancel);
  // Snapshot of every live ViewsWindow (UI thread). Used by the drag
  // controller to enumerate merge targets.
  static std::vector<ViewsWindow*> GetLiveWindows();
  // The root window that owns this shell (never null while live).
  Delegate* delegate() const { return delegate_; }
  // Re-request cached state after the HTML reverse API is installed.
  bool RequestTabSnapshot();
  void SetTabSnapshot(const std::string& json);
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
  void OnBrowserDestroyed(CefRefPtr<CefBrowserView> browser_view,
                          CefRefPtr<CefBrowser> browser) override;
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

  // Synchronize the docked toolbar with the active content view.
  void UpdateBrowserToolbar();

  // Release window-owned controls without changing the content view tree.
  void DestroyWindowControls();

  // Create the menu model shown by the hamburger menu button.
  void CreateMenuModel();
  CefRefPtr<CefMenuButton> CreateMenuButton();

  // Merge content + tab bar draggable regions (each converted from its own
  // view's coordinates) and push the result to the window.
  void ApplyDraggableRegions();

  // Whether |view| is a direct child of this window. CefWindow can host
  // children inside an internal container view, so CefView::GetParentView()
  // must not be compared against the window itself.
  bool IsWindowChild(CefRefPtr<CefView> view) const;

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
  bool window_ui_initialized_ = false;
  bool window_closing_ = false;

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
