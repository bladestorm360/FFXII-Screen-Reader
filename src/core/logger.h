#pragma once
#include <string>

namespace Log {

void Init(const std::string& gameDir);
void Shutdown();
void Write(const char* category, const char* message);
const std::string& GetGameDir();

uint64_t GetStartTick();

} // namespace Log
