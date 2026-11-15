#include <VimEditor.h>

#include <NanoTest/NanoTest.h>

#include <initializer_list>
#include <string>

using namespace nano;
using namespace vim;

namespace
{
void type(Editor& editor, std::initializer_list<std::string> keys)
{
    for (const auto& key: keys)
        editor.feed(key);
}

bool isAt(const Editor& editor, int row, int column)
{
    return editor.row() == row && editor.column() == column;
}
} // namespace

auto tNormalArrows = test("VimEditorKeys/arrows, Home, End, Enter, Backspace move in normal mode") = []
{
    auto editor = Editor {"abc\ndef\nghi"};
    editor.feed("Right");
    check(isAt(editor, 0, 1));
    editor.feed("End");
    check(isAt(editor, 0, 2));
    editor.feed("Home");
    check(isAt(editor, 0, 0));
    editor.feed("Down");
    check(isAt(editor, 1, 0));
    editor.feed("Enter");
    check(isAt(editor, 2, 0));
    editor.feed("Up");
    check(isAt(editor, 1, 0));
    type(editor, {"l", "l", "Backspace"});
    check(isAt(editor, 1, 1));
    editor.feed("Left");
    check(isAt(editor, 1, 0));
    type(editor, {"l", "h"});
    check(isAt(editor, 1, 0));
    editor.feed("k");
    check(isAt(editor, 0, 0));
};

auto tNormalUnknownKeyClearsPending = test("VimEditorKeys/a named key or Esc drops a half-typed command") = []
{
    auto editor = Editor {"abc\ndef"};
    type(editor, {"d", "Tab", "j", "k"});
    check(editor.text() == "abc\ndef");
    type(editor, {"d", "Esc", "j"});
    check(editor.text() == "abc\ndef");
    check(editor.row() == 1);
    type(editor, {"q"});
    check(editor.text() == "abc\ndef");
};

auto tZeroIsAMotion = test("VimEditorKeys/0 moves, 10 is a count") = []
{
    auto editor = Editor {"abcdefghijklmn"};
    type(editor, {"$", "0"});
    check(editor.column() == 0);
    type(editor, {"1", "0", "l"});
    check(editor.column() == 10);
    type(editor, {"1", "0", "0", "0", "0", "0", "0", "h"});
    check(editor.column() == 0);
};

auto tCountG = test("VimEditorKeys/a count before G and gg picks the line") = []
{
    auto editor = Editor {"a\n  b\nc"};
    type(editor, {"2", "g", "g"});
    check(isAt(editor, 1, 2));
    type(editor, {"9", "G"});
    check(editor.row() == 2);
    type(editor, {"g", "g"});
    check(editor.row() == 0);
};

auto tBigX = test("VimEditorKeys/X deletes before the cursor") = []
{
    auto editor = Editor {"abcd"};
    type(editor, {"$", "2", "X"});
    check(editor.text() == "ad");
    check(editor.column() == 1);
    type(editor, {"0", "X"});
    check(editor.text() == "ad");
    type(editor, {"p"});
    check(editor.text() == "abcd");

    auto empty = Editor {};
    type(empty, {"x", "X", "~", "r", "a"});
    check(empty.text().empty());
    check(!empty.modified());
};

auto tTilde = test("VimEditorKeys/~ flips case and moves on") = []
{
    auto editor = Editor {"aB-c"};
    type(editor, {"3", "~"});
    check(editor.text() == "Ab-c");
    check(editor.column() == 3);
    type(editor, {"5", "~"});
    check(editor.text() == "Ab-C");
    check(editor.column() == 3);
    editor.feed("u");
    check(editor.text() == "Ab-c");
};

auto tAppend = test("VimEditorKeys/a appends after the cursor, I on a blank line") = []
{
    auto editor = Editor {"ac"};
    type(editor, {"a", "b", "Esc"});
    check(editor.text() == "abc");

    auto blank = Editor {"   "};
    type(blank, {"I", "x", "Esc"});
    check(blank.text() == "  x ");

    auto empty = Editor {};
    type(empty, {"a", "x", "Esc"});
    check(empty.text() == "x");
};

auto tPutCounts = test("VimEditorKeys/counted p and P, and put with nothing yanked") = []
{
    auto editor = Editor {"ab"};
    editor.feed("p");
    check(!editor.modified());
    type(editor, {"y", "l", "3", "p"});
    check(editor.text() == "aaaab");
    check(editor.column() == 3);
    type(editor, {"0", "P"});
    check(editor.text() == "aaaaab");

    auto lines = Editor {"one\n  two"};
    type(lines, {"j", "y", "y", "k", "2", "P"});
    check(lines.text() == "  two\n  two\none\n  two");
    check(isAt(lines, 0, 2));
};

