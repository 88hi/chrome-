#include "pakpatch.h"

#include <windows.h>

#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "detours.h"

#include "pakfile.h"
#include "portable.h"
#include "settings_server.h"
#include "utils.h"
#include "version.h"

namespace {
#if defined(_M_ARM64)
#define BUILD_ARCH " (ARM64)"
#elif defined(_M_X64)
#define BUILD_ARCH " (64-bit)"
#else
#define BUILD_ARCH " (32-bit)"
#endif

HANDLE resources_pak_map = nullptr;
std::atomic<bool> g_bookmarks_pak_patched{false};
std::atomic<uint32_t> g_bm_cand_orig{0};
std::atomic<uint32_t> g_bm_cand_compacted{0};
std::atomic<uint32_t> g_bm_cand_new{0};
std::atomic<uint32_t> g_bm_cand_hits{0};
std::atomic<bool> g_bm_from_cache{false};
std::mutex g_bm_diag_mutex;
std::string g_bm_pak_diag = "uninit";

auto RawCreateFileMapping = CreateFileMappingW;
auto RawMapViewOfFile = MapViewOfFile;

// The browser sets the id variable to this when its pak has no target, as in a
// Chromium fork without the page, so its renderers, which map the same pak,
// skip the scan instead of each repeating it in full.
constexpr wchar_t kPakTargetMissing[] = L"none";

struct PakBlobHeader {
  uint32_t resource_id;
  uint32_t length;
};

using PatchFn = bool (*)(uint8_t*, uint32_t, size_t&);

struct PakPatchTarget {
  const wchar_t* id_env;
  const wchar_t* blob_env;
  const wchar_t* section_prefix;
  const wchar_t* cache_name;
  const char* build_tag;
  size_t min_entry_size;
  PatchFn patch_fn;
};

// Keeps published sections alive for the browser's lifetime so child processes
// can open them by name.
[[maybe_unused]] HANDLE published_blob_sections[2] = {nullptr, nullptr};

// The loader calls `PakPatch()` only in the browser and in `--type=renderer`
// children (chrome++.cc `Loader`), so an empty `--type` means the browser.
bool IsBrowserProcess() {
  return GetSwitchValue(GetCommandLineW(), L"--type").value_or(L"").empty();
}

uint16_t GetPakTargetId(const wchar_t* id_env) {
  wchar_t value[8];
  DWORD len = GetEnvironmentVariableW(id_env, value, ARRAYSIZE(value));
  if (len == 0 || len >= ARRAYSIZE(value)) {
    return 0;
  }
  wchar_t* end = nullptr;
  unsigned long id = wcstoul(value, &end, 10);
  if (end == value || id == 0 || id > 0xFFFF) {
    return 0;
  }
  return static_cast<uint16_t>(id);
}

bool IsPakTargetMissing(const wchar_t* id_env) {
  wchar_t value[8];
  const DWORD len = GetEnvironmentVariableW(id_env, value, ARRAYSIZE(value));
  return len > 0 && len < ARRAYSIZE(value) &&
         std::wstring_view(value, len) == kPakTargetMissing;
}

// Browser side: copy the patched slot into a named read-only section and
// publish the name through the environment. Read-only is load-bearing: the
// bytes feed privileged WebUI pages in every renderer, so a writable section
// would be an injection vector. The DACL grants read to Everyone and to
// RestrictedCode (the renderer's restricting SID), so a renderer's pre-lockdown
// token can open the section at the same point it maps the pak.
void PublishPatchedEntry(uint8_t* buffer,
                         uint16_t resource_id,
                         const PakPatchTarget& target,
                         size_t slot_index) {
  SetEnvironmentVariableW(target.blob_env, nullptr);

  const auto slot = FindResourceSlot(buffer, resource_id);
  if (!slot) {
    return;
  }

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GR;;;WD)(A;;GR;;;RC)", SDDL_REVISION_1,
          &sa.lpSecurityDescriptor, nullptr)) {
    return;
  }

  const std::wstring name =
      std::wstring(target.section_prefix) + std::to_wstring(GetCurrentProcessId());
  const DWORD size = static_cast<DWORD>(sizeof(PakBlobHeader) + slot->length);
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                      0, size, name.c_str());
  const DWORD create_error = GetLastError();
  LocalFree(sa.lpSecurityDescriptor);
  if (!section) {
    return;
  }
  if (create_error == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return;
  }

  auto* view =
      static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size));
  if (!view) {
    CloseHandle(section);
    return;
  }
  auto* header = reinterpret_cast<PakBlobHeader*>(view);
  header->resource_id = resource_id;
  header->length = slot->length;
  memcpy(view + sizeof(PakBlobHeader), buffer + slot->offset, slot->length);
  UnmapViewOfFile(view);

  if (slot_index < std::size(published_blob_sections)) {
    published_blob_sections[slot_index] = section;
  }
  SetEnvironmentVariableW(target.blob_env, name.c_str());
  DebugLog(L"PakPatch: published resource {} ({} bytes) as {}", resource_id,
           slot->length, name);
}

