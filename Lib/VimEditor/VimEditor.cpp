#include "VimEditor.h"

#include <SplitText.h>

#include <algorithm>
#include <cctype>
#include <optional>

namespace vim
{
namespace
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

enum class CharClass
{
    space,
    word,
    punctuation
};

CharClass classOf(char c)
{
    auto byte = static_cast<unsigned char>(c);

    if (byte >= 0x80 || std::isalnum(byte) != 0 || c == '_')
        return CharClass::word;

    if (std::isspace(byte) != 0)
        return CharClass::space;

    return CharClass::punctuation;
}

int firstNonBlank(const std::string& line)
{
    auto at = 0;

    while (at < size(line)
           && (line[static_cast<std::size_t>(at)] == ' '
               || line[static_cast<std::size_t>(at)] == '\t'))
        ++at;

    return at >= size(line) ? lastCharStart(line) : at;
}

bool isCancelKey(const std::string& key)
{
    return key == "Esc" || key == "C-c";
}

bool isBackspaceKey(const std::string& key)
{
    return key == "Backspace" || key == "C-h";
}

constexpr auto largestCount = 99999;

constexpr auto noPreviousPatternNotice = "E35: No previous regular expression";

bool isPrintable(const std::string& key)
{
    if (key.empty())
        return false;

    auto first = static_cast<unsigned char>(key[0]);

    if (first >= 0x80)
        return true;

    return key.size() == 1 && first >= 0x20 && first != 0x7F;
}
} // namespace

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

    Position wordForward(Position p)
    {
        auto start = classAt(p);

        if (start != CharClass::space)
            while (!atLineEnd(p) && classAt(p) == start)
                p.column = nextChar(line(p.row), p.column);

        while (true)
        {
            if (atLineEnd(p))
            {
                if (p.row + 1 >= rowCount())
                    return p;

                ++p.row;
                p.column = 0;

                if (line(p.row).empty())
                    return p;

                continue;
            }

            if (classAt(p) != CharClass::space)
                return p;

            p.column = nextChar(line(p.row), p.column);
        }
    }

    Position wordEnd(Position p)
    {
        if (!forward(p))
            return p;

        while (classAt(p) == CharClass::space)
            if (!forward(p))
                return p;

        auto cls = classAt(p);

        while (true)
        {
            auto next = nextChar(line(p.row), p.column);

            if (next >= size(line(p.row))
                || classOf(line(p.row)[static_cast<std::size_t>(next)]) != cls)
                return p;

            p.column = next;
        }
    }

    Position wordBack(Position p)
    {
        if (!backward(p))
            return p;

        while (classAt(p) == CharClass::space && !line(p.row).empty())
            if (!backward(p))
                return p;

        if (line(p.row).empty())
            return p;

        auto cls = classAt(p);

        while (p.column > 0)
        {
            auto before = previousChar(line(p.row), p.column);

            if (classOf(line(p.row)[static_cast<std::size_t>(before)]) != cls)
                break;

            p.column = before;
        }

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
        {
            e.yank.clear();

            for (auto r = first; r < last; ++r)
                e.yank += (r == first ? "" : "\n") + line(r);

            e.yankLinewise = true;
        }

        e.buffer.eraseLines(first, last - first);

        e.cursorRow = std::min(first, rowCount() - 1);
        e.cursorColumn = firstNonBlank(line(e.cursorRow));
    }

    void yankLines(int count)
    {
        auto last = std::min(rowCount(), e.cursorRow + count);
        e.yank.clear();

        for (auto r = e.cursorRow; r < last; ++r)
            e.yank += (r == e.cursorRow ? "" : "\n") + line(r);

        e.yankLinewise = true;
    }

    void put(bool after, int count)
    {
        if (e.yank.empty() && !e.yankLinewise)
            return;

        pushUndo();

        if (e.yankLinewise)
        {
            auto pieces = ::text::splitText(e.yank);
            auto at = after ? e.cursorRow + 1 : e.cursorRow;

            for (auto n = 0; n < count; ++n)
                e.buffer.insertLines(at, pieces);

            e.cursorRow = at;
            e.cursorColumn = firstNonBlank(line(at));
            return;
        }

        if (after && !line(e.cursorRow).empty())
            e.cursorColumn = nextChar(line(e.cursorRow), e.cursorColumn);

        auto text = std::string {};

        for (auto n = 0; n < count; ++n)
            text += e.yank;

        insertRaw(text);
        e.cursorColumn = previousChar(line(e.cursorRow), e.cursorColumn);
    }

    std::pair<int, int> innerWord()
    {
        const auto& text = line(e.cursorRow);

        if (text.empty())
            return {0, 0};

        auto cls = classOf(text[static_cast<std::size_t>(e.cursorColumn)]);
        auto start = e.cursorColumn;
        auto end = nextChar(text, e.cursorColumn);

        while (start > 0)
        {
            auto before = previousChar(text, start);

            if (classOf(text[static_cast<std::size_t>(before)]) != cls)
                break;

            start = before;
        }

        while (end < size(text)
               && classOf(text[static_cast<std::size_t>(end)]) == cls)
            end = nextChar(text, end);

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

    void join(int count)
    {
        for (auto n = 0; n < std::max(1, count - 1); ++n)
        {
            if (e.cursorRow + 1 >= rowCount())
                return;

            auto next = line(e.cursorRow + 1);
            auto at = 0;

            while (at < size(next)
                   && (next[static_cast<std::size_t>(at)] == ' '
                       || next[static_cast<std::size_t>(at)] == '\t'))
                ++at;

            next = next.substr(static_cast<std::size_t>(at));
            auto current = line(e.cursorRow);

            while (!current.empty() && current.back() == ' ')
                current.pop_back();

            auto joinAt = size(current);

            if (!current.empty() && !next.empty())
                current += " ";

            current += next;
            e.buffer.replaceLine(e.cursorRow, current);
            e.buffer.eraseLines(e.cursorRow + 1, 1);
            e.cursorColumn = joinAt;
        }
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

    void restoreSearchStart()
    {
        e.cursorRow = e.searchStartRow;
        e.cursorColumn = e.searchStartColumn;
        e.currentMode = Mode::normal;
        e.command.clear();
        e.liveMatch.reset();
    }

    std::optional<::text::SearchResult> findFromSearchStart(const std::string& pattern)
    {
        return ::text::search(
            e.buffer.lines(),
            pattern,
            {e.searchStartRow, static_cast<std::size_t>(e.searchStartColumn)},
            e.searchForward);
    }

    void acceptSearch()
    {
        auto pattern = e.command.empty() ? e.lastPattern : e.command;
        restoreSearchStart();

        if (pattern.empty())
        {
            e.notice = noPreviousPatternNotice;
            return;
        }

        e.lastPattern = pattern;
        e.lastForward = e.searchForward;
        jumpToMatch(findFromSearchStart(pattern), pattern, e.searchForward);
    }

    void eraseLastCommandCharacter()
    {
        e.command.erase(
            static_cast<std::size_t>(previousChar(e.command, size(e.command))));
    }

    void showLiveMatch()
    {
        auto found =
            e.command.empty() ? std::nullopt : findFromSearchStart(e.command);
        e.liveMatch.reset();

        if (!found)
        {
            e.cursorRow = e.searchStartRow;
            e.cursorColumn = e.searchStartColumn;
            return;
        }

        e.cursorRow = found->at.line;
        e.cursorColumn = static_cast<int>(found->at.column);
        e.liveMatch = Editor::SearchMatch {found->at.line,
                                           static_cast<int>(found->at.column),
                                           static_cast<int>(found->length)};
    }

    void editSearchPattern(const std::string& key)
    {
        if (isBackspaceKey(key))
        {
            if (e.command.empty())
            {
                restoreSearchStart();
                return;
            }

            eraseLastCommandCharacter();
        }
        else if (isPrintable(key))
        {
            e.command += key;
        }

        showLiveMatch();
    }

    Outcome feedSearch(const std::string& key)
    {
        if (isCancelKey(key))
            restoreSearchStart();
        else if (key == "Enter")
            acceptSearch();
        else
            editSearchPattern(key);

        return Outcome::none;
    }

    Outcome writeFile()
    {
        e.markSaved();
        return Outcome::save;
    }

    Outcome writeFileAndClose()
    {
        e.markSaved();
        return Outcome::saveAndClose;
    }

    Outcome closeIfSaved()
    {
        if (!e.modified())
            return Outcome::close;

        e.notice = "E37: No write since last change (add ! to override)";
        return Outcome::none;
    }

    Outcome closeDiscardingChanges() { return Outcome::discard; }

    struct ExCommand
    {
        std::string_view name;
        Outcome (EditorOps::*run)();
    };

    Outcome runCommandLine()
    {
        static constexpr ExCommand exCommands[] = {
            {"w", &EditorOps::writeFile},
            {"wq", &EditorOps::writeFileAndClose},
            {"x", &EditorOps::writeFileAndClose},
            {"q", &EditorOps::closeIfSaved},
            {"q!", &EditorOps::closeDiscardingChanges},
        };

        auto ran = e.command;
        e.command.clear();
        e.currentMode = Mode::normal;

        for (const auto& exCommand: exCommands)
            if (exCommand.name == ran)
                return (this->*exCommand.run)();

        if (!ran.empty())
            e.notice = "E492: Not an editor command: " + ran;

        return Outcome::none;
    }

    void eraseCommandCharacterOrLeave()
    {
        if (e.command.empty())
            e.currentMode = Mode::normal;
        else
            eraseLastCommandCharacter();
    }

    Outcome feedCommandLine(const std::string& key)
    {
        if (isCancelKey(key))
        {
            e.currentMode = Mode::normal;
            e.command.clear();
        }
        else if (key == "Backspace")
        {
            eraseCommandCharacterOrLeave();
        }
        else if (key == "Enter")
        {
            return runCommandLine();
        }
        else if (isPrintable(key))
        {
            e.command += key;
        }

        return Outcome::none;
    }

    const std::string& currentLine() { return line(e.cursorRow); }

    void deleteBackToColumn(int from)
    {
        e.buffer.erase({e.cursorRow, static_cast<std::size_t>(from)},
                       toText(cursor()));
        e.cursorColumn = from;
    }

    int skipSpacesBackFrom(int column)
    {
        const auto& text = currentLine();

        while (column > 0
               && classOf(text[static_cast<std::size_t>(column - 1)])
                      == CharClass::space)
            --column;

        return column;
    }

    int skipClassBackFrom(int column, CharClass cls)
    {
        const auto& text = currentLine();

        while (column > 0)
        {
            auto before = previousChar(text, column);

            if (classOf(text[static_cast<std::size_t>(before)]) != cls)
                break;

            column = before;
        }

        return column;
    }

    // Where C-w deletes back to: past the spaces, then the word before them.
    int getStartOfWordBehindCursor()
    {
        auto from = skipSpacesBackFrom(e.cursorColumn);

        if (from == 0)
            return from;

        const auto& text = currentLine();
        auto cls = classOf(text[static_cast<std::size_t>(previousChar(text, from))]);
        return skipClassBackFrom(from, cls);
    }

    void deleteWordBehindCursor() { deleteBackToColumn(getStartOfWordBehindCursor()); }

    void deleteToLineStart() { deleteBackToColumn(0); }

    void joinWithPreviousLine()
    {
        if (e.cursorRow == 0)
            return;

        auto previous = e.cursorRow - 1;
        auto joinColumn = size(line(previous));
        e.buffer.erase({previous, static_cast<std::size_t>(joinColumn)},
                       toText(cursor()));
        e.cursorRow = previous;
        e.cursorColumn = joinColumn;
    }

    void deleteCharacterBehindCursor()
    {
        if (e.cursorColumn > 0)
            deleteBackToColumn(previousChar(currentLine(), e.cursorColumn));
        else
            joinWithPreviousLine();
    }

    void deleteCharacterUnderCursor()
    {
        const auto& text = currentLine();

        if (e.cursorColumn < size(text))
        {
            auto to = nextChar(text, e.cursorColumn);
            e.buffer.erase(toText(cursor()),
                           {e.cursorRow, static_cast<std::size_t>(to)});
        }
        else if (e.cursorRow + 1 < rowCount())
        {
            e.buffer.erase(toText(cursor()), {e.cursorRow + 1, 0});
        }
    }

    void leaveInsertMode()
    {
        e.currentMode = Mode::normal;
        e.cursorColumn = previousChar(currentLine(), e.cursorColumn);
        clampNormal();
    }

    void insertLineBreak() { insertRaw("\n"); }
    void insertTab() { insertRaw("    "); }
    void moveLeftInLine() { e.cursorColumn = previousChar(currentLine(), e.cursorColumn); }
    void moveRightInLine() { e.cursorColumn = nextChar(currentLine(), e.cursorColumn); }
    void moveToLineStart() { e.cursorColumn = 0; }
    void moveToLineEnd() { e.cursorColumn = size(currentLine()); }
    void moveUpOneRow() { moveVertical(-1); }
    void moveDownOneRow() { moveVertical(1); }

    struct InsertKey
    {
        std::string_view key;
        void (EditorOps::*run)();
    };

    // C-w and C-u at the start of a line join it to the one above, like Backspace.
    bool isDeletingBackInLine(const std::string& key)
    {
        return (key == "C-w" || key == "C-u") && e.cursorColumn > 0;
    }

    void runInsertKey(const std::string& key)
    {
        static constexpr InsertKey insertKeys[] = {
            {"Esc", &EditorOps::leaveInsertMode},
            {"C-c", &EditorOps::leaveInsertMode},
            {"Enter", &EditorOps::insertLineBreak},
            {"Tab", &EditorOps::insertTab},
            {"Backspace", &EditorOps::deleteCharacterBehindCursor},
            {"C-h", &EditorOps::deleteCharacterBehindCursor},
            {"C-w", &EditorOps::deleteCharacterBehindCursor},
            {"C-u", &EditorOps::deleteCharacterBehindCursor},
            {"Delete", &EditorOps::deleteCharacterUnderCursor},
            {"Left", &EditorOps::moveLeftInLine},
            {"Right", &EditorOps::moveRightInLine},
            {"Home", &EditorOps::moveToLineStart},
            {"End", &EditorOps::moveToLineEnd},
            {"Up", &EditorOps::moveUpOneRow},
            {"Down", &EditorOps::moveDownOneRow},
        };

        if (isDeletingBackInLine(key))
        {
            if (key == "C-w")
                deleteWordBehindCursor();
            else
                deleteToLineStart();

            return;
        }

        for (const auto& insertKey: insertKeys)
        {
            if (insertKey.key == key)
            {
                (this->*insertKey.run)();
                return;
            }
        }

        if (isPrintable(key))
            insertRaw(key);
    }

    Outcome feedInsert(const std::string& key)
    {
        runInsertKey(key);

        if (key != "Up" && key != "Down")
            e.desiredColumn = -1;

        return Outcome::none;
    }

    struct Range
    {
        Position from;
        Position to;
    };

    // What normal mode has been typed so far, split into its count and name.
    struct NormalCommand
    {
        int count = 1;
        bool hasCount = false;
        std::string name;
    };

    Position moveLeftOnce(Position p)
    {
        p.column = previousChar(line(p.row), p.column);
        return p;
    }

    Position moveRightOnce(Position p)
    {
        p.column = std::min(nextChar(line(p.row), p.column), size(line(p.row)));
        return p;
    }

    Position moveToStartOf(Position p)
    {
        p.column = 0;
        return p;
    }

    Position moveToFirstNonBlankOf(Position p)
    {
        p.column = firstNonBlank(line(p.row));
        return p;
    }

    Position moveToEndOf(Position p)
    {
        p.column = size(line(p.row));
        return p;
    }

    struct Motion
    {
        std::string_view name;
        Position (EditorOps::*move)(Position);
    };

    static constexpr Motion motions[] = {
        {"h", &EditorOps::moveLeftOnce},
        {"l", &EditorOps::moveRightOnce},
        {"w", &EditorOps::wordForward},
        {"b", &EditorOps::wordBack},
        {"e", &EditorOps::wordEnd},
        {"0", &EditorOps::moveToStartOf},
        {"^", &EditorOps::moveToFirstNonBlankOf},
        {"$", &EditorOps::moveToEndOf},
    };

    static std::optional<Motion> findMotion(std::string_view name)
    {
        for (const auto& motion: motions)
            if (motion.name == name)
                return motion;

        return std::nullopt;
    }

    std::optional<Position> applyMotion(std::string_view name, Position from, int count)
    {
        auto motion = findMotion(name);

        if (!motion)
            return std::nullopt;

        for (auto n = 0; n < count; ++n)
            from = (this->*motion->move)(from);

        return from;
    }

    void redoOnce()
    {
        if (auto restored = e.buffer.redo(toText(cursor())))
        {
            e.cursorRow = restored->line;
            e.cursorColumn = static_cast<int>(restored->column);
            clampNormal();
        }
    }

    // Named keys normal mode treats as motions; any other named key is ignored.
    static std::optional<std::string> mapNormalKey(const std::string& key)
    {
        struct KeyAlias
        {
            std::string_view key;
            std::string_view motion;
        };

        static constexpr KeyAlias aliases[] = {
            {"Left", "h"},
            {"Backspace", "h"},
            {"Right", "l"},
            {"Up", "k"},
            {"Down", "j"},
            {"Enter", "j"},
            {"Home", "0"},
            {"End", "$"},
        };

        for (const auto& alias: aliases)
            if (alias.key == key)
                return std::string {alias.motion};

        if (isPrintable(key))
            return key;

        return std::nullopt;
    }

    // A leading 0 is the motion, not a count.
    std::size_t getCountLength()
    {
        auto index = std::size_t {0};
        const auto& typed = e.pending;

        while (index < typed.size()
               && std::isdigit(static_cast<unsigned char>(typed[index])) != 0
               && !(index == 0 && typed[index] == '0'))
            ++index;

        return index;
    }

    // vim's own shorthands: S is cc, Y is yy.
    static std::string expandShorthand(const std::string& name)
    {
        if (name == "S")
            return "cc";

        if (name == "Y")
            return "yy";

        return name;
    }

    NormalCommand parsePending()
    {
        auto countLength = getCountLength();
        auto command = NormalCommand {};
        command.hasCount = countLength > 0;

        if (command.hasCount)
            command.count = std::clamp(
                std::stoi(e.pending.substr(0, countLength)), 1, largestCount);

        command.name = expandShorthand(e.pending.substr(countLength));
        return command;
    }

    void moveByMotion(const NormalCommand& command)
    {
        setCursor(*applyMotion(command.name, cursor(), command.count));
        clampNormal();
    }

    void moveByRows(const NormalCommand& command)
    {
        moveVertical(command.name == "j" ? command.count : -command.count);
        clampNormal();
    }

    void goToLine(const NormalCommand& command)
    {
        auto isLastLine = command.name == "G" && !command.hasCount;
        auto target = isLastLine ? rowCount() - 1 : command.count - 1;
        e.cursorRow = std::clamp(target, 0, rowCount() - 1);
        e.cursorColumn = firstNonBlank(currentLine());
    }

    void deleteCharactersUnderCursor(const NormalCommand& command)
    {
        const auto& text = currentLine();

        if (text.empty())
            return;

        pushUndo();
        auto to = advance(text, e.cursorColumn, command.count, size(text));
        erase(cursor(), {e.cursorRow, to});
        clampNormal();
    }

    void deleteCharactersBeforeCursor(const NormalCommand& command)
    {
        const auto& text = currentLine();

        if (text.empty())
            return;

        pushUndo();
        auto from = e.cursorColumn;

        for (auto n = 0; n < command.count && from > 0; ++n)
            from = previousChar(text, from);

        erase({e.cursorRow, from}, cursor());
        clampNormal();
    }

    void insertBeforeCursor(const NormalCommand&)
    {
        pushUndo();
        e.currentMode = Mode::insert;
    }

    void appendAfterCursor(const NormalCommand&)
    {
        pushUndo();

        if (!currentLine().empty())
            e.cursorColumn = nextChar(currentLine(), e.cursorColumn);

        e.currentMode = Mode::insert;
    }

    void insertAtFirstNonBlank(const NormalCommand&)
    {
        pushUndo();
        e.cursorColumn = firstNonBlank(currentLine());
        e.currentMode = Mode::insert;
    }

    void appendAtLineEnd(const NormalCommand&)
    {
        pushUndo();
        e.cursorColumn = size(currentLine());
        e.currentMode = Mode::insert;
    }

    void openLineAt(int at)
    {
        pushUndo();
        e.buffer.insertLines(at, {std::string {}});
        e.cursorRow = at;
        e.cursorColumn = 0;
        e.currentMode = Mode::insert;
    }

    void openLineBelow(const NormalCommand&) { openLineAt(e.cursorRow + 1); }
    void openLineAbove(const NormalCommand&) { openLineAt(e.cursorRow); }

    void deleteToLineEnd(const NormalCommand&)
    {
        pushUndo();
        erase(cursor(), {e.cursorRow, size(currentLine())});
        clampNormal();
    }

    void changeToLineEnd(const NormalCommand&)
    {
        pushUndo();
        erase(cursor(), {e.cursorRow, size(currentLine())});
        e.currentMode = Mode::insert;
    }

    void joinLines(const NormalCommand& command)
    {
        pushUndo();
        join(command.count);
        clampNormal();
    }

    void putAfterCursor(const NormalCommand& command)
    {
        put(true, command.count);
        clampNormal();
    }

    void putBeforeCursor(const NormalCommand& command)
    {
        put(false, command.count);
        clampNormal();
    }

    void undoChanges(const NormalCommand& command)
    {
        for (auto n = 0; n < command.count; ++n)
        {
            auto restored = e.buffer.undo(toText(cursor()));

            if (!restored)
                break;

            e.cursorRow = restored->line;
            e.cursorColumn = static_cast<int>(restored->column);
        }

        clampNormal();
    }

    static char flipCase(char c)
    {
        auto byte = static_cast<unsigned char>(c);

        if (std::isupper(byte) != 0)
            return static_cast<char>(std::tolower(byte));

        if (std::islower(byte) != 0)
            return static_cast<char>(std::toupper(byte));

        return c;
    }

    void flipCaseUnderCursor(const NormalCommand& command)
    {
        if (currentLine().empty())
            return;

        pushUndo();
        auto changed = currentLine();

        for (auto n = 0; n < command.count && e.cursorColumn < size(changed); ++n)
        {
            auto& c = changed[static_cast<std::size_t>(e.cursorColumn)];
            c = flipCase(c);
            e.cursorColumn = nextChar(changed, e.cursorColumn);
        }

        e.buffer.replaceLine(e.cursorRow, std::move(changed));
        clampNormal();
    }

    void openSearchPrompt(bool forward)
    {
        e.currentMode = Mode::search;
        e.searchForward = forward;
        e.searchStartRow = e.cursorRow;
        e.searchStartColumn = e.cursorColumn;
        e.command.clear();
        e.liveMatch.reset();
    }

    void searchForward(const NormalCommand&) { openSearchPrompt(true); }
    void searchBackward(const NormalCommand&) { openSearchPrompt(false); }

    void repeatSearch(const NormalCommand& command)
    {
        if (e.lastPattern.empty())
        {
            e.notice = noPreviousPatternNotice;
            return;
        }

        auto forward = command.name == "n" ? e.lastForward : !e.lastForward;

        for (auto n = 0; n < command.count; ++n)
        {
            auto found =
                ::text::search(e.buffer.lines(), e.lastPattern, toText(cursor()), forward);

            if (!jumpToMatch(found, e.lastPattern, forward))
                break;
        }
    }

    void openCommandLine(const NormalCommand&)
    {
        e.currentMode = Mode::command;
        e.command.clear();
    }

    void replaceCharacter(const std::string& replacement)
    {
        const auto& text = currentLine();

        if (text.empty() || !isPrintable(replacement))
            return;

        pushUndo();
        auto to = nextChar(text, e.cursorColumn);
        auto changed = text;
        changed.replace(static_cast<std::size_t>(e.cursorColumn),
                        static_cast<std::size_t>(to - e.cursorColumn),
                        replacement);
        e.buffer.replaceLine(e.cursorRow, std::move(changed));
    }

    void deleteLinesWithUndo(int count)
    {
        pushUndo();
        deleteLines(count, true);
    }

    void changeLines(int count)
    {
        pushUndo();
        yankLines(count);
        auto last = std::min(rowCount(), e.cursorRow + count);
        e.buffer.eraseLines(e.cursorRow + 1, last - e.cursorRow - 1);
        e.buffer.replaceLine(e.cursorRow, {});
        e.cursorColumn = 0;
        e.currentMode = Mode::insert;
    }

    void deleteRange(Range range)
    {
        pushUndo();
        erase(range.from, range.to);
        clampNormal();
    }

    void changeRange(Range range)
    {
        pushUndo();
        erase(range.from, range.to);
        e.currentMode = Mode::insert;
    }

    void yankRange(Range range)
    {
        e.yank = extract(range.from, range.to);
        e.yankLinewise = false;
    }

    struct Operator
    {
        char name;
        void (EditorOps::*onLines)(int count);
        void (EditorOps::*onRange)(Range range);
    };

    static constexpr Operator operators[] = {
        {'d', &EditorOps::deleteLinesWithUndo, &EditorOps::deleteRange},
        {'c', &EditorOps::changeLines, &EditorOps::changeRange},
        {'y', &EditorOps::yankLines, &EditorOps::yankRange},
    };

    static std::optional<Operator> findOperator(const std::string& name)
    {
        if (name.size() < 2)
            return std::nullopt;

        for (const auto& op: operators)
            if (op.name == name[0])
                return op;

        return std::nullopt;
    }

    std::optional<Range> getTextObjectRange(const std::string& target)
    {
        if (target != "iw" && target != "aw")
            return std::nullopt;

        auto [start, end] = target == "iw" ? innerWord() : aroundWord();
        return Range {{e.cursorRow, start}, {e.cursorRow, end}};
    }

    // cw acts like ce on a word, as in vim.
    std::string getOperatorMotionName(char op, const std::string& target)
    {
        if (op == 'c' && target == "w" && classAt(cursor()) != CharClass::space)
            return "e";

        return target;
    }

    std::optional<Range> getMotionRange(char op, const std::string& target, int count)
    {
        auto from = cursor();
        auto name = getOperatorMotionName(op, target);
        auto moved = applyMotion(name, from, count);

        if (!moved)
            return std::nullopt;

        auto to = *moved;

        if (name == "e")
            to.column = nextChar(line(to.row), to.column); // inclusive

        if (name == "w" && to.row > from.row)
            to = {from.row, size(line(from.row))};

        if (to < from)
            std::swap(from, to);

        return Range {from, to};
    }

    // d c y followed by themselves, a text object or a motion.
    void applyOperator(const Operator& op, const NormalCommand& command)
    {
        auto target = command.name.substr(1);

        if (target.size() == 1 && target[0] == op.name)
        {
            (this->*op.onLines)(command.count);
            return;
        }

        auto range = getTextObjectRange(target);

        if (!range)
            range = getMotionRange(op.name, target, command.count);

        if (range)
            (this->*op.onRange)(*range);
    }

    using NormalHandler = void (EditorOps::*)(const NormalCommand&);

    struct NormalKey
    {
        std::string_view name;
        NormalHandler run;
    };

    static std::optional<NormalHandler> findNormalHandler(const std::string& name)
    {
        static constexpr NormalKey normalKeys[] = {
            {"gg", &EditorOps::goToLine},
            {"G", &EditorOps::goToLine},
            {"x", &EditorOps::deleteCharactersUnderCursor},
            {"X", &EditorOps::deleteCharactersBeforeCursor},
            {"i", &EditorOps::insertBeforeCursor},
            {"a", &EditorOps::appendAfterCursor},
            {"I", &EditorOps::insertAtFirstNonBlank},
            {"A", &EditorOps::appendAtLineEnd},
            {"o", &EditorOps::openLineBelow},
            {"O", &EditorOps::openLineAbove},
            {"D", &EditorOps::deleteToLineEnd},
            {"C", &EditorOps::changeToLineEnd},
            {"J", &EditorOps::joinLines},
            {"p", &EditorOps::putAfterCursor},
            {"P", &EditorOps::putBeforeCursor},
            {"u", &EditorOps::undoChanges},
            {"~", &EditorOps::flipCaseUnderCursor},
            {"/", &EditorOps::searchForward},
            {"?", &EditorOps::searchBackward},
            {"n", &EditorOps::repeatSearch},
            {"N", &EditorOps::repeatSearch},
            {":", &EditorOps::openCommandLine},
        };

        for (const auto& normalKey: normalKeys)
            if (normalKey.name == name)
                return normalKey.run;

        return std::nullopt;
    }

    // A prefix that waits for the rest of its command.
    static bool isWaitingForMoreKeys(const std::string& name)
    {
        static constexpr std::string_view prefixes[] = {
            "g", "d", "c", "y", "r", "di", "da", "ci", "ca", "yi", "ya"};

        return std::find(std::begin(prefixes), std::end(prefixes), name)
               != std::end(prefixes);
    }

    static bool isRowMotion(const std::string& name)
    {
        return name == "j" || name == "k";
    }

    void runNormalCommand(const NormalCommand& command)
    {
        const auto& name = command.name;

        if (!isRowMotion(name))
            e.desiredColumn = -1;

        if (isWaitingForMoreKeys(name))
            return;

        e.pending.clear();

        if (findMotion(name))
            moveByMotion(command);
        else if (isRowMotion(name))
            moveByRows(command);
        else if (auto handler = findNormalHandler(name))
            (this->**handler)(command);
        else if (name.size() > 1 && name[0] == 'r')
            replaceCharacter(name.substr(1));
        else if (auto op = findOperator(name))
            applyOperator(*op, command);
    }

    Outcome feedNormal(const std::string& key)
    {
        if (isCancelKey(key))
        {
            e.pending.clear();
            return Outcome::none;
        }

        if (key == "C-r")
        {
            e.pending.clear();
            redoOnce();
            return Outcome::none;
        }

        auto mapped = mapNormalKey(key);

        if (!mapped)
        {
            e.pending.clear();
            return Outcome::none;
        }

        e.pending += *mapped;
        auto command = parsePending();

        if (!command.name.empty())
            runNormalCommand(command);

        return Outcome::none;
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

        if (column >= row.end && row.end > row.start
            && (!lastOfLine || e.currentMode != Mode::insert))
            column = previousChar(text, row.end);

        e.cursorRow = row.line;
        e.cursorColumn = column;
    }
};

Editor::Editor(std::string text)
{
    setText(std::move(text));
}

void Editor::setText(std::string text)
{
    buffer.setText(text);
    cursorRow = 0;
    cursorColumn = 0;
    currentMode = Mode::normal;
    command.clear();
    pending.clear();
    desiredColumn = -1;
    savedText = this->text();
}

std::string Editor::text() const
{
    return buffer.text();
}

void Editor::insertText(std::string_view text)
{
    auto ops = EditorOps {*this};
    ops.pushUndo();

    if (currentMode == Mode::normal && !buffer.line(cursorRow).empty())
        cursorColumn = nextChar(buffer.line(cursorRow), cursorColumn);

    ops.insertRaw(text);
    ops.clampNormal();
    desiredColumn = -1;
}

bool Editor::modified() const
{
    return text() != savedText;
}

void Editor::markSaved()
{
    savedText = text();
}

Mode Editor::mode() const
{
    return currentMode;
}

bool Editor::hasPendingKeys() const
{
    return !pending.empty();
}

void Editor::setCursor(int row, int column)
{
    auto ops = EditorOps {*this};
    ops.setCursor({row, column});
    ops.clampNormal();
    desiredColumn = -1;
}

const std::vector<std::string>& Editor::lines() const
{
    return buffer.lines();
}

int Editor::row() const
{
    return cursorRow;
}

int Editor::column() const
{
    return cursorColumn;
}

std::string Editor::commandLine() const
{
    return command;
}

char Editor::promptChar() const
{
    if (currentMode == Mode::command)
        return ':';

    if (currentMode == Mode::search)
        return searchForward ? '/' : '?';

    return 0;
}

std::optional<Editor::SearchMatch> Editor::searchMatch() const
{
    return currentMode == Mode::search ? liveMatch : std::nullopt;
}

std::string Editor::message() const
{
    return notice;
}

void Editor::setWrapWidth(int columns)
{
    wrapWidth = std::max(0, columns);
}

std::vector<Editor::VisualRow> Editor::visualRows() const
{
    auto rows = std::vector<VisualRow> {};

    const auto& all = buffer.lines();

    for (std::size_t l = 0; l < all.size(); ++l)
    {
        const auto& text = all[l];
        auto lineIndex = static_cast<int>(l);
        auto start = 0;

        if (wrapWidth > 0)
        {
            while (codepoints(text, start, size(text)) > wrapWidth)
            {
                auto cut = advance(text, start, wrapWidth, size(text));
                auto space = text.substr(0, static_cast<std::size_t>(cut))
                                 .rfind(' ', static_cast<std::size_t>(cut - 1));
                auto end = space != std::string::npos
                                   && static_cast<int>(space) >= start
                                   && static_cast<int>(space) + 1 > start
                               ? static_cast<int>(space) + 1
                               : cut;
                rows.push_back({lineIndex, start, end});
                start = end;
            }
        }

        rows.push_back({lineIndex, start, size(text)});
    }

    return rows;
}

int Editor::cursorVisualRow() const
{
    auto rows = visualRows();

    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& row = rows[i];

        if (row.line != cursorRow)
            continue;

        auto lastOfLine = i + 1 == rows.size() || rows[i + 1].line != cursorRow;

        if (cursorColumn < row.end || lastOfLine)
            return static_cast<int>(i);
    }

    return 0;
}

int Editor::cursorVisualColumn() const
{
    auto rows = visualRows();
    const auto& row = rows[static_cast<std::size_t>(cursorVisualRow())];
    return codepoints(buffer.line(cursorRow), row.start, cursorColumn);
}

Outcome Editor::feed(const std::string& key)
{
    notice.clear();
    auto ops = EditorOps {*this};

    if (key == "C-s")
    {
        currentMode = Mode::normal;
        command.clear();
        pending.clear();
        ops.clampNormal();
        markSaved();
        return Outcome::saveAndClose;
    }

    if (key == "C-v")
        return Outcome::paste;

    if (currentMode == Mode::search)
        return ops.feedSearch(key);

    if (currentMode == Mode::command)
        return ops.feedCommandLine(key);

    if (currentMode == Mode::insert)
        return ops.feedInsert(key);

    return ops.feedNormal(key);
}
} // namespace vim