auto tYankMotions = test("VimEditorKeys/y with motions and text objects") = []
{
    auto editor = Editor {"foo bar baz"};
    type(editor, {"y", "w", "$", "p"});
    check(editor.text() == "foo bar bazfoo ");
    check(editor.row() == 0);

    auto words = Editor {"foo bar baz"};
    type(words, {"w", "y", "i", "w", "0", "P"});
    check(words.text() == "barfoo bar baz");

    auto around = Editor {"foo bar baz"};
    type(around, {"w", "y", "a", "w", "0", "P"});
    check(around.text() == "bar foo bar baz");

    auto back = Editor {"foo bar"};
    type(back, {"$", "y", "b", "0", "P"});
    check(back.text() == "bafoo bar");

    auto ends = Editor {"foo bar"};
    type(ends, {"y", "e", "$", "p"});
    check(ends.text() == "foo barfoo");
};

auto tDeleteMotions = test("VimEditorKeys/d with motions, counts and text objects") = []
{
    auto editor = Editor {"one two three four"};
    type(editor, {"2", "d", "w"});
    check(editor.text() == "three four");
    type(editor, {"w", "d", "b"});
    check(editor.text() == "four");
    type(editor, {"d", "$"});
    check(editor.text().empty());

    auto around = Editor {"one two"};
    type(around, {"w", "d", "a", "w"});
    check(around.text() == "one");

    auto lastWord = Editor {"one\ntwo"};
    type(lastWord, {"d", "w"});
    check(lastWord.text() == "\ntwo");

    auto caret = Editor {"  abc"};
    type(caret, {"$", "d", "^"});
    check(caret.text() == "  c");
    type(caret, {"d", "0"});
    check(caret.text() == "c");

    auto unknown = Editor {"abc"};
    type(unknown, {"d", "z", "x"});
    check(unknown.text() == "bc");

    auto badObject = Editor {"abc"};
    type(badObject, {"d", "i", "z"});
    check(badObject.text() == "abc");
    check(!badObject.modified());
};

auto tChangeWordOnSpace = test("VimEditorKeys/cw on a space changes the space like dw") = []
{
    auto editor = Editor {"a  b"};
    type(editor, {"l", "c", "w", "X", "Esc"});
    check(editor.text() == "aXb");

    auto across = Editor {"ab cd"};
    type(across, {"c", "a", "w", "Z", "Esc"});
    check(across.text() == "Zcd");
};

auto tCountedChangeLine = test("VimEditorKeys/2cc and counted dd past the end") = []
{
    auto editor = Editor {"a\nb\nc"};
    type(editor, {"2", "c", "c", "x", "Esc"});
    check(editor.text() == "x\nc");
    check(editor.mode() == Mode::normal);
    type(editor, {"p"});
    check(editor.text() == "x\na\nb\nc");

    auto all = Editor {"a\nb"};
    type(all, {"5", "d", "d"});
    check(all.text().empty());
    check(isAt(all, 0, 0));
};

auto tCountedJoin = test("VimEditorKeys/3J joins three lines, trimming spaces") = []
{
    auto editor = Editor {"a  \n   b\n\nc"};
    type(editor, {"3", "J"});
    check(editor.text() == "a b\nc");
    check(editor.column() == 2);
    type(editor, {"J", "J"});
    check(editor.text() == "a b c");
};

auto tReplaceNamedKey = test("VimEditorKeys/r then a named key does nothing") = []
{
    auto editor = Editor {"abc"};
    type(editor, {"r", "Tab", "x"});
    check(editor.text() == "bc");
    type(editor, {"r", "\xc3\xa9"});
    check(editor.text() == "\xc3\xa9" "c");
};

auto tCountedUndo = test("VimEditorKeys/2u undoes two changes, extra undo stops") = []
{
    auto editor = Editor {"abc"};
    type(editor, {"x", "x", "x"});
    check(editor.text().empty());
    type(editor, {"2", "u"});
    check(editor.text() == "bc");
    type(editor, {"9", "u"});
    check(editor.text() == "abc");
    type(editor, {"C-r", "C-r", "C-r", "C-r"});
    check(editor.text().empty());
};

auto tSearchRepeatWithoutPattern = test("VimEditorKeys/n with no pattern and empty Enter say so") = []
{
    auto editor = Editor {"abc"};
    editor.feed("n");
    check(editor.message() == "E35: No previous regular expression");
    type(editor, {"/", "Enter"});
    check(editor.message() == "E35: No previous regular expression");
    check(editor.mode() == Mode::normal);
    editor.feed("l");
    check(editor.message().empty());
};

