#include "bookmark_popup.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bookmark_model.h"
#include "config.h"
#include "inputhook.h"
#include "tabbookmark.h"
#include "utils.h"

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kPopupClassName[] = L"ChromePlusBookmarkPopup";
constexpr wchar_t kDragBadgeClassName[] = L"ChromePlusBookmarkDragBadge";
constexpr UINT kMsgFaviconReady = WM_APP + 0x41;
constexpr UINT_PTR kHoverTimerId = 0x424D4856;  // 'BMHV'
constexpr UINT kHoverDelayNormalMs = 110;
constexpr UINT kHoverDelaySafeTriangleMs = 280;
constexpr UINT kDefaultDpi = 96;

enum class ResizeMode {
  kNone = 0,
  kWidth,
  kHeight,
  kBoth,
};

enum class DropAction {
  kNone = 0,
  kInsertBefore,
  kInsertAfter,
  kDropIntoFolder,
  kSuperDrag,
};

struct PopupTheme {
  D2D1_COLOR_F bg{};
  D2D1_COLOR_F border{};
  D2D1_COLOR_F divider{};
  D2D1_COLOR_F text{};
  D2D1_COLOR_F muted{};
  D2D1_COLOR_F hover{};
  D2D1_COLOR_F accent{};
  D2D1_COLOR_F accent_soft{};
  D2D1_COLOR_F folder_fill{};
  D2D1_COLOR_F folder_stroke{};
  D2D1_COLOR_F scrollbar{};
  bool is_dark = false;
};

D2D1_COLOR_F ColorFromRgb(uint32_t rgb, float alpha = 1.0f) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
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

PopupTheme ResolveTheme() {
  const int mode = config.GetBookmarksTheme();
  bool dark = false;
  if (mode == 0) {
    dark = IsSystemDarkTheme();
  } else if (mode == 2) {
    dark = true;
  }

  PopupTheme t;
  if (mode == 3) {
    const uint32_t bg = config.GetBookmarksBgColor();
    const uint32_t fg = config.GetBookmarksTextColor();
    const uint32_t hv = config.GetBookmarksHoverColor();
    const int lum = ((bg >> 16) & 0xFF) * 299 + ((bg >> 8) & 0xFF) * 587 +
                    (bg & 0xFF) * 114;
    t.is_dark = lum < 128000;
    t.bg = ColorFromRgb(bg);
    t.text = ColorFromRgb(fg);
    t.hover = ColorFromRgb(hv);
    t.border = t.is_dark ? ColorFromRgb(0x4A4D52) : ColorFromRgb(0xCFD3D9);
    t.divider = t.is_dark ? ColorFromRgb(0x3E4147) : ColorFromRgb(0xE5E7EB);
    t.muted = t.is_dark ? ColorFromRgb(0x9AA0A6) : ColorFromRgb(0x5F6368);
  } else if (dark) {
    t.is_dark = true;
    t.bg = ColorFromRgb(0x292A2D);
    t.border = ColorFromRgb(0x45474A);
    t.divider = ColorFromRgb(0x3C4043);
    t.text = ColorFromRgb(0xE8EAED);
    t.muted = ColorFromRgb(0x9AA0A6);
    t.hover = ColorFromRgb(0x3C4043);
  } else {
    t.is_dark = false;
    t.bg = ColorFromRgb(0xF9F9FA);
    t.border = ColorFromRgb(0xD0D4DA);
    t.divider = ColorFromRgb(0xE5E7EB);
    t.text = ColorFromRgb(0x202124);
    t.muted = ColorFromRgb(0x5F6368);
    t.hover = ColorFromRgb(0xE8F0FE);
  }
  t.accent = t.is_dark ? ColorFromRgb(0x8AB4F8) : ColorFromRgb(0x1A73E8);
  t.accent_soft =
      t.is_dark ? ColorFromRgb(0x8AB4F8, 0.18f) : ColorFromRgb(0x1A73E8, 0.14f);
  t.folder_fill = ColorFromRgb(0xF6C343);
  t.folder_stroke = ColorFromRgb(0xD49B1A);
  t.scrollbar = t.is_dark ? ColorFromRgb(0x5F6368, 0.7f)
                          : ColorFromRgb(0xBDC1C6, 0.8f);
  return t;
}

UINT GetDpiForWindowSafe(HWND hwnd) {
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

void ApplyDwmRoundCorners(HWND hwnd, int logical_radius) {
  constexpr DWORD kDwmwaWindowCornerPreference = 33;
  enum DWM_WINDOW_CORNER_PREFERENCE {
    DWMWCP_DEFAULT = 0,
    DWMWCP_DONOTROUND = 1,
    DWMWCP_ROUND = 2,
    DWMWCP_ROUNDSMALL = 3,
  };
  const DWM_WINDOW_CORNER_PREFERENCE pref =
      logical_radius == 0 ? DWMWCP_DONOTROUND
                          : (logical_radius <= 4 ? DWMWCP_ROUNDSMALL
                                                 : DWMWCP_ROUND);
  ::DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &pref,
                          sizeof(pref));
}

struct SharedD2D {
  ComPtr<ID2D1Factory> d2d_factory;
  ComPtr<IDWriteFactory> dwrite_factory;
  bool attempted = false;

  bool Ensure() {
    if (d2d_factory && dwrite_factory) {
      return true;
    }
    if (attempted) {
      return false;
    }
    attempted = true;
    if (FAILED(::D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            d2d_factory.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (FAILED(::DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(
                dwrite_factory.ReleaseAndGetAddressOf())))) {
      d2d_factory.Reset();
      return false;
    }
    return true;
  }
};

SharedD2D& GetD2D() {
  static SharedD2D d2d;
  return d2d;
}

double Sign2D(POINT p1, POINT p2, POINT p3) {
  return static_cast<double>(p1.x - p3.x) * (p2.y - p3.y) -
         static_cast<double>(p2.x - p3.x) * (p1.y - p3.y);
}

bool PointInTriangle(POINT pt, POINT a, POINT b, POINT c) {
  const double d1 = Sign2D(pt, a, b);
  const double d2 = Sign2D(pt, b, c);
  const double d3 = Sign2D(pt, c, a);
  const bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
  const bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
  return !(has_neg && has_pos);
}

struct PopupWindow {
  HWND hwnd = nullptr;
  int level = 0;
  int parent_item_index = -1;
  std::wstring folder_id = L"__bookmark_bar__";
  BookmarkSource folder_source = BookmarkSource::kLocal;
  UINT dpi = kDefaultDpi;

  std::shared_ptr<const std::vector<BookmarkNode>> items;
  std::vector<BookmarkNode> empty_fallback;

  // Layout metrics (physical pixels)
  int col_width = 240;
  int item_height = 28;
  int icon_size = 16;
  int pad_x = 6;
  int pad_y = 6;
  int rows_per_col = 1;
  int col_count = 1;
  int scroll_x = 0;
  int max_scroll_x = 0;
  int scrollbar_h = 0;
  int width = 252;
  int height = 200;

  // Live override during edge resize (logical px; <=0 means use config)
  int override_col_width_logical = 0;
  int override_max_height_logical = 0;

  // Interaction state
  int hovered_index = -1;
  int open_child_index = -1;
  int pending_hover_index = -1;
  std::optional<POINT> last_mouse_screen;

  ResizeMode resize_mode = ResizeMode::kNone;
  POINT resize_start_pt{};
  int resize_start_col_w_logical = 0;
  int resize_start_max_h_logical = 0;

  // Rendering resources
  ComPtr<ID2D1HwndRenderTarget> rt;
  ComPtr<IDWriteTextFormat> text_format;
  std::wstring cached_font_family;
  float cached_font_size = 0.0f;
  std::unordered_map<const FaviconBitmap*, ComPtr<ID2D1Bitmap>> bitmap_cache;
};

struct DragState {
  bool pressing = false;
  bool active = false;
  PopupWindow* src_win = nullptr;
  int src_index = -1;
  BookmarkNode src_node;
  POINT start_screen{};

  PopupWindow* target_win = nullptr;
  int target_index = -1;
  DropAction action = DropAction::kNone;
  std::wstring target_folder_name;

