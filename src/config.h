#ifndef CHROME_PLUS_SRC_CONFIG_H_
#define CHROME_PLUS_SRC_CONFIG_H_

#include <atomic>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Config {
 public:
  static Config& Instance();

  void ReloadTabsConfig();
  void ReloadBookmarksConfig();
  void UpdateBookmarkPopupSize(int column_width, int max_height);
  void UpdateBookmarkButtonOffset(int offset_x, int offset_y);

  // general
  const std::wstring& GetCommandLine() const { return command_line_; }
  const std::wstring& GetLaunchOnStartup() const { return launch_on_startup_; }
  const std::wstring& GetLaunchOnExit() const { return launch_on_exit_; }
  const std::optional<std::wstring>& GetUserDataDir() const {
    return user_data_dir_;
  }
  const std::optional<std::wstring>& GetDiskCacheDir() const {
    return disk_cache_dir_;
  }
  const std::wstring& GetBossKey() const { return boss_key_; }
  const std::wstring& GetTranslateKey() const { return translate_key_; }
  bool IsWin32K() const { return win32k_; }
  bool IsIgnorePolicies() const { return ignore_policies_; }
  bool IsSuppressFalseUpgradeNotification() const {
    return suppress_false_upgrade_notification_;
  }
  bool IsManagedUpdates() const { return managed_updates_; }
  const std::wstring& GetUiScale() const { return ui_scale_; }

  // proxy
  bool IsProxyEnabled() const { return proxy_enabled_; }
  const std::wstring& GetProxyServer() const { return proxy_server_; }
  const std::wstring& GetProxyBypassList() const { return proxy_bypass_list_; }

  // tabs (atomic for lock-free hot-reload from the settings UI)
  bool IsKeepLastTab() const {
    return keep_last_tab_.load(std::memory_order_relaxed);
  }
  bool IsDoubleClickClose() const {
    return double_click_close_.load(std::memory_order_relaxed);
  }
  bool IsRightClickClose() const {
    return right_click_close_.load(std::memory_order_relaxed);
  }
  bool IsRightClickNewTabClipboard() const {
    return right_click_new_tab_clipboard_.load(std::memory_order_relaxed);
  }
  bool IsWheelTab() const {
    return wheel_tab_.load(std::memory_order_relaxed);
  }
  bool IsWheelTabWhenPressRightButton() const {
    return wheel_tab_when_press_rbutton_.load(std::memory_order_relaxed);
  }
  bool IsHoverTab() const {
    return hover_tab_.load(std::memory_order_relaxed);
  }
  int GetHoverTabDelay() const {
    return hover_tab_delay_.load(std::memory_order_relaxed);
  }
  int GetOpenUrlNewTabMode() const {
    return open_url_new_tab_.load(std::memory_order_relaxed);
  }
  int GetBookmarkNewTabMode() const {
    return bookmark_new_tab_.load(std::memory_order_relaxed);
  }
  bool IsNewTabDisable() const {
    return new_tab_disable_.load(std::memory_order_relaxed);
  }
  int GetSuperDragMode() const {
    return super_drag_mode_.load(std::memory_order_relaxed);
  }
  const std::vector<std::wstring>& GetDisableTabNames() const {
    return disable_tab_names_;
  }

  // bookmarks popup (atomic/synchronized for hot-reload)
  bool IsBookmarksPopupEnabled() const {
    return bookmarks_enabled_.load(std::memory_order_relaxed);
  }
  const std::wstring& GetBookmarksHotkey() const { return bookmarks_hotkey_; }
  int GetBookmarksButtonAnchor() const {
    return bookmarks_button_anchor_.load(std::memory_order_relaxed);
  }
  int GetBookmarksButtonOffsetX() const {
    return bookmarks_button_offset_x_.load(std::memory_order_relaxed);
  }
  int GetBookmarksButtonOffsetY() const {
    return bookmarks_button_offset_y_.load(std::memory_order_relaxed);
  }
  int GetBookmarksButtonSize() const {
    return bookmarks_button_size_.load(std::memory_order_relaxed);
  }
  int GetBookmarksColumnWidth() const {
    return bookmarks_column_width_.load(std::memory_order_relaxed);
  }
  int GetBookmarksMaxHeight() const {
    return bookmarks_max_height_.load(std::memory_order_relaxed);
  }
  int GetBookmarksItemHeight() const {
    return bookmarks_item_height_.load(std::memory_order_relaxed);
  }
  int GetBookmarksFontSize() const {
    return bookmarks_font_size_.load(std::memory_order_relaxed);
  }
  std::wstring GetBookmarksFontFamily() const;
  int GetBookmarksIconSize() const {
    return bookmarks_icon_size_.load(std::memory_order_relaxed);
  }
  int GetBookmarksCornerRadius() const {
    return bookmarks_corner_radius_.load(std::memory_order_relaxed);
  }
  int GetBookmarksTheme() const {
    return bookmarks_theme_.load(std::memory_order_relaxed);
  }
  uint32_t GetBookmarksBgColor() const {
    return bookmarks_bg_color_.load(std::memory_order_relaxed);
  }
  uint32_t GetBookmarksTextColor() const {
    return bookmarks_text_color_.load(std::memory_order_relaxed);
  }
  uint32_t GetBookmarksHoverColor() const {
    return bookmarks_hover_color_.load(std::memory_order_relaxed);
  }
  bool IsBookmarksShowOther() const {
    return bookmarks_show_other_.load(std::memory_order_relaxed);
  }

  // keymapping
  using KeyMappingPair = std::pair<std::wstring, std::wstring>;
  const auto& GetKeyMappings() const { return key_mappings_; }

 private:
  Config();
  ~Config() = default;
  Config(const Config&) = delete;
  Config& operator=(const Config&) = delete;

  void LoadConfig();
  void LoadKeyMappings();

  std::optional<std::wstring> LoadDirPath(const std::wstring& dir_type);
  int LoadHoverTabDelay();

 private:
  // general
  std::wstring command_line_;
  std::wstring launch_on_startup_;
  std::wstring launch_on_exit_;
  std::optional<std::wstring> user_data_dir_;
  std::optional<std::wstring> disk_cache_dir_;
  std::wstring boss_key_;
  std::wstring translate_key_;
  bool win32k_;
  bool ignore_policies_;
  bool suppress_false_upgrade_notification_;
  bool managed_updates_;
  std::wstring ui_scale_;

  // proxy
  bool proxy_enabled_;
  std::wstring proxy_server_;
  std::wstring proxy_bypass_list_;

  // tabs
  std::atomic<bool> keep_last_tab_;
  std::atomic<bool> double_click_close_;
  std::atomic<bool> right_click_close_;
  std::atomic<bool> right_click_new_tab_clipboard_;
  std::atomic<bool> wheel_tab_;
  std::atomic<bool> wheel_tab_when_press_rbutton_;
  std::atomic<bool> hover_tab_;
  std::atomic<int> hover_tab_delay_;
  std::atomic<int> open_url_new_tab_;
  std::atomic<int> bookmark_new_tab_;
  std::atomic<bool> new_tab_disable_;
  std::atomic<int> super_drag_mode_;
  std::vector<std::wstring> disable_tab_names_;

  // bookmarks popup
  std::atomic<bool> bookmarks_enabled_{true};
  std::wstring bookmarks_hotkey_;
  std::atomic<int> bookmarks_button_anchor_{0};
  std::atomic<int> bookmarks_button_offset_x_{0};
  std::atomic<int> bookmarks_button_offset_y_{0};
  std::atomic<int> bookmarks_button_size_{28};
  std::atomic<int> bookmarks_column_width_{240};
  std::atomic<int> bookmarks_max_height_{720};
  std::atomic<int> bookmarks_item_height_{28};
  std::atomic<int> bookmarks_font_size_{13};
  std::wstring bookmarks_font_family_;
  std::atomic<int> bookmarks_icon_size_{16};
  std::atomic<int> bookmarks_corner_radius_{6};
  std::atomic<int> bookmarks_theme_{0};
  std::atomic<uint32_t> bookmarks_bg_color_{0xF9F9F9};
  std::atomic<uint32_t> bookmarks_text_color_{0x202124};
  std::atomic<uint32_t> bookmarks_hover_color_{0xE8F0FE};
  std::atomic<bool> bookmarks_show_other_{true};

  // keymapping
  std::vector<KeyMappingPair> key_mappings_;
};

extern const Config& config;

#endif  // CHROME_PLUS_SRC_CONFIG_H_
