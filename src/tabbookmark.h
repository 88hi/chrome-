#ifndef CHROME_PLUS_SRC_TABBOOKMARK_H_
#define CHROME_PLUS_SRC_TABBOOKMARK_H_

#include <windows.h>

#include <string>

void TabBookmark();
// Opens a new tab in `root` and navigates/searches `text` via the omnibox
// without modifying the system clipboard.
void OpenInNewTab(HWND root, std::wstring text);

#endif  // CHROME_PLUS_SRC_TABBOOKMARK_H_
