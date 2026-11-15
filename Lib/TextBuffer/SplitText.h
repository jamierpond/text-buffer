#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace text
{
// One string per line; '\r' is dropped, so CRLF text splits like LF text.
std::vector<std::string> splitText(std::string_view text);
} // namespace text
