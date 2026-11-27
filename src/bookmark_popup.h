#ifndef CHROME_PLUS_SRC_BOOKMARK_POPUP_H_
#define CHROME_PLUS_SRC_BOOKMARK_POPUP_H_

#include <windows.h>

void InitBookmarkPopup();
void ToggleBookmarkPopup(HWND chrome_root,
                         POINT anchor_screen_pt,
                         HWND trigger_btn_hwnd);
void CloseAllBookmarkPopups();
bool IsBookmarkPopupOpen();
bool IsBookmarkPopupWindow(HWND hwnd);
void RefreshOpenBookmarkPopups();

#endif  // CHROME_PLUS_SRC_BOOKMARK_POPUP_H_
