#ifndef CHROME_PLUS_SRC_UIA_H_
#define CHROME_PLUS_SRC_UIA_H_

#include <uiautomation.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

struct TabHitResult {
  Microsoft::WRL::ComPtr<IUIAutomationElement> tab;
  int tab_count = 0;
  bool on_close_button = false;
};

[[nodiscard]] std::optional<TabHitResult>
FindTabHitResult(POINT pt, bool need_count, bool need_close_button);
// Deliberately not [[nodiscard]]: the caller has no fallback action;
// failures are logged at the failure site.
bool SelectTab(const TabHitResult& hit_result);
bool SelectTab(const Microsoft::WRL::ComPtr<IUIAutomationElement>& tab);
[[nodiscard]] Microsoft::WRL::ComPtr<IUIAutomationElement> FindSelectedTab(
    HWND hwnd);
[[nodiscard]] std::optional<int> FindTabCount(HWND hwnd);
[[nodiscard]] bool IsOnTabBar(POINT pt);
[[nodiscard]] bool IsOnNewTabButton(POINT pt);
[[nodiscard]] bool IsOnBookmark(POINT pt);
[[nodiscard]] bool IsOmniboxFocused();
[[nodiscard]] bool SetOmniboxText(const std::wstring& text);
[[nodiscard]] bool IsOnNewTab(HWND hwnd,
                              const std::vector<std::wstring>& extra_tab_names);

struct BookmarkAnchorRect {
  RECT rect{};
  bool found = false;
  int gap_left = 0;
  bool has_gap_left = false;
};
// anchor_mode: 0=left of address bar, 1=leftmost of toolbar, 2=top-left of tab strip, 3=right of address bar.
[[nodiscard]] std::optional<BookmarkAnchorRect> FindBookmarkButtonAnchor(
    HWND hwnd,
    int anchor_mode);
[[nodiscard]] bool IsChromeWindowFullscreen(HWND hwnd);

#endif  // CHROME_PLUS_SRC_UIA_H_
