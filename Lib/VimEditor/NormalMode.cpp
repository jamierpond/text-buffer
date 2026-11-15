#include "EditorOps.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace vim
{
constexpr auto largestCount = 99999;

struct NormalMode : EditorOps
{
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
        Position (NormalMode::*move)(Position);
    };

    static std::optional<Motion> findMotion(std::string_view name)
    {
        static constexpr Motion motions[] = {
            {"h", &NormalMode::moveLeftOnce},
            {"l", &NormalMode::moveRightOnce},
            {"w", &NormalMode::wordForward},
            {"b", &NormalMode::wordBack},
            {"e", &NormalMode::wordEnd},
            {"0", &NormalMode::moveToStartOf},
            {"^", &NormalMode::moveToFirstNonBlankOf},
            {"$", &NormalMode::moveToEndOf},
        };

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
    static bool isCountDigit(const std::string& typed, std::size_t index)
    {
        auto c = typed[index];

        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
            return false;

        return index > 0 || c != '0';
    }

    std::size_t getCountLength()
    {
        auto index = std::size_t {0};
        const auto& typed = e.pending;

        while (index < typed.size() && isCountDigit(typed, index))
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
        void (NormalMode::*onLines)(int count);
        void (NormalMode::*onRange)(Range range);
    };

    static std::optional<Operator> findOperator(const std::string& name)
    {
        if (name.size() < 2)
            return std::nullopt;

        static constexpr Operator operators[] = {
            {'d', &NormalMode::deleteLinesWithUndo, &NormalMode::deleteRange},
            {'c', &NormalMode::changeLines, &NormalMode::changeRange},
            {'y', &NormalMode::yankLines, &NormalMode::yankRange},
        };

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
        if (op != 'c' || target != "w")
            return target;

        if (classAt(cursor()) == CharClass::space)
            return target;

        return "e";
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

    using NormalHandler = void (NormalMode::*)(const NormalCommand&);

    struct NormalKey
    {
        std::string_view name;
        NormalHandler run;
    };

    static std::optional<NormalHandler> findNormalHandler(const std::string& name)
    {
        static constexpr NormalKey normalKeys[] = {
            {"gg", &NormalMode::goToLine},
            {"G", &NormalMode::goToLine},
            {"x", &NormalMode::deleteCharactersUnderCursor},
            {"X", &NormalMode::deleteCharactersBeforeCursor},
            {"i", &NormalMode::insertBeforeCursor},
            {"a", &NormalMode::appendAfterCursor},
            {"I", &NormalMode::insertAtFirstNonBlank},
            {"A", &NormalMode::appendAtLineEnd},
            {"o", &NormalMode::openLineBelow},
            {"O", &NormalMode::openLineAbove},
            {"D", &NormalMode::deleteToLineEnd},
            {"C", &NormalMode::changeToLineEnd},
            {"J", &NormalMode::joinLines},
            {"p", &NormalMode::putAfterCursor},
            {"P", &NormalMode::putBeforeCursor},
            {"u", &NormalMode::undoChanges},
            {"~", &NormalMode::flipCaseUnderCursor},
            {"/", &NormalMode::searchForward},
            {"?", &NormalMode::searchBackward},
            {"n", &NormalMode::repeatSearch},
            {"N", &NormalMode::repeatSearch},
            {":", &NormalMode::openCommandLine},
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
        dispatchNormalCommand(command);
    }

    void runReplaceOrOperator(const NormalCommand& command)
    {
        const auto& name = command.name;

        if (name.size() > 1 && name[0] == 'r')
            replaceCharacter(name.substr(1));
        else if (auto op = findOperator(name))
            applyOperator(*op, command);
    }

    void dispatchNormalCommand(const NormalCommand& command)
    {
        const auto& name = command.name;

        if (findMotion(name))
            moveByMotion(command);
        else if (isRowMotion(name))
            moveByRows(command);
        else if (auto handler = findNormalHandler(name))
            (this->**handler)(command);
        else
            runReplaceOrOperator(command);
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
};

Outcome feedNormalMode(Editor& editor, const std::string& key)
{
    return NormalMode {{editor}}.feedNormal(key);
}
} // namespace vim
