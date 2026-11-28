#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "utils.h"

namespace {

std::shared_mutex g_bookmarks_font_mutex;

uint32_t ParseHexColor(std::wstring_view str, uint32_t fallback) {
  if (!str.empty() && str.front() == L'#') {
    str.remove_prefix(1);
  }
  if (str.size() != 6) {
    return fallback;
  }
  uint32_t value = 0;
  for (wchar_t c : str) {
    value <<= 4;
    if (c >= L'0' && c <= L'9') {
      value |= static_cast<uint32_t>(c - L'0');
    } else if (c >= L'a' && c <= L'f') {
      value |= static_cast<uint32_t>(c - L'a' + 10);
    } else if (c >= L'A' && c <= L'F') {
      value |= static_cast<uint32_t>(c - L'A' + 10);
    } else {
      return fallback;
    }
  }
  return value;
}

void EnsureIniExists() {
  const std::wstring& path = GetIniPath();
  if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return;
  }
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    constexpr uint8_t kBom[2] = {0xFF, 0xFE};
    DWORD written = 0;
    ::WriteFile(file, kBom, sizeof(kBom), &written, nullptr);
    ::CloseHandle(file);
  }
}

}  // namespace

Config& Config::Instance() {
  static Config instance;
  return instance;
}

Config::Config() {
  LoadConfig();
}

void Config::LoadConfig() {
  // general
  command_line_ = GetIniString(L"general", L"command_line", L"");
  launch_on_startup_ = GetIniString(L"general", L"launch_on_startup", L"");
  launch_on_exit_ = GetIniString(L"general", L"launch_on_exit", L"");
  user_data_dir_ = LoadDirPath(L"data");
  disk_cache_dir_ = LoadDirPath(L"cache");
  boss_key_ = GetIniString(L"general", L"boss_key", L"");
  translate_key_ = GetIniString(L"general", L"translate_key", L"");
  win32k_ = GetIniInt(L"general", L"win32k", 0) != 0;
  ignore_policies_ = GetIniInt(L"general", L"ignore_policies", 0) != 0;
  suppress_false_upgrade_notification_ =
      GetIniInt(L"general", L"suppress_false_upgrade_notification", 0) != 0;
  managed_updates_ = GetIniInt(L"general", L"managed_updates", 0) != 0;
  ui_scale_ = GetIniString(L"general", L"ui_scale", L"");

  // proxy
  proxy_enabled_ = GetIniInt(L"proxy", L"enabled", 0) != 0;
  proxy_server_ = GetIniString(L"proxy", L"server", L"");
  proxy_bypass_list_ =
      GetIniString(L"proxy", L"bypass_list", L"localhost;127.0.0.1;<local>");

  // tabs
  ReloadTabsConfig();
  disable_tab_names_ = StringSplit(
      GetIniString(L"tabs", L"new_tab_disable_name", L""), L',', L"\"");

  // bookmarks
  bookmarks_hotkey_ = GetIniString(L"bookmarks", L"hotkey", L"");
  ReloadBookmarksConfig();

  // keymapping
  LoadKeyMappings();
}

void Config::ReloadTabsConfig() {
  keep_last_tab_.store(GetIniInt(L"tabs", L"keep_last_tab", 1) != 0,
                       std::memory_order_relaxed);
  double_click_close_.store(GetIniInt(L"tabs", L"double_click_close", 1) != 0,
                            std::memory_order_relaxed);
  right_click_close_.store(GetIniInt(L"tabs", L"right_click_close", 1) != 0,
                           std::memory_order_relaxed);
  right_click_new_tab_clipboard_.store(
      GetIniInt(L"tabs", L"right_click_new_tab_clipboard", 1) != 0,
      std::memory_order_relaxed);
  wheel_tab_.store(GetIniInt(L"tabs", L"wheel_tab", 1) != 0,
                   std::memory_order_relaxed);
  wheel_tab_when_press_rbutton_.store(
      GetIniInt(L"tabs", L"wheel_tab_when_press_rbutton", 1) != 0,
      std::memory_order_relaxed);
  hover_tab_.store(GetIniInt(L"tabs", L"hover_tab", 1) != 0,
                   std::memory_order_relaxed);
  hover_tab_delay_.store(LoadHoverTabDelay(), std::memory_order_relaxed);
  open_url_new_tab_.store(GetIniInt(L"tabs", L"open_url_new_tab", 0),
                          std::memory_order_relaxed);
  bookmark_new_tab_.store(GetIniInt(L"tabs", L"open_bookmark_new_tab", 0),
                          std::memory_order_relaxed);
  new_tab_disable_.store(GetIniInt(L"tabs", L"new_tab_disable", 1) != 0,
                         std::memory_order_relaxed);
  super_drag_mode_.store(GetIniInt(L"tabs", L"super_drag", 2),
                         std::memory_order_relaxed);
}

