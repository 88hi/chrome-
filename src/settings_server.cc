#include "settings_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "config.h"
#include "utils.h"
#include "version.h"

namespace {

constexpr uint16_t kSettingsPort = 19876;
constexpr std::string_view kExpectedHost = "127.0.0.1:19876";

std::string g_session_token;

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int needed = ::WideCharToMultiByte(
      CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
      nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), needed);
  return out;
}

std::string GenerateSessionToken() {
  std::array<uint8_t, 8> bytes{};
  if (::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    const uint64_t fallback =
        (::GetTickCount64() << 16) ^ static_cast<uint64_t>(::GetCurrentProcessId());
    memcpy(bytes.data(), &fallback, sizeof(fallback));
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string token;
  token.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    token.push_back(kHex[b >> 4]);
    token.push_back(kHex[b & 0x0F]);
  }
  return token;
}

std::string EscapeJsonString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0xF]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

void AppendJsonField(std::string& json,
                     std::string_view key,
                     std::string_view val,
                     bool& first) {
  if (!first) {
    json.push_back(',');
  }
  first = false;
  json.push_back('"');
  json.append(key);
  json.append("\":\"");
  json.append(EscapeJsonString(val));
  json.push_back('"');
}

void AppendJsonField(std::string& json,
                     std::string_view key,
                     int val,
                     bool& first) {
  if (!first) {
    json.push_back(',');
  }
  first = false;
  json.push_back('"');
  json.append(key);
  json.append("\":");
  json.append(std::to_string(val));
}

void AppendJsonField(std::string& json,
                     std::string_view key,
                     bool val,
                     bool& first) {
  if (!first) {
    json.push_back(',');
  }
  first = false;
  json.push_back('"');
  json.append(key);
  json.append("\":");
  json.append(val ? "true" : "false");
}

