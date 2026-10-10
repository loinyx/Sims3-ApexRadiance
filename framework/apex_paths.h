#pragma once
// Where Apex Radiance keeps its files: Documents\Electronic Arts\<the game's localized folder>\Apex Radiance\ (beside,
// never inside, official S3SS's own S3SS folder). Apex reads S3SS.toml only to migrate and to respect S3SS's settings;
// Only the backed-up room-ambient compatibility correction may write it. The previous standalone build (S3SSApex.asi) kept its files in ...\S3SS\Apex\: they are read
// once for the migration (ApexConfig::EnsureMigrated) and never written or deleted.
#include <string>

namespace ApexPaths {

// Documents\Electronic Arts\<game folder>\  (trailing backslash; empty if Documents cannot be resolved)
const std::wstring& GameDocumentsDirectory();
// ...\S3SS\  (official S3SS's folder; only the room-ambient correction may write its config)
const std::wstring& S3SSDirectory();
// ...\Apex Radiance\  (trailing backslash)
const std::wstring& ApexDirectory();
// Creates ...\Apex Radiance\ if needed.
bool EnsureApexDirectory();
// Development-only output is grouped here. Legacy diagnostic folders remain untouched.
std::wstring DiagnosticsDirectory();

std::wstring ConfigFile();       // ApexRadiance.toml
std::wstring LogFile();          // ApexRadiance_LOG.txt
std::wstring S3SSConfigFile();   // S3SS.toml (room-ambient compatibility exception)
std::wstring MigrationBackup();  // S3SS.toml.pre-split.bak (in the Apex Radiance folder)
const char* ImGuiIniFileUtf8();  // apex_radiance_imgui.ini (UTF-8, for ImGuiIO::IniFilename; stable pointer)

// The previous standalone build's folder ...\S3SS\Apex\ and its settings: read-only, migration only (empty when
// Documents cannot be resolved). Its apex_imgui.ini is not carried over (the menu's scale changed).
std::wstring LegacyApexDirectory();
std::wstring LegacyConfigFile(); // ...\S3SS\Apex\Apex.toml

} // namespace ApexPaths
