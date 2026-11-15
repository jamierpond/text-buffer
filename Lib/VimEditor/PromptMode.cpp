#include "EditorOps.h"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace vim
{
struct PromptMode : EditorOps
{
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
        Outcome (PromptMode::*run)();
    };

    Outcome runCommandLine()
    {
        static constexpr ExCommand exCommands[] = {
            {"w", &PromptMode::writeFile},
            {"wq", &PromptMode::writeFileAndClose},
            {"x", &PromptMode::writeFileAndClose},
            {"q", &PromptMode::closeIfSaved},
            {"q!", &PromptMode::closeDiscardingChanges},
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

    Outcome feedPrompt(const std::string& key)
    {
        if (e.currentMode == Mode::search)
            return feedSearch(key);

        return feedCommandLine(key);
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
};

Outcome feedPromptMode(Editor& editor, const std::string& key)
{
    return PromptMode {{editor}}.feedPrompt(key);
}
} // namespace vim
