#pragma once

#include "EditorText.h"
#include "VimEditor.h"

#include <SplitText.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace vim
{
struct Position
{
    int row = 0;
    int column = 0;

    bool operator==(const Position&) const = default;

    bool operator<(const Position& other) const
    {
        return row < other.row || (row == other.row && column < other.column);
    }
};

// Editor internals that work on positions, kept out of the header.
struct EditorOps
{
    Editor& e;

    const std::string& line(int row) { return e.buffer.line(row); }
    int rowCount() { return e.buffer.lineCount(); }

    static ::text::Position toText(Position p)
    {
        return {p.row, static_cast<std::size_t>(p.column)};
    }
    Position cursor() { return {e.cursorRow, e.cursorColumn}; }

    void setCursor(Position p)
    {
        e.cursorRow = std::clamp(p.row, 0, rowCount() - 1);
        e.cursorColumn = std::clamp(p.column, 0, size(line(e.cursorRow)));
    }

    void clampNormal()
    {
        e.cursorRow = std::clamp(e.cursorRow, 0, rowCount() - 1);
        const auto& current = line(e.cursorRow);

        if (e.currentMode == Mode::insert)
        {
            e.cursorColumn = std::clamp(e.cursorColumn, 0, size(current));
            return;
        }

        if (e.cursorColumn >= size(current))
            e.cursorColumn = lastCharStart(current);
    }

    void pushUndo() { e.buffer.snapshot(toText(cursor())); }

    // Class at a position; past the end of a line reads as whitespace.
    CharClass classAt(Position p)
    {
        const auto& text = line(p.row);

        if (p.column >= size(text))
            return CharClass::space;

        return classOf(text[static_cast<std::size_t>(p.column)]);
    }

    bool atLineEnd(Position p) { return p.column >= size(line(p.row)); }

    bool forward(Position& p)
    {
        if (!atLineEnd(p))
        {
            p.column = nextChar(line(p.row), p.column);

            if (!atLineEnd(p) || line(p.row).empty())
                return true;
        }

        if (p.row + 1 >= rowCount())
            return false;

        ++p.row;
        p.column = 0;
        return true;
    }

    bool backward(Position& p)
    {
        if (p.column > 0)
        {
            p.column = previousChar(line(p.row), p.column);
            return true;
        }

        if (p.row == 0)
            return false;

        --p.row;
        p.column = lastCharStart(line(p.row));
        return true;
    }

    bool isLastRow(int row) { return row + 1 >= rowCount(); }

    // Stops on the next non-blank character or on an empty line.
    Position skipBlanksForward(Position p)
    {
        while (true)
        {
            p.column = skipClassForward(line(p.row), p.column, CharClass::space);

            if (!atLineEnd(p) || isLastRow(p.row))
                return p;

            p = {p.row + 1, 0};

            if (line(p.row).empty())
                return p;
        }
    }

    Position wordForward(Position p)
    {
        auto start = classAt(p);

        if (start != CharClass::space)
            p.column = skipClassForward(line(p.row), p.column, start);

        return skipBlanksForward(p);
    }

    // False when the text ends first.
    bool skipSpacesForward(Position& p)
    {
        while (classAt(p) == CharClass::space)
            if (!forward(p))
                return false;

        return true;
    }

    Position moveToLastOfClass(Position p)
    {
        const auto& text = line(p.row);
        auto end = skipClassForward(text, p.column, classAt(p));
        p.column = previousChar(text, end);
        return p;
    }

    Position wordEnd(Position p)
    {
        if (!forward(p) || !skipSpacesForward(p))
            return p;

        return moveToLastOfClass(p);
    }

    // Stops on an empty line; false when the text starts first.
    bool skipSpacesBackward(Position& p)
    {
        while (classAt(p) == CharClass::space && !line(p.row).empty())
            if (!backward(p))
                return false;

        return true;
    }

    Position wordBack(Position p)
    {
        if (!backward(p) || !skipSpacesBackward(p))
            return p;

        if (line(p.row).empty())
            return p;

        p.column = skipClassBack(line(p.row), p.column, classAt(p));
        return p;
    }

    std::string extract(Position from, Position to)
    {
        if (from.row == to.row)
            return line(from.row).substr(
                static_cast<std::size_t>(from.column),
                static_cast<std::size_t>(to.column - from.column));

        auto text = line(from.row).substr(static_cast<std::size_t>(from.column));

        for (auto r = from.row + 1; r < to.row; ++r)
            text += "\n" + line(r);

        text += "\n" + line(to.row).substr(0, static_cast<std::size_t>(to.column));
        return text;
    }

    void erase(Position from, Position to, bool keepYank = false)
    {
        if (!(from < to))
            return;

        if (!keepYank)
        {
            e.yank = extract(from, to);
            e.yankLinewise = false;
        }

        e.buffer.erase(toText(from), toText(to));
        setCursor(from);
    }

    void insertRaw(std::string_view text)
    {
        auto end = e.buffer.insert(toText(cursor()), text);
        e.cursorRow = end.line;
        e.cursorColumn = static_cast<int>(end.column);
    }

    void deleteLines(int count, bool yankIt)
    {
        auto first = e.cursorRow;
        auto last = std::min(rowCount(), first + count);

        if (yankIt)
            yankLines(count);

        e.buffer.eraseLines(first, last - first);

        e.cursorRow = std::min(first, rowCount() - 1);
        e.cursorColumn = firstNonBlank(line(e.cursorRow));
    }

    void yankLines(int count)
    {
        auto last = std::min(rowCount(), e.cursorRow + count);
        e.yank = line(e.cursorRow);

        for (auto r = e.cursorRow + 1; r < last; ++r)
            e.yank += "\n" + line(r);

        e.yankLinewise = true;
    }

    void putLines(bool after, int count)
    {
        auto pieces = ::text::splitText(e.yank);
        auto at = after ? e.cursorRow + 1 : e.cursorRow;

        for (auto n = 0; n < count; ++n)
            e.buffer.insertLines(at, pieces);

        e.cursorRow = at;
        e.cursorColumn = firstNonBlank(line(at));
    }

    void putCharacters(bool after, int count)
    {
        if (after && !currentLine().empty())
            e.cursorColumn = nextChar(currentLine(), e.cursorColumn);

        auto text = std::string {};

        for (auto n = 0; n < count; ++n)
            text += e.yank;

        insertRaw(text);
        e.cursorColumn = previousChar(currentLine(), e.cursorColumn);
    }

    bool hasNothingYanked() { return e.yank.empty() && !e.yankLinewise; }

    void put(bool after, int count)
    {
        if (hasNothingYanked())
            return;

        pushUndo();

        if (e.yankLinewise)
            putLines(after, count);
        else
            putCharacters(after, count);
    }

    std::pair<int, int> innerWord()
    {
        const auto& text = line(e.cursorRow);

        if (text.empty())
            return {0, 0};

        auto cls = classOf(text[static_cast<std::size_t>(e.cursorColumn)]);
        auto start = skipClassBack(text, e.cursorColumn, cls);
        auto end = skipClassForward(text, nextChar(text, e.cursorColumn), cls);
        return {start, end};
    }

    std::pair<int, int> aroundWord()
    {
        auto [start, end] = innerWord();
        const auto& text = line(e.cursorRow);
        auto trailing = end;

        while (trailing < size(text)
               && classOf(text[static_cast<std::size_t>(trailing)])
                      == CharClass::space)
            ++trailing;

        if (trailing > end)
            return {start, trailing};

        while (start > 0
               && classOf(text[static_cast<std::size_t>(start - 1)])
                      == CharClass::space)
            --start;

        return {start, end};
    }

    static std::string getJoinSeparator(const std::string& left,
                                        const std::string& right)
    {
        if (left.empty() || right.empty())
            return {};

        return " ";
    }

    // False when there is no next line to join.
    bool joinNextLine()
    {
        if (isLastRow(e.cursorRow))
            return false;

        const auto& below = line(e.cursorRow + 1);
        auto next = below.substr(static_cast<std::size_t>(countLeadingBlanks(below)));
        auto current = line(e.cursorRow);

        while (!current.empty() && current.back() == ' ')
            current.pop_back();

        auto joinAt = size(current);
        current += getJoinSeparator(current, next) + next;
        e.buffer.replaceLine(e.cursorRow, current);
        e.buffer.eraseLines(e.cursorRow + 1, 1);
        e.cursorColumn = joinAt;
        return true;
    }

    // J joins count lines, so count - 1 joins, and at least one.
    void join(int count)
    {
        for (auto n = 0; n < std::max(1, count - 1); ++n)
            if (!joinNextLine())
                return;
    }

    bool jumpToMatch(const std::optional<::text::SearchResult>& found,
                     const std::string& pattern,
                     bool forward)
    {
        if (!found)
        {
            e.notice = "E486: Pattern not found: " + pattern;
            return false;
        }

        if (found->wrapped)
            e.notice = forward ? "search hit BOTTOM, continuing at TOP"
                               : "search hit TOP, continuing at BOTTOM";

        e.cursorRow = found->at.line;
        e.cursorColumn = static_cast<int>(found->at.column);
        return true;
    }

    const std::string& currentLine() { return line(e.cursorRow); }

    bool isStoppingOnSoftBreak(int column, const Editor::VisualRow& row, bool lastOfLine)
    {
        if (column < row.end || row.end <= row.start)
            return false;

        return !lastOfLine || e.currentMode != Mode::insert;
    }

    void moveVertical(int delta)
    {
        auto rows = e.visualRows();
        auto current = e.cursorVisualRow();

        if (e.desiredColumn < 0)
            e.desiredColumn = e.cursorVisualColumn();

        auto target =
            std::clamp(current + delta, 0, static_cast<int>(rows.size()) - 1);
        const auto& row = rows[static_cast<std::size_t>(target)];
        const auto& text = line(row.line);
        auto column = advance(text, row.start, e.desiredColumn, row.end);

        // Landing on a soft break would put the cursor on the next row.
        auto lastOfLine = row.end >= size(text);

        if (isStoppingOnSoftBreak(column, row, lastOfLine))
            column = previousChar(text, row.end);

        e.cursorRow = row.line;
        e.cursorColumn = column;
    }
};

Outcome feedPromptMode(Editor& editor, const std::string& key);
Outcome feedInsertMode(Editor& editor, const std::string& key);
Outcome feedNormalMode(Editor& editor, const std::string& key);
} // namespace vim
