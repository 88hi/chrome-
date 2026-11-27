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
  const std::vector<std::wstring>& GetDisableTabNames() const {
    return disable_tab_names_;
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
  std::vector<std::wstring> disable_tab_names_;

  // keymapping
  std::vector<KeyMappingPair> key_mappings_;
};

extern const Config& config;

#endif  // CHROME_PLUS_SRC_CONFIG_H_