void EnsureIniFileExistsUtf16Le() {
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

void WriteIniValue(const wchar_t* section,
                   const wchar_t* key,
                   const std::wstring& value) {
  ::WritePrivateProfileStringW(section, key, value.c_str(),
                               GetIniPath().c_str());
}

std::string BuildConfigJson() {
  std::string json = "{";
  json += "\"version\":\"" RELEASE_VER_STR "\",";
  json += "\"ini_path\":\"" + EscapeJsonString(WideToUtf8(GetIniPath())) + "\",";

  // [tabs]
  json += "\"tabs\":{";
  bool first = true;
  AppendJsonField(json, "double_click_close",
                  GetIniInt(L"tabs", L"double_click_close", 1) != 0, first);
  AppendJsonField(json, "right_click_close",
                  GetIniInt(L"tabs", L"right_click_close", 1) != 0, first);
  AppendJsonField(json, "right_click_new_tab_clipboard",
                  GetIniInt(L"tabs", L"right_click_new_tab_clipboard", 1) != 0,
                  first);
  AppendJsonField(json, "keep_last_tab",
                  GetIniInt(L"tabs", L"keep_last_tab", 1) != 0, first);
  AppendJsonField(json, "wheel_tab",
                  GetIniInt(L"tabs", L"wheel_tab", 1) != 0, first);
  AppendJsonField(json, "wheel_tab_when_press_rbutton",
                  GetIniInt(L"tabs", L"wheel_tab_when_press_rbutton", 1) != 0,
                  first);
  AppendJsonField(json, "hover_tab",
                  GetIniInt(L"tabs", L"hover_tab", 1) != 0, first);
  AppendJsonField(json, "hover_tab_delay",
                  GetIniInt(L"tabs", L"hover_tab_delay", 400), first);
  AppendJsonField(json, "open_url_new_tab",
                  GetIniInt(L"tabs", L"open_url_new_tab", 0), first);
  AppendJsonField(json, "open_bookmark_new_tab",
                  GetIniInt(L"tabs", L"open_bookmark_new_tab", 0), first);
  AppendJsonField(json, "new_tab_disable",
                  GetIniInt(L"tabs", L"new_tab_disable", 1) != 0, first);
  AppendJsonField(
      json, "new_tab_disable_name",
      WideToUtf8(GetIniString(L"tabs", L"new_tab_disable_name", L"")), first);
  json += "},";

  // [proxy]
  json += "\"proxy\":{";
  first = true;
  AppendJsonField(json, "enabled", GetIniInt(L"proxy", L"enabled", 0) != 0,
                  first);
  AppendJsonField(json, "server",
                  WideToUtf8(GetIniString(L"proxy", L"server", L"127.0.0.1:7890")),
                  first);
  AppendJsonField(
      json, "bypass_list",
      WideToUtf8(GetIniString(L"proxy", L"bypass_list",
                              L"localhost;127.0.0.1;<local>")),
      first);
  json += "},";

  // [general]
  json += "\"general\":{";
  first = true;
  AppendJsonField(json, "boss_key",
                  WideToUtf8(GetIniString(L"general", L"boss_key", L"")),
                  first);
  AppendJsonField(json, "translate_key",
                  WideToUtf8(GetIniString(L"general", L"translate_key", L"")),
                  first);
  AppendJsonField(json, "command_line",
                  WideToUtf8(GetIniString(L"general", L"command_line", L"")),
                  first);
  AppendJsonField(json, "data_dir",
                  WideToUtf8(GetIniString(L"general", L"data_dir", L"")),
                  first);
  AppendJsonField(json, "cache_dir",
                  WideToUtf8(GetIniString(L"general", L"cache_dir", L"")),
                  first);
  AppendJsonField(
      json, "launch_on_startup",
      WideToUtf8(GetIniString(L"general", L"launch_on_startup", L"")), first);
  AppendJsonField(json, "launch_on_exit",
                  WideToUtf8(GetIniString(L"general", L"launch_on_exit", L"")),
                  first);
  AppendJsonField(json, "ignore_policies",
                  GetIniInt(L"general", L"ignore_policies", 0) != 0, first);
  AppendJsonField(
      json, "suppress_false_upgrade_notification",
      GetIniInt(L"general", L"suppress_false_upgrade_notification", 0) != 0,
      first);
  AppendJsonField(json, "win32k", GetIniInt(L"general", L"win32k", 0) != 0,
                  first);
  AppendJsonField(json, "ui_scale",
                  WideToUtf8(GetIniString(L"general", L"ui_scale", L"")),
                  first);
  json += "}}";
  return json;
}

// Minimal, exception-free parser for flat JSON object: { "section.key": value }
// where value is a JSON string, integer, or boolean.
struct FlatJsonParser {
  std::string_view src;
  size_t pos = 0;

  void SkipWs() {
    while (pos < src.size() &&
           (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' ||
            src[pos] == '\n')) {
      ++pos;
    }
  }

  bool Consume(char ch) {
    SkipWs();
    if (pos < src.size() && src[pos] == ch) {
      ++pos;
      return true;
    }
    return false;
  }

  static void AppendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseHex4(uint32_t& value) {
    if (pos + 4 > src.size()) {
      return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = src[pos++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  bool ParseString(std::string& out) {
    SkipWs();
    if (pos >= src.size() || src[pos] != '"') {
      return false;
    }
    ++pos;
    out.clear();
    while (pos < src.size()) {
      char c = src[pos++];
      if (c == '"') {
        return true;
      }
      if (c == '\\') {
        if (pos >= src.size()) {
          return false;
        }
        char esc = src[pos++];
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            out.push_back(esc);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            uint32_t cp = 0;
            if (!ParseHex4(cp)) {
              return false;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && pos + 2 <= src.size() &&
                src[pos] == '\\' && src[pos + 1] == 'u') {
              pos += 2;
              uint32_t low = 0;
              if (!ParseHex4(low)) {
                return false;
              }
              if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
              }
            }
            AppendUtf8Codepoint(out, cp);
            break;
          }
          default:
            return false;
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }

  bool ParsePrimitiveAsString(std::string& out) {
    SkipWs();
    if (pos >= src.size()) {
      return false;
    }
    if (src[pos] == '"') {
      return ParseString(out);
    }
    if (src.substr(pos).starts_with("true")) {
      pos += 4;
      out = "1";
      return true;
    }
    if (src.substr(pos).starts_with("false")) {
      pos += 5;
      out = "0";
      return true;
    }
    const size_t start = pos;
    if (src[pos] == '-') {
      ++pos;
    }
    while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) {
      ++pos;
    }
    if (pos == start) {
      return false;
    }
    out = std::string(src.substr(start, pos - start));
    return true;
  }
};

struct AllowedIniKey {
  std::string_view json_key;
  const wchar_t* section;
  const wchar_t* ini_key;
  bool is_int_range = false;
  int min_val = 0;
  int max_val = 0;
};

constexpr AllowedIniKey kAllowedKeys[] = {
    // tabs
    {"tabs.double_click_close", L"tabs", L"double_click_close", true, 0, 1},
    {"tabs.right_click_close", L"tabs", L"right_click_close", true, 0, 1},
    {"tabs.right_click_new_tab_clipboard", L"tabs",
     L"right_click_new_tab_clipboard", true, 0, 1},
    {"tabs.keep_last_tab", L"tabs", L"keep_last_tab", true, 0, 1},
    {"tabs.wheel_tab", L"tabs", L"wheel_tab", true, 0, 1},
    {"tabs.wheel_tab_when_press_rbutton", L"tabs",
     L"wheel_tab_when_press_rbutton", true, 0, 1},
    {"tabs.hover_tab", L"tabs", L"hover_tab", true, 0, 1},
    {"tabs.hover_tab_delay", L"tabs", L"hover_tab_delay", true, 0, 5000},
    {"tabs.open_url_new_tab", L"tabs", L"open_url_new_tab", true, 0, 2},
    {"tabs.open_bookmark_new_tab", L"tabs", L"open_bookmark_new_tab", true, 0,
     2},
    {"tabs.new_tab_disable", L"tabs", L"new_tab_disable", true, 0, 1},
    {"tabs.new_tab_disable_name", L"tabs", L"new_tab_disable_name"},
    // proxy
    {"proxy.enabled", L"proxy", L"enabled", true, 0, 1},
    {"proxy.server", L"proxy", L"server"},
    {"proxy.bypass_list", L"proxy", L"bypass_list"},
    // general
    {"general.boss_key", L"general", L"boss_key"},
    {"general.translate_key", L"general", L"translate_key"},
    {"general.command_line", L"general", L"command_line"},
    {"general.data_dir", L"general", L"data_dir"},
    {"general.cache_dir", L"general", L"cache_dir"},
    {"general.launch_on_startup", L"general", L"launch_on_startup"},
    {"general.launch_on_exit", L"general", L"launch_on_exit"},
    {"general.ignore_policies", L"general", L"ignore_policies", true, 0, 1},
    {"general.suppress_false_upgrade_notification", L"general",
     L"suppress_false_upgrade_notification", true, 0, 1},
    {"general.win32k", L"general", L"win32k", true, 0, 1},
    {"general.ui_scale", L"general", L"ui_scale"},
};

bool ApplyFlatConfigJson(std::string_view body) {
  FlatJsonParser parser{body, 0};
  if (!parser.Consume('{')) {
    return false;
  }

  EnsureIniFileExistsUtf16Le();

  bool any_written = false;
  while (true) {
    parser.SkipWs();
    if (parser.pos < body.size() && body[parser.pos] == '}') {
      ++parser.pos;
      break;
    }
    std::string key;
    std::string value;
    if (!parser.ParseString(key) || !parser.Consume(':') ||
        !parser.ParsePrimitiveAsString(value)) {
      return false;
    }

    for (const auto& allowed : kAllowedKeys) {
      if (allowed.json_key == key) {
        if (allowed.is_int_range) {
          int num = 0;
          const auto [ptr, ec] = std::from_chars(
              value.data(), value.data() + value.size(), num);
          if (ec == std::errc{} && ptr == value.data() + value.size()) {
            num = std::clamp(num, allowed.min_val, allowed.max_val);
            WriteIniValue(allowed.section, allowed.ini_key,
                          std::to_wstring(num));
            any_written = true;
          }
        } else {
          // Strip CR/LF to keep INI lines well-formed.
          std::erase_if(value, [](char c) { return c == '\r' || c == '\n'; });
          WriteIniValue(allowed.section, allowed.ini_key, Utf8ToWide(value));
          any_written = true;
        }
        break;
      }
    }

    if (parser.Consume(',')) {
      continue;
    }
    if (parser.Consume('}')) {
      break;
    }
    return false;
  }

  if (any_written) {
    Config::Instance().ReloadTabsConfig();
  }
  return any_written;
}

const std::string& GetSettingsHtml() {
  static const std::string kHtml =
      std::string(R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Chrome++ 设置</title>
<style>
:root{color-scheme:light dark;--bg:#f8f9fa;--card:#fff;--text:#202124;--muted:#5f6368;--border:#dadce0;--accent:#1a73e8;--accent-hover:#1557b0;--input-bg:#fff;--chip:#e8f0fe;--chip-text:#1967d2;--ok:#188038;--warn-bg:#fef7e0;--warn-text:#b06000}
@media(prefers-color-scheme:dark){:root{--bg:#202124;--card:#292a2d;--text:#e8eaed;--muted:#9aa0a6;--border:#3c4043;--accent:#8ab4f8;--accent-hover:#aecbfa;--input-bg:#202124;--chip:#394457;--chip-text:#8ab4f8;--ok:#81c995;--warn-bg:#3c321b;--warn-text:#fdd663}}
*{box-sizing:border-box}
body{margin:0;font:14px/1.5 "Segoe UI","Microsoft YaHei",system-ui,sans-serif;background:var(--bg);color:var(--text)}
header{position:sticky;top:0;z-index:10;background:var(--card);border-bottom:1px solid var(--border);padding:14px 24px;display:flex;align-items:center;justify-content:space-between;gap:12px}
header h1{margin:0;font-size:18px;font-weight:600;display:flex;align-items:center;gap:10px}
.badge{font-size:12px;font-weight:500;padding:2px 8px;border-radius:999px;background:var(--chip);color:var(--chip-text)}
.actions{display:flex;align-items:center;gap:10px}
button{border:none;border-radius:8px;padding:8px 18px;font-size:14px;font-weight:500;cursor:pointer;background:var(--accent);color:#fff}
@media(prefers-color-scheme:dark){button{color:#202124}}
button:hover{background:var(--accent-hover)}
button.secondary{background:transparent;color:var(--accent);border:1px solid var(--border)}
main{max-width:780px;margin:20px auto 48px;padding:0 16px;display:flex;flex-direction:column;gap:16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;overflow:hidden}
.card-title{padding:14px 20px;font-size:15px;font-weight:600;border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center}
.card-title span.sub{font-size:12px;font-weight:400;color:var(--muted)}
.row{padding:12px 20px;border-bottom:1px solid var(--border);display:flex;align-items:center;justify-content:space-between;gap:16px}
.row:last-child{border-bottom:none}
.row.col{flex-direction:column;align-items:stretch;gap:6px}
.label{display:flex;flex-direction:column;gap:2px}
.label .t{font-weight:500}
.label .d{font-size:12px;color:var(--muted)}
input[type=text],input[type=number],select{background:var(--input-bg);color:var(--text);border:1px solid var(--border);border-radius:6px;padding:7px 10px;font-size:13px;min-width:220px}
.row.col input[type=text]{width:100%}
input[type=checkbox]{width:18px;height:18px;accent-color:var(--accent);cursor:pointer;flex:none}
.presets{display:flex;flex-wrap:wrap;gap:6px;margin-top:4px}
.preset{padding:3px 9px;font-size:12px;border-radius:6px;background:var(--chip);color:var(--chip-text);border:none;cursor:pointer}
.notice{background:var(--warn-bg);color:var(--warn-text);padding:10px 14px;border-radius:8px;font-size:13px}
.path{font-size:12px;color:var(--muted);word-break:break-all}
#toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);background:#202124;color:#fff;padding:10px 18px;border-radius:8px;font-size:13px;opacity:0;pointer-events:none;transition:opacity .2s}
#toast.show{opacity:1}
</style>
</head>
<body>
<header>
  <h1>Chrome++ 设置 <span class="badge" id="ver">Next</span></h1>
  <div class="actions">
    <button class="secondary" id="reloadBtn" type="button">重新读取</button>
    <button id="saveBtn" type="button">保存设置</button>
  </div>
</header>
<main>
  <div class="path">配置文件：<code id="iniPath">chrome++.ini</code></div>

  <section class="card">
    <div class="card-title">本地代理网络 [proxy] <span class="sub">修改后需重启 Chrome 生效</span></div>
    <div class="row">
      <div class="label"><span class="t">启用本地代理</span><span class="d">启动 Chrome 时自动注入 --proxy-server 参数，无需修改系统代理</span></div>
      <input type="checkbox" id="proxy.enabled">
    </div>
    <div class="row col">
      <div class="label"><span class="t">代理服务器地址 (server)</span><span class="d">支持 HTTP/HTTPS/SOCKS5，例如 127.0.0.1:7890 或 socks5://127.0.0.1:10808</span></div>
      <input type="text" id="proxy.server" placeholder="127.0.0.1:7890">
      <div class="presets">
        <button type="button" class="preset" data-proxy="http://127.0.0.1:7890">Clash HTTP (7890)</button>
        <button type="button" class="preset" data-proxy="socks5://127.0.0.1:7890">Clash SOCKS5 (7890)</button>
        <button type="button" class="preset" data-proxy="http://127.0.0.1:10809">v2rayN HTTP (10809)</button>
        <button type="button" class="preset" data-proxy="socks5://127.0.0.1:10808">v2rayN SOCKS5 (10808)</button>
      </div>
    </div>
    <div class="row col">
      <div class="label"><span class="t">绕过代理列表 (bypass_list)</span><span class="d">以分号分隔，默认跳过本机地址以确保设置页正常访问</span></div>
      <input type="text" id="proxy.bypass_list" placeholder="localhost;127.0.0.1;<local>">
    </div>
  </section>

  <section class="card">
    <div class="card-title">标签页与书签 [tabs] <span class="sub">保存后立即热生效</span></div>
    <div class="row"><div class="label"><span class="t">双击关闭标签页</span><span class="d">double_click_close</span></div><input type="checkbox" id="tabs.double_click_close"></div>
    <div class="row"><div class="label"><span class="t">右键关闭标签页</span><span class="d">right_click_close（按住 Shift 保留原菜单）</span></div><input type="checkbox" id="tabs.right_click_close"></div>
    <div class="row"><div class="label"><span class="t">右键 + 读取剪贴板打开站点或搜索</span><span class="d">right_click_new_tab_clipboard（按住 Shift 保留原菜单）</span></div><input type="checkbox" id="tabs.right_click_new_tab_clipboard"></div>
    <div class="row"><div class="label"><span class="t">保留最后一个标签页</span><span class="d">keep_last_tab（防止关闭最后标签时退出窗口）</span></div><input type="checkbox" id="tabs.keep_last_tab"></div>
    <div class="row"><div class="label"><span class="t">标签栏滚轮切换标签</span><span class="d">wheel_tab</span></div><input type="checkbox" id="tabs.wheel_tab"></div>
    <div class="row"><div class="label"><span class="t">按住右键滚轮切换标签</span><span class="d">wheel_tab_when_press_rbutton</span></div><input type="checkbox" id="tabs.wheel_tab_when_press_rbutton"></div>
    <div class="row"><div class="label"><span class="t">鼠标悬停自动激活标签页</span><span class="d">hover_tab</span></div><input type="checkbox" id="tabs.hover_tab"></div>
    <div class="row"><div class="label"><span class="t">悬停激活延迟（毫秒）</span><span class="d">hover_tab_delay（0 - 5000）</span></div><input type="number" id="tabs.hover_tab_delay" min="0" max="5000" step="50"></div>
    <div class="row"><div class="label"><span class="t">地址栏输入在新标签页打开</span><span class="d">open_url_new_tab</span></div><select id="tabs.open_url_new_tab"><option value="0">关闭</option><option value="1">前台新标签页</option><option value="2">后台新标签页</option></select></div>
    <div class="row"><div class="label"><span class="t">书签在新标签页打开</span><span class="d">open_bookmark_new_tab</span></div><select id="tabs.open_bookmark_new_tab"><option value="0">关闭</option><option value="1">前台新标签页</option><option value="2">后台新标签页</option></select></div>
    <div class="row"><div class="label"><span class="t">当前为新标签页时禁用上述两项</span><span class="d">new_tab_disable</span></div><input type="checkbox" id="tabs.new_tab_disable"></div>
    <div class="row col"><div class="label"><span class="t">额外新标签页标题列表</span><span class="d">new_tab_disable_name（用引号包裹、逗号分隔，修改后重启生效）</span></div><input type="text" id="tabs.new_tab_disable_name"></div>
  </section>
)HTML") +
      std::string(R"HTML(
  <section class="card">
    <div class="card-title">常规与启动 [general] <span class="sub">修改后需重启 Chrome 生效</span></div>
    <div class="row col">
      <div class="label"><span class="t">紧凑顶栏 / 界面缩放 (ui_scale)</span><span class="d">压缩标签栏与地址栏高度（自动附加 --top-chrome-touch-ui=disabled 与 --force-device-scale-factor）。留空跟随系统 DPI；若网页变小，可在 Chrome 外观设置里将“页面缩放”调为 110%～125%</span></div>
      <input type="text" id="general.ui_scale" placeholder="留空默认，例如 0.85 或 0.9">
      <div class="presets">
        <button type="button" class="preset" data-scale="">默认（跟随系统）</button>
        <button type="button" class="preset" data-scale="0.9">紧凑 (0.9)</button>
        <button type="button" class="preset" data-scale="0.85">更紧凑 (0.85)</button>
        <button type="button" class="preset" data-scale="0.8">超紧凑 (0.8)</button>
        <button type="button" class="preset" data-scale="1.0">1.0（覆盖系统高缩放）</button>
      </div>
    </div>
    <div class="row"><div class="label"><span class="t">老板键 (boss_key)</span><span class="d">例如 Ctrl+Alt+B</span></div><input type="text" id="general.boss_key"></div>
    <div class="row"><div class="label"><span class="t">网页翻译快捷键 (translate_key)</span><span class="d">例如 Alt+T</span></div><input type="text" id="general.translate_key"></div>
    <div class="row col"><div class="label"><span class="t">附加启动参数 (command_line)</span><span class="d">追加到 Chrome 启动命令行的参数</span></div><input type="text" id="general.command_line"></div>
    <div class="row col"><div class="label"><span class="t">数据目录 (data_dir)</span><span class="d">留空默认 %app%\..\Data，填 none 使用系统默认</span></div><input type="text" id="general.data_dir"></div>
    <div class="row col"><div class="label"><span class="t">缓存目录 (cache_dir)</span><span class="d">留空默认 %app%\..\Cache，填 none 使用系统默认</span></div><input type="text" id="general.cache_dir"></div>
    <div class="row col"><div class="label"><span class="t">启动时运行 (launch_on_startup)</span><span class="d">多条命令用分号 ; 分隔</span></div><input type="text" id="general.launch_on_startup"></div>
    <div class="row col"><div class="label"><span class="t">退出时运行 (launch_on_exit)</span><span class="d">多条命令用分号 ; 分隔</span></div><input type="text" id="general.launch_on_exit"></div>
    <div class="row"><div class="label"><span class="t">忽略企业策略 (ignore_policies)</span><span class="d">阻止从注册表读取 Chrome 企业策略</span></div><input type="checkbox" id="general.ignore_policies"></div>
    <div class="row"><div class="label"><span class="t">抑制错误的“已过期”升级提示</span><span class="d">suppress_false_upgrade_notification</span></div><input type="checkbox" id="general.suppress_false_upgrade_notification"></div>
    <div class="row"><div class="label"><span class="t">启用 win32k 兜底 (win32k)</span><span class="d">仅在启动崩溃时开启</span></div><input type="checkbox" id="general.win32k"></div>
  </section>

  <div class="notice">提示：[tabs] 标签页相关开关保存后立即生效；[proxy] 本地代理与 [general] 启动项修改后请完全关闭并重启 Chrome。</div>
</main>
<div id="toast"></div>
<script>
const token = '__CHROME_PLUS_TOKEN__';
const $ = id => document.getElementById(id);
const toast = msg => {
  const el = $('toast');
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(toast.t);
  toast.t = setTimeout(() => el.classList.remove('show'), 2600);
};
const fields = [
  'tabs.double_click_close','tabs.right_click_close','tabs.right_click_new_tab_clipboard',
  'tabs.keep_last_tab','tabs.wheel_tab','tabs.wheel_tab_when_press_rbutton',
  'tabs.hover_tab','tabs.hover_tab_delay','tabs.open_url_new_tab',
  'tabs.open_bookmark_new_tab','tabs.new_tab_disable','tabs.new_tab_disable_name',
  'proxy.enabled','proxy.server','proxy.bypass_list',
  'general.ui_scale','general.boss_key','general.translate_key','general.command_line',
  'general.data_dir','general.cache_dir','general.launch_on_startup',
  'general.launch_on_exit','general.ignore_policies',
  'general.suppress_false_upgrade_notification','general.win32k'
];
async function loadConfig() {
  try {
    const res = await fetch('/api/config', {
      headers: {'X-Chrome-Plus-Token': token}
    });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const data = await res.json();
    $('ver').textContent = 'v' + (data.version || '');
    $('iniPath').textContent = data.ini_path || 'chrome++.ini';
    for (const f of fields) {
      const [sec, key] = f.split('.');
      const val = data[sec]?.[key];
      const el = $(f);
      if (!el || val === undefined) continue;
      if (el.type === 'checkbox') el.checked = !!val;
      else el.value = String(val);
    }
  } catch (e) {
    toast('读取配置失败：' + e.message);
  }
}
async function saveConfig() {
  const payload = {};
  for (const f of fields) {
    const el = $(f);
    if (!el) continue;
    if (el.type === 'checkbox') payload[f] = el.checked ? 1 : 0;
    else if (el.type === 'number' || el.tagName === 'SELECT') payload[f] = parseInt(el.value, 10) || 0;
    else payload[f] = el.value.trim();
  }
  try {
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Chrome-Plus-Token': token},
      body: JSON.stringify(payload)
    });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    toast('已保存到 chrome++.ini（标签页设置已立即生效）');
  } catch (e) {
    toast('保存失败：' + e.message);
  }
}
$('saveBtn').addEventListener('click', saveConfig);
$('reloadBtn').addEventListener('click', async () => { await loadConfig(); toast('已重新读取配置'); });
document.querySelectorAll('.preset').forEach(btn => {
  btn.addEventListener('click', () => {
    if (btn.dataset.proxy !== undefined) {
      $('proxy.server').value = btn.dataset.proxy;
      $('proxy.enabled').checked = true;
    } else if (btn.dataset.scale !== undefined) {
      $('general.ui_scale').value = btn.dataset.scale;
    }
  });
});
loadConfig();
</script>
</body>
</html>)HTML");
  return kHtml;
}

void SendHttpResponse(SOCKET client,
                      std::string_view status,
                      std::string_view content_type,
                      std::string_view body) {
  std::string response;
  response.reserve(256 + body.size());
  response.append("HTTP/1.1 ");
  response.append(status);
  response.append("\r\nContent-Type: ");
  response.append(content_type);
  response.append("\r\nContent-Length: ");
  response.append(std::to_string(body.size()));
  response.append(
      "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
      "Connection: close\r\n\r\n");
  response.append(body);

  size_t total_sent = 0;
  while (total_sent < response.size()) {
    const int sent = ::send(
        client, response.data() + total_sent,
        static_cast<int>(response.size() - total_sent), 0);
    if (sent <= 0) {
      break;
    }
    total_sent += static_cast<size_t>(sent);
  }
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string_view GetHeaderValue(std::string_view headers,
                                std::string_view target_name) {
  size_t line_start = 0;
  while (line_start < headers.size()) {
    size_t line_end = headers.find("\r\n", line_start);
    if (line_end == std::string_view::npos) {
      line_end = headers.size();
    }
    const std::string_view line =
        headers.substr(line_start, line_end - line_start);
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos &&
        EqualsIgnoreCaseAscii(line.substr(0, colon), target_name)) {
      std::string_view val = line.substr(colon + 1);
      while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
        val.remove_prefix(1);
      }
      while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) {
        val.remove_suffix(1);
      }
      return val;
    }
    line_start = line_end + 2;
  }
  return {};
}

void HandleClient(SOCKET client) {
  constexpr DWORD kTimeoutMs = 3000;
  ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&kTimeoutMs), sizeof(kTimeoutMs));
  ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&kTimeoutMs), sizeof(kTimeoutMs));

  constexpr size_t kMaxRequestBytes = 64 * 1024;
  std::string request;
  request.reserve(4096);
  size_t header_end = std::string::npos;
  size_t expected_total = 0;

  std::array<char, 4096> chunk{};
  while (request.size() < kMaxRequestBytes) {
    const int n = ::recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
    if (n <= 0) {
      break;
    }
    request.append(chunk.data(), static_cast<size_t>(n));
    if (header_end == std::string::npos) {
      header_end = request.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        const std::string_view headers(request.data(), header_end);
        const std::string_view cl = GetHeaderValue(headers, "Content-Length");
        size_t content_length = 0;
        if (!cl.empty()) {
          std::from_chars(cl.data(), cl.data() + cl.size(), content_length);
        }
        expected_total = header_end + 4 + content_length;
      }
    }
    if (header_end != std::string::npos && request.size() >= expected_total) {
      break;
    }
  }

  if (header_end == std::string::npos) {
    ::closesocket(client);
    return;
  }

  const std::string_view header_block(request.data(), header_end);
  const size_t first_crlf = header_block.find("\r\n");
  const std::string_view request_line =
      first_crlf == std::string_view::npos
          ? header_block
          : header_block.substr(0, first_crlf);
  const std::string_view headers =
      first_crlf == std::string_view::npos
          ? std::string_view{}
          : header_block.substr(first_crlf + 2);

  // Guard against DNS rebinding.
  const std::string_view host = GetHeaderValue(headers, "Host");
  if (host != kExpectedHost) {
    SendHttpResponse(client, "403 Forbidden", "text/plain; charset=utf-8",
                     "Forbidden");
    ::closesocket(client);
    return;
  }

  const size_t sp1 = request_line.find(' ');
  const size_t sp2 =
      sp1 == std::string_view::npos ? sp1 : request_line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
    SendHttpResponse(client, "400 Bad Request", "text/plain; charset=utf-8",
                     "Bad Request");
    ::closesocket(client);
    return;
  }

  const std::string_view method = request_line.substr(0, sp1);
  const std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  const size_t q = target.find('?');
  const std::string_view path =
      q == std::string_view::npos ? target : target.substr(0, q);

  if (method == "GET" && path == "/") {
    std::string html = GetSettingsHtml();
    ReplaceStringInPlace(html, "__CHROME_PLUS_TOKEN__", g_session_token);
    SendHttpResponse(client, "200 OK", "text/html; charset=utf-8", html);
  } else if (method == "GET" && path == "/api/config") {
    if (GetHeaderValue(headers, "X-Chrome-Plus-Token") != g_session_token) {
      SendHttpResponse(client, "403 Forbidden", "application/json",
                       R"({"error":"forbidden"})");
    } else {
      SendHttpResponse(client, "200 OK", "application/json; charset=utf-8",
                       BuildConfigJson());
    }
  } else if (method == "POST" && path == "/api/config") {
    if (GetHeaderValue(headers, "X-Chrome-Plus-Token") != g_session_token) {
      SendHttpResponse(client, "403 Forbidden", "application/json",
                       R"({"error":"forbidden"})");
    } else {
      const std::string_view body(request.data() + header_end + 4,
                                  request.size() - (header_end + 4));
      if (ApplyFlatConfigJson(body)) {
        SendHttpResponse(client, "200 OK", "application/json; charset=utf-8",
                         R"({"ok":true})");
      } else {
        SendHttpResponse(client, "400 Bad Request", "application/json",
                         R"({"error":"invalid_payload"})");
      }
    }
  } else {
    SendHttpResponse(client, "404 Not Found", "text/plain; charset=utf-8",
                     "Not Found");
  }

  ::closesocket(client);
}

}  // namespace

