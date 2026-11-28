#include "bookmark_button.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bookmark_popup.h"
#include "config.h"
#include "inputhook.h"
#include "uia.h"
#include "utils.h"

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kBtnClassName[] = L"ChromePlusBookmarkButton";
constexpr wchar_t kCoordinatorClassName[] = L"ChromePlusBookmarkCoordinator";
constexpr UINT_PTR kSyncTimerId = 0x424D4254;  // 'BMBT'
constexpr UINT kSyncIntervalMs = 300;
constexpr UINT kMsgRefreshUi = WM_APP + 0x42;
constexpr UINT kDefaultDpi = 96;

UINT GetDpiSafe(HWND hwnd) {
  const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
  if (user32) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    if (const auto fn = reinterpret_cast<GetDpiForWindowFn>(
            ::GetProcAddress(user32, "GetDpiForWindow"))) {
      const UINT dpi = fn(hwnd);
      if (dpi > 0) {
        return dpi;
      }
    }
  }
  return kDefaultDpi;
}

int ScalePx(int logical_px, UINT dpi) {
  return ::MulDiv(logical_px, static_cast<int>(dpi), kDefaultDpi);
}

int UnscalePx(int physical_px, UINT dpi) {
  return ::MulDiv(physical_px, kDefaultDpi, static_cast<int>(dpi));
}

bool IsSystemDarkTheme() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  if (::RegGetValueW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
          &size) == ERROR_SUCCESS) {
    return value == 0;
  }
  return false;
}

struct ButtonEntry {
  HWND chrome_root = nullptr;
  HWND btn_hwnd = nullptr;
  UINT dpi = kDefaultDpi;
  int size_px = 28;
  bool hovered = false;
  bool pressed = false;
  bool dragging = false;
  POINT drag_start_cursor{};
  int drag_start_offset_x = 0;
  int drag_start_offset_y = 0;
};

struct ButtonManager {
  HWND coordinator_hwnd = nullptr;
  HWINEVENTHOOK win_event_hook = nullptr;
  std::unordered_map<HWND, std::unique_ptr<ButtonEntry>> buttons;
  ComPtr<ID2D1Factory> d2d_factory;
  UINT hotkey_vk = 0;
  UINT hotkey_mods = 0;
  bool classes_registered = false;
};

ButtonManager& GetBtnManager() {
  static ButtonManager mgr;
  return mgr;
}

ButtonEntry* FindButtonByHwnd(HWND btn_hwnd) {
  for (const auto& [root, entry] : GetBtnManager().buttons) {
    if (entry->btn_hwnd == btn_hwnd) {
      return entry.get();
    }
  }
  return nullptr;
}

void PaintLayeredButton(ButtonEntry* entry) {
  if (!entry || !entry->btn_hwnd) {
    return;
  }
  auto& mgr = GetBtnManager();
  if (!mgr.d2d_factory) {
    if (FAILED(::D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            mgr.d2d_factory.ReleaseAndGetAddressOf()))) {
      return;
    }
  }

  const int w = entry->size_px;
  const int h = entry->size_px;
  if (w <= 0 || h <= 0) {
    return;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HDC screen_dc = ::GetDC(nullptr);
  HDC mem_dc = ::CreateCompatibleDC(screen_dc);
  HBITMAP dib =
      ::CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!mem_dc || !dib) {
    if (dib) ::DeleteObject(dib);
    if (mem_dc) ::DeleteDC(mem_dc);
    ::ReleaseDC(nullptr, screen_dc);
    return;
  }
  HGDIOBJ old_bmp = ::SelectObject(mem_dc, dib);

  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.0f, 96.0f);
  ComPtr<ID2D1DCRenderTarget> dc_rt;
  if (SUCCEEDED(mgr.d2d_factory->CreateDCRenderTarget(&props, &dc_rt))) {
    RECT rc{0, 0, w, h};
    if (SUCCEEDED(dc_rt->BindDC(mem_dc, &rc))) {
      dc_rt->BeginDraw();
      dc_rt->Clear(D2D1::ColorF(0, 0, 0, 0));

      const bool dark = (config.GetBookmarksTheme() == 2) ||
                        (config.GetBookmarksTheme() == 0 && IsSystemDarkTheme());

      // Invisible 1/255 hit-test fill when idle so no gray box clips the
      // adjacent address bar; Chrome-style circular pill on hover/active.
      D2D1_COLOR_F pill_bg =
          dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.004f)
               : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.004f);
      if (entry->pressed) {
        pill_bg = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f)
                       : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f);
      } else if (entry->hovered || IsBookmarkPopupOpen()) {
        pill_bg = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f)
                       : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f);
      }

      ComPtr<ID2D1SolidColorBrush> bg_brush;
      dc_rt->CreateSolidColorBrush(pill_bg, &bg_brush);
      const float cx = static_cast<float>(w) * 0.5f;
      const float cy = static_cast<float>(h) * 0.5f;
      const float pill_r = static_cast<float>(w) * 0.45f;
      dc_rt->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(cx, cy), pill_r, pill_r),
          bg_brush.Get());

      // Draw crisp 5-point star icon
      ComPtr<ID2D1PathGeometry> star_geo;
      if (SUCCEEDED(mgr.d2d_factory->CreatePathGeometry(&star_geo))) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(star_geo->Open(&sink))) {
          const float star_cy = cy + 0.5f;
          const float outer_r = static_cast<float>(w) * 0.28f;
          const float inner_r = outer_r * 0.44f;
          constexpr double kPi = std::numbers::pi;
          for (int i = 0; i < 10; ++i) {
            const double angle = -kPi / 2.0 + i * (kPi / 5.0);
            const float rad = (i % 2 == 0) ? outer_r : inner_r;
            const D2D1_POINT_2F pt = D2D1::Point2F(
                cx + static_cast<float>(std::cos(angle)) * rad,
                star_cy + static_cast<float>(std::sin(angle)) * rad);
            if (i == 0) {
              sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED);
            } else {
              sink->AddLine(pt);
            }
          }
          sink->EndFigure(D2D1_FIGURE_END_CLOSED);
          sink->Close();

          ComPtr<ID2D1SolidColorBrush> fill_brush;
          ComPtr<ID2D1SolidColorBrush> stroke_brush;
          dc_rt->CreateSolidColorBrush(D2D1::ColorF(0xF6C343, 1.0f),
                                       &fill_brush);
          dc_rt->CreateSolidColorBrush(
              dark ? D2D1::ColorF(0xE0A82E, 1.0f)
                   : D2D1::ColorF(0xC88A12, 1.0f),
              &stroke_brush);
          dc_rt->FillGeometry(star_geo.Get(), fill_brush.Get());
          dc_rt->DrawGeometry(star_geo.Get(), stroke_brush.Get(), 1.15f);
        }
      }

      dc_rt->EndDraw();
    }
  }

  POINT src_pt{0, 0};
  SIZE wnd_sz{w, h};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  ::UpdateLayeredWindow(entry->btn_hwnd, screen_dc, nullptr, &wnd_sz, mem_dc,
                        &src_pt, 0, &blend, ULW_ALPHA);

  ::SelectObject(mem_dc, old_bmp);
  ::DeleteObject(dib);
  ::DeleteDC(mem_dc);
  ::ReleaseDC(nullptr, screen_dc);
}

