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

struct PopupTheme {
  D2D1_COLOR_F bg{};
  D2D1_COLOR_F border{};
  D2D1_COLOR_F divider{};
  D2D1_COLOR_F text{};
  D2D1_COLOR_F muted{};
  D2D1_COLOR_F hover{};
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
  // Windows 11 DWMWA_WINDOW_CORNER_PREFERENCE (33)
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

// Point-in-triangle test for hover safe-triangle hysteresis.
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

struct PopupManagerState {
  HWND chrome_root = nullptr;
  HWND trigger_btn = nullptr;
  POINT root_anchor_pt{};
  std::shared_ptr<const std::vector<BookmarkNode>> root_snapshot;
  std::vector<std::unique_ptr<PopupWindow>> stack;
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
      max_h_px = std::min(max_h_px, std::max(below_space, ScalePx(160, win->dpi)));
    } else {
      open_upward = true;
      max_h_px = std::min(max_h_px, std::max(above_space, ScalePx(160, win->dpi)));
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
    // Re-fit one fewer row if horizontal scrollbar pushed height past max_h_px.
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

  // Back tab + body
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

  win->rt->BeginDraw();
  win->rt->Clear(theme.bg);

  ComPtr<ID2D1SolidColorBrush> border_brush;
  ComPtr<ID2D1SolidColorBrush> divider_brush;
  ComPtr<ID2D1SolidColorBrush> text_brush;
  ComPtr<ID2D1SolidColorBrush> muted_brush;
  ComPtr<ID2D1SolidColorBrush> hover_brush;
  ComPtr<ID2D1SolidColorBrush> folder_fill;
  ComPtr<ID2D1SolidColorBrush> folder_stroke;
  ComPtr<ID2D1SolidColorBrush> scroll_brush;

  win->rt->CreateSolidColorBrush(theme.border, &border_brush);
  win->rt->CreateSolidColorBrush(theme.divider, &divider_brush);
  win->rt->CreateSolidColorBrush(theme.text, &text_brush);
  win->rt->CreateSolidColorBrush(theme.muted, &muted_brush);
  win->rt->CreateSolidColorBrush(theme.hover, &hover_brush);
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
    const bool is_active =
        !is_empty_placeholder &&
        (i == win->hovered_index || i == win->open_child_index);

    if (is_active) {
      const D2D1_RECT_F bg_rc = D2D1::RectF(
          static_cast<float>(rc.left + ScalePx(2, win->dpi)),
          static_cast<float>(rc.top + 1),
          static_cast<float>(rc.right - ScalePx(2, win->dpi)),
          static_cast<float>(rc.bottom - 1));
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
              1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
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
            is_empty_placeholder ? muted_brush.Get() : text_brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
      }
    }

    if (node.is_folder) {
      const float ax =
          static_cast<float>(rc.right) - static_cast<float>(ScalePx(9, win->dpi));
      const float ay =
          static_cast<float>(rc.top + rc.bottom) * 0.5f;
      const float ar = static_cast<float>(ScalePx(3, win->dpi));
      DrawFolderArrow(win->rt.Get(), ax, ay, ar, muted_brush.Get());
    }
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

void HandleHoverTimer(PopupWindow* win) {
  ::KillTimer(win->hwnd, kHoverTimerId);
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
  // Build safe triangle from previous cursor to top/bottom of child's facing edge.
  const bool child_on_right = child_rc.left >= prev.x;
  const LONG edge_x = child_on_right ? child_rc.left : child_rc.right;
  const POINT top_corner{edge_x, child_rc.top};
  const POINT bot_corner{edge_x, child_rc.bottom};
  return PointInTriangle(cur_screen, prev, top_corner, bot_corner);
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
      if (win->resize_mode == ResizeMode::kNone) {
        win->hovered_index = -1;
        ::InvalidateRect(hwnd, nullptr, FALSE);
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
  if (::RegisterClassExW(&wc) || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
    mgr.class_registered = true;
  }
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
    std::shared_ptr<const std::vector<BookmarkNode>> items,
    POINT anchor_pt,
    bool align_to_parent_right,
    RECT parent_rect) {
  EnsureWindowClassRegistered();
  auto win = std::make_unique<PopupWindow>();
  win->level = level;
  win->parent_item_index = parent_item_index;
  win->items = std::move(items);
  if (!win->items || win->items->empty()) {
    BookmarkNode empty_node;
    empty_node.id = L"__empty__";
    empty_node.title = L"(空文件夹)";
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
  auto& stack = GetManager().stack;
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
  if (auto child = CreatePopupLevel(
          parent->level + 1, item_index, std::move(children_copy),
          POINT{parent_rect.right, item_top_left.y}, true, parent_rect)) {
    GetManager().stack.push_back(std::move(child));
  }
}

bool PopupMouseHook(WPARAM wParam, LPARAM lParam) {
  if (!IsBookmarkPopupOpen()) {
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
  auto& stack = GetManager().stack;
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
    int next = top->hovered_index < 0 ? 0 : (top->hovered_index + delta + count) % count;
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

  auto root_popup = CreatePopupLevel(
      0, -1, mgr.root_snapshot, anchor_screen_pt, false, RECT{});
  if (root_popup) {
    const HWND notify_hwnd = root_popup->hwnd;
    mgr.stack.push_back(std::move(root_popup));
    BookmarkModel::Instance().RequestFaviconsAsync(notify_hwnd,
                                                   kMsgFaviconReady);
  }
}

void CloseAllBookmarkPopups() {
  ClosePopupsFromLevel(0);
  auto& mgr = GetManager();
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