void StartSettingsServer() {
  g_session_token = GenerateSessionToken();

  std::thread([] {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      return;
    }

    SOCKET listen_sock = INVALID_SOCKET;
    for (int attempt = 0; attempt < 30; ++attempt) {
      listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (listen_sock == INVALID_SOCKET) {
        ::Sleep(300);
        continue;
      }

      const BOOL reuse = TRUE;
      ::setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = htons(kSettingsPort);

      if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr),
                 sizeof(addr)) == 0 &&
          ::listen(listen_sock, SOMAXCONN) == 0) {
        break;
      }

      const int err = ::WSAGetLastError();
      ::closesocket(listen_sock);
      listen_sock = INVALID_SOCKET;
      // Retry when the previous Chrome instance is still shutting down.
      if (err == WSAEADDRINUSE) {
        ::Sleep(300);
        continue;
      }
      DebugLog(L"SettingsServer: bind/listen failed on port {} (WSA error {})",
               kSettingsPort, err);
      return;
    }

    if (listen_sock == INVALID_SOCKET) {
      return;
    }

    DebugLog(L"SettingsServer: listening on http://127.0.0.1:{}", kSettingsPort);

    while (true) {
      SOCKET client = ::accept(listen_sock, nullptr, nullptr);
      if (client == INVALID_SOCKET) {
        const int err = ::WSAGetLastError();
        if (err == WSAEINTR || err == WSAEWOULDBLOCK || err == WSAECONNRESET) {
          continue;
        }
        break;
      }
      HandleClient(client);
    }
    ::closesocket(listen_sock);
  }).detach();
}
