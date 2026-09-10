// Copyright (c) 2021 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "myapp/cefclient/browser/views_overlay_controls.h"

#include <algorithm>
#include <array>
#include <string>

#include "include/views/cef_box_layout.h"
#include "include/views/cef_window.h"
#include "myapp/cefclient/browser/views_style.h"

namespace client {

namespace {

constexpr int kInsets = 4;
constexpr int kLocationBarPadding = 100;

// White with 80% opacity.
constexpr auto kBackgroundColor = CefColorSetARGB(255 * .80, 255, 255, 255);

// Window control glyphs, matching the Windows custom titlebar buttons
// (see views_window.cc CreateTitleBar): U+2212 minus, U+25A1 white square,
// U+2750 restore, U+00D7 multiplication sign.
CefString GetLabel(ViewsOverlayControls::Command command, bool maximized) {
  switch (command) {
    case ViewsOverlayControls::Command::kMinimize:
      return CefString(u"\u2212");
    case ViewsOverlayControls::Command::kMaximize:
      return maximized ? CefString(u"\u2750") : CefString(u"\u25A1");
    case ViewsOverlayControls::Command::kClose:
      return CefString(u"\u00D7");
  }
  NOTREACHED();
  return CefString();
}

std::array<ViewsOverlayControls::Command, 3> GetButtons() {
#if defined(OS_MAC)
  return {ViewsOverlayControls::Command::kClose,
          ViewsOverlayControls::Command::kMaximize,
          ViewsOverlayControls::Command::kMinimize};
#else
  return {ViewsOverlayControls::Command::kMinimize,
          ViewsOverlayControls::Command::kMaximize,
          ViewsOverlayControls::Command::kClose};
#endif
}

cef_docking_mode_t GetPanelDockingMode(bool use_bottom_controls) {
#if defined(OS_MAC)
  return use_bottom_controls ? CEF_DOCKING_MODE_BOTTOM_LEFT
                             : CEF_DOCKING_MODE_TOP_LEFT;
#else
  return use_bottom_controls ? CEF_DOCKING_MODE_BOTTOM_RIGHT
                             : CEF_DOCKING_MODE_TOP_RIGHT;
#endif
}
cef_docking_mode_t GetMenuDockingMode(bool use_bottom_controls) {
#if defined(OS_MAC)
  return use_bottom_controls ? CEF_DOCKING_MODE_BOTTOM_RIGHT
                             : CEF_DOCKING_MODE_TOP_RIGHT;
#else
  return use_bottom_controls ? CEF_DOCKING_MODE_BOTTOM_LEFT
                             : CEF_DOCKING_MODE_TOP_LEFT;
#endif
}

}  // namespace

ViewsOverlayControls::ViewsOverlayControls(bool with_window_buttons,
                                           bool use_bottom_controls)
    : with_window_buttons_(with_window_buttons),
      use_bottom_controls_(use_bottom_controls) {}

void ViewsOverlayControls::Initialize(CefRefPtr<CefWindow> window,
                                      CefRefPtr<CefMenuButton> menu_button,
                                      CefRefPtr<CefView> location_bar,
                                      bool is_chrome_toolbar,
                                      bool menu_in_panel) {
  DCHECK(!window_);

  // |menu_button| and |location_bar| are both optional. The frameless HTML tab
  // bar mode only needs the window control buttons overlay; the menu entry and
  // address bar are provided elsewhere (Chrome toolbar / HTML tab bar).

  window_ = window;
  window_maximized_ = window_->IsMaximized();

  CefInsets insets;
  if (use_bottom_controls_) {
    insets.Set(0, kInsets, kInsets, kInsets);
  } else {
    insets.Set(kInsets, kInsets, 0, kInsets);
  }

  if (with_window_buttons_) {
    // Window control buttons. These controls are currently text which means
    // that we can't use a transparent background because subpixel text
    // rendering will break.
    // See comments on the related DCHECK in Label::PaintText.
    panel_ = CefPanel::CreatePanel(this);

    // Use a horizontal box layout.
    CefBoxLayoutSettings panel_layout_settings;
    panel_layout_settings.horizontal = true;
    panel_->SetToBoxLayout(panel_layout_settings);

    // When requested, dock the menu button as the first (leftmost) child of the
    // panel so the group reads [menu][min][max][close], matching the Windows
    // custom titlebar layout.
    if (menu_in_panel && menu_button) {
      menu_button->SetBackgroundColor(kBackgroundColor);
      panel_->AddChildView(menu_button);
      has_panel_menu_ = true;
    }

    for (auto button : GetButtons()) {
      panel_->AddChildView(CreateButton(button));
    }
    panel_controller_ = window->AddOverlayView(
        panel_, GetPanelDockingMode(use_bottom_controls_),
        /*can_activate=*/false);
    panel_controller_->SetInsets(insets);
    panel_controller_->SetVisible(true);
  }

  // Menu button (optional). Only docked as a separate overlay when it is not
  // already part of the window control button panel above.
  if (menu_button && !menu_in_panel) {
    menu_button->SetBackgroundColor(kBackgroundColor);
    menu_controller_ = window_->AddOverlayView(
        menu_button, GetMenuDockingMode(use_bottom_controls_),
        /*can_activate=*/false);
    menu_controller_->SetInsets(insets);
    menu_controller_->SetVisible(true);
  }

  // Location bar (optional). Will be made visible in UpdateControls().
  if (location_bar) {
    location_bar_ = location_bar;
    is_chrome_toolbar_ = is_chrome_toolbar;
    // Use a 100% transparent background for the Chrome toolbar.
    location_bar_->SetBackgroundColor(is_chrome_toolbar_ ? 0
                                                         : kBackgroundColor);
    location_controller_ = window_->AddOverlayView(
        location_bar_, CEF_DOCKING_MODE_CUSTOM, /*can_activate=*/false);
  }
}

void ViewsOverlayControls::Destroy() {
  window_ = nullptr;
  panel_ = nullptr;
  if (panel_controller_) {
    panel_controller_->Destroy();
    panel_controller_ = nullptr;
  }
  if (menu_controller_) {
    menu_controller_->Destroy();
    menu_controller_ = nullptr;
  }
  location_bar_ = nullptr;
  if (location_controller_) {
    location_controller_->Destroy();
    location_controller_ = nullptr;
  }
}

void ViewsOverlayControls::UpdateControls() {
  // Without a location bar (frameless HTML tab bar mode) there is nothing to
  // reposition here; the window buttons overlay docks itself automatically.
  if (!location_bar_ || !location_controller_ || !menu_controller_) {
    MaybeUpdateMaximizeButton();
    return;
  }

  // Update location bar size, position and visibility.
  const auto window_bounds = window_->GetBounds();
  auto bounds = window_bounds;
  bounds.x = kLocationBarPadding;
  bounds.width -= kLocationBarPadding * 2;

  if (is_chrome_toolbar_) {
    // Fit the standard Chrome toolbar.
    const auto preferred_size = location_bar_->GetPreferredSize();
    bounds.height =
        std::max(menu_controller_->GetSize().height, preferred_size.height);
  } else {
    bounds.height = menu_controller_->GetSize().height;
  }

  if (use_bottom_controls_) {
    bounds.y = window_bounds.height - bounds.height - kInsets;
  } else {
    bounds.y = kInsets;
  }

  if (bounds.width < kLocationBarPadding * 2) {
    // Not enough space.
    location_controller_->SetVisible(false);
  } else {
    location_bar_->SetSize(CefSize(bounds.width, bounds.height));
    location_controller_->SetBounds(bounds);
    location_controller_->SetVisible(true);
  }

  MaybeUpdateMaximizeButton();
}

void ViewsOverlayControls::UpdateDraggableRegions(
    std::vector<CefDraggableRegion>& window_regions) {
  if (panel_controller_ && panel_controller_->IsVisible()) {
    window_regions.emplace_back(panel_controller_->GetBounds(),
                                /*draggable=*/false);
  }

  if (menu_controller_ && menu_controller_->IsVisible()) {
    window_regions.emplace_back(menu_controller_->GetBounds(),
                                /*draggable=*/false);
  }

  if (location_controller_ && location_controller_->IsVisible()) {
    window_regions.emplace_back(location_controller_->GetBounds(),
                                /*draggable=*/false);
  }
}

void ViewsOverlayControls::OnButtonPressed(CefRefPtr<CefButton> button) {
  auto command = static_cast<Command>(button->GetID());
  switch (command) {
    case ViewsOverlayControls::Command::kMinimize:
      window_->Minimize();
      break;
    case ViewsOverlayControls::Command::kMaximize:
      if (window_->IsMaximized()) {
        window_->Restore();
      } else {
        window_->Maximize();
      }
      break;
    case ViewsOverlayControls::Command::kClose:
      window_->Close();
      return;
  }

  // Explicitly reset button state because the button may have moved and it
  // won't receive the corresponding mouse move events.
  button->SetState(CEF_BUTTON_STATE_NORMAL);
  button->SetInkDropEnabled(false);
  button->SetInkDropEnabled(true);

  if (command == Command::kMaximize) {
    MaybeUpdateMaximizeButton();
  }
}

void ViewsOverlayControls::OnThemeChanged(CefRefPtr<CefView> view) {
  // Apply colors when the theme changes.
  views_style::OnThemeChanged(view);
}

CefRefPtr<CefLabelButton> ViewsOverlayControls::CreateButton(Command command) {
  CefRefPtr<CefLabelButton> button = CefLabelButton::CreateLabelButton(
      this, GetLabel(command, window_maximized_));
  button->SetID(static_cast<int>(command));
  button->SetInkDropEnabled(true);
  button->SetFocusable(false);  // Don't give focus to the button.
  return button;
}

void ViewsOverlayControls::MaybeUpdateMaximizeButton() {
  if (!with_window_buttons_ || window_->IsMaximized() == window_maximized_) {
    return;
  }
  window_maximized_ = !window_maximized_;

  auto max_button = panel_->GetChildViewAt(has_panel_menu_ ? 2 : 1);
  auto command = static_cast<Command>(max_button->GetID());
  DCHECK(command == Command::kMaximize);
  max_button->AsButton()->AsLabelButton()->SetText(
      GetLabel(command, window_maximized_));

  // Adjust overlay size and placement due to layout changing.
  panel_controller_->SizeToPreferredSize();
}

}  // namespace client
