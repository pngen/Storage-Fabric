#pragma once
// Test utilities for the Storage Fabric test suite.
// CHECK/CHECK_EQ print the failing expression (and values) to stderr then exit
// non-zero. Each test executable returns 0 on success, non-zero on failure.
// No timeouts, no watchdogs; deterministic and bounded.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>

#include "storagefabric/core/bytes.h"

namespace sfbtest {
inline int g_checks_failed = 0;
}  // namespace sfbtest

// --- CHECK: fails with a message and exits non-zero -------------------------
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "[FAILED] %s (line %d)\n", #cond, __LINE__); \
            ++sfbtest::g_checks_failed;                                      \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

// --- CHECK_EQ: compares two printable values --------------------------------
// Only use for types with an operator<< and operator==. Cast enum classes to
// int before using here.
#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        auto va_ = (a);                                                      \
        auto vb_ = (b);                                                      \
        if (!(va_ == vb_)) {                                                 \
            std::ostringstream oss_;                                         \
            oss_ << "[FAILED] " << #a << " == " << #b << " (line " << __LINE__ \
                 << ") [" << va_ << " != " << vb_ << "]";                    \
            std::fprintf(stderr, "%s\n", oss_.str().c_str());               \
            ++sfbtest::g_checks_failed;                                      \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

// --- CHECK_NE: two printable values differ ---------------------------------
#define CHECK_NE(a, b)                                                       \
    do {                                                                     \
        auto va_ = (a);                                                      \
        auto vb_ = (b);                                                      \
        if (va_ == vb_) {                                                    \
            std::ostringstream oss_;                                         \
            oss_ << "[FAILED] " << #a << " != " << #b << " (line " << __LINE__ \
                 << ") [both were " << va_ << "]";                           \
            std::fprintf(stderr, "%s\n", oss_.str().c_str());               \
            ++sfbtest::g_checks_failed;                                      \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

// --- CHECK_OK: a Result<T> must have a value -------------------------------
#define CHECK_OK(res)                                                        \
    do {                                                                     \
        const auto& r_ = (res);                                              \
        if (!r_.ok()) {                                                      \
            std::fprintf(stderr, "[FAILED] %s (line %d): %s\n", #res,        \
                         __LINE__, r_.status().to_string().c_str());          \
            ++sfbtest::g_checks_failed;                                      \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

// --- CHECK_STATUS: a Status must be ok --------------------------------------
#define CHECK_STATUS(st)                                                     \
    do {                                                                     \
        const auto& st_ = (st);                                              \
        if (!st_.ok()) {                                                     \
            std::fprintf(stderr, "[FAILED] %s (line %d): %s\n", #st,         \
                         __LINE__, st_.to_string().c_str());                  \
            ++sfbtest::g_checks_failed;                                      \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

// --- CHECK_CODE: a Status/Result must carry the given StatusCode ------------
#define CHECK_CODE(res, code)                                                \
    do {                                                                     \
        const auto& r_ = (res);                                              \
        if (!(r_.error_code() == (code))) {                                  \
            std::fprintf(stderr,                                             \
                "[FAILED] %s (line %d): expected code %d got %d (%s)\n",      \
                #res, __LINE__, static_cast<int>(code),                      \
                static_cast<int>(r_.error_code()), r_.status().to_string().c_str()); \
            ++sfbtest::g_checks_failed;                                      \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

// --- Bytes equality helper --------------------------------------------------
inline bool bytes_eq(const std::vector<std::uint8_t>& a,
                     const std::vector<std::uint8_t>& b) {
    return a == b;
}

// --- ByteSpan construction helper ------------------------------------------
// Returns a storagefabric::ByteSpan over a vector. The vector must outlive the
// span; callers manage lifetime.
inline storagefabric::ByteSpan bspan(const std::vector<std::uint8_t>& v) {
    return storagefabric::ByteSpan(v.data(), v.size());
}
