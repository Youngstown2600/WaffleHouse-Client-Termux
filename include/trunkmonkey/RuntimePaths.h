#pragma once
#include <filesystem>

namespace trunkmonkey::runtime {
std::filesystem::path configDir();
std::filesystem::path stateDir();
std::filesystem::path settingsPath();
std::filesystem::path logPath();
std::filesystem::path tempDir();
std::filesystem::path pjsipLogPath();
std::filesystem::path defaultProfilePath(const std::filesystem::path& executablePath = {});
void configurePortableEnvironment();
void ensureUserDirectories();
}
