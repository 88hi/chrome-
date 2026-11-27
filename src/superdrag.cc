#include "superdrag.h"

#include <windows.h>

#include <ole2.h>
#include <shlobj.h>

#include <cstdlib>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>

#include "detours.h"

#include "config.h"
#include "tabbookmark.h"
#include "uia.h"
#include "utils.h"

namespace {

auto RawDoDragDrop = ::DoDragDrop;

std::wstring SanitizeDragText(std::wstring_view raw, bool is_url) {
  constexpr size_t kMaxLength = 2048;
  std::wstring out;
  out.reserve(raw.size() < kMaxLength ? raw.size() : kMaxLength);

  bool prev_space = true;
  for (wchar_t ch : raw) {
    if (ch == L'\0') {
      break;
    }
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
      if (is_url) {
        continue;
      }
      ch = L' ';
    }
    if (std::iswspace(ch)) {
      if (is_url) {
        continue;
      }
      if (!prev_space) {
        out.push_back(L' ');
        prev_space = true;
      }
    } else {
      out.push_back(ch);
      prev_space = false;
    }
    if (out.size() >= kMaxLength) {
      break;
    }
  }

  while (!out.empty() && std::iswspace(out.back())) {
    out.pop_back();
  }
  return out;
}

std::optional<std::wstring> ReadHGlobalText(IDataObject* data_obj,
                                            CLIPFORMAT format,
                                            bool is_url) {
  FORMATETC fmt{};
  fmt.cfFormat = format;
  fmt.ptd = nullptr;
  fmt.dwAspect = DVASPECT_CONTENT;
  fmt.lindex = -1;
  fmt.tymed = TYMED_HGLOBAL;

  STGMEDIUM stg{};
  if (FAILED(data_obj->GetData(&fmt, &stg))) {
    return std::nullopt;
  }

  std::optional<std::wstring> result;
  if (stg.tymed == TYMED_HGLOBAL && stg.hGlobal) {
    const SIZE_T bytes = ::GlobalSize(stg.hGlobal);
    if (const auto* text =
            static_cast<const wchar_t*>(::GlobalLock(stg.hGlobal))) {
      const size_t max_chars = bytes / sizeof(wchar_t);
      size_t len = 0;
      while (len < max_chars && text[len] != L'\0') {
        ++len;
      }
      std::wstring cleaned =
          SanitizeDragText(std::wstring_view(text, len), is_url);
      if (!cleaned.empty()) {
        result = std::move(cleaned);
      }
      ::GlobalUnlock(stg.hGlobal);
    }
  }
  ::ReleaseStgMedium(&stg);
  return result;
}

bool HasFileDrop(IDataObject* data_obj) {
  FORMATETC fmt{};
  fmt.cfFormat = CF_HDROP;
  fmt.ptd = nullptr;
  fmt.dwAspect = DVASPECT_CONTENT;
  fmt.lindex = -1;
  fmt.tymed = TYMED_HGLOBAL;
  return data_obj->QueryGetData(&fmt) == S_OK;
}

std::optional<std::wstring> ExtractDragContent(IDataObject* data_obj) {
  if (!data_obj || HasFileDrop(data_obj)) {
    return std::nullopt;
  }

  static const CLIPFORMAT cf_ineturl_w = static_cast<CLIPFORMAT>(
      ::RegisterClipboardFormatW(L"UniformResourceLocatorW"));
  if (cf_ineturl_w != 0) {
    if (auto url = ReadHGlobalText(data_obj, cf_ineturl_w, /*is_url=*/true)) {
      if (url->starts_with(L"javascript:") || url->starts_with(L"JAVASCRIPT:")) {
        return std::nullopt;
      }
      return url;
    }
  }

  return ReadHGlobalText(data_obj, CF_UNICODETEXT, /*is_url=*/false);
}

HRESULT WINAPI MyDoDragDrop(LPDATAOBJECT pDataObj,
                            LPDROPSOURCE pDropSource,
                            DWORD dwOKEffects,
                            LPDWORD pdwEffect) {
  POINT start_pt{};
  const bool has_start_pt = ::GetCursorPos(&start_pt) != FALSE;
  const HWND start_root =
      has_start_pt ? GetChromeRootWindowAt(start_pt) : nullptr;

  const HRESULT hr = RawDoDragDrop(pDataObj, pDropSource, dwOKEffects, pdwEffect);

  const int mode = config.GetSuperDragMode();
  if (mode == 0 || !pDataObj || !start_root) {
    return hr;
  }

  // If a drop target (input box, tab bar, bookmark bar, external app) accepted
  // the drop, leave native drag-and-drop untouched.
  const bool drop_accepted =
      (hr == DRAGDROP_S_DROP) && pdwEffect && (*pdwEffect != DROPEFFECT_NONE);
  if (drop_accepted) {
    return hr;
  }

  // Ignore drags explicitly canceled via Escape or right mouse button.
  if ((::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 ||
      (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
    return hr;
  }

  POINT drop_pt{};
  if (!::GetCursorPos(&drop_pt)) {
    return hr;
  }

  // Require the drop to land inside the same Chrome window and off the tab bar.
  if (GetChromeRootWindowAt(drop_pt) != start_root || IsOnTabBar(drop_pt)) {
    return hr;
  }

  constexpr int kMinDragDistancePx = 12;
  if (std::abs(drop_pt.x - start_pt.x) < kMinDragDistancePx &&
      std::abs(drop_pt.y - start_pt.y) < kMinDragDistancePx) {
    return hr;
  }

  if (auto content = ExtractDragContent(pDataObj)) {
    OpenInNewTab(start_root, std::move(*content), /*background=*/mode == 2);
  }

  return hr;
}

}  // namespace

void SuperDrag() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&RawDoDragDrop, MyDoDragDrop);
  const auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"SuperDrag: DetourAttach(DoDragDrop) failed: {}", status);
  }
}
