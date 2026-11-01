#pragma once
#include <string>
namespace trafficprofile {
void Allow(bool allowed);
std::wstring Routes();
std::wstring File(const wchar_t* name);
}
