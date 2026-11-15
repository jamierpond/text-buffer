#include "EditorText.h"

#include <algorithm>
#include <cctype>

namespace vim
{
bool isContinuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

int size(const std::string& text)
{
    return static_cast<int>(text.size());
}

int nextChar(const std::string& line, int at)
{
    if (at >= size(line))
        return size(line);

    ++at;

    while (at < size(line) && isContinuation(line[static_cast<std::size_t>(at)]))
        ++at;

    return at;
}

int previousChar(const std::string& line, int at)
{
    if (at <= 0)
        return 0;

    --at;

    while (at > 0 && isContinuation(line[static_cast<std::size_t>(at)]))
        --at;

    return at;
}

int lastCharStart(const std::string& line)
{
    return line.empty() ? 0 : previousChar(line, size(line));
}

int codepoints(const std::string& line, int from, int to)
{
    auto count = 0;

    for (auto i = from; i < to; ++i)
        if (!isContinuation(line[static_cast<std::size_t>(i)]))
            ++count;

    return count;
}

int advance(const std::string& line, int from, int count, int limit)
{
    while (count > 0 && from < limit)
    {
        from = nextChar(line, from);
        --count;
    }

    return std::min(from, limit);
}


// Bytes of multi-byte characters count as word characters.
bool isWordByte(char c)
{
    auto byte = static_cast<unsigned char>(c);

    if (byte >= 0x80)
        return true;

    return std::isalnum(byte) != 0 || c == '_';
}

CharClass classOf(char c)
{
    auto byte = static_cast<unsigned char>(c);

    if (isWordByte(c))
        return CharClass::word;

    if (std::isspace(byte) != 0)
        return CharClass::space;

    return CharClass::punctuation;
}

bool isBlank(char c)
{
    return c == ' ' || c == '\t';
}

int countLeadingBlanks(const std::string& line)
{
    auto blanks = std::find_if_not(line.begin(), line.end(), isBlank);
    return static_cast<int>(blanks - line.begin());
}

int firstNonBlank(const std::string& line)
{
    auto at = countLeadingBlanks(line);
    return at >= size(line) ? lastCharStart(line) : at;
}

int skipClassBack(const std::string& text, int column, CharClass cls)
{
    while (column > 0)
    {
        auto before = previousChar(text, column);

        if (classOf(text[static_cast<std::size_t>(before)]) != cls)
            break;

        column = before;
    }

    return column;
}

int skipClassForward(const std::string& text, int column, CharClass cls)
{
    while (column < size(text) && classOf(text[static_cast<std::size_t>(column)]) == cls)
        column = nextChar(text, column);

    return column;
}

// Where a soft-wrapped row that starts at `start` ends, given that at most
// `cut` fits: just after the last space if there is one, else at `cut`.
int getWrapEnd(const std::string& text, int start, int cut)
{
    auto space = text.substr(0, static_cast<std::size_t>(cut))
                     .rfind(' ', static_cast<std::size_t>(cut - 1));

    if (space == std::string::npos)
        return cut;

    auto spaceColumn = static_cast<int>(space);

    if (spaceColumn < start || spaceColumn + 1 <= start)
        return cut;

    return spaceColumn + 1;
}

bool isCancelKey(const std::string& key)
{
    return key == "Esc" || key == "C-c";
}

bool isBackspaceKey(const std::string& key)
{
    return key == "Backspace" || key == "C-h";
}

bool isPrintable(const std::string& key)
{
    if (key.empty())
        return false;

    auto first = static_cast<unsigned char>(key[0]);

    if (first >= 0x80)
        return true;

    return key.size() == 1 && first >= 0x20 && first != 0x7F;
}
} // namespace vim