  HWND badge_hwnd = nullptr;
  std::wstring last_badge_text;
  bool last_badge_accent = false;
};

struct PopupManagerState {
  HWND chrome_root = nullptr;
  HWND trigger_btn = nullptr;
  POINT root_anchor_pt{};
  std::shared_ptr<const std::vector<BookmarkNode>> root_snapshot;
  std::vector<std::unique_ptr<PopupWindow>> stack;
  DragState drag;
  bool class_registered = false;
  bool hooks_registered = false;
};

PopupManagerState& GetManager() {
  static PopupManagerState mgr;
  return mgr;
}

void ClosePopupsFromLevel(int from_level);
void OpenChildPopup(PopupWindow* parent, int item_index);
void RelayoutAndResizePopup(PopupWindow* win, bool is_reposition);
void CancelItemDrag();

const std::vector<BookmarkNode>& GetItemsRef(const PopupWindow* win) {
  if (win->items && !win->items->empty()) {
    return *win->items;
  }
  return win->empty_fallback;
}

PopupWindow* FindPopupByHwnd(HWND hwnd) {
  for (const auto& p : GetManager().stack) {
    if (p->hwnd == hwnd) {
      return p.get();
    }
  }
  return nullptr;
}

void InvalidateAllPopups() {
  for (const auto& p : GetManager().stack) {
    if (p->hwnd && ::IsWindow(p->hwnd)) {
      ::InvalidateRect(p->hwnd, nullptr, FALSE);
    }
  }
}

RECT GetWorkAreaAtPoint(POINT pt) {
  HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  if (mon && ::GetMonitorInfoW(mon, &mi)) {
    return mi.rcWork;
  }
  RECT rc{0, 0, ::GetSystemMetrics(SM_CXSCREEN),
          ::GetSystemMetrics(SM_CYSCREEN)};
  return rc;
}

RECT GetItemRect(const PopupWindow* win, int index) {
  const int col = index / win->rows_per_col;
  const int row = index % win->rows_per_col;
  const int left = win->pad_x + col * win->col_width - win->scroll_x;
  const int top = win->pad_y + row * win->item_height;
  return RECT{left, top, left + win->col_width, top + win->item_height};
}

int HitTestItem(const PopupWindow* win, POINT client_pt) {
  if (client_pt.y < win->pad_y ||
      client_pt.y >= win->height - win->pad_y - win->scrollbar_h) {
    return -1;
  }
  const int virtual_x = client_pt.x - win->pad_x + win->scroll_x;
  if (virtual_x < 0) {
    return -1;
  }
  const int col = virtual_x / win->col_width;
  const int row = (client_pt.y - win->pad_y) / win->item_height;
  if (col < 0 || col >= win->col_count || row < 0 || row >= win->rows_per_col) {
    return -1;
  }
  const int idx = col * win->rows_per_col + row;
  const auto& items = GetItemsRef(win);
  if (idx < 0 || idx >= static_cast<int>(items.size())) {
    return -1;
  }
  if (items[idx].id == L"__empty__") {
    return -1;
  }
  return idx;
}

ResizeMode HitTestResizeEdge(const PopupWindow* win, POINT client_pt) {
  if (win->level != 0) {
    return ResizeMode::kNone;
  }
  const int grip = ScalePx(6, win->dpi);
  const int corner = ScalePx(12, win->dpi);
  const bool on_right = client_pt.x >= win->width - grip;
  const bool on_bottom = client_pt.y >= win->height - grip;
  if (client_pt.x >= win->width - corner &&
      client_pt.y >= win->height - corner) {
    return ResizeMode::kBoth;
  }
  if (on_right) {
    return ResizeMode::kWidth;
  }
  if (on_bottom) {
    return ResizeMode::kHeight;
  }
  return ResizeMode::kNone;
}

// Floating layered drag badge so users clearly see Super Drag vs Reorder.
void HideDragBadge() {
  auto& drag = GetManager().drag;
  if (drag.badge_hwnd && ::IsWindow(drag.badge_hwnd)) {
    ::ShowWindow(drag.badge_hwnd, SW_HIDE);
  }
  drag.last_badge_text.clear();
}

void UpdateDragBadge(POINT screen_pt,
                     const std::wstring& text,
                     bool accent_style,
                     UINT dpi) {
  auto& mgr = GetManager();
  auto& drag = mgr.drag;
  if (!GetD2D().Ensure()) {
    return;
  }

  if (!drag.badge_hwnd || !::IsWindow(drag.badge_hwnd)) {
    drag.badge_hwnd = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST,
        kDragBadgeClassName, nullptr, WS_POPUP, 0, 0, 10, 10, mgr.chrome_root,
        nullptr, hInstance, nullptr);
    if (!drag.badge_hwnd) {
      return;
    }
  }

  const int max_w = ScalePx(260, dpi);
  const int h = ScalePx(28, dpi);
  const int pad_x = ScalePx(10, dpi);
  const float font_sz = static_cast<float>(ScalePx(12, dpi));
  const int bx = screen_pt.x + ScalePx(14, dpi);
  const int by = screen_pt.y + ScalePx(18, dpi);

  const bool need_repaint =
      (text != drag.last_badge_text) || (accent_style != drag.last_badge_accent);
  if (!need_repaint && ::IsWindowVisible(drag.badge_hwnd)) {
    ::SetWindowPos(drag.badge_hwnd, HWND_TOPMOST, bx, by, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE);
    return;
  }

  drag.last_badge_text = text;
  drag.last_badge_accent = accent_style;

  ComPtr<IDWriteTextFormat> fmt;
  if (FAILED(GetD2D().dwrite_factory->CreateTextFormat(
          config.GetBookmarksFontFamily().c_str(), nullptr,
          DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, font_sz, L"", &fmt))) {
    return;
  }
  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  ComPtr<IDWriteInlineObject> ellipsis;
  if (SUCCEEDED(GetD2D().dwrite_factory->CreateEllipsisTrimmingSign(
          fmt.Get(), &ellipsis))) {
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    fmt->SetTrimming(&trimming, ellipsis.Get());
  }

  int w = ScalePx(140, dpi);
  ComPtr<IDWriteTextLayout> layout;
  if (SUCCEEDED(GetD2D().dwrite_factory->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(),
          static_cast<float>(max_w - pad_x * 2), static_cast<float>(h),
          &layout))) {
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics))) {
      w = std::clamp(static_cast<int>(std::ceil(metrics.width)) + pad_x * 2 + 2,
                     ScalePx(72, dpi), max_w);
    }
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
  if (mem_dc && dib) {
    HGDIOBJ old_bmp = ::SelectObject(mem_dc, dib);
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1DCRenderTarget> dc_rt;
    if (SUCCEEDED(GetD2D().d2d_factory->CreateDCRenderTarget(&props, &dc_rt))) {
      RECT rc{0, 0, w, h};
      if (SUCCEEDED(dc_rt->BindDC(mem_dc, &rc))) {
        const PopupTheme theme = ResolveTheme();
        dc_rt->BeginDraw();
        dc_rt->Clear(D2D1::ColorF(0, 0, 0, 0));

        const D2D1_COLOR_F bg = accent_style
                                    ? ColorFromRgb(0x1A73E8, 0.94f)
                                    : (theme.is_dark
                                           ? ColorFromRgb(0x35363A, 0.95f)
                                           : ColorFromRgb(0xFFFFFF, 0.95f));
        const D2D1_COLOR_F fg = accent_style ? ColorFromRgb(0xFFFFFF, 1.0f)
                                             : theme.text;
        const D2D1_COLOR_F bd = accent_style ? ColorFromRgb(0x1557B0, 1.0f)
                                             : theme.border;

        ComPtr<ID2D1SolidColorBrush> bg_b;
        ComPtr<ID2D1SolidColorBrush> fg_b;
        ComPtr<ID2D1SolidColorBrush> bd_b;
        dc_rt->CreateSolidColorBrush(bg, &bg_b);
        dc_rt->CreateSolidColorBrush(fg, &fg_b);
        dc_rt->CreateSolidColorBrush(bd, &bd_b);

        const float rad = static_cast<float>(ScalePx(6, dpi));
        const D2D1_ROUNDED_RECT rrc = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, static_cast<float>(w) - 0.5f,
                        static_cast<float>(h) - 0.5f),
            rad, rad);
        dc_rt->FillRoundedRectangle(rrc, bg_b.Get());
        dc_rt->DrawRoundedRectangle(rrc, bd_b.Get(), 1.0f);
        dc_rt->DrawTextW(
            text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(),
            D2D1::RectF(static_cast<float>(pad_x), 0.0f,
                        static_cast<float>(w - pad_x), static_cast<float>(h)),
            fg_b.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        dc_rt->EndDraw();
      }
    }

    POINT dst_pt{bx, by};
    POINT src_pt{0, 0};
    SIZE sz{w, h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ::UpdateLayeredWindow(drag.badge_hwnd, screen_dc, &dst_pt, &sz, mem_dc,
                          &src_pt, 0, &blend, ULW_ALPHA);
    ::ShowWindow(drag.badge_hwnd, SW_SHOWNOACTIVATE);

    ::SelectObject(mem_dc, old_bmp);
  }
  if (dib) ::DeleteObject(dib);
  if (mem_dc) ::DeleteDC(mem_dc);
  ::ReleaseDC(nullptr, screen_dc);
}

