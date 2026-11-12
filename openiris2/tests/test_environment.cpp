// What every test runs in, set before any test starts, as OpenIris 2's own tests have it:
// - the plugins from this build's dpi module first, then OpenIris 2's (the rendered eyes), and none
//   from the user's plugins folder (OpenIris 2's PLUGINS-SPEC 22.3);
// - every plugin on, whatever this computer's plugins.json says (PLUGINS-SPEC 21.8);
// - a preferences folder of the test program's own, removed when it ends.
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

#include "openiris/plugin_loader.hpp"

namespace {

void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

struct TestEnvironment {
  std::filesystem::path preferences;
  TestEnvironment() {
#if defined(_WIN32)
    constexpr const char* kSeparator = ";";
#else
    constexpr const char* kSeparator = ":";
#endif
    set_env("OPENIRIS_PLUGINS", std::string(DPI_PLUGINS_DIR) + kSeparator + OPENIRIS2_PLUGINS_DIR);
    set_env("OPENIRIS_NO_USER_PLUGINS", "1");
    std::mt19937_64 rng(std::random_device{}());
    preferences = std::filesystem::temp_directory_path() / ("openiris2-dpi-test-preferences-" + std::to_string(rng()));
    std::error_code ec;
    std::filesystem::create_directories(preferences, ec);
    set_env("OPENIRIS_PREFERENCES", preferences.string());
    set_env("OPENIRIS_PLUGINS_ALL_ON", "1");
    oi::set_plugins_all_on();
  }
  ~TestEnvironment() {
    std::error_code ec;
    if (!preferences.empty()) std::filesystem::remove_all(preferences, ec);
  }
};

const TestEnvironment kEnvironment;

}  // namespace
