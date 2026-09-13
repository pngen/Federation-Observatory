// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Minimal, dependency-free test harness. Deliberately has no timeout facility: a test
// that hangs is a defect to diagnose, not something to bound away.

#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace fotest {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

class Registry {
 public:
  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  void add(TestCase test) { tests_.push_back(std::move(test)); }
  [[nodiscard]] const std::vector<TestCase>& tests() const { return tests_; }

 private:
  std::vector<TestCase> tests_;
};

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    Registry::instance().add(TestCase{suite, name, std::move(body)});
  }
};

class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
  throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

/// Render a value for a failure message. Every type used with FO_CHECK_EQ in this
/// repository is streamable; a build error here means a non-streamable type was compared.
template <class T>
std::string describe(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

}  // namespace fotest

#define FO_TEST(suite, name)                                                        \
  static void suite##_##name##_body();                                              \
  static const ::fotest::Registrar suite##_##name##_registrar(#suite, #name,        \
                                                              suite##_##name##_body); \
  static void suite##_##name##_body()

#define FO_CHECK(condition)                                                          \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::fotest::fail(__FILE__, __LINE__, "check failed: " #condition);                \
    }                                                                                \
  } while (false)

#define FO_CHECK_EQ(actual, expected)                                                \
  do {                                                                               \
    const auto& fo_actual = (actual);                                                \
    const auto& fo_expected = (expected);                                            \
    if (!(fo_actual == fo_expected)) {                                               \
      ::fotest::fail(__FILE__, __LINE__,                                             \
                     std::string("expected ") + #actual + " == " + #expected +       \
                         "\n           actual:   " + ::fotest::describe(fo_actual) +  \
                         "\n           expected: " + ::fotest::describe(fo_expected)); \
    }                                                                                \
  } while (false)

#define FO_REQUIRE(condition)                                                        \
  do {                                                                               \
    if (!(condition)) {                                                              \
      ::fotest::fail(__FILE__, __LINE__, "requirement failed: " #condition);          \
    }                                                                                \
  } while (false)

/// Unwrap a Result, failing the test with the error text when it is not ok.
#define FO_UNWRAP(expression)                                                                   \
  ([&] {                                                                                        \
    auto fo_result = (expression);                                                              \
    if (!fo_result.ok()) {                                                                      \
      ::fotest::fail(__FILE__, __LINE__,                                                        \
                     std::string("unexpected error from " #expression ": ") +                   \
                         fo_result.error().to_string());                                        \
    }                                                                                           \
    return fo_result.take();                                                                    \
  }())

#define FO_EXPECT_STATUS(expression)                                                            \
  do {                                                                                          \
    const auto fo_status = (expression);                                                        \
    if (!fo_status.ok()) {                                                                      \
      ::fotest::fail(__FILE__, __LINE__,                                                        \
                     std::string("unexpected status from " #expression ": ") +                  \
                         fo_status.error().to_string());                                        \
    }                                                                                           \
  } while (false)

int run_all_tests(const char* suite_banner);
