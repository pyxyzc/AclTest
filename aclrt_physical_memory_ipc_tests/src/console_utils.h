#pragma once

#include <iostream>
#include <string>

namespace acltest {
namespace internal {

namespace style {
inline constexpr const char* RED = "\033[31m";
inline constexpr const char* GREEN = "\033[32m";
inline constexpr const char* BLUE = "\033[94m";
inline constexpr const char* YELLOW = "\033[93m";
inline constexpr const char* RESET = "\033[0m";
}  // namespace style

inline void PrintColor(const std::string& msg, const char* color)
{
    std::cout << color << msg << style::RESET << "\n";
}

inline void PrintRed(const std::string& msg)
{
    PrintColor(msg, style::RED);
}

inline void PrintGreen(const std::string& msg)
{
    PrintColor(msg, style::GREEN);
}

inline void PrintBlue(const std::string& msg)
{
    PrintColor(msg, style::BLUE);
}

inline void PrintYellow(const std::string& msg)
{
    PrintColor(msg, style::YELLOW);
}

}  // namespace internal
}  // namespace acltest