auto tSearchEditing = test("VimEditorKeys/Backspace edits the pattern and leaves when empty") = []
{
    auto editor = Editor {"abc xyz"};
    type(editor, {"/", "x", "q"});
    check(!editor.searchMatch());
    check(editor.column() == 0);
    editor.feed("C-h");
    check(editor.searchMatch().has_value());
    check(editor.column() == 4);
    type(editor, {"Backspace", "Tab"});
    check(editor.commandLine().empty());
    check(editor.column() == 0);
    editor.feed("Backspace");
    check(editor.mode() == Mode::normal);
    check(editor.promptChar() == 0);

    type(editor, {"?", "C-c"});
    check(editor.mode() == Mode::normal);
};

auto tSearchCount = test("VimEditorKeys/2n jumps two matches") = []
{
    auto editor = Editor {"a1 a2 a3 a4"};
    type(editor, {"/", "a", "Enter"});
    check(editor.column() == 3);
    type(editor, {"2", "n"});
    check(editor.column() == 9);
    type(editor, {"3", "N"});
    check(editor.column() == 0);
    type(editor, {"/", "z", "Enter", "3", "n"});
    check(editor.message() == "E486: Pattern not found: z");
    check(editor.column() == 0);
};

auto tCommandLineEditing = test("VimEditorKeys/the command line takes text and Esc leaves") = []
{
    auto editor = Editor {"a"};
    type(editor, {":", "w", "x", "Backspace", "Tab"});
    check(editor.commandLine() == "w");
    check(editor.promptChar() == ':');
    editor.feed("C-c");
    check(editor.mode() == Mode::normal);
    type(editor, {":", "Enter"});
    check(editor.message().empty());
    check(editor.mode() == Mode::normal);
};

auto tInsertMovement = test("VimEditorKeys/arrows, Home, End and Delete in insert mode") = []
{
    auto editor = Editor {"abc\ndef"};
    type(editor, {"i", "End"});
    check(editor.column() == 3);
    editor.feed("Left");
    check(editor.column() == 2);
    editor.feed("Right");
    check(editor.column() == 3);
    editor.feed("Delete");
    check(editor.text() == "abcdef");
    type(editor, {"Home", "Delete"});
    check(editor.text() == "bcdef");
    type(editor, {"Tab"});
    check(editor.text() == "    bcdef");
    check(editor.column() == 4);
    type(editor, {"End", "Delete", "C-w", "C-u", "C-v"});
    check(editor.text().empty());
    check(editor.mode() == Mode::insert);
};

auto tInsertVertical = test("VimEditorKeys/Up and Down in insert mode keep the column") = []
{
    auto editor = Editor {"abcd\nx\nabcd"};
    type(editor, {"A", "Down"});
    check(isAt(editor, 1, 1));
    editor.feed("Down");
    check(isAt(editor, 2, 4));
    editor.feed("Up");
    check(isAt(editor, 1, 1));
    type(editor, {"Left", "Down"});
    check(isAt(editor, 2, 0));
};

auto tInsertTextInNormalMode = test("VimEditorKeys/insertText in normal mode goes after the cursor") = []
{
    auto editor = Editor {"ac"};
    editor.insertText("b");
    check(editor.text() == "abc");
    check(editor.mode() == Mode::normal);
    check(editor.column() == 2);

    auto empty = Editor {};
    empty.insertText("xy");
    check(empty.text() == "xy");
    check(empty.column() == 1);
};

auto tSaveKeyLeavesModes = test("VimEditorKeys/C-s from insert and command modes") = []
{
    auto editor = Editor {"ab"};
    type(editor, {"A", "c"});
    check(editor.feed("C-s") == Outcome::saveAndClose);
    check(editor.mode() == Mode::normal);
    check(editor.column() == 2);
    check(!editor.modified());

    type(editor, {":", "q"});
    check(editor.feed("C-s") == Outcome::saveAndClose);
    check(editor.commandLine().empty());
    check(editor.mode() == Mode::normal);
};

auto tWordMotionEdges = test("VimEditorKeys/w b e at the edges of the text") = []
{
    auto editor = Editor {"ab\n\ncd  "};
    editor.feed("w");
    check(isAt(editor, 1, 0));
    editor.feed("w");
    check(isAt(editor, 2, 0));
    type(editor, {"w", "w"});
    check(isAt(editor, 2, 3));
    type(editor, {"e", "e"});
    check(isAt(editor, 2, 3));
    type(editor, {"g", "g", "b", "e", "e"});
    check(isAt(editor, 2, 1));
    type(editor, {"5", "b"});
    check(isAt(editor, 0, 0));
};

auto tYankAcrossLines = test("VimEditorKeys/yb from a line start yanks across the break") = []
{
    auto editor = Editor {"ab cd\nef"};
    type(editor, {"j", "y", "b", "P"});
    check(editor.text() == "ab cd\ncd\nef");
};