void Config::ReloadBookmarksConfig() {
  bookmarks_enabled_.store(GetIniInt(L"bookmarks", L"enabled", 1) != 0,
                           std::memory_order_relaxed);
  bookmarks_button_anchor_.store(
      std::clamp(GetIniInt(L"bookmarks", L"button_anchor", 0), 0, 3),
      std::memory_order_relaxed);
  bookmarks_button_offset_x_.store(
      std::clamp(GetIniInt(L"bookmarks", L"button_offset_x", 0), -4000, 4000),
      std::memory_order_relaxed);
  bookmarks_button_offset_y_.store(
      std::clamp(GetIniInt(L"bookmarks", L"button_offset_y", 0), -2000, 2000),
      std::memory_order_relaxed);
  bookmarks_button_size_.store(
      std::clamp(GetIniInt(L"bookmarks", L"button_size", 28), 18, 48),
      std::memory_order_relaxed);
  bookmarks_column_width_.store(
      std::clamp(GetIniInt(L"bookmarks", L"column_width", 240), 150, 600),
      std::memory_order_relaxed);
  bookmarks_max_height_.store(
      std::clamp(GetIniInt(L"bookmarks", L"max_height", 720), 200, 2000),
      std::memory_order_relaxed);
  bookmarks_item_height_.store(
      std::clamp(GetIniInt(L"bookmarks", L"item_height", 28), 20, 48),
      std::memory_order_relaxed);
  bookmarks_font_size_.store(
      std::clamp(GetIniInt(L"bookmarks", L"font_size", 13), 10, 24),
      std::memory_order_relaxed);
  bookmarks_icon_size_.store(
      std::clamp(GetIniInt(L"bookmarks", L"icon_size", 16), 12, 28),
      std::memory_order_relaxed);
  bookmarks_corner_radius_.store(
      std::clamp(GetIniInt(L"bookmarks", L"corner_radius", 6), 0, 16),
      std::memory_order_relaxed);
  bookmarks_theme_.store(
      std::clamp(GetIniInt(L"bookmarks", L"theme", 0), 0, 3),
      std::memory_order_relaxed);
  bookmarks_bg_color_.store(
      ParseHexColor(GetIniString(L"bookmarks", L"bg_color", L"#F9F9F9"),
                    0xF9F9F9),
      std::memory_order_relaxed);
  bookmarks_text_color_.store(
      ParseHexColor(GetIniString(L"bookmarks", L"text_color", L"#202124"),
                    0x202124),
      std::memory_order_relaxed);
  bookmarks_hover_color_.store(
      ParseHexColor(GetIniString(L"bookmarks", L"hover_color", L"#E8F0FE"),
                    0xE8F0FE),
      std::memory_order_relaxed);
  bookmarks_show_other_.store(
      GetIniInt(L"bookmarks", L"show_other_bookmarks", 1) != 0,
      std::memory_order_relaxed);

  std::wstring font =
      GetIniString(L"bookmarks", L"font_family", L"Microsoft YaHei UI");
  if (font.empty()) {
    font = L"Microsoft YaHei UI";
  }
  {
    std::unique_lock lock(g_bookmarks_font_mutex);
    bookmarks_font_family_ = std::move(font);
  }
}

