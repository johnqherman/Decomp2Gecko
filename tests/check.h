// tiny test harness on purpose, repo doesn't allow third-party deps
#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

struct Failure : std::exception {
    std::string message;
    explicit Failure(std::string text) : message(std::move(text)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

template <class T> std::string describe(const T& value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(const std::string& value) { return "\"" + value + "\""; }

} // namespace testing

#define TEST(name) \
    static void test_##name(); \
    static testing::Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            throw testing::Failure( \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK failed: " #condition); \
        } \
    } while (0)

#define CHECK_EQ(actual, expected) \
    do { \
        auto actual_value = (actual); \
        auto expected_value = (expected); \
        if (!(actual_value == expected_value)) { \
            throw testing::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                ": CHECK_EQ failed: " #actual " == " #expected "\n    actual:   " + testing::describe(actual_value) + \
                "\n    expected: " + testing::describe(expected_value)); \
        } \
    } while (0)

#define CHECK_THROWS(expression, ExceptionType) \
    do { \
        bool threw_expected = false; \
        try { \
            (void)(expression); \
        } catch (const ExceptionType&) { \
            threw_expected = true; \
        } \
        if (!threw_expected) { \
            throw testing::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                ": expected " #expression " to throw " #ExceptionType); \
        } \
    } while (0)