// Renderer fast path: overwrite this process's copy-on-write pak view with
// the browser's already-patched bytes.
uint16_t ApplyPatchedEntry(uint8_t* buffer, const PakPatchTarget& target) {
  wchar_t name[64];
  DWORD len = GetEnvironmentVariableW(target.blob_env, name, ARRAYSIZE(name));
  if (len == 0 || len >= ARRAYSIZE(name)) {
    return 0;
  }

  HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
  if (!section) {
    return 0;
  }

  uint16_t applied_id = 0;
  if (const auto* view = static_cast<const uint8_t*>(
          MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0))) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(view, &mbi, sizeof(mbi)) &&
        mbi.RegionSize >= sizeof(PakBlobHeader)) {
      const auto* header = reinterpret_cast<const PakBlobHeader*>(view);
      std::optional<PakResourceSlot> slot;
      if (header->resource_id > 0 && header->resource_id <= 0xFFFF &&
          header->length <= mbi.RegionSize - sizeof(PakBlobHeader)) {
        slot = FindResourceSlot(buffer,
                                static_cast<uint16_t>(header->resource_id));
      }
      if (slot && slot->length == header->length) {
        memcpy(buffer + slot->offset, view + sizeof(PakBlobHeader),
               slot->length);
        applied_id = static_cast<uint16_t>(header->resource_id);
        DebugLog(L"PakPatch: applied published resource {}",
                 header->resource_id);
      }
    }
    UnmapViewOfFile(view);
  }
  CloseHandle(section);
  return applied_id;
}

// The #172 settings-page injection, run on each candidate decompressed pak
// entry until it finds the one holding the settings-about-page HTML.
bool PatchSettingsHtml(uint8_t* begin, uint32_t size, size_t& new_len) {
  BYTE search_start[] = R"(</settings-about-page>)";
  auto match = SearchMemory(
      std::span<uint8_t>(begin, size),
      std::span<const uint8_t>(search_start, sizeof(search_start) - 1));
  if (match.empty()) {
    return false;
  }

  // Compress the HTML for writing patch information.
  std::string html(reinterpret_cast<char*>(begin), size);
  compression_html(html);

  // RemoveUpdateError
  ReplaceStringInPlace(html, R"(?hidden="${!this.showUpdateStatus_}")",
                       R"(hidden="true")");
  ReplaceStringInPlace(html, R"(?hidden="${!this.shouldShowIcons_()}")",
                       R"(hidden="true")");

  const std::string settings_url(GetSettingsServerUrl());
  const std::string product_title =
      R"({aboutBrowserVersion}</div><div class="secondary">Powered by <a target="_blank" href=")" +
      settings_url + R"(">Chrome++ 设置</a> )" RELEASE_VER_STR BUILD_ARCH
      R"(</div>)";
  ReplaceStringInPlace(html, R"({aboutBrowserVersion}</div>)", product_title);

  // Also inject a native-looking menu item into the chrome://settings left
  // sidebar right above the Extensions link.
  size_t ext_pos = html.find(R"(id="extensionsLink")");
  if (ext_pos == std::string::npos) {
    ext_pos = html.find(R"(href="chrome://extensions")");
  }
  if (ext_pos != std::string::npos) {
    const size_t search_from = ext_pos > 300 ? ext_pos - 300 : 0;
    const size_t anchor_pos = html.rfind("<a ", ext_pos);
    if (anchor_pos != std::string::npos && anchor_pos >= search_from) {
      const std::string sidebar_item =
          R"(<a role="menuitem" class="cr-nav-menu-item" href=")" +
          settings_url +
          R"(" target="_blank"><cr-icon icon="cr:settings"></cr-icon>Chrome++ 设置<cr-icon icon="cr:open-in-new"></cr-icon></a>)";
      html.insert(anchor_pos, sidebar_item);
    }
  }

  if (html.length() > size) {
    return false;
  }
  memcpy(begin, html.c_str(), html.length());
  new_len = html.length();
  return true;
}

