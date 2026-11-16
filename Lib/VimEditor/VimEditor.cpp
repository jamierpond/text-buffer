#include "VimEditor.h"

#include "EditorOps.h"

#include <algorithm>

namespace vim
{
Editor::Editor(std::string_view text)
{
    setText(text);
}

void Editor::setText(std::string_view text)
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
                auto end = getWrapEnd(text, start, cut);
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

    if (currentMode == Mode::search || currentMode == Mode::command)
        return feedPromptMode(*this, key);

    if (currentMode == Mode::insert)
        return feedInsertMode(*this, key);

    return feedNormalMode(*this, key);
}
} // namespace vim
