#pragma once
#include <string>

namespace usage {
bool secureLoad(std::string& json, std::string& error);
bool secureSave(const std::string& json, std::string& error);
bool secureClear(std::string& error);
}  // namespace usage