void ComputePopupGeometry(PopupWindow* win,
                          POINT anchor_pt,
                          bool align_to_parent_right,
                          RECT parent_rect,
                          int* out_x,
                          int* out_y) {
  const auto& items = GetItemsRef(win);
  const int item_count = static_cast<int>(items.size());

  const int logical_col_w = win->override_col_width_logical > 0
                                ? win->override_col_width_logical
                                : config.GetBookmarksColumnWidth();
  const int logical_max_h = win->override_max_height_logical > 0
                                ? win->override_max_height_logical
                                : config.GetBookmarksMaxHeight();

  win->col_width = ScalePx(logical_col_w, win->dpi);
  win->item_height = ScalePx(config.GetBookmarksItemHeight(), win->dpi);
  win->icon_size = ScalePx(config.GetBookmarksIconSize(), win->dpi);
  win->pad_x = ScalePx(5, win->dpi);
  win->pad_y = ScalePx(5, win->dpi);

  const RECT work = GetWorkAreaAtPoint(anchor_pt);
  const int margin = ScalePx(8, win->dpi);
  const int work_w = (work.right - work.left) - margin * 2;
  const int work_h = (work.bottom - work.top) - margin * 2;

  int max_h_px = std::min(ScalePx(logical_max_h, win->dpi), work_h);
  bool open_upward = false;

  if (win->level == 0) {
    const int below_space = work.bottom - anchor_pt.y - margin;
    const int above_space = anchor_pt.y - work.top - margin;
    if (below_space >= ScalePx(240, win->dpi) || below_space >= above_space) {
      max_h_px =
          std::min(max_h_px, std::max(below_space, ScalePx(160, win->dpi)));
    } else {
      open_upward = true;
      max_h_px =
          std::min(max_h_px, std::max(above_space, ScalePx(160, win->dpi)));
    }
  }

  const int min_inner_h = win->item_height;
  const int avail_inner_h = std::max(min_inner_h, max_h_px - win->pad_y * 2);
  win->rows_per_col = std::max(1, avail_inner_h / win->item_height);
  win->col_count =
      std::max(1, (item_count + win->rows_per_col - 1) / win->rows_per_col);

  const int visible_rows = std::min(item_count, win->rows_per_col);
  const int total_w = win->pad_x * 2 + win->col_count * win->col_width;
  if (total_w > work_w && work_w > win->col_width) {
    win->width = work_w;
    win->scrollbar_h = ScalePx(10, win->dpi);
    win->max_scroll_x = total_w - win->width;
    win->scroll_x = std::clamp(win->scroll_x, 0, win->max_scroll_x);
  } else {
    win->width = total_w;
    win->scrollbar_h = 0;
    win->max_scroll_x = 0;
    win->scroll_x = 0;
  }

  win->height =
      win->pad_y * 2 + std::max(1, visible_rows) * win->item_height +
      win->scrollbar_h;
  if (win->height > max_h_px && win->rows_per_col > 1 && win->scrollbar_h > 0) {
    const int adj_inner =
        std::max(min_inner_h, max_h_px - win->pad_y * 2 - win->scrollbar_h);
    win->rows_per_col = std::max(1, adj_inner / win->item_height);
    win->col_count =
        std::max(1, (item_count + win->rows_per_col - 1) / win->rows_per_col);
    const int new_total_w = win->pad_x * 2 + win->col_count * win->col_width;
    win->max_scroll_x = std::max(0, new_total_w - win->width);
    win->scroll_x = std::clamp(win->scroll_x, 0, win->max_scroll_x);
    win->height = win->pad_y * 2 +
                  std::min(item_count, win->rows_per_col) * win->item_height +
                  win->scrollbar_h;
  }

  int x = anchor_pt.x;
  int y = anchor_pt.y;
  if (win->level == 0) {
    if (open_upward) {
      y = anchor_pt.y - win->height;
    }
    if (x + win->width > work.right - margin) {
      x = work.right - margin - win->width;
    }
    if (x < work.left + margin) {
      x = work.left + margin;
    }
    if (y + win->height > work.bottom - margin) {
      y = work.bottom - margin - win->height;
    }
    if (y < work.top + margin) {
      y = work.top + margin;
    }
  } else {
    if (align_to_parent_right) {
      x = parent_rect.right - ScalePx(2, win->dpi);
      if (x + win->width > work.right - margin) {
        x = parent_rect.left - win->width + ScalePx(2, win->dpi);
      }
    }
    if (x < work.left + margin) {
      x = work.left + margin;
    }
    if (x + win->width > work.right - margin) {
      x = work.right - margin - win->width;
    }
    if (y + win->height > work.bottom - margin) {
      y = work.bottom - margin - win->height;
    }
    if (y < work.top + margin) {
      y = work.top + margin;
    }
  }

  *out_x = x;
  *out_y = y;
}

void EnsureRenderResources(PopupWindow* win) {
  auto& d2d = GetD2D();
  if (!d2d.Ensure()) {
    return;
  }
  if (!win->rt) {
    const D2D1_RENDER_TARGET_PROPERTIES rt_props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
        D2D1::HwndRenderTargetProperties(
            win->hwnd, D2D1::SizeU(win->width, win->height));
    d2d.d2d_factory->CreateHwndRenderTarget(rt_props, hwnd_props,
                                            win->rt.ReleaseAndGetAddressOf());
    if (win->rt) {
      win->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }
  }

  const std::wstring font_family = config.GetBookmarksFontFamily();
  const float font_size =
      static_cast<float>(ScalePx(config.GetBookmarksFontSize(), win->dpi));
  if (!win->text_format || win->cached_font_family != font_family ||
      std::abs(win->cached_font_size - font_size) > 0.1f) {
    win->text_format.Reset();
    if (SUCCEEDED(d2d.dwrite_factory->CreateTextFormat(
            font_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font_size,
            L"", win->text_format.ReleaseAndGetAddressOf()))) {
      win->text_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      win->text_format->SetParagraphAlignment(
          DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      ComPtr<IDWriteInlineObject> ellipsis;
      if (SUCCEEDED(d2d.dwrite_factory->CreateEllipsisTrimmingSign(
              win->text_format.Get(), &ellipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        win->text_format->SetTrimming(&trimming, ellipsis.Get());
      }
      win->cached_font_family = font_family;
      win->cached_font_size = font_size;
    }
  }
}

ID2D1Bitmap* GetOrCreateD2DBitmap(PopupWindow* win, const FaviconBitmap* bmp) {
  if (!bmp || !win->rt || bmp->width <= 0 || bmp->height <= 0) {
    return nullptr;
  }
  auto it = win->bitmap_cache.find(bmp);
  if (it != win->bitmap_cache.end()) {
    return it->second.Get();
  }
  ComPtr<ID2D1Bitmap> d2d_bmp;
  const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(
      DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(win->rt->CreateBitmap(
          D2D1::SizeU(bmp->width, bmp->height), bmp->bgra_pixels.data(),
          static_cast<UINT32>(bmp->width * 4), props,
          d2d_bmp.ReleaseAndGetAddressOf()))) {
    return nullptr;
  }
  ID2D1Bitmap* raw = d2d_bmp.Get();
  win->bitmap_cache.emplace(bmp, std::move(d2d_bmp));
  return raw;
}

void DrawFolderIcon(ID2D1HwndRenderTarget* rt,
                    float x,
                    float y,
                    float size,
                    ID2D1SolidColorBrush* fill_brush,
                    ID2D1SolidColorBrush* stroke_brush) {
  const float tab_w = size * 0.44f;
  const float tab_h = size * 0.18f;
  const float top = y + size * 0.14f;
  const float bottom = y + size * 0.86f;
  const float left = x + size * 0.06f;
  const float right = x + size * 0.94f;

  const D2D1_RECT_F body = D2D1::RectF(left, top + tab_h * 0.7f, right, bottom);
  const D2D1_RECT_F tab =
      D2D1::RectF(left, top, left + tab_w, top + tab_h * 1.4f);
  rt->FillRoundedRectangle(D2D1::RoundedRect(tab, 1.5f, 1.5f), fill_brush);
  rt->FillRoundedRectangle(D2D1::RoundedRect(body, 2.0f, 2.0f), fill_brush);
  rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.0f, 2.0f), stroke_brush,
                           1.0f);
}

void DrawGlobeFallbackIcon(ID2D1HwndRenderTarget* rt,
                           float x,
                           float y,
                           float size,
                           ID2D1SolidColorBrush* brush) {
  const float cx = x + size * 0.5f;
  const float cy = y + size * 0.5f;
  const float r = size * 0.38f;
  rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, 1.1f);
  rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r * 0.48f, r), brush,
                  1.0f);
  rt->DrawLine(D2D1::Point2F(cx - r, cy), D2D1::Point2F(cx + r, cy), brush,
               1.0f);
}