std::wstring Config::GetBookmarksFontFamily() const {
  std::shared_lock lock(g_bookmarks_font_mutex);
  return bookmarks_font_family_.empty() ? L"Microsoft YaHei UI"
                                        : bookmarks_font_family_;
}

void Config::UpdateBookmarkPopupSize(int column_width, int max_height) {
  column_width = std::clamp(column_width, 150, 600);
  max_height = std::clamp(max_height, 200, 2000);
  bookmarks_column_width_.store(column_width, std::memory_order_relaxed);
  bookmarks_max_height_.store(max_height, std::memory_order_relaxed);

  EnsureIniExists();
  const std::wstring w_str = std::to_wstring(column_width);
  const std::wstring h_str = std::to_wstring(max_height);
  ::WritePrivateProfileStringW(L"bookmarks", L"column_width", w_str.c_str(),
                               GetIniPath().c_str());
  ::WritePrivateProfileStringW(L"bookmarks", L"max_height", h_str.c_str(),
                               GetIniPath().c_str());
}

void Config::UpdateBookmarkButtonOffset(int offset_x, int offset_y) {
  offset_x = std::clamp(offset_x, -4000, 4000);
  offset_y = std::clamp(offset_y, -2000, 2000);
  bookmarks_button_offset_x_.store(offset_x, std::memory_order_relaxed);
  bookmarks_button_offset_y_.store(offset_y, std::memory_order_relaxed);

  EnsureIniExists();
  const std::wstring x_str = std::to_wstring(offset_x);
  const std::wstring y_str = std::to_wstring(offset_y);
  ::WritePrivateProfileStringW(L"bookmarks", L"button_offset_x", x_str.c_str(),
                               GetIniPath().c_str());
  ::WritePrivateProfileStringW(L"bookmarks", L"button_offset_y", y_str.c_str(),
                               GetIniPath().c_str());
}

void Config::LoadKeyMappings() {
  std::vector<wchar_t> buffer(4096);
  const DWORD chars_read = ::GetPrivateProfileSectionW(
      L"keymapping", buffer.data(), static_cast<DWORD>(buffer.size()),
      GetIniPath().c_str());

  if (chars_read == 0) {
    return;
  }

  const wchar_t* current = buffer.data();
  while (*current != L'\0') {
    const std::wstring_view line(current);
    current += line.length() + 1;

    const auto eq_pos = line.find(L'=');
    if (eq_pos == std::wstring_view::npos || eq_pos == 0) {
      continue;
    }

    std::wstring_view key = line.substr(0, eq_pos);
    std::wstring_view value = line.substr(eq_pos + 1);

    while (!key.empty() && (key.back() == L' ' || key.back() == L'\t')) {
      key.remove_suffix(1);
    }
    while (!value.empty() &&
           (value.front() == L' ' || value.front() == L'\t')) {
      value.remove_prefix(1);
    }

    if (!key.empty() && !value.empty()) {
      key_mappings_.emplace_back(std::wstring(key), std::wstring(value));
    }
  }
}

std::optional<std::wstring> Config::LoadDirPath(const std::wstring& dir_type) {
  std::wstring path = GetAbsolutePath(GetAppDir() + L"\\..\\" + dir_type);
  std::wstring dir_key = dir_type + L"_dir";
  std::wstring dir_buffer =
      GetIniString(L"general", dir_key.c_str(), path.c_str());

  if (dir_buffer == L"none") {
    return std::nullopt;
  }

  if (dir_buffer.empty()) {
    dir_buffer = path;
  }

  std::wstring expanded_path = ExpandEnvironmentPath(dir_buffer);
  ReplaceStringInPlace(expanded_path, L"%app%", GetAppDir());
  return GetAbsolutePath(expanded_path);
}

int Config::LoadHoverTabDelay() {
  constexpr int kDefaultDelayMs = 400;
  constexpr int kMaxDelayMs = 5000;
  const int delay = GetIniInt(L"tabs", L"hover_tab_delay", kDefaultDelayMs);
  if (delay < 0 || delay > kMaxDelayMs) {
    return kDefaultDelayMs;
  }
  return delay;
}

const Config& config = Config::Instance();
