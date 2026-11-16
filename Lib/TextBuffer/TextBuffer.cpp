#include "TextBuffer.h"
#include "SplitText.h"

#include <algorithm>
#include <functional>

namespace text
{
std::vector<std::string> splitText(std::string_view text)
{
    auto lines = std::vector<std::string> {std::string {}};

    for (auto c: text)
    {
        if (c == '\n')
            lines.emplace_back();
        else if (c != '\r')
            lines.back() += c;
    }

    return lines;
}

namespace
{
bool isBefore(Position a, Position b)
{
    if (a.line != b.line)
        return a.line < b.line;

    return a.column < b.column;
}

char foldAscii(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
} // namespace

TextBuffer::TextBuffer(std::string_view text)
{
    setText(text);
}

void TextBuffer::setText(std::string_view text)
{
    buffer = splitText(text);
    undoStack.clear();
    redoStack.clear();
}

std::string TextBuffer::text() const
{
    auto out = std::string {};

    for (std::size_t i = 0; i < buffer.size(); ++i)
        out += (i == 0 ? "" : "\n") + buffer[i];

    return out;
}

const std::vector<std::string>& TextBuffer::lines() const
{
    return buffer;
}

int TextBuffer::lineCount() const
{
    return static_cast<int>(buffer.size());
}

const std::string& TextBuffer::line(int index) const
{
    return buffer[static_cast<std::size_t>(std::clamp(index, 0, lineCount() - 1))];
}

Position TextBuffer::clamp(Position at) const
{
    at.line = std::clamp(at.line, 0, lineCount() - 1);
    at.column = std::min(at.column, line(at.line).size());
    return at;
}

Position TextBuffer::insert(Position at, std::string_view text)
{
    at = clamp(at);
    auto pieces = splitText(text);
    auto& first = buffer[static_cast<std::size_t>(at.line)];
    auto tail = first.substr(at.column);
    first.resize(at.column);
    first += pieces.front();

    if (pieces.size() == 1)
    {
        auto end = Position {at.line, first.size()};
        first += tail;
        return end;
    }

    auto row = at.line;

    for (std::size_t i = 1; i < pieces.size(); ++i)
        buffer.insert(buffer.begin() + ++row, pieces[i]);

    auto& last = buffer[static_cast<std::size_t>(row)];
    auto end = Position {row, last.size()};
    last += tail;
    return end;
}

std::string TextBuffer::erase(Position from, Position to)
{
    from = clamp(from);
    to = clamp(to);

    if (isBefore(to, from))
        std::swap(from, to);

    if (from == to)
        return {};

    auto& first = buffer[static_cast<std::size_t>(from.line)];

    if (from.line == to.line)
    {
        auto removed = first.substr(from.column, to.column - from.column);
        first.erase(from.column, to.column - from.column);
        return removed;
    }

    auto removed = first.substr(from.column);

    for (auto r = from.line + 1; r < to.line; ++r)
        removed += "\n" + buffer[static_cast<std::size_t>(r)];

    const auto& last = buffer[static_cast<std::size_t>(to.line)];
    removed += "\n" + last.substr(0, to.column);

    auto tail = last.substr(to.column);
    first.resize(from.column);
    first += tail;
    buffer.erase(buffer.begin() + from.line + 1, buffer.begin() + to.line + 1);
    return removed;
}

void TextBuffer::insertLines(int at, const std::vector<std::string>& lines)
{
    at = std::clamp(at, 0, lineCount());
    buffer.insert(buffer.begin() + at, lines.begin(), lines.end());
}

std::vector<std::string> TextBuffer::eraseLines(int first, int count)
{
    first = std::clamp(first, 0, lineCount() - 1);
    auto last = std::clamp(first + count, first, lineCount());
    auto removed =
        std::vector<std::string>(buffer.begin() + first, buffer.begin() + last);
    buffer.erase(buffer.begin() + first, buffer.begin() + last);

    if (buffer.empty())
        buffer.emplace_back();

    return removed;
}

void TextBuffer::replaceLine(int index, std::string text)
{
    buffer[static_cast<std::size_t>(std::clamp(index, 0, lineCount() - 1))] =
        std::move(text);
}

void TextBuffer::snapshot(Position cursor)
{
    undoStack.push_back({buffer, cursor});
    redoStack.clear();
}

std::optional<Position> TextBuffer::undo(Position cursor)
{
    return restoreSnapshot(undoStack, redoStack, cursor);
}

std::optional<Position> TextBuffer::redo(Position cursor)
{
    return restoreSnapshot(redoStack, undoStack, cursor);
}

std::optional<Position> TextBuffer::restoreSnapshot(std::vector<Snapshot>& takeFrom,
                                                    std::vector<Snapshot>& saveTo,
                                                    Position cursor)
{
    if (takeFrom.empty())
        return std::nullopt;

    saveTo.push_back({buffer, cursor});
    auto restored = std::move(takeFrom.back());
    takeFrom.pop_back();
    buffer = std::move(restored.lines);
    return clamp(restored.cursor);
}

bool TextBuffer::canUndo() const
{
    return !undoStack.empty();
}

bool TextBuffer::canRedo() const
{
    return !redoStack.empty();
}

bool ignoresCase(std::string_view needle)
{
    return std::none_of(
        needle.begin(), needle.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
}

std::vector<Match> findAll(std::string_view line, std::string_view needle)
{
    auto matches = std::vector<Match> {};

    if (needle.empty() || needle.size() > line.size())
        return matches;

    auto fold = ignoresCase(needle);
    auto equal = [fold](char a, char b)
    { return fold ? foldAscii(a) == foldAscii(b) : a == b; };

    auto at = std::size_t {0};

    while (at + needle.size() <= line.size())
    {
        auto hit = std::equal(needle.begin(),
                              needle.end(),
                              line.begin() + static_cast<std::ptrdiff_t>(at),
                              equal);

        if (hit)
        {
            matches.push_back({at, needle.size()});
            at += needle.size();
        }
        else
        {
            ++at;
        }
    }

    return matches;
}

namespace
{
using Matches = std::vector<Match>;

struct SearchRequest
{
    std::span<const std::string> lines;
    std::string_view needle;
    Position from;
    bool forward = true;
};

std::optional<Match> findFirstMatch(const Matches& matches,
                                    const std::function<bool(const Match&)>& isWanted)
{
    auto found = std::find_if(matches.begin(), matches.end(), isWanted);

    if (found == matches.end())
        return std::nullopt;

    return *found;
}

std::optional<Match> findLastMatch(const Matches& matches,
                                   const std::function<bool(const Match&)>& isWanted)
{
    auto found = std::find_if(matches.rbegin(), matches.rend(), isWanted);

    if (found == matches.rend())
        return std::nullopt;

    return *found;
}

bool isPastStart(const Match& match, const SearchRequest& request)
{
    if (request.forward)
        return match.start > request.from.column;

    return match.start < request.from.column;
}

std::optional<Match> findInSearchOrder(const Matches& matches,
                                       const SearchRequest& request,
                                       const std::function<bool(const Match&)>& isWanted)
{
    if (request.forward)
        return findFirstMatch(matches, isWanted);

    return findLastMatch(matches, isWanted);
}

// The match on the start line that the search reaches before leaving it.
std::optional<Match> findMatchPastStart(const Matches& onStartLine,
                                        const SearchRequest& request)
{
    return findInSearchOrder(onStartLine,
                             request,
                             [&request](const Match& match)
                             { return isPastStart(match, request); });
}

// The match on the start line that the search reaches only after wrapping
// all the way round to it.
std::optional<Match> findMatchBackAtStart(const Matches& onStartLine,
                                          const SearchRequest& request)
{
    return findInSearchOrder(onStartLine,
                             request,
                             [&request](const Match& match)
                             { return !isPastStart(match, request); });
}

int getLineCount(const SearchRequest& request)
{
    return static_cast<int>(request.lines.size());
}

int getLineStepsAway(const SearchRequest& request, int steps)
{
    auto lineCount = getLineCount(request);
    auto offset = request.forward ? steps : lineCount - steps;
    return (request.from.line + offset) % lineCount;
}

bool hasWrapped(const SearchRequest& request, int line)
{
    return request.forward ? line < request.from.line : line > request.from.line;
}

Matches findAllOnLine(const SearchRequest& request, int line)
{
    return findAll(request.lines[static_cast<std::size_t>(line)], request.needle);
}

// Every line but the start one, in the order the search visits them.
std::optional<SearchResult> searchOtherLines(const SearchRequest& request)
{
    for (auto steps = 1; steps < getLineCount(request); ++steps)
    {
        auto line = getLineStepsAway(request, steps);
        auto found = findAllOnLine(request, line);

        if (found.empty())
            continue;

        const auto& match = request.forward ? found.front() : found.back();
        return SearchResult {
            {line, match.start}, match.length, hasWrapped(request, line)};
    }

    return std::nullopt;
}

SearchResult makeStartLineResult(const SearchRequest& request,
                                 const Match& match,
                                 bool wrapped)
{
    return SearchResult {{request.from.line, match.start}, match.length, wrapped};
}
} // namespace

std::optional<SearchResult> search(std::span<const std::string> lines,
                                   std::string_view needle,
                                   Position from,
                                   bool forward)
{
    if (lines.empty() || needle.empty())
        return std::nullopt;

    from.line = std::clamp(from.line, 0, static_cast<int>(lines.size()) - 1);
    auto request = SearchRequest {lines, needle, from, forward};
    auto onStartLine = findAllOnLine(request, from.line);

    if (auto match = findMatchPastStart(onStartLine, request))
        return makeStartLineResult(request, *match, false);

    if (auto found = searchOtherLines(request))
        return found;

    if (auto match = findMatchBackAtStart(onStartLine, request))
        return makeStartLineResult(request, *match, true);

    return std::nullopt;
}
} // namespace text