void DrawFolderArrow(ID2D1HwndRenderTarget* rt,
                     float cx,
                     float cy,
                     float r,
                     ID2D1SolidColorBrush* brush) {
  ComPtr<ID2D1PathGeometry> geo;
  if (FAILED(GetD2D().d2d_factory->CreatePathGeometry(&geo))) {
    return;
  }
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(geo->Open(&sink))) {
    return;
  }
  sink->BeginFigure(D2D1::Point2F(cx - r * 0.5f, cy - r),
                    D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine(D2D1::Point2F(cx + r * 0.7f, cy));
  sink->AddLine(D2D1::Point2F(cx - r * 0.5f, cy + r));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->Close();
  rt->FillGeometry(geo.Get(), brush);
}

void PaintPopup(PopupWindow* win) {
  EnsureRenderResources(win);
  if (!win->rt) {
    return;
  }

  const D2D1_SIZE_U size = win->rt->GetPixelSize();
  if (size.width != static_cast<UINT32>(win->width) ||
      size.height != static_cast<UINT32>(win->height)) {
    win->rt->Resize(D2D1::SizeU(win->width, win->height));
  }

  const PopupTheme theme = ResolveTheme();
  const auto& drag = GetManager().drag;

  win->rt->BeginDraw();
  win->rt->Clear(theme.bg);

  ComPtr<ID2D1SolidColorBrush> border_brush;
  ComPtr<ID2D1SolidColorBrush> divider_brush;
  ComPtr<ID2D1SolidColorBrush> text_brush;
  ComPtr<ID2D1SolidColorBrush> muted_brush;
  ComPtr<ID2D1SolidColorBrush> hover_brush;
  ComPtr<ID2D1SolidColorBrush> accent_brush;
  ComPtr<ID2D1SolidColorBrush> accent_soft_brush;
  ComPtr<ID2D1SolidColorBrush> folder_fill;
  ComPtr<ID2D1SolidColorBrush> folder_stroke;
  ComPtr<ID2D1SolidColorBrush> scroll_brush;

  win->rt->CreateSolidColorBrush(theme.border, &border_brush);
  win->rt->CreateSolidColorBrush(theme.divider, &divider_brush);
  win->rt->CreateSolidColorBrush(theme.text, &text_brush);
  win->rt->CreateSolidColorBrush(theme.muted, &muted_brush);
  win->rt->CreateSolidColorBrush(theme.hover, &hover_brush);
  win->rt->CreateSolidColorBrush(theme.accent, &accent_brush);
  win->rt->CreateSolidColorBrush(theme.accent_soft, &accent_soft_brush);
  win->rt->CreateSolidColorBrush(theme.folder_fill, &folder_fill);
  win->rt->CreateSolidColorBrush(theme.folder_stroke, &folder_stroke);
  win->rt->CreateSolidColorBrush(theme.scrollbar, &scroll_brush);

  const float h_gap = static_cast<float>(ScalePx(8, win->dpi));
  const float arrow_space = static_cast<float>(ScalePx(16, win->dpi));
  const float item_radius = static_cast<float>(ScalePx(4, win->dpi));
  const float icon_sz = static_cast<float>(win->icon_size);

  // Column dividers
  for (int c = 1; c < win->col_count; ++c) {
    const float x = static_cast<float>(win->pad_x + c * win->col_width -
                                       win->scroll_x) -
                    0.5f;
    if (x > 0 && x < win->width) {
      win->rt->DrawLine(
          D2D1::Point2F(x, static_cast<float>(win->pad_y)),
          D2D1::Point2F(
              x, static_cast<float>(win->height - win->pad_y - win->scrollbar_h)),
          divider_brush.Get(), 1.0f);
    }
  }

  const auto& items = GetItemsRef(win);
  const int count = static_cast<int>(items.size());
  for (int i = 0; i < count; ++i) {
    const RECT rc = GetItemRect(win, i);
    if (rc.right <= 0 || rc.left >= win->width) {
      continue;
    }

    const BookmarkNode& node = items[i];
    const bool is_empty_placeholder = (node.id == L"__empty__");
    const bool is_drag_source =
        drag.active && drag.src_win == win && drag.src_index == i;
    const bool is_drop_folder =
        drag.active && drag.target_win == win && drag.target_index == i &&
        drag.action == DropAction::kDropIntoFolder;
    const bool is_active =
        !is_empty_placeholder && !drag.active &&
        (i == win->hovered_index || i == win->open_child_index);

    const D2D1_RECT_F bg_rc = D2D1::RectF(
        static_cast<float>(rc.left + ScalePx(2, win->dpi)),
        static_cast<float>(rc.top + 1),
        static_cast<float>(rc.right - ScalePx(2, win->dpi)),
        static_cast<float>(rc.bottom - 1));

    if (is_drop_folder) {
      win->rt->FillRoundedRectangle(
          D2D1::RoundedRect(bg_rc, item_radius, item_radius),
          accent_soft_brush.Get());
      win->rt->DrawRoundedRectangle(
          D2D1::RoundedRect(bg_rc, item_radius, item_radius),
          accent_brush.Get(), 1.5f);
    } else if (is_active) {
      win->rt->FillRoundedRectangle(
          D2D1::RoundedRect(bg_rc, item_radius, item_radius),
          hover_brush.Get());
    }

    const float icon_x = static_cast<float>(rc.left) + h_gap;
    const float icon_y =
        static_cast<float>(rc.top) +
        (static_cast<float>(win->item_height) - icon_sz) * 0.5f;

    if (!is_empty_placeholder) {
      if (node.is_folder) {
        DrawFolderIcon(win->rt.Get(), icon_x, icon_y, icon_sz,
                       folder_fill.Get(), folder_stroke.Get());
      } else {
        const auto fav = BookmarkModel::Instance().GetFavicon(node.url);
        ID2D1Bitmap* bmp = GetOrCreateD2DBitmap(win, fav.get());
        if (bmp) {
          win->rt->DrawBitmap(
              bmp, D2D1::RectF(icon_x, icon_y, icon_x + icon_sz,
                               icon_y + icon_sz),
              is_drag_source ? 0.45f : 1.0f,
              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
          DrawGlobeFallbackIcon(win->rt.Get(), icon_x, icon_y, icon_sz,
                                muted_brush.Get());
        }
      }
    }

    if (win->text_format) {
      const float text_left =
          is_empty_placeholder ? icon_x : icon_x + icon_sz + h_gap;
      const float text_right =
          static_cast<float>(rc.right) -
          (node.is_folder ? arrow_space : h_gap * 0.7f);
      if (text_right > text_left) {
        const std::wstring& label =
            node.title.empty() ? node.url : node.title;
        win->rt->DrawTextW(
            label.c_str(), static_cast<UINT32>(label.size()),
            win->text_format.Get(),
            D2D1::RectF(text_left, static_cast<float>(rc.top), text_right,
                        static_cast<float>(rc.bottom)),
            (is_empty_placeholder || is_drag_source) ? muted_brush.Get()
                                                     : text_brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
      }
    }

    if (node.is_folder) {
      const float ax =
          static_cast<float>(rc.right) - static_cast<float>(ScalePx(9, win->dpi));
      const float ay = static_cast<float>(rc.top + rc.bottom) * 0.5f;
      const float ar = static_cast<float>(ScalePx(3, win->dpi));
      DrawFolderArrow(win->rt.Get(), ax, ay, ar, muted_brush.Get());
    }
  }

  // Draw drag-reorder insertion indicator bar
  if (drag.active && drag.target_win == win &&
      (drag.action == DropAction::kInsertBefore ||
       drag.action == DropAction::kInsertAfter) &&
      drag.target_index >= 0 && drag.target_index < count) {
    const RECT rc = GetItemRect(win, drag.target_index);
    const float y = static_cast<float>(
        drag.action == DropAction::kInsertBefore ? rc.top : rc.bottom);
    const float x1 = static_cast<float>(rc.left + ScalePx(6, win->dpi));
    const float x2 = static_cast<float>(rc.right - ScalePx(6, win->dpi));
    const float dot_r = static_cast<float>(ScalePx(3, win->dpi));
    win->rt->DrawLine(D2D1::Point2F(x1, y), D2D1::Point2F(x2, y),
                      accent_brush.Get(), 2.0f);
    win->rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x1, y), dot_r, dot_r),
                         accent_brush.Get());
  }

  // Horizontal scrollbar if overflowing screen width
  if (win->scrollbar_h > 0 && win->max_scroll_x > 0) {
    const float total_w =
        static_cast<float>(win->pad_x * 2 + win->col_count * win->col_width);
    const float track_w = static_cast<float>(win->width - win->pad_x * 2);
    const float thumb_w =
        std::max(static_cast<float>(ScalePx(24, win->dpi)),
                 track_w * (static_cast<float>(win->width) / total_w));
    const float ratio =
        static_cast<float>(win->scroll_x) / static_cast<float>(win->max_scroll_x);
    const float thumb_x =
        static_cast<float>(win->pad_x) + ratio * (track_w - thumb_w);
    const float sb_y = static_cast<float>(win->height - win->scrollbar_h + 2);
    const float sb_h = static_cast<float>(win->scrollbar_h - 4);
    win->rt->FillRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(thumb_x, sb_y, thumb_x + thumb_w, sb_y + sb_h),
            sb_h * 0.5f, sb_h * 0.5f),
        scroll_brush.Get());
  }

  // Outer 1px border
  win->rt->DrawRectangle(
      D2D1::RectF(0.5f, 0.5f, static_cast<float>(win->width) - 0.5f,
                  static_cast<float>(win->height) - 0.5f),
      border_brush.Get(), 1.0f);

  if (win->rt->EndDraw() == D2DERR_RECREATE_TARGET) {
    win->rt.Reset();
    win->bitmap_cache.clear();
  }
}

