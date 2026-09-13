// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.

#include "harness.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::string> g_filters;
bool g_list_only = false;
bool g_verbose = false;

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    if (argument == "--list") {
      g_list_only = true;
    } else if (argument == "--verbose") {
      g_verbose = true;
    } else if (argument.rfind("--filter=", 0) == 0) {
      g_filters.push_back(argument.substr(9));
    } else {
      std::printf("usage: %s [--list] [--verbose] [--filter=<suite.substring>]\n", argv[0]);
      return 2;
    }
  }

  const std::vector<fotest::TestCase>& tests = fotest::Registry::instance().tests();
  if (g_list_only) {
    for (const fotest::TestCase& test : tests) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  for (const fotest::TestCase& test : tests) {
    const std::string label = test.suite + "." + test.name;
    if (!g_filters.empty()) {
      bool matched = false;
      for (const std::string& filter : g_filters) {
        if (label.find(filter) != std::string::npos) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        continue;
      }
    }
    if (g_verbose) {
      std::printf("[ RUN  ] %s\n", label.c_str());
      std::fflush(stdout);
    }
    try {
      test.body();
      ++passed;
      if (g_verbose) {
        std::printf("[  OK  ] %s\n", label.c_str());
      }
    } catch (const std::exception& error) {
      ++failed;
      failures.push_back(label + ": " + error.what());
      std::printf("[ FAIL ] %s\n         %s\n", label.c_str(), error.what());
      std::fflush(stdout);
    } catch (...) {
      ++failed;
      failures.push_back(label + ": unknown exception");
      std::printf("[ FAIL ] %s\n         unknown exception\n", label.c_str());
      std::fflush(stdout);
    }
  }

  std::printf("\n%d passed, %d failed, %d total\n", static_cast<int>(passed),
              static_cast<int>(failed), static_cast<int>(passed + failed));
  if (!failures.empty()) {
    std::printf("failures:\n");
    for (const std::string& failure : failures) {
      std::printf("  - %s\n", failure.c_str());
    }
  }
  return failed == 0 ? 0 : 1;
}

int run_all_tests(const char* suite_banner) {
  (void)suite_banner;
  return 0;
}