std::string CompactTemplateSegment(std::string_view seg, bool is_css) {
  std::string cleaned;
  cleaned.reserve(seg.size());
  for (size_t i = 0; i < seg.size();) {
    if (is_css && i + 1 < seg.size() && seg[i] == '/' && seg[i + 1] == '*') {
      const size_t end = seg.find("*/", i + 2);
      if (end != std::string_view::npos) {
        i = end + 2;
        continue;
      }
    }
    if (!is_css && i + 3 < seg.size() && seg[i] == '<' && seg[i + 1] == '!' &&
        seg[i + 2] == '-' && seg[i + 3] == '-') {
      const size_t end = seg.find("-->", i + 4);
      if (end != std::string_view::npos) {
        i = end + 3;
        continue;
      }
    }
    cleaned.push_back(seg[i++]);
  }

  std::string out;
  out.reserve(cleaned.size());
  size_t i = 0;
  const size_t n = cleaned.size();
  while (i < n) {
    size_t eol = cleaned.find('\n', i);
    if (eol == std::string::npos) {
      eol = n;
    }
    size_t s = i;
    size_t e = eol;
    while (s < e &&
           (cleaned[s] == ' ' || cleaned[s] == '\t' || cleaned[s] == '\r')) {
      ++s;
    }
    while (e > s && (cleaned[e - 1] == ' ' || cleaned[e - 1] == '\t' ||
                     cleaned[e - 1] == '\r')) {
      --e;
    }
    if (s < e) {
      if (!out.empty()) {
        if (is_css) {
          const char prev = out.back();
          const char next = cleaned[s];
          if (prev != '{' && prev != '}' && prev != ';' && prev != ':' &&
              prev != ',' && next != '{' && next != '}' && next != ';') {
            out.push_back(' ');
          }
        } else {
          if (out.back() != '>' || cleaned[s] != '<') {
            out.push_back(' ');
          }
        }
      }
      bool prev_space = false;
      for (size_t k = s; k < e; ++k) {
        const char ch = cleaned[k];
        if (ch == ' ' || ch == '\t') {
          if (!prev_space) {
            out.push_back(' ');
            prev_space = true;
          }
        } else {
          // In CSS, preserve space before ':' (descendant combinator like `a :hover`).
          if (is_css && prev_space &&
              (ch == '{' || ch == '}' || ch == ';' || ch == ',')) {
            out.pop_back();
          }
          out.push_back(ch);
          prev_space = false;
          if (is_css &&
              (ch == '{' || ch == '}' || ch == ';' || ch == ',' || ch == ':')) {
            while (k + 1 < e &&
                   (cleaned[k + 1] == ' ' || cleaned[k + 1] == '\t')) {
              ++k;
            }
          }
        }
      }
    }
    i = (eol < n) ? (eol + 1) : n;
  }
  return out;
}

// JS-aware compactor that strips license comments and compacts multiline
// Lit/Polymer `html\`...\`` and `css\`...\`` template literals while keeping
// strings, regexes, and `${...}` interpolations untouched.
void CompactJsForPakPatch(std::string& js) {
  auto is_ident_char = [](unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == '$';
  };
  auto is_regex_keyword = [](std::string_view kw) {
    return kw == "return" || kw == "case" || kw == "throw" || kw == "else" ||
           kw == "do" || kw == "in" || kw == "of" || kw == "typeof" ||
           kw == "void" || kw == "delete" || kw == "new" || kw == "yield" ||
           kw == "await";
  };

  std::string out;
  out.reserve(js.size());
  const size_t n = js.size();
  size_t i = 0;
  bool can_start_regex = true;

  while (i < n) {
    const char c = js[i];
    if (c == '\'' || c == '"') {
      const char quote = c;
      out.push_back(js[i++]);
      while (i < n) {
        const char sc = js[i++];
        out.push_back(sc);
        if (sc == '\\' && i < n) {
          out.push_back(js[i++]);
        } else if (sc == quote) {
          break;
        }
      }
      can_start_regex = false;
      continue;
    }
    if (c == '/' && i + 1 < n && js[i + 1] == '*') {
      const size_t end = js.find("*/", i + 2);
      if (end != std::string::npos) {
        if (!out.empty() &&
            is_ident_char(static_cast<unsigned char>(out.back()))) {
          out.push_back(' ');
        }
        i = end + 2;
        continue;
      }
    }
    if (c == '/' && i + 1 < n && js[i + 1] == '/') {
      size_t eol = js.find('\n', i);
      if (eol == std::string::npos) {
        eol = n;
      }
      const std::string_view line(js.data() + i, eol - i);
      if ((i == 0 || js[i - 1] == '\n') &&
          (line.starts_with("// Copyright") ||
           line.starts_with("// Use of this source") ||
           line.starts_with("// found in the LICENSE") ||
           line.starts_with("// clang-format") ||
           line.starts_with("// eslint-disable"))) {
        i = (eol < n) ? (eol + 1) : n;
        continue;
      }
      out.append(line);
      if (eol < n) {
        out.push_back('\n');
      }
      i = (eol < n) ? (eol + 1) : n;
      continue;
    }
    if (c == '/' && can_start_regex) {
      size_t r = i + 1;
      bool in_class = false;
      bool closed = false;
      while (r < n && js[r] != '\n' && js[r] != '\r') {
        const char rc = js[r++];
        if (rc == '\\' && r < n && js[r] != '\n' && js[r] != '\r') {
          ++r;
        } else if (rc == '[') {
          in_class = true;
        } else if (rc == ']') {
          in_class = false;
        } else if (rc == '/' && !in_class) {
          closed = true;
          break;
        }
      }
      if (closed) {
        while (r < n && std::isalpha(static_cast<unsigned char>(js[r]))) {
          ++r;
        }
        out.append(js.data() + i, r - i);
        i = r;
        can_start_regex = false;
        continue;
      }
    }
    if (c == '`') {
      // Pre-scan this template literal's static segments to classify HTML vs CSS.
      bool tpl_has_nl = false;
      bool tpl_has_html = false;
      bool tpl_has_css_brace = false;
      bool tpl_has_css_colon = false;
      bool tpl_has_css_semi = false;
      size_t scan = i + 1;
      while (scan < n) {
        const char tc = js[scan];
        if (tc == '\\' && scan + 1 < n) {
          scan += 2;
          continue;
        }
        if (tc == '`') {
          break;
        }
        if (tc == '$' && scan + 1 < n && js[scan + 1] == '{') {
          scan += 2;
          int depth = 1;
          while (scan < n && depth > 0) {
            const char ec = js[scan++];
            if (ec == '\'' || ec == '"' || ec == '`') {
              const char q = ec;
              while (scan < n) {
                const char qch = js[scan++];
                if (qch == '\\' && scan < n) {
                  ++scan;
                } else if (qch == q) {
                  break;
                }
              }
            } else if (ec == '{') {
              ++depth;
            } else if (ec == '}') {
              --depth;
            }
          }
          continue;
        }
        if (tc == '\n') tpl_has_nl = true;
        if (tc == '<') tpl_has_html = true;
        if (tc == '{') tpl_has_css_brace = true;
        if (tc == ':') tpl_has_css_colon = true;
        if (tc == ';') tpl_has_css_semi = true;
        ++scan;
      }

      const bool compact_html = tpl_has_nl && tpl_has_html;
      const bool compact_css = tpl_has_nl && !tpl_has_html &&
                               tpl_has_css_brace && tpl_has_css_colon &&
                               tpl_has_css_semi;

      auto flush_seg = [&](size_t seg_start, size_t seg_end) {
        const std::string_view seg(js.data() + seg_start, seg_end - seg_start);
        if ((!compact_html && !compact_css) ||
            seg.find('\n') == std::string_view::npos) {
          out.append(seg);
          return;
        }
        const bool lead_ws = !seg.empty() && (seg.front() == ' ' ||
                                              seg.front() == '\t' ||
                                              seg.front() == '\n' ||
                                              seg.front() == '\r');
        const bool trail_ws = !seg.empty() && (seg.back() == ' ' ||
                                               seg.back() == '\t' ||
                                               seg.back() == '\n' ||
                                               seg.back() == '\r');
        std::string comp = CompactTemplateSegment(seg, compact_css);
        if (compact_html) {
          if (lead_ws && (comp.empty() || comp.front() != '<')) {
            out.push_back(' ');
          }
          out.append(comp);
          if (trail_ws && !comp.empty() && comp.back() != '>') {
            out.push_back(' ');
          }
        } else {
          out.append(comp);
        }
      };

      out.push_back(js[i++]);
      size_t seg_start = i;
      while (i < n) {
        const char tc = js[i];
        if (tc == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        if (tc == '`') {
          flush_seg(seg_start, i);
          out.push_back(js[i++]);
          break;
        }
        if (tc == '$' && i + 1 < n && js[i + 1] == '{') {
          flush_seg(seg_start, i);
          out.push_back('$');
          out.push_back('{');
          i += 2;
          int depth = 1;
          while (i < n && depth > 0) {
            const char ec = js[i++];
            out.push_back(ec);
            if (ec == '\'' || ec == '"' || ec == '`') {
              const char q = ec;
              while (i < n) {
                const char qch = js[i++];
                out.push_back(qch);
                if (qch == '\\' && i < n) {
                  out.push_back(js[i++]);
                } else if (qch == q) {
                  break;
                }
              }
            } else if (ec == '{') {
              ++depth;
            } else if (ec == '}') {
              --depth;
            }
          }
          seg_start = i;
          continue;
        }
        ++i;
      }
      can_start_regex = false;
      continue;
    }
    if (is_ident_char(static_cast<unsigned char>(c))) {
      const size_t start = i;
      while (i < n && is_ident_char(static_cast<unsigned char>(js[i]))) {
        out.push_back(js[i++]);
      }
      can_start_regex =
          is_regex_keyword(std::string_view(js.data() + start, i - start));
      continue;
    }
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      can_start_regex = (c != ')' && c != ']');
    }
    out.push_back(js[i++]);
  }
  js = std::move(out);
}

// Prepends a lightweight `#cp=<base64url_json>` handler to `chrome://bookmarks`'s
// JS bundle so Chrome++ popup edits (delete/edit/add/drag-reorder) execute
// BEFORE `<bookmarks-app>` upgrades or `bookmarks-router` rewrites the URL,
// synchronizing immediately into Chrome's live BookmarkModel and native bar.
bool PatchBookmarksJs(uint8_t* begin, uint32_t size, size_t& new_len) {
  const std::span<uint8_t> buf(begin, size);
  auto has_str = [&](std::string_view needle) {
    return !SearchMemory(
                buf, std::span<const uint8_t>(
                         reinterpret_cast<const uint8_t*>(needle.data()),
                         needle.size()))
                .empty();
  };

  const bool has_bookmarks_api = has_str("chrome.bookmarks");
  if (!has_bookmarks_api) {
    return false;
  }
  const bool has_app = has_str("bookmarks-app");
  const bool has_private = has_str("chrome.bookmarkManagerPrivate");
  const bool has_get_tree = has_str("chrome.bookmarks.getTree");
  if (!has_app && !(has_private && has_get_tree)) {
    return false;
  }

  g_bm_cand_hits.fetch_add(1, std::memory_order_relaxed);
  g_bm_cand_orig.store(size, std::memory_order_relaxed);

  std::string js(reinterpret_cast<char*>(begin), size);
  if (js.size() >= 3 && static_cast<unsigned char>(js[0]) == 0xEF &&
      static_cast<unsigned char>(js[1]) == 0xBB &&
      static_cast<unsigned char>(js[2]) == 0xBF) {
    js.erase(0, 3);
  }
  CompactJsForPakPatch(js);
  g_bm_cand_compacted.store(static_cast<uint32_t>(js.size()),
                            std::memory_order_relaxed);

  // Valid ES module syntax even before hoisted `import` declarations; runs
  // before `customElements.define('bookmarks-app', ...)` so `location.hash`
  // is captured before `bookmarks-router` can call `history.replaceState`.
  static constexpr std::string_view kSyncSnippet =
      "if(location.hash.startsWith(\"#cp=\"))(async()=>{"
      "document.documentElement.style.display=\"none\";"
      "let h=location.hash,a=\"\",ok=0,M={},C=[],B=chrome.bookmarks,"
      "P=(f,...x)=>new Promise(r=>{try{let q=f.call(B,...x,v=>{chrome.runtime?.lastError;r(v)});"
      "q?.then&&q.then(r,()=>r())}catch{r()}}),"
      "fl=(n,o=[])=>{for(let e of n||[])o.push(e),e.children&&fl(e.children,o);return o},"
      "fr=(r,b,s)=>{let x=r.filter(e=>e.folderType==(b?\"bookmarks-bar\":\"other\"));"
      "return x.find(e=>!!e.syncing==s)||x[0]||r.find(e=>e.id==(b?\"1\":\"2\"))||r[b?0:1]||r[0]},"
      "fn=(A,i,t,u,F,s)=>{let m=M[i]||i,e=m&&A.find(x=>x.id==m),K=x=>F==null||!!x.children==!!F;"
      "if(e&&K(e)&&(M[i]||(F?!t||e.title==t:(!u||e.url==u)&&(!t||e.title==t))))return e;"
      "let c=A.filter(x=>x.parentId&&x.parentId!=\"0\"&&K(x)&&(F?x.title==t:u?x.url==u:t&&x.title==t));"
      "return c.find(x=>x.title==t&&!!x.syncing==s)||c.find(x=>x.title==t)||"
      "c.find(x=>!!x.syncing==s)||c[0]||(e&&K(e)&&(!u||e.url==u)?e:null)};"
      "try{let p=JSON.parse(decodeURIComponent(escape(atob(h.slice(4).replace(/-/g,\"+\").replace(/_/g,\"/\")))));"
      "a=p.a||\"\";"
      "for(let o of p.o||[]){let T=await P(B.getTree);if(!T?.[0])break;"
      "let r=T[0].children||[],A=fl(r),w=o.ds==\"a\",q=o.ss==\"a\",k=o.k;"
      "if(k==\"d\"){let n=fn(A,o.id,o.ot,o.ou,o.f,q);if(n)await P(n.children?B.removeTree:B.remove,n.id),ok++}"
      "else if(k==\"u\"){let n=fn(A,o.id,o.ot,o.ou,o.f,q);if(n)await P(B.update,n.id,o.hu&&!n.children?{title:o.t,url:o.u}:{title:o.t}),ok++}"
      "else{let d=o.df==\"__bookmark_bar__\"?fr(r,1,w):o.df==\"__other__\"?fr(r,0,w):fn(A,o.df,o.dt,\"\",!0,w),"
      "n=k==\"m\"?fn(A,o.id,o.ot,o.ou,o.f,q):1;"
      "if(n&&d&&n.id!=d.id){let L=d.children||[],g=(o.ai||o.at||o.au)&&fn(L,o.ai,o.at,o.au,null,w),"
      "x=g&&g.id!=n.id?L.indexOf(g):-1,idx=x>=0?(o.ia?x+1:x):-1;"
      "if(k==\"a\"){let z={parentId:d.id,title:o.t};if(!o.f)z.url=o.u;if(idx>=0&&idx<=L.length)z.index=idx;"
      "let v=await P(B.create,z);if(v?.id)M[o.id]=v.id,C.push(o.id+\":\"+v.id),ok++}"
      "else if(k==\"m\"){let z={parentId:d.id};if(idx>=0)z.index=idx;await P(B.move,n.id,z),ok++}}}}}catch{}"
      "if(a.startsWith(\"http://127.0.0.1:\"))"
      "location.replace(a+\"&ok=\"+ok+(C.length?\"&map=\"+encodeURIComponent(C.join(\",\")):\"\"));"
      "else window.close()})();\n";

  std::string patched;
  patched.reserve(kSyncSnippet.size() + js.size());
  patched.append(kSyncSnippet);
  patched.append(js);
  g_bm_cand_new.store(static_cast<uint32_t>(patched.size()),
                      std::memory_order_relaxed);
  if (patched.length() > size) {
    return false;
  }
  memcpy(begin, patched.c_str(), patched.length());
  new_len = patched.length();
  return true;
}

// A file the cache below holds results for. The identity changes whenever
// Chrome replaces resources.pak, which a new version always does.
struct PakFileIdentity {
  uint32_t volume_serial;
  uint64_t file_index;
  uint64_t size;
  uint64_t last_write_time;

  friend bool operator==(const PakFileIdentity&,
                         const PakFileIdentity&) = default;
};

std::optional<PakFileIdentity> GetFileIdentity(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(file, &info)) {
    return std::nullopt;
  }
  return PakFileIdentity{
      .volume_serial = info.dwVolumeSerialNumber,
      .file_index = (uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow,
      .size = (uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow,
      .last_write_time = (uint64_t{info.ftLastWriteTime.dwHighDateTime} << 32) |
                         info.ftLastWriteTime.dwLowDateTime,
  };
}

// Set where the pak's mapping is created, before `PatchResourcesPak` runs.
std::optional<PakFileIdentity> resources_pak_identity;

struct PakCacheHeader {
  uint32_t magic;
  uint64_t build_hash;
  PakFileIdentity pak;
  // 0 when the pak has no target.
  uint16_t resource_id;
  uint32_t length;
  // Guards against a damaged file, which would otherwise hand Chrome a broken
  // entry on every start.
  uint64_t entry_hash;
};

constexpr uint32_t kPakCacheMagic = 0x43505045;
constexpr int64_t kMaxPakCacheSize = 64 * 1024 * 1024;

constexpr PakPatchTarget kSettingsTarget{
    .id_env = L"CHROME_PLUS_PAK_RES_ID",
    .blob_env = L"CHROME_PLUS_PAK_BLOB",
    .section_prefix = L"Local\\ChromePlusPakBlob_",
    .cache_name = L"Chrome++_PakCache.bin",
    .build_tag = RELEASE_VER_STR BUILD_ARCH,
    .min_entry_size = 10 * 1024,
    .patch_fn = PatchSettingsHtml,
};

constexpr PakPatchTarget kBookmarksTarget{
    .id_env = L"CHROME_PLUS_PAK_BM_ID",
    .blob_env = L"CHROME_PLUS_PAK_BM_BLOB",
    .section_prefix = L"Local\\ChromePlusPakBmBlob_",
    .cache_name = L"Chrome++_PakCache_Bm.bin",
    .build_tag = RELEASE_VER_STR BUILD_ARCH "-bm-sync-v3",
    .min_entry_size = 2 * 1024,
    .patch_fn = PatchBookmarksJs,
};

uint64_t HashEntry(std::span<const uint8_t> entry) {
  return Fnv1aHash(std::as_bytes(entry));
}

uint64_t HashBuildTag(const char* tag) {
  return Fnv1aHash(
      std::as_bytes(std::span<const char>(tag, std::strlen(tag))));
}

std::optional<std::wstring> GetPakCacheDir() {
  using GetUserDataDirectoryThunkFn =
      bool (*)(wchar_t*, size_t, wchar_t*, size_t, bool*);
  HMODULE chrome_elf = GetModuleHandleW(L"chrome_elf.dll");
  if (!chrome_elf) {
    return std::nullopt;
  }
  auto get_user_data_dir = reinterpret_cast<GetUserDataDirectoryThunkFn>(
      GetProcAddress(chrome_elf, "GetUserDataDirectoryThunk"));
  if (!get_user_data_dir) {
    return std::nullopt;
  }
  wchar_t dir[MAX_PATH] = {};
  wchar_t invalid_dir[MAX_PATH] = {};
  if (!get_user_data_dir(dir, std::size(dir), invalid_dir,
                         std::size(invalid_dir), nullptr) ||
      dir[0] == L'\0') {
    return std::nullopt;
  }
  return std::wstring(dir);
}

std::wstring GetPakCachePath(const std::wstring& dir,
                             const wchar_t* cache_name) {
  return dir + L"\\" + cache_name;
}

std::optional<std::vector<uint8_t>> ReadPakCache(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> data;
  LARGE_INTEGER size;
  if (GetFileSizeEx(file, &size) && size.QuadPart <= kMaxPakCacheSize) {
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                 nullptr) &&
        read == bytes.size()) {
      data = std::move(bytes);
    }
  }
  CloseHandle(file);
  return data;
}

std::optional<uint16_t> ApplyCachedResult(uint8_t* buffer,
                                          const PakFileIdentity& identity,
                                          const PakPatchTarget& target,
                                          bool skip_bytes,
                                          uint16_t* hint_id) {
  const std::optional<std::wstring> dir = GetPakCacheDir();
  if (!dir) {
    return std::nullopt;
  }
  const std::optional<std::vector<uint8_t>> data =
      ReadPakCache(GetPakCachePath(*dir, target.cache_name));
  if (!data || data->size() < sizeof(PakCacheHeader)) {
    return std::nullopt;
  }
  PakCacheHeader header;
  memcpy(&header, data->data(), sizeof(header));
  const auto entry = std::span(*data).subspan(sizeof(header));
  if (header.magic != kPakCacheMagic || header.pak != identity ||
      entry.size() != header.length ||
      HashEntry(entry) != header.entry_hash) {
    return std::nullopt;
  }
  const bool build_matches =
      header.build_hash == HashBuildTag(target.build_tag);
  if (header.resource_id == 0) {
    if (!build_matches) {
      return std::nullopt;
    }
    DebugLog(L"PakPatch: cached result: no target");
    return 0;
  }
  if (hint_id) {
    *hint_id = header.resource_id;
  }
  if (skip_bytes || !build_matches) {
    return std::nullopt;
  }
  const std::optional<PakResourceSlot> slot =
      FindResourceSlot(buffer, header.resource_id);
  if (!slot || slot->length != entry.size()) {
    return std::nullopt;
  }
  std::ranges::copy(entry, buffer + slot->offset);
  DebugLog(L"PakPatch: applied cached resource {}", header.resource_id);
  return header.resource_id;
}