void ExecuteBookmarkClick(PopupWindow* win, int index, bool background) {
  const auto& items = GetItemsRef(win);
  if (index < 0 || index >= static_cast<int>(items.size())) {
    return;
  }
  const BookmarkNode& node = items[index];
  if (node.is_folder) {
    OpenChildPopup(win, index);
    return;
  }
  if (node.url.empty()) {
    return;
  }
  HWND root = GetManager().chrome_root;
  std::wstring url = node.url;
  if (!background) {
    CloseAllBookmarkPopups();
  }
  OpenInNewTab(root, std::move(url), background);
}

const BookmarkNode* FindFolderNodeRecursive(
    const std::vector<BookmarkNode>& nodes,
    BookmarkSource source,
    const std::wstring& folder_id) {
  for (const auto& n : nodes) {
    if (n.is_folder && n.id == folder_id &&
        (n.is_virtual_root || n.source == source)) {
      return &n;
    }
    if (n.is_folder) {
      if (const BookmarkNode* hit =
              FindFolderNodeRecursive(n.children, source, folder_id)) {
        return hit;
      }
    }
  }
  return nullptr;
}

void ReloadPopupStackItems() {
  auto& mgr = GetManager();
  if (mgr.stack.empty()) {
    return;
  }
  mgr.root_snapshot = BookmarkModel::Instance().GetRootItems();

  for (size_t lvl = 0; lvl < mgr.stack.size(); ++lvl) {
    PopupWindow* win = mgr.stack[lvl].get();
    if (lvl == 0) {
      win->items = mgr.root_snapshot;
    } else {
      const BookmarkNode* folder = FindFolderNodeRecursive(
          *mgr.root_snapshot, win->folder_source, win->folder_id);
      if (!folder) {
        ClosePopupsFromLevel(static_cast<int>(lvl));
        if (lvl > 0) {
          mgr.stack[lvl - 1]->open_child_index = -1;
        }
        break;
      }
      win->items = std::make_shared<std::vector<BookmarkNode>>(folder->children);
      // Keep parent_item_index synced if parent order shifted.
      const auto& parent_items = GetItemsRef(mgr.stack[lvl - 1].get());
      for (size_t i = 0; i < parent_items.size(); ++i) {
        if (parent_items[i].is_folder && parent_items[i].id == win->folder_id) {
          win->parent_item_index = static_cast<int>(i);
          mgr.stack[lvl - 1]->open_child_index = static_cast<int>(i);
          break;
        }
      }
    }

    win->empty_fallback.clear();
    if (!win->items || win->items->empty()) {
      BookmarkNode empty_node;
      empty_node.id = L"__empty__";
      empty_node.title =
          lvl == 0 ? BookmarkModel::Instance().GetDiagnosticInfo()
                   : L"(空文件夹)";
      win->empty_fallback.push_back(std::move(empty_node));
    }
    win->hovered_index = -1;
    RelayoutAndResizePopup(win, lvl > 0);
    ::InvalidateRect(win->hwnd, nullptr, FALSE);
  }
}

void UpdateDragTarget(POINT screen_pt) {
  auto& mgr = GetManager();
  auto& drag = mgr.drag;
  drag.target_win = nullptr;
  drag.target_index = -1;
  drag.action = DropAction::kNone;
  drag.target_folder_name.clear();

  const bool is_url = !drag.src_node.is_folder && !drag.src_node.url.empty();

  // Hit-test open popup stack from topmost level down to root.
  PopupWindow* hit_win = nullptr;
  for (int i = static_cast<int>(mgr.stack.size()) - 1; i >= 0; --i) {
    RECT wr{};
    if (::GetWindowRect(mgr.stack[i]->hwnd, &wr) && ::PtInRect(&wr, screen_pt)) {
      hit_win = mgr.stack[i].get();
      break;
    }
  }

  if (hit_win) {
    POINT client_pt = screen_pt;
    ::ScreenToClient(hit_win->hwnd, &client_pt);
    const auto& items = GetItemsRef(hit_win);
    const int count = static_cast<int>(items.size());
    int hit = HitTestItem(hit_win, client_pt);

    if (hit >= 0) {
      const RECT rc = GetItemRect(hit_win, hit);
      const int rel_y = client_pt.y - rc.top;
      const int row_h = std::max(1, static_cast<int>(rc.bottom - rc.top));
      const BookmarkNode& target_node = items[hit];
      const bool is_same_node =
          (hit_win == drag.src_win && hit == drag.src_index);

      if (target_node.is_folder && !is_same_node &&
          rel_y >= row_h / 4 && rel_y <= (row_h * 3) / 4) {
        drag.target_win = hit_win;
        drag.target_index = hit;
        drag.action = DropAction::kDropIntoFolder;
        drag.target_folder_name = target_node.title;
      } else if (target_node.is_virtual_root) {
        // Can insert before the virtual Other Bookmarks folder or drop into it.
        if (rel_y < row_h / 3) {
          drag.target_win = hit_win;
          drag.target_index = hit;
          drag.action = DropAction::kInsertBefore;
        } else {
          drag.target_win = hit_win;
          drag.target_index = hit;
          drag.action = DropAction::kDropIntoFolder;
          drag.target_folder_name = target_node.title;
        }
      } else {
        drag.target_win = hit_win;
        drag.target_index = hit;
        drag.action =
            (rel_y < row_h / 2) ? DropAction::kInsertBefore
                                : DropAction::kInsertAfter;
      }
    } else if (count > 0 && items[0].id != L"__empty__") {
      // Below the last item of a column -> insert after the last item in that column.
      const int virtual_x = client_pt.x - hit_win->pad_x + hit_win->scroll_x;
      const int col = std::clamp(virtual_x / std::max(1, hit_win->col_width), 0,
                                 hit_win->col_count - 1);
      int last_in_col = std::min(count - 1, (col + 1) * hit_win->rows_per_col - 1);
      if (last_in_col >= 0 && items[last_in_col].is_virtual_root &&
          last_in_col > 0) {
        --last_in_col;
      }
      if (last_in_col >= 0 && !items[last_in_col].is_virtual_root) {
        drag.target_win = hit_win;
        drag.target_index = last_in_col;
        drag.action = DropAction::kInsertAfter;
      }
    } else if (hit_win != drag.src_win && hit_win->level > 0) {
      drag.target_win = hit_win;
      drag.target_index = -1;
      drag.action = DropAction::kInsertAfter;
    }

    // Check if reorder would be a no-op (dropping right onto/adjacent to self).
    if (drag.target_win == drag.src_win &&
        (drag.action == DropAction::kInsertBefore ||
         drag.action == DropAction::kInsertAfter)) {
      const bool no_op =
          (drag.target_index == drag.src_index) ||
          (drag.action == DropAction::kInsertAfter &&
           drag.target_index == drag.src_index - 1) ||
          (drag.action == DropAction::kInsertBefore &&
           drag.target_index == drag.src_index + 1);
      if (no_op) {
        const int dx = std::abs(screen_pt.x - drag.start_screen.x);
        const int flick_thresh = ScalePx(12, hit_win->dpi);
        if (is_url && dx >= flick_thresh) {
          drag.target_win = nullptr;
          drag.target_index = -1;
          drag.action = DropAction::kSuperDrag;
        } else {
          drag.target_win = nullptr;
          drag.target_index = -1;
          drag.action = DropAction::kNone;
        }
      }
    }
  } else if (is_url) {
    // Cursor dragged outside all popup windows -> Super Drag background open!
    drag.action = DropAction::kSuperDrag;
  }

  // Update cursor & floating drag badge.
  const UINT dpi = drag.src_win ? drag.src_win->dpi : kDefaultDpi;
  const std::wstring& label =
      drag.src_node.title.empty() ? drag.src_node.url : drag.src_node.title;
  if (drag.action == DropAction::kSuperDrag) {
    ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
    UpdateDragBadge(screen_pt, L"后台打开: " + label, true, dpi);
  } else if (drag.action == DropAction::kDropIntoFolder) {
    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    UpdateDragBadge(screen_pt,
                    L"移入「" + drag.target_folder_name + L"」", true, dpi);
  } else if (drag.action == DropAction::kInsertBefore ||
             drag.action == DropAction::kInsertAfter) {
    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    UpdateDragBadge(screen_pt, L"排序: " + label, false, dpi);
  } else {
    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    UpdateDragBadge(screen_pt, label, false, dpi);
  }

  InvalidateAllPopups();
}

