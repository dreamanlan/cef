// Copyright 2020 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CEF_LIBCEF_BROWSER_CHROME_BROWSER_DELEGATE_H_
#define CEF_LIBCEF_BROWSER_CHROME_BROWSER_DELEGATE_H_
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "chrome/browser/ui/page_action/page_action_icon_type.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom-forward.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/base/window_open_disposition.h"

class Browser;
class BrowserUiController;
class BrowserWebContentsDelegate;
class BrowserWindow;
class BrowserWindowInterface;
class DesktopBrowserWindowCapabilities;
class ExclusiveAccessManager;
class GURL;
class Profile;
class UnloadController;

namespace chrome {
class BrowserCommandController;
}

namespace content {
struct GlobalRenderFrameHostId;
class WebContents;
}  // namespace content

namespace gfx {
class Rect;
}

namespace web_app {
class AppBrowserController;
}

namespace cef {

// Delegate for the chrome Browser object. Lifespan is controlled by the Browser
// object. See the ChromeBrowserDelegate documentation for additional details.
// Only accessed on the UI thread.
class BrowserDelegate {
 public:
  // Opaque ref-counted base class for CEF-specific parameters passed via
  // Browser::CreateParams::cef_params and possibly shared by multiple Browser
  // instances.
  class CreateParams : public base::RefCounted<CreateParams> {
   public:
    virtual ~CreateParams() = default;
  };

  // Called from the Browser constructor to create a new delegate.
  static std::unique_ptr<BrowserDelegate> Create(
      Browser* browser,
      scoped_refptr<CreateParams> cef_params,
      const BrowserWindowInterface* opener);

  // Called from BrowserWindowFeatures after the window and its controllers
  // have been initialized. The returned delegate is owned by that features
  // object and implements CEF's WebContents-specific behavior.
  static std::unique_ptr<BrowserWebContentsDelegate> CreateWebContentsDelegate(
      BrowserWindowInterface* browser,
      ExclusiveAccessManager& exclusive_access_manager,
      chrome::BrowserCommandController& command_controller,
      UnloadController& unload_controller,
      web_app::AppBrowserController* app_browser_controller,
      BrowserWindow& window,
      DesktopBrowserWindowCapabilities& capabilities,
      BrowserUiController& browser_ui_controller);

  // Optionally override Browser creation in
  // DevToolsWindow::CreateDevToolsBrowser. The returned Browser, if any, will
  // take ownership of |devtools_contents|.
  static Browser* CreateDevToolsBrowser(
      Profile* profile,
      BrowserWindowInterface* opener,
      content::WebContents* inspected_web_contents,
      std::unique_ptr<content::WebContents>& devtools_contents);

  virtual ~BrowserDelegate() = default;

  // Optionally override chrome::AddWebContents behavior. This is most often
  // called via Browser::AddNewContents for new popup browsers and provides an
  // opportunity for CEF to create a new Browser instead of proceeding with
  // default Browser or tab creation.
  virtual std::unique_ptr<content::WebContents> AddWebContents(
      std::unique_ptr<content::WebContents> new_contents) = 0;

  // Called immediately after |new_contents| is created via chrome::Navigate.
  // This is most often called for navigations targeting a new tab without a
  // pre-existing WebContents.
  virtual void OnWebContentsCreated(content::WebContents* new_contents) = 0;

  // Initialize the CEF browser host for a renderer-created popup. Called by
  // ChromeBrowserWebContentsDelegate after Chrome initializes the tab helpers.
  virtual void OnPopupWebContentsCreated(
      content::WebContents* source_contents,
      const content::GlobalRenderFrameHostId& opener_id,
      const std::string& frame_name,
      const GURL& target_url,
      content::WebContents* new_contents) = 0;

  // Add or remove ownership of the WebContents.
  virtual void SetAsDelegate(content::WebContents* web_contents,
                             bool set_delegate) = 0;

  // Return true to show the status bubble. This should consistently return the
  // same value for the lifespan of a Browser.
  virtual bool ShowStatusBubble(bool show_by_default) {
    return show_by_default;
  }

  // Return true to handle (or disable) a command. ID values come from
  // chrome/app/chrome_command_ids.h.
  virtual bool HandleCommand(int command_id,
                             WindowOpenDisposition disposition) {
    return false;
  }

  // Return true if the app menu item should be visible. ID values come from
  // chrome/app/chrome_command_ids.h.
  virtual bool IsAppMenuItemVisible(int command_id) { return true; }

  // Return true if the app menu item should be enabled. ID values come from
  // chrome/app/chrome_command_ids.h.
  virtual bool IsAppMenuItemEnabled(int command_id) { return true; }

  // Return true if the page action icon should be visible.
  virtual bool IsPageActionIconVisible(PageActionIconType icon_type) {
    return true;
  }

  enum class ToolbarButtonType {
    kCast_DEPRECATED = 0,
    kDownload_DEPRECATED,
    kSendTabToSelf_DEPRECATED,
    kSidePanel_DEPRECATED,
    kMedia,
    kTabSearch_DEPRECATED,
    kBatterySaver,
    kAvatar,
    kMaxValue = kAvatar,
  };

  // Return true if the toolbar button should be visible.
  virtual bool IsToolbarButtonVisible(ToolbarButtonType button_type) {
    return true;
  }

  // Optionally modify the bounding box for the Find bar.
  virtual void UpdateFindBarBoundingBox(gfx::Rect* bounds) {}

  // Optionally modify the top inset for dialogs.
  virtual void UpdateDialogTopInset(int* dialog_top_y) {}

  // Optionally override support for the specified window feature of type
  // Browser::WindowFeature (passed as underlying int to avoid circular
  // include).
  virtual std::optional<bool> SupportsWindowFeature(int feature) const {
    return std::nullopt;
  }

  // Returns true if draggable regions are supported.
  virtual bool SupportsDraggableRegion() const { return false; }

  // Update the window's draggable region, or forward the update to its
  // contents when window-level draggable regions are not supported.
  virtual void UpdateDraggableRegions(
      const std::vector<blink::mojom::DraggableRegionPtr>& regions,
      content::WebContents* contents) = 0;

  // Returns the draggable region, if any, relative to the web contents.
  // Called from PictureInPictureBrowserFrameView::NonClientHitTest and
  // BrowserView::ShouldDescendIntoChildForEventHandling.
  virtual const std::optional<SkRegion> GetDraggableRegion() const {
    return std::nullopt;
  }

  // Called at the end of a fullscreen transition.
  virtual void WindowFullscreenStateChanged() {}

  // Returns true if this browser is Views-hosted.
  virtual bool IsViewsHosted() const { return false; }

  // Returns true if this browser has a Views-hosted opener. Only
  // applicable for Browsers of type picture_in_picture and devtools.
  virtual bool HasViewsHostedOpener() const { return false; }
};

}  // namespace cef

#endif  // CEF_LIBCEF_BROWSER_CHROME_BROWSER_DELEGATE_H_