void SaveResultToCache(uint8_t* buffer,
                       uint16_t resource_id,
                       const PakFileIdentity& identity,
                       const PakPatchTarget& target) {
  const std::optional<std::wstring> dir = GetPakCacheDir();
  if (!dir) {
    return;
  }
  std::span<const uint8_t> entry;
  if (resource_id != 0) {
    const std::optional<PakResourceSlot> slot =
        FindResourceSlot(buffer, resource_id);
    if (!slot) {
      return;
    }
    entry = std::span(buffer + slot->offset, slot->length);
  }
  PakCacheHeader header;
  memset(&header, 0, sizeof(header));
  header.magic = kPakCacheMagic;
  header.build_hash = HashBuildTag(target.build_tag);
  header.pak = identity;
  header.resource_id = resource_id;
  header.length = static_cast<uint32_t>(entry.size());
  header.entry_hash = HashEntry(entry);

  const std::wstring path = GetPakCachePath(*dir, target.cache_name);
  const std::wstring temp_path =
      path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  HANDLE file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) &&
            written == sizeof(header);
  if (ok && !entry.empty()) {
    ok = WriteFile(file, entry.data(), static_cast<DWORD>(entry.size()),
                   &written, nullptr) &&
         written == entry.size();
  }
  CloseHandle(file);
  if (!ok || !MoveFileExW(temp_path.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(temp_path.c_str());
  }
}

__declspec(noinline) uint16_t PatchOneTarget(uint8_t* buffer,
                                             const PakPatchTarget& target,
                                             size_t slot_index,
                                             uint16_t skip_resource_id) {
  const bool is_browser = IsBrowserProcess();
  if (!is_browser) {
    if (IsPakTargetMissing(target.id_env)) {
      return 0;
    }
    if (const uint16_t applied = ApplyPatchedEntry(buffer, target);
        applied != 0) {
      return applied;
    }
  }

  std::optional<uint16_t> cached_id;
  uint16_t cached_hint_id = 0;
  if (is_browser && resources_pak_identity) {
    cached_id = ApplyCachedResult(buffer, *resources_pak_identity, target,
                                  /*skip_bytes=*/false, &cached_hint_id);
  }
  uint16_t matched_id = 0;
  if (cached_id) {
    matched_id = *cached_id;
    if (slot_index == 1) {
      g_bm_from_cache.store(true, std::memory_order_relaxed);
    }
  } else {
    const uint16_t target_id =
        cached_hint_id != 0 ? cached_hint_id : GetPakTargetId(target.id_env);
    matched_id = PatchPakResource(buffer, target.patch_fn, target_id,
                                  target.min_entry_size, skip_resource_id);
    if (matched_id == 0 && target_id != 0) {
      matched_id = PatchPakResource(buffer, target.patch_fn, 0,
                                    target.min_entry_size, skip_resource_id);
    }
    if (is_browser && resources_pak_identity) {
      SaveResultToCache(buffer, matched_id, *resources_pak_identity, target);
    }
  }

  if (!is_browser) {
    return matched_id;
  }
  if (matched_id == 0) {
    DebugLog(L"PakPatch: no target in resources.pak for {}", target.id_env);
    SetEnvironmentVariableW(target.id_env, kPakTargetMissing);
    return 0;
  }
  SetEnvironmentVariableW(target.id_env, std::to_wstring(matched_id).c_str());
  PublishPatchedEntry(buffer, matched_id, target, slot_index);
  return matched_id;
}

