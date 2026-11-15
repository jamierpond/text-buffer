#pragma once

#include <string>

namespace vim
{
enum class CharClass
{
    space,
    word,
    punctuation
};

bool isContinuation(char c);
int size(const std::string& text);
int nextChar(const std::string& line, int at);
int previousChar(const std::string& line, int at);
int lastCharStart(const std::string& line);
int codepoints(const std::string& line, int from, int to);
int advance(const std::string& line, int from, int count, int limit);
bool isWordByte(char c);
CharClass classOf(char c);
bool isBlank(char c);
int countLeadingBlanks(const std::string& line);
int firstNonBlank(const std::string& line);
int skipClassBack(const std::string& text, int column, CharClass cls);
int skipClassForward(const std::string& text, int column, CharClass cls);
int getWrapEnd(const std::string& text, int start, int cut);
bool isCancelKey(const std::string& key);
bool isBackspaceKey(const std::string& key);
bool isPrintable(const std::string& key);

inline constexpr auto noPreviousPatternNotice = "E35: No previous regular expression";
} // namespace vim
