#pragma once

#include <string>
#include <vector>

namespace sandvm::cli {

// `sandvm disk ...`; args are the words after "disk".
int cmd_disk(const std::vector<std::wstring>& args);

}  // namespace sandvm::cli