uint16_t SafePatchBookmarksTarget(uint8_t* buffer, uint16_t skip_id) {
  __try {
    return PatchOneTarget(buffer, kBookmarksTarget, 1, skip_id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

void PatchResourcesPak(uint8_t* buffer) {
  const uint16_t settings_id = PatchOneTarget(buffer, kSettingsTarget, 0, 0);
  const uint16_t bm_id = SafePatchBookmarksTarget(buffer, settings_id);
  if (IsBrowserProcess()) {
    std::string diag;
    if (bm_id != 0) {
      g_bookmarks_pak_patched.store(true, std::memory_order_relaxed);
      if (g_bm_from_cache.load(std::memory_order_relaxed)) {
        diag = "ok(cached id=" + std::to_string(bm_id) + ")";
      } else {
        diag = "ok(id=" + std::to_string(bm_id) + ", orig=" +
               std::to_string(g_bm_cand_orig.load(std::memory_order_relaxed)) +
               ", comp=" +
               std::to_string(
                   g_bm_cand_compacted.load(std::memory_order_relaxed)) +
               ", new=" +
               std::to_string(g_bm_cand_new.load(std::memory_order_relaxed)) +
               ")";
      }
    } else if (g_bm_cand_hits.load(std::memory_order_relaxed) > 0) {
      diag = "no_fit(hits=" +
             std::to_string(g_bm_cand_hits.load(std::memory_order_relaxed)) +
             ", orig=" +
             std::to_string(g_bm_cand_orig.load(std::memory_order_relaxed)) +
             ", comp=" +
             std::to_string(
                 g_bm_cand_compacted.load(std::memory_order_relaxed)) +
             ", new=" +
             std::to_string(g_bm_cand_new.load(std::memory_order_relaxed)) +
             ")";
    } else if (g_bm_from_cache.load(std::memory_order_relaxed)) {
      diag = "cached_none";
    } else {
      diag = "not_found";
    }
    std::lock_guard lock(g_bm_diag_mutex);
    g_bm_pak_diag = std::move(diag);
  }
}

HANDLE WINAPI MyMapViewOfFile(_In_ HANDLE hFileMappingObject,
                              _In_ DWORD dwDesiredAccess,
                              _In_ DWORD dwFileOffsetHigh,
                              _In_ DWORD dwFileOffsetLow,
                              _In_ SIZE_T dwNumberOfBytesToMap) {
  if (hFileMappingObject == resources_pak_map) {
    // Modify it to be modifiable.
    LPVOID buffer =
        RawMapViewOfFile(hFileMappingObject, FILE_MAP_COPY, dwFileOffsetHigh,
                         dwFileOffsetLow, dwNumberOfBytesToMap);

    // No more hook needed.
    resources_pak_map = nullptr;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&RawMapViewOfFile, MyMapViewOfFile);
    auto status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Unhook RawMapViewOfFile failed {}", status);
    }

    if (buffer) {
      PatchResourcesPak(static_cast<uint8_t*>(buffer));
    }

    return buffer;
  }

  return RawMapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh,
                          dwFileOffsetLow, dwNumberOfBytesToMap);
}

// Identify `resources.pak` where it is mapped, by querying the handle, rather
// than where it is opened. Chrome maps the pak with
// `CreateFileMapping`/`MapViewOfFile` (`base::MemoryMappedFile`), so this hook
// catches it directly in every process that loads it -- including the renderer,
// which opens the pak by path itself (`AddDataPackFromPath` in
// `ChromeMainDelegate::PreSandboxStartup`, before sandbox lockdown). The
// previous `CreateFile` hook existed only to record that handle so this hook
// could match it later; querying the handle here drops that second hook.
// `GetFileInformationByHandleEx(FileNameInfo)` reads the path of a handle the
// process already holds, which the sandbox permits -- it brokers new opens
// (`NtCreateFile`/`NtOpenFile`), not operations on existing handles.
// https://chromium.googlesource.com/chromium/src/+/main/docs/design/sandbox.md
bool IsResourcesPak(HANDLE hFile) {
  if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE) {
    return false;
  }
  alignas(FILE_NAME_INFO)
      BYTE buffer[sizeof(FILE_NAME_INFO) + MAX_PATH * 2 * sizeof(wchar_t)];
  auto* info = reinterpret_cast<FILE_NAME_INFO*>(buffer);
  if (!GetFileInformationByHandleEx(hFile, FileNameInfo, info,
                                    sizeof(buffer))) {
    return false;
  }
  std::wstring_view name(info->FileName,
                         info->FileNameLength / sizeof(wchar_t));
  return name.ends_with(L"resources.pak");
}

HANDLE WINAPI MyCreateFileMapping(_In_ HANDLE hFile,
                                  _In_opt_ LPSECURITY_ATTRIBUTES lpAttributes,
                                  _In_ DWORD flProtect,
                                  _In_ DWORD dwMaximumSizeHigh,
                                  _In_ DWORD dwMaximumSizeLow,
                                  _In_opt_ LPCTSTR lpName) {
  if (IsResourcesPak(hFile)) {
    resources_pak_identity = GetFileIdentity(hFile);
    // Force copy-on-write so the mapped view can be patched in memory.
    resources_pak_map =
        RawCreateFileMapping(hFile, lpAttributes, PAGE_WRITECOPY,
                             dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

    // No more hook needed.
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&RawCreateFileMapping, MyCreateFileMapping);
    auto status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Unhook RawCreateFileMapping failed {}", status);
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&RawMapViewOfFile, MyMapViewOfFile);
    status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      DebugLog(L"Hook RawMapViewOfFile failed {}", status);
    }

    return resources_pak_map;
  }
  return RawCreateFileMapping(hFile, lpAttributes, flProtect, dwMaximumSizeHigh,
                              dwMaximumSizeLow, lpName);
}

}  // namespace

void PakPatch() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&RawCreateFileMapping, MyCreateFileMapping);
  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    DebugLog(L"Hook RawCreateFileMapping failed {}", status);
  }
}

bool IsBookmarksPakPatched() {
  return g_bookmarks_pak_patched.load(std::memory_order_relaxed);
}

std::string GetBookmarksPakDiagnostic() {
  std::lock_guard lock(g_bm_diag_mutex);
  return g_bm_pak_diag;
}