void CancelItemDrag() {
  auto& drag = GetManager().drag;
  if (!drag.pressing && !drag.active) {
    return;
  }
  if (drag.src_win && ::GetCapture() == drag.src_win->hwnd) {
    ::ReleaseCapture();
  }
  HideDragBadge();
  drag.pressing = false;
  drag.active = false;
  drag.src_win = nullptr;
  drag.src_index = -1;
  drag.target_win = nullptr;
  drag.target_index = -1;
  drag.action = DropAction::kNone;
  drag.target_folder_name.clear();
  InvalidateAllPopups();
}

void CommitItemDrag(POINT release_screen) {
  auto& mgr = GetManager();
  auto& drag = mgr.drag;

  const DropAction action = drag.action;
  const BookmarkNode src_node = drag.src_node;
  PopupWindow* target_win = drag.target_win;
  const int target_index = drag.target_index;

  CancelItemDrag();

  if (action == DropAction::kSuperDrag) {
    if (!src_node.url.empty() && mgr.chrome_root) {
      // Allow dropping either over Chrome or via horizontal flick over popup.
      HWND root = mgr.chrome_root;
      std::wstring url = src_node.url;
      CloseAllBookmarkPopups();
      OpenInNewTab(root, std::move(url), /*background=*/true);
    }
    return;
  }

  if (!target_win || action == DropAction::kNone) {
    return;
  }

  const auto& target_items = GetItemsRef(target_win);
  const bool valid_idx =
      target_index >= 0 && target_index < static_cast<int>(target_items.size());

  BookmarkSource dst_source = target_win->folder_source;
  std::wstring dst_folder_id = target_win->folder_id;
  std::wstring anchor_id;
  bool insert_after = true;

  if (action == DropAction::kDropIntoFolder && valid_idx) {
    const BookmarkNode& folder = target_items[target_index];
    dst_source = folder.is_virtual_root ? src_node.source : folder.source;
    dst_folder_id = folder.is_virtual_root ? L"__other__" : folder.id;
    anchor_id.clear();
    insert_after = true;
  } else if ((action == DropAction::kInsertBefore ||
              action == DropAction::kInsertAfter) &&
             valid_idx) {
    const BookmarkNode& anchor = target_items[target_index];
    if (anchor.is_virtual_root) {
      // Inserting before virtual Other Bookmarks -> append to end of bookmark_bar.
      dst_source = src_node.source;
      dst_folder_id = L"__bookmark_bar__";
      anchor_id.clear();
      insert_after = true;
    } else {
      dst_source = anchor.source;
      anchor_id = anchor.id;
      insert_after = (action == DropAction::kInsertAfter);
    }
  } else if (action == DropAction::kInsertAfter && target_win->level > 0) {
    anchor_id.clear();
    insert_after = true;
  } else {
    (void)release_screen;
    return;
  }

  if (BookmarkModel::Instance().MoveBookmark(
          src_node.source, src_node.id, dst_source, dst_folder_id, anchor_id,
          insert_after, config.IsBookmarksShowOther())) {
    ReloadPopupStackItems();
  }
}

void HandleHoverTimer(PopupWindow* win) {
  ::KillTimer(win->hwnd, kHoverTimerId);
  if (GetManager().drag.active) {
    return;
  }
  const int target = win->pending_hover_index;
  if (target < 0) {
    if (win->open_child_index >= 0) {
      ClosePopupsFromLevel(win->level + 1);
      win->open_child_index = -1;
      ::InvalidateRect(win->hwnd, nullptr, FALSE);
    }
    return;
  }
  const auto& items = GetItemsRef(win);
  if (target >= static_cast<int>(items.size())) {
    return;
  }
  if (items[target].is_folder) {
    if (win->open_child_index != target) {
      OpenChildPopup(win, target);
    }
  } else if (win->open_child_index >= 0) {
    ClosePopupsFromLevel(win->level + 1);
    win->open_child_index = -1;
    ::InvalidateRect(win->hwnd, nullptr, FALSE);
  }
}

bool IsMovingTowardChildPopup(const PopupWindow* win, POINT cur_screen) {
  auto& stack = GetManager().stack;
  if (win->level + 1 >= static_cast<int>(stack.size()) ||
      !win->last_mouse_screen) {
    return false;
  }
  RECT child_rc{};
  if (!::GetWindowRect(stack[win->level + 1]->hwnd, &child_rc)) {
    return false;
  }
  const POINT prev = *win->last_mouse_screen;
  const bool child_on_right = child_rc.left >= prev.x;
  const LONG edge_x = child_on_right ? child_rc.left : child_rc.right;
  const POINT top_corner{edge_x, child_rc.top};
  const POINT bot_corner{edge_x, child_rc.bottom};
  return PointInTriangle(cur_screen, prev, top_corner, bot_corner);
}

