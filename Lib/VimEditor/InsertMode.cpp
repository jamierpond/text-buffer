#include "EditorOps.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace vim
{
struct InsertMode : EditorOps
{
    void deleteBackToColumn(int from)
    {
        e.buffer.erase({e.cursorRow, static_cast<std::size_t>(from)},
                       toText(cursor()));
        e.cursorColumn = from;
    }

    // Where C-w deletes back to: past the spaces, then the word before them.
    int getStartOfWordBehindCursor()
    {
        const auto& text = currentLine();
        auto from = skipClassBack(text, e.cursorColumn, CharClass::space);

        if (from == 0)
            return from;

        auto cls = classOf(text[static_cast<std::size_t>(previousChar(text, from))]);
        return skipClassBack(text, from, cls);
    }

    // At the start of a line, C-w and C-u join it to the one above.
    void deleteWordBehindCursor()
    {
        if (e.cursorColumn == 0)
            joinWithPreviousLine();
        else
            deleteBackToColumn(getStartOfWordBehindCursor());
    }

    void deleteToLineStart()
    {
        if (e.cursorColumn == 0)
            joinWithPreviousLine();
        else
            deleteBackToColumn(0);
    }

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
        void (*run)(InsertMode&);
    };

    void runInsertKey(const std::string& key)
    {
        static constexpr InsertKey insertKeys[] = {
            {"Esc", [](InsertMode& mode) { mode.leaveInsertMode(); }},
            {"C-c", [](InsertMode& mode) { mode.leaveInsertMode(); }},
            {"Enter", [](InsertMode& mode) { mode.insertLineBreak(); }},
            {"Tab", [](InsertMode& mode) { mode.insertTab(); }},
            {"Backspace", [](InsertMode& mode) { mode.deleteCharacterBehindCursor(); }},
            {"C-h", [](InsertMode& mode) { mode.deleteCharacterBehindCursor(); }},
            {"C-w", [](InsertMode& mode) { mode.deleteWordBehindCursor(); }},
            {"C-u", [](InsertMode& mode) { mode.deleteToLineStart(); }},
            {"Delete", [](InsertMode& mode) { mode.deleteCharacterUnderCursor(); }},
            {"Left", [](InsertMode& mode) { mode.moveLeftInLine(); }},
            {"Right", [](InsertMode& mode) { mode.moveRightInLine(); }},
            {"Home", [](InsertMode& mode) { mode.moveToLineStart(); }},
            {"End", [](InsertMode& mode) { mode.moveToLineEnd(); }},
            {"Up", [](InsertMode& mode) { mode.moveUpOneRow(); }},
            {"Down", [](InsertMode& mode) { mode.moveDownOneRow(); }},
        };

        for (const auto& insertKey: insertKeys)
            if (insertKey.key == key)
                return insertKey.run(*this);

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
};

Outcome feedInsertMode(Editor& editor, const std::string& key)
{
    return InsertMode {{editor}}.feedInsert(key);
}
} // namespace vim
