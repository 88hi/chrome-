#ifndef CHROME_PLUS_SRC_PAKPATCH_H_
#define CHROME_PLUS_SRC_PAKPATCH_H_

#include <string>

void PakPatch();
bool IsBookmarksPakPatched();
std::string GetBookmarksPakDiagnostic();

#endif  // CHROME_PLUS_SRC_PAKPATCH_H_
