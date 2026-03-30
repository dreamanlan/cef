#pragma once
#include <string>

// Returns full path of the current executable, UTF-8 encoded.
// Returns empty string on failure.
std::string GetExePath();

// Returns directory part of the current executable path, UTF-8 encoded.
// Returns empty string on failure.
std::string GetExeDir();

// Returns last directory name of the current executable path, UTF-8 encoded.
// Returns empty string on failure.
std::string GetExeLastDirName();

// Returns full path of the .app directory on macOS, UTF-8 encoded.
// Returns empty string on failure or on non-macOS platforms.
std::string GetMacAppDirPath();

// Returns ~/Library/Application Support/<appname>/ on macOS, where <appname>
// is derived from the .app bundle name. Returns empty string on failure or
// on non-macOS platforms.
std::string GetMacAppSupportDir();

// Returns app directory name of the current executable path, UTF-8 encoded.
// Returns empty string on failure.
std::string GetMacAppDirName();