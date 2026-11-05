#include "SecureStore.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#else
#include <libsecret-1/libsecret/secret.h>
#endif

namespace usage {
namespace {
#if !defined(_WIN32)
const SecretSchema kSchema = {
    "com.smalltv.Usage",
    SECRET_SCHEMA_NONE,
    {{"application", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, (SecretSchemaAttributeType)0}},
};
#endif

#if defined(_WIN32)
std::filesystem::path credentialsPath() {
  const char* appData = std::getenv("APPDATA");
  std::filesystem::path base = appData ? appData : ".";
  return base / "smalltv" / "usage-credentials.bin";
}

std::string winError(const char* prefix) {
  return std::string(prefix) + " (Windows error " + std::to_string(GetLastError()) + ")";
}
#endif
}  // namespace

bool secureLoad(std::string& json, std::string& error) {
  json.clear();
  error.clear();
#if defined(_WIN32)
  const auto path = credentialsPath();
  std::ifstream file(path, std::ios::binary);
  if (!file) return true;
  std::string encrypted((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
  if (encrypted.empty()) return true;
  DATA_BLOB input{};
  input.pbData = (BYTE*)encrypted.data();
  input.cbData = (DWORD)encrypted.size();
  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    error = winError("Cannot decrypt usage credentials");
    return false;
  }
  json.assign((const char*)output.pbData, output.cbData);
  LocalFree(output.pbData);
  return true;
#else
  GError* glibError = nullptr;
  gchar* secret = secret_password_lookup_sync(&kSchema, nullptr, &glibError,
                                               "application", "smalltv", nullptr);
  if (glibError) {
    error = glibError->message ? glibError->message : "OS keyring lookup failed";
    g_error_free(glibError);
    return false;
  }
  if (secret) {
    json = secret;
    g_free(secret);
  }
  return true;
#endif
}

bool secureSave(const std::string& json, std::string& error) {
  error.clear();
#if defined(_WIN32)
  const auto path = credentialsPath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "Cannot create credential directory";
    return false;
  }
  DATA_BLOB input{};
  input.pbData = (BYTE*)json.data();
  input.cbData = (DWORD)json.size();
  DATA_BLOB output{};
  if (!CryptProtectData(&input, L"SmallTV usage credentials", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    error = winError("Cannot encrypt usage credentials");
    return false;
  }
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      LocalFree(output.pbData);
      error = "Cannot write usage credentials";
      return false;
    }
    file.write((const char*)output.pbData, output.cbData);
  }
  LocalFree(output.pbData);
  if (!MoveFileExA(temporary.c_str(), path.string().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = winError("Cannot replace usage credentials");
    return false;
  }
  return true;
#else
  GError* glibError = nullptr;
  const gboolean ok = secret_password_store_sync(
      &kSchema, SECRET_COLLECTION_DEFAULT, "SmallTV usage credentials",
      json.c_str(), nullptr, &glibError, "application", "smalltv", nullptr);
  if (!ok) {
    error = glibError && glibError->message ? glibError->message : "OS keyring save failed";
    if (glibError) g_error_free(glibError);
    return false;
  }
  return true;
#endif
}

bool secureClear(std::string& error) {
  error.clear();
#if defined(_WIN32)
  std::error_code ec;
  std::filesystem::remove(credentialsPath(), ec);
  if (ec) {
    error = "Cannot remove usage credentials";
    return false;
  }
  return true;
#else
  GError* glibError = nullptr;
  secret_password_clear_sync(&kSchema, nullptr, &glibError,
                             "application", "smalltv", nullptr);
  if (glibError) {
    error = glibError->message ? glibError->message : "OS keyring clear failed";
    g_error_free(glibError);
    return false;
  }
  return true;
#endif
}

}  // namespace usage