LRESULT CALLBACK DragBadgeWndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wParam,
                                  LPARAM lParam) {
  if (msg == WM_MOUSEACTIVATE) {
    return MA_NOACTIVATE;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK PopupWndProc(HWND hwnd,
                              UINT msg,
                              WPARAM wParam,
                              LPARAM lParam) {
  PopupWindow* win = FindPopupByHwnd(hwnd);
  if (!win) {
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT ps{};
      ::BeginPaint(hwnd, &ps);
      PaintPopup(win);
      ::EndPaint(hwnd, &ps);
      return 0;
    }

    case kMsgFaviconReady:
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_SETCURSOR: {
      const auto& drag = GetManager().drag;
      if (drag.active) {
        ::SetCursor(::LoadCursorW(
            nullptr, drag.action == DropAction::kSuperDrag ? IDC_HAND
                                                           : IDC_ARROW));
        return TRUE;
      }
      if (win->level == 0 && LOWORD(lParam) == HTCLIENT) {
        POINT pt{};
        ::GetCursorPos(&pt);
        ::ScreenToClient(hwnd, &pt);
        const ResizeMode mode = HitTestResizeEdge(win, pt);
        if (mode == ResizeMode::kBoth) {
          ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENWSE));
          return TRUE;
        }
        if (mode == ResizeMode::kWidth) {
          ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
          return TRUE;
        }
        if (mode == ResizeMode::kHeight) {
          ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
          return TRUE;
        }
      }
      ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
      return TRUE;
    }

    case WM_CAPTURECHANGED:
      if (GetManager().drag.pressing || GetManager().drag.active) {
        CancelItemDrag();
      }
      return 0;

    case WM_TIMER:
      if (wParam == kHoverTimerId) {
        HandleHoverTimer(win);
        return 0;
      }
      break;

    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
      ::TrackMouseEvent(&tme);

      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (win->resize_mode != ResizeMode::kNone) {
        POINT cur_screen{};
        ::GetCursorPos(&cur_screen);
        const int dx_logical =
            UnscalePx(cur_screen.x - win->resize_start_pt.x, win->dpi);
        const int dy_logical =
            UnscalePx(cur_screen.y - win->resize_start_pt.y, win->dpi);
        if (win->resize_mode == ResizeMode::kWidth ||
            win->resize_mode == ResizeMode::kBoth) {
          win->override_col_width_logical =
              std::clamp(win->resize_start_col_w_logical + dx_logical, 150, 600);
        }
        if (win->resize_mode == ResizeMode::kHeight ||
            win->resize_mode == ResizeMode::kBoth) {
          win->override_max_height_logical =
              std::clamp(win->resize_start_max_h_logical + dy_logical, 200, 2000);
        }
        RelayoutAndResizePopup(win, false);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      auto& drag = GetManager().drag;
      if (drag.pressing && drag.src_win == win) {
        POINT cur_screen{};
        ::GetCursorPos(&cur_screen);
        if (!drag.active) {
          const int thresh = ScalePx(6, win->dpi);
          if (std::abs(cur_screen.x - drag.start_screen.x) >= thresh ||
              std::abs(cur_screen.y - drag.start_screen.y) >= thresh) {
            drag.active = true;
            ::KillTimer(hwnd, kHoverTimerId);
          }
        }
        if (drag.active) {
          UpdateDragTarget(cur_screen);
          return 0;
        }
      }

      POINT cur_screen = pt;
      ::ClientToScreen(hwnd, &cur_screen);

      const int hit = (HitTestResizeEdge(win, pt) == ResizeMode::kNone)
                          ? HitTestItem(win, pt)
                          : -1;
      if (hit != win->hovered_index) {
        win->hovered_index = hit;
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }

      if (hit != win->open_child_index) {
        win->pending_hover_index = hit;
        const UINT delay = IsMovingTowardChildPopup(win, cur_screen)
                               ? kHoverDelaySafeTriangleMs
                               : kHoverDelayNormalMs;
        ::SetTimer(hwnd, kHoverTimerId, delay, nullptr);
      } else {
        ::KillTimer(hwnd, kHoverTimerId);
      }
      win->last_mouse_screen = cur_screen;
      return 0;
    }

    case WM_MOUSELEAVE:
      if (win->resize_mode == ResizeMode::kNone && !GetManager().drag.active) {
        win->hovered_index = -1;
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;

    case WM_RBUTTONDOWN:
      if (GetManager().drag.pressing || GetManager().drag.active) {
        CancelItemDrag();
        return 0;
      }
      return 0;

    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const ResizeMode mode = HitTestResizeEdge(win, pt);
      if (mode != ResizeMode::kNone) {
        ClosePopupsFromLevel(win->level + 1);
        win->open_child_index = -1;
        win->resize_mode = mode;
        ::GetCursorPos(&win->resize_start_pt);
        win->resize_start_col_w_logical = config.GetBookmarksColumnWidth();
        win->resize_start_max_h_logical = config.GetBookmarksMaxHeight();
        ::SetCapture(hwnd);
        return 0;
      }

      const int hit = HitTestItem(win, pt);
      if (hit >= 0) {
        const auto& items = GetItemsRef(win);
        if (!items[hit].is_virtual_root) {
          auto& drag = GetManager().drag;
          drag.pressing = true;
          drag.active = false;
          drag.src_win = win;
          drag.src_index = hit;
          drag.src_node = items[hit];
          ::GetCursorPos(&drag.start_screen);
          ::SetCapture(hwnd);
        }
      }
      return 0;
    }

    case WM_LBUTTONUP: {
      if (win->resize_mode != ResizeMode::kNone) {
        ::ReleaseCapture();
        win->resize_mode = ResizeMode::kNone;
        const int final_w = win->override_col_width_logical > 0
                                ? win->override_col_width_logical
                                : config.GetBookmarksColumnWidth();
        const int final_h = win->override_max_height_logical > 0
                                ? win->override_max_height_logical
                                : config.GetBookmarksMaxHeight();
        win->override_col_width_logical = 0;
        win->override_max_height_logical = 0;
        Config::Instance().UpdateBookmarkPopupSize(final_w, final_h);
        return 0;
      }

      auto& drag = GetManager().drag;
      if (drag.pressing && drag.src_win == win) {
        if (drag.active) {
          POINT cur_screen{};
          ::GetCursorPos(&cur_screen);
          CommitItemDrag(cur_screen);
          return 0;
        }
        const int clicked_idx = drag.src_index;
        CancelItemDrag();
        const bool ctrl_held = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        ExecuteBookmarkClick(win, clicked_idx, /*background=*/ctrl_held);
        return 0;
      }

      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int hit = HitTestItem(win, pt);
      if (hit >= 0) {
        const bool ctrl_held = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        ExecuteBookmarkClick(win, hit, /*background=*/ctrl_held);
      }
      return 0;
    }

    case WM_MBUTTONUP: {
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      const int hit = HitTestItem(win, pt);
      if (hit >= 0) {
        ExecuteBookmarkClick(win, hit, /*background=*/true);
      }
      return 0;
    }

    case WM_MOUSEWHEEL: {
      if (win->max_scroll_x > 0) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const int step = win->col_width / 2;
        win->scroll_x = std::clamp(
            win->scroll_x + (delta > 0 ? -step : step), 0, win->max_scroll_x);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;
    }
  }

  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void EnsureWindowClassRegistered() {
  auto& mgr = GetManager();
  if (mgr.class_registered) {
    return;
  }
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = PopupWndProc;
  wc.hInstance = hInstance;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kPopupClassName;
  ::RegisterClassExW(&wc);

  WNDCLASSEXW badge_wc{sizeof(badge_wc)};
  badge_wc.lpfnWndProc = DragBadgeWndProc;
  badge_wc.hInstance = hInstance;
  badge_wc.lpszClassName = kDragBadgeClassName;
  ::RegisterClassExW(&badge_wc);

  mgr.class_registered = true;
}

void RelayoutAndResizePopup(PopupWindow* win, bool is_reposition) {
  int x = 0;
  int y = 0;
  RECT parent_rect{};
  POINT anchor = GetManager().root_anchor_pt;
  if (win->level > 0 &&
      win->level - 1 < static_cast<int>(GetManager().stack.size())) {
    const auto& parent = GetManager().stack[win->level - 1];
    ::GetWindowRect(parent->hwnd, &parent_rect);
    const RECT item_rc = GetItemRect(parent.get(), win->parent_item_index);
    POINT item_top_left{item_rc.left, item_rc.top};
    ::ClientToScreen(parent->hwnd, &item_top_left);
    anchor = POINT{parent_rect.right, item_top_left.y};
  }
  ComputePopupGeometry(win, anchor, win->level > 0, parent_rect, &x, &y);
  UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
  if (!is_reposition && win->level == 0) {
    flags |= SWP_NOMOVE;
  }
  ::SetWindowPos(win->hwnd, nullptr, x, y, win->width, win->height, flags);
  ApplyDwmRoundCorners(win->hwnd, config.GetBookmarksCornerRadius());
}

std::unique_ptr<PopupWindow> CreatePopupLevel(
    int level,
    int parent_item_index,
    std::wstring folder_id,
    BookmarkSource folder_source,
    std::shared_ptr<const std::vector<BookmarkNode>> items,
    POINT anchor_pt,
    bool align_to_parent_right,
    RECT parent_rect) {
  EnsureWindowClassRegistered();
  auto win = std::make_unique<PopupWindow>();
  win->level = level;
  win->parent_item_index = parent_item_index;
  win->folder_id = std::move(folder_id);
  win->folder_source = folder_source;
  win->items = std::move(items);
  if (!win->items || win->items->empty()) {
    BookmarkNode empty_node;
    empty_node.id = L"__empty__";
    empty_node.title =
        level == 0 ? BookmarkModel::Instance().GetDiagnosticInfo()
                   : L"(空文件夹)";
    win->empty_fallback.push_back(std::move(empty_node));
  }

  auto& mgr = GetManager();
  HWND owner = level == 0 ? mgr.chrome_root : mgr.stack[level - 1]->hwnd;
  win->dpi = GetDpiForWindowSafe(owner);

  int x = 0;
  int y = 0;
  ComputePopupGeometry(win.get(), anchor_pt, align_to_parent_right, parent_rect,
                       &x, &y);

  win->hwnd = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPopupClassName, nullptr, WS_POPUP,
      x, y, win->width, win->height, owner, nullptr, hInstance, nullptr);
  if (!win->hwnd) {
    return nullptr;
  }

  ApplyDwmRoundCorners(win->hwnd, config.GetBookmarksCornerRadius());
  ::ShowWindow(win->hwnd, SW_SHOWNOACTIVATE);
  ::InvalidateRect(win->hwnd, nullptr, FALSE);
  return win;
}

void ClosePopupsFromLevel(int from_level) {
  auto& mgr = GetManager();
  if (mgr.drag.src_win && mgr.drag.src_win->level >= from_level) {
    CancelItemDrag();
  }
  auto& stack = mgr.stack;
  while (static_cast<int>(stack.size()) > from_level) {
    auto win = std::move(stack.back());
    stack.pop_back();
    if (win->hwnd && ::IsWindow(win->hwnd)) {
      ::KillTimer(win->hwnd, kHoverTimerId);
      ::DestroyWindow(win->hwnd);
    }
  }
}

void OpenChildPopup(PopupWindow* parent, int item_index) {
  const auto& items = GetItemsRef(parent);
  if (item_index < 0 || item_index >= static_cast<int>(items.size())) {
    return;
  }
  const BookmarkNode& folder = items[item_index];
  if (!folder.is_folder) {
    return;
  }

  ClosePopupsFromLevel(parent->level + 1);
  parent->open_child_index = item_index;
  ::InvalidateRect(parent->hwnd, nullptr, FALSE);

  RECT parent_rect{};
  ::GetWindowRect(parent->hwnd, &parent_rect);
  const RECT item_rc = GetItemRect(parent, item_index);
  POINT item_top_left{item_rc.left, item_rc.top};
  ::ClientToScreen(parent->hwnd, &item_top_left);

  auto children_copy =
      std::make_shared<std::vector<BookmarkNode>>(folder.children);
  const std::wstring child_folder_id =
      folder.is_virtual_root ? L"__other__" : folder.id;
  if (auto child = CreatePopupLevel(
          parent->level + 1, item_index, child_folder_id, folder.source,
          std::move(children_copy), POINT{parent_rect.right, item_top_left.y},
          true, parent_rect)) {
    GetManager().stack.push_back(std::move(child));
  }
}

bool PopupMouseHook(WPARAM wParam, LPARAM lParam) {
  if (!IsBookmarkPopupOpen()) {
    return false;
  }
  if (GetManager().drag.active) {
    if (wParam == WM_RBUTTONDOWN) {
      CancelItemDrag();
      return true;
    }
    return false;
  }
  if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
      wParam == WM_MBUTTONDOWN || wParam == WM_NCLBUTTONDOWN ||
      wParam == WM_NCRBUTTONDOWN || wParam == WM_NCMBUTTONDOWN) {
    const auto* pmouse = reinterpret_cast<const MOUSEHOOKSTRUCT*>(lParam);
    HWND target = ::WindowFromPoint(pmouse->pt);
    if (target == GetManager().trigger_btn || IsBookmarkPopupWindow(target)) {
      return false;
    }
    CloseAllBookmarkPopups();
  }
  return false;
}

bool PopupKeyboardHook(WPARAM wParam, LPARAM lParam) {
  if (!IsBookmarkPopupOpen() || (lParam & 0x80000000)) {
    return false;
  }
  // Ignore synthetic SendInput keys (scan_code == 0), e.g. from OpenInNewTab.
  const UINT scan_code = (lParam >> 16) & 0xFF;
  if (scan_code == 0) {
    return false;
  }

  auto& mgr = GetManager();
  if (mgr.drag.pressing || mgr.drag.active) {
    if (wParam == VK_ESCAPE) {
      CancelItemDrag();
      return true;
    }
    return false;
  }

  auto& stack = mgr.stack;
  if (stack.empty()) {
    return false;
  }
  PopupWindow* top = stack.back().get();
  const auto& items = GetItemsRef(top);
  const int count = static_cast<int>(items.size());

  if (wParam == VK_ESCAPE) {
    if (stack.size() > 1) {
      ClosePopupsFromLevel(static_cast<int>(stack.size()) - 1);
      stack.back()->open_child_index = -1;
      ::InvalidateRect(stack.back()->hwnd, nullptr, FALSE);
    } else {
      CloseAllBookmarkPopups();
    }
    return true;
  }

  if (count == 0 || (count == 1 && items[0].id == L"__empty__")) {
    return false;
  }

  if (wParam == VK_DOWN || wParam == VK_UP) {
    const int delta = (wParam == VK_DOWN) ? 1 : -1;
    const int next =
        top->hovered_index < 0 ? 0 : (top->hovered_index + delta + count) % count;
    top->hovered_index = next;
    ::InvalidateRect(top->hwnd, nullptr, FALSE);
    return true;
  }

  if (wParam == VK_RIGHT) {
    if (top->hovered_index >= 0 && items[top->hovered_index].is_folder) {
      OpenChildPopup(top, top->hovered_index);
      return true;
    }
    if (top->hovered_index >= 0 &&
        top->hovered_index + top->rows_per_col < count) {
      top->hovered_index += top->rows_per_col;
      ::InvalidateRect(top->hwnd, nullptr, FALSE);
      return true;
    }
    return false;
  }

  if (wParam == VK_LEFT) {
    if (top->hovered_index >= top->rows_per_col) {
      top->hovered_index -= top->rows_per_col;
      ::InvalidateRect(top->hwnd, nullptr, FALSE);
      return true;
    }
    if (stack.size() > 1) {
      ClosePopupsFromLevel(static_cast<int>(stack.size()) - 1);
      stack.back()->open_child_index = -1;
      ::InvalidateRect(stack.back()->hwnd, nullptr, FALSE);
      return true;
    }
    return false;
  }

  if (wParam == VK_RETURN && top->hovered_index >= 0) {
    const bool ctrl_held = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    ExecuteBookmarkClick(top, top->hovered_index, /*background=*/ctrl_held);
    return true;
  }

  return false;
}

}  // namespace

void InitBookmarkPopup() {
  auto& mgr = GetManager();
  if (mgr.hooks_registered) {
    return;
  }
  mgr.hooks_registered = true;
  RegisterMouseHandler(PopupMouseHook, HandlerPriority::kHigh);
  RegisterKeyboardHandler(PopupKeyboardHook, HandlerPriority::kHigh);
}

void ToggleBookmarkPopup(HWND chrome_root,
                         POINT anchor_screen_pt,
                         HWND trigger_btn_hwnd) {
  auto& mgr = GetManager();
  if (IsBookmarkPopupOpen() && mgr.chrome_root == chrome_root) {
    CloseAllBookmarkPopups();
    return;
  }

  CloseAllBookmarkPopups();

  BookmarkModel::Instance().EnsureLoaded(config.IsBookmarksShowOther());
  mgr.chrome_root = chrome_root;
  mgr.trigger_btn = trigger_btn_hwnd;
  mgr.root_anchor_pt = anchor_screen_pt;
  mgr.root_snapshot = BookmarkModel::Instance().GetRootItems();

  BookmarkSource root_src = BookmarkSource::kLocal;
  if (mgr.root_snapshot && !mgr.root_snapshot->empty()) {
    root_src = (*mgr.root_snapshot)[0].source;
  }

  auto root_popup = CreatePopupLevel(
      0, -1, L"__bookmark_bar__", root_src, mgr.root_snapshot, anchor_screen_pt,
      false, RECT{});
  if (root_popup) {
    const HWND notify_hwnd = root_popup->hwnd;
    mgr.stack.push_back(std::move(root_popup));
    BookmarkModel::Instance().RequestFaviconsAsync(notify_hwnd,
                                                   kMsgFaviconReady);
  }
}

void CloseAllBookmarkPopups() {
  CancelItemDrag();
  auto& mgr = GetManager();
  if (mgr.drag.badge_hwnd && ::IsWindow(mgr.drag.badge_hwnd)) {
    ::DestroyWindow(mgr.drag.badge_hwnd);
    mgr.drag.badge_hwnd = nullptr;
  }
  ClosePopupsFromLevel(0);
  mgr.chrome_root = nullptr;
  mgr.trigger_btn = nullptr;
  mgr.root_snapshot.reset();
}

bool IsBookmarkPopupOpen() {
  return !GetManager().stack.empty();
}

bool IsBookmarkPopupWindow(HWND hwnd) {
  return hwnd && FindPopupByHwnd(hwnd) != nullptr;
}

void RefreshOpenBookmarkPopups() {
  auto& mgr = GetManager();
  if (mgr.stack.empty()) {
    return;
  }
  for (auto& win : mgr.stack) {
    win->text_format.Reset();
    RelayoutAndResizePopup(win.get(), true);
    ::InvalidateRect(win->hwnd, nullptr, FALSE);
  }
}