void UpdateButtonPosition(ButtonEntry* entry) {
  if (!entry || !entry->btn_hwnd) {
    return;
  }
  if (!config.IsBookmarksPopupEnabled() ||
      !::IsWindow(entry->chrome_root) ||
      !::IsWindowVisible(entry->chrome_root) ||
      ::IsIconic(entry->chrome_root) ||
      IsChromeWindowFullscreen(entry->chrome_root)) {
    ::ShowWindow(entry->btn_hwnd, SW_HIDE);
    return;
  }

  const int anchor_mode = config.GetBookmarksButtonAnchor();
  const auto anchor = FindBookmarkButtonAnchor(entry->chrome_root, anchor_mode);
  if (!anchor) {
    ::ShowWindow(entry->btn_hwnd, SW_HIDE);
    return;
  }

  entry->dpi = GetDpiSafe(entry->chrome_root);
  const int new_sz = ScalePx(config.GetBookmarksButtonSize(), entry->dpi);
  const int off_x = ScalePx(config.GetBookmarksButtonOffsetX(), entry->dpi);
  const int off_y = ScalePx(config.GetBookmarksButtonOffsetY(), entry->dpi);

  int x = 0;
  int y = 0;
  if (anchor_mode == 0) {
    if (anchor->has_gap_left) {
      x = (anchor->gap_left + anchor->rect.left - new_sz) / 2 + off_x;
    } else {
      x = anchor->rect.left - new_sz - ScalePx(2, entry->dpi) + off_x;
    }
    y = (anchor->rect.top + anchor->rect.bottom - new_sz) / 2 + off_y;
  } else if (anchor_mode == 1) {
    x = anchor->rect.left + ScalePx(4, entry->dpi) + off_x;
    y = (anchor->rect.top + anchor->rect.bottom - new_sz) / 2 + off_y;
  } else {
    x = anchor->rect.left + ScalePx(6, entry->dpi) + off_x;
    y = anchor->rect.top + ScalePx(4, entry->dpi) + off_y;
  }

  const bool size_changed = (new_sz != entry->size_px);
  entry->size_px = new_sz;
  ::SetWindowPos(entry->btn_hwnd, HWND_TOP, x, y, new_sz, new_sz,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  if (size_changed) {
    PaintLayeredButton(entry);
  }
}

void TriggerPopupForWindow(HWND chrome_root, HWND btn_hwnd) {
  POINT anchor{0, 0};
  if (btn_hwnd && ::IsWindow(btn_hwnd) && ::IsWindowVisible(btn_hwnd)) {
    RECT rc{};
    ::GetWindowRect(btn_hwnd, &rc);
    anchor = POINT{rc.left, rc.bottom + 2};
  } else if (const auto bar = FindBookmarkButtonAnchor(
                 chrome_root, config.GetBookmarksButtonAnchor())) {
    anchor = POINT{bar->rect.left, bar->rect.bottom + 2};
  } else {
    RECT rc{};
    ::GetWindowRect(chrome_root, &rc);
    anchor = POINT{rc.left + 12, rc.top + 76};
  }
  ToggleBookmarkPopup(chrome_root, anchor, btn_hwnd);
}

LRESULT CALLBACK ButtonWndProc(HWND hwnd,
                               UINT msg,
                               WPARAM wParam,
                               LPARAM lParam) {
  ButtonEntry* entry = FindButtonByHwnd(hwnd);
  if (!entry) {
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;

    case WM_MOUSEMOVE: {
      if (entry->dragging) {
        POINT cur{};
        ::GetCursorPos(&cur);
        const int dx = cur.x - entry->drag_start_cursor.x;
        const int dy = cur.y - entry->drag_start_cursor.y;
        const int new_off_x =
            entry->drag_start_offset_x + UnscalePx(dx, entry->dpi);
        const int new_off_y =
            entry->drag_start_offset_y + UnscalePx(dy, entry->dpi);
        Config::Instance().UpdateBookmarkButtonOffset(new_off_x, new_off_y);
        UpdateButtonPosition(entry);
        return 0;
      }
      if (!entry->hovered) {
        entry->hovered = true;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&tme);
        PaintLayeredButton(entry);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      entry->hovered = false;
      entry->pressed = false;
      PaintLayeredButton(entry);
      return 0;

    case WM_LBUTTONDOWN: {
      if (::GetKeyState(VK_MENU) & 0x8000) {
        entry->dragging = true;
        ::GetCursorPos(&entry->drag_start_cursor);
        entry->drag_start_offset_x = config.GetBookmarksButtonOffsetX();
        entry->drag_start_offset_y = config.GetBookmarksButtonOffsetY();
        ::SetCapture(hwnd);
        return 0;
      }
      entry->pressed = true;
      PaintLayeredButton(entry);
      return 0;
    }

    case WM_LBUTTONUP: {
      if (entry->dragging) {
        ::ReleaseCapture();
        entry->dragging = false;
        return 0;
      }
      entry->pressed = false;
      PaintLayeredButton(entry);
      TriggerPopupForWindow(entry->chrome_root, entry->btn_hwnd);
      PaintLayeredButton(entry);
      return 0;
    }
  }

  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL CALLBACK EnumChromeRootsProc(HWND hwnd, LPARAM lParam) {
  auto* list = reinterpret_cast<std::vector<HWND>*>(lParam);
  if (!IsChromeWindow(hwnd)) {
    return TRUE;
  }
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (pid == ::GetCurrentProcessId() && !::GetParent(hwnd)) {
    list->push_back(hwnd);
  }
  return TRUE;
}

void SyncButtonsWithChromeWindows() {
  auto& mgr = GetBtnManager();
  std::vector<HWND> roots;
  ::EnumWindows(EnumChromeRootsProc, reinterpret_cast<LPARAM>(&roots));

  // Remove stale buttons for closed Chrome windows.
  for (auto it = mgr.buttons.begin(); it != mgr.buttons.end();) {
    if (!::IsWindow(it->first)) {
      if (it->second->btn_hwnd && ::IsWindow(it->second->btn_hwnd)) {
        ::DestroyWindow(it->second->btn_hwnd);
      }
      it = mgr.buttons.erase(it);
    } else {
      ++it;
    }
  }

  if (!config.IsBookmarksPopupEnabled()) {
    for (auto& [root, entry] : mgr.buttons) {
      if (entry->btn_hwnd) {
        ::ShowWindow(entry->btn_hwnd, SW_HIDE);
      }
    }
    return;
  }

  for (HWND root : roots) {
    auto it = mgr.buttons.find(root);
    if (it == mgr.buttons.end()) {
      auto anchor =
          FindBookmarkButtonAnchor(root, config.GetBookmarksButtonAnchor());
      if (!anchor) {
        continue;
      }
      auto entry = std::make_unique<ButtonEntry>();
      entry->chrome_root = root;
      entry->dpi = GetDpiSafe(root);
      entry->size_px = ScalePx(config.GetBookmarksButtonSize(), entry->dpi);
      entry->btn_hwnd = ::CreateWindowExW(
          WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBtnClassName,
          nullptr, WS_POPUP, 0, 0, entry->size_px, entry->size_px, root,
          nullptr, hInstance, nullptr);
      if (!entry->btn_hwnd) {
        continue;
      }
      ButtonEntry* raw = entry.get();
      mgr.buttons.emplace(root, std::move(entry));
      PaintLayeredButton(raw);
      UpdateButtonPosition(raw);
    } else {
      UpdateButtonPosition(it->second.get());
    }
  }
}

void CALLBACK WinEventProc(HWINEVENTHOOK,
                           DWORD event,
                           HWND hwnd,
                           LONG id_object,
                           LONG,
                           DWORD,
                           DWORD) {
  if (id_object != OBJID_WINDOW || !hwnd) {
    return;
  }
  auto& mgr = GetBtnManager();
  auto it = mgr.buttons.find(hwnd);
  if (it != mgr.buttons.end()) {
    if (event == EVENT_OBJECT_DESTROY) {
      if (it->second->btn_hwnd) {
        ::DestroyWindow(it->second->btn_hwnd);
      }
      mgr.buttons.erase(it);
      return;
    }
    UpdateButtonPosition(it->second.get());
  }
}

LRESULT CALLBACK CoordinatorWndProc(HWND hwnd,
                                    UINT msg,
                                    WPARAM wParam,
                                    LPARAM lParam) {
  switch (msg) {
    case WM_TIMER:
      if (wParam == kSyncTimerId) {
        SyncButtonsWithChromeWindows();
        return 0;
      }
      break;
    case kMsgRefreshUi:
      for (auto& [root, entry] : GetBtnManager().buttons) {
        PaintLayeredButton(entry.get());
        UpdateButtonPosition(entry.get());
      }
      RefreshOpenBookmarkPopups();
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool BookmarkHotkeyHandler(WPARAM wParam, LPARAM lParam) {
  if ((lParam & 0x80000000) || !config.IsBookmarksPopupEnabled()) {
    return false;
  }
  const auto& mgr = GetBtnManager();
  if (mgr.hotkey_vk == 0 || wParam != mgr.hotkey_vk) {
    return false;
  }
  const bool shift_ok =
      !(mgr.hotkey_mods & MOD_SHIFT) || IsKeyPressed(VK_SHIFT);
  const bool ctrl_ok =
      !(mgr.hotkey_mods & MOD_CONTROL) || IsKeyPressed(VK_CONTROL);
  const bool alt_ok = !(mgr.hotkey_mods & MOD_ALT) || IsKeyPressed(VK_MENU);
  const bool no_extra_shift =
      (mgr.hotkey_mods & MOD_SHIFT) || !IsKeyPressed(VK_SHIFT);
  const bool no_extra_ctrl =
      (mgr.hotkey_mods & MOD_CONTROL) || !IsKeyPressed(VK_CONTROL);
  const bool no_extra_alt =
      (mgr.hotkey_mods & MOD_ALT) || !IsKeyPressed(VK_MENU);

  if (!(shift_ok && ctrl_ok && alt_ok && no_extra_shift && no_extra_ctrl &&
        no_extra_alt)) {
    return false;
  }

  HWND fg = ::GetForegroundWindow();
  HWND root = ::GetAncestor(fg, GA_ROOT);
  if (!root || !IsChromeWindow(root)) {
    return false;
  }
  HWND btn = nullptr;
  if (auto it = mgr.buttons.find(root); it != mgr.buttons.end()) {
    btn = it->second->btn_hwnd;
  }
  TriggerPopupForWindow(root, btn);
  return true;
}

}  // namespace

void BookmarkButton() {
  InitBookmarkPopup();

  auto& mgr = GetBtnManager();
  if (!mgr.classes_registered) {
    WNDCLASSEXW btn_wc{sizeof(btn_wc)};
    btn_wc.lpfnWndProc = ButtonWndProc;
    btn_wc.hInstance = hInstance;
    btn_wc.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
    btn_wc.lpszClassName = kBtnClassName;
    ::RegisterClassExW(&btn_wc);

    WNDCLASSEXW coord_wc{sizeof(coord_wc)};
    coord_wc.lpfnWndProc = CoordinatorWndProc;
    coord_wc.hInstance = hInstance;
    coord_wc.lpszClassName = kCoordinatorClassName;
    ::RegisterClassExW(&coord_wc);

    mgr.classes_registered = true;
  }

  if (!mgr.coordinator_hwnd) {
    mgr.coordinator_hwnd = ::CreateWindowExW(
        0, kCoordinatorClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, hInstance, nullptr);
    if (mgr.coordinator_hwnd) {
      ::SetTimer(mgr.coordinator_hwnd, kSyncTimerId, kSyncIntervalMs, nullptr);
    }
  }

  if (!mgr.win_event_hook) {
    mgr.win_event_hook = ::SetWinEventHook(
        EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        WinEventProc, ::GetCurrentProcessId(), ::GetCurrentThreadId(),
        WINEVENT_OUTOFCONTEXT);
  }

  const std::wstring& hk = config.GetBookmarksHotkey();
  if (!hk.empty()) {
    const UINT parsed = ParseHotkeys(hk, /*no_repeat=*/false);
    mgr.hotkey_mods = LOWORD(parsed);
    mgr.hotkey_vk = HIWORD(parsed);
    if (mgr.hotkey_vk != 0) {
      RegisterKeyboardHandler(BookmarkHotkeyHandler, HandlerPriority::kHigh);
    }
  }
}

void RefreshBookmarkButtons() {
  HWND coord = GetBtnManager().coordinator_hwnd;
  if (coord && ::IsWindow(coord)) {
    ::PostMessageW(coord, kMsgRefreshUi, 0, 0);
  }
}
