#pragma once
// Storage Fabric - status and result types.
// Error handling is explicit and deterministic. Operations that can fail
// return Result<T>; invariant violations are reported as Status with a code
// and a human-readable message. Exceptions are not used to signal ordinary
// failure paths.

#include <cstdint>
#include <string>
#include <utility>
#include <new>
#include <stdexcept>

namespace storagefabric {

enum class StatusCode : std::uint32_t {
    Ok = 0,
    InvalidArgument,
    NotImplemented,
    Unsupported,
    NotFound,
    AlreadyExists,
    DuplicateIdentity,
    InvalidState,
    InvalidTransition,
    IntegrityMismatch,
    DigestMismatch,
    LengthMismatch,
    Truncated,
    Corrupted,
    Malformed,
    Overflow,
    StaleGeneration,
    StaleAuthority,
    StaleObservation,
    DuplicateReservation,
    StaleReservation,
    ReservationOvercommit,
    NegativeAccounting,
    InsufficientCapacity,
    BackendUnavailable,
    BackendDegraded,
    PathUnsafe,
    PolicyViolation,
    EvictionUnsafe,
    DedupRefcountUnderflow,
    IoError,
    ProtocolError,
    TrailingGarbage,
    Reentrancy,
    Contention,
    KernelError,
    Internal,
};

constexpr const char* status_name(StatusCode c) noexcept {
    switch (c) {
        case StatusCode::Ok: return "Ok";
        case StatusCode::InvalidArgument: return "InvalidArgument";
        case StatusCode::NotImplemented: return "NotImplemented";
        case StatusCode::Unsupported: return "Unsupported";
        case StatusCode::NotFound: return "NotFound";
        case StatusCode::AlreadyExists: return "AlreadyExists";
        case StatusCode::DuplicateIdentity: return "DuplicateIdentity";
        case StatusCode::InvalidState: return "InvalidState";
        case StatusCode::InvalidTransition: return "InvalidTransition";
        case StatusCode::IntegrityMismatch: return "IntegrityMismatch";
        case StatusCode::DigestMismatch: return "DigestMismatch";
        case StatusCode::LengthMismatch: return "LengthMismatch";
        case StatusCode::Truncated: return "Truncated";
        case StatusCode::Corrupted: return "Corrupted";
        case StatusCode::Malformed: return "Malformed";
        case StatusCode::Overflow: return "Overflow";
        case StatusCode::StaleGeneration: return "StaleGeneration";
        case StatusCode::StaleAuthority: return "StaleAuthority";
        case StatusCode::StaleObservation: return "StaleObservation";
        case StatusCode::DuplicateReservation: return "DuplicateReservation";
        case StatusCode::StaleReservation: return "StaleReservation";
        case StatusCode::ReservationOvercommit: return "ReservationOvercommit";
        case StatusCode::NegativeAccounting: return "NegativeAccounting";
        case StatusCode::InsufficientCapacity: return "InsufficientCapacity";
        case StatusCode::BackendUnavailable: return "BackendUnavailable";
        case StatusCode::BackendDegraded: return "BackendDegraded";
        case StatusCode::PathUnsafe: return "PathUnsafe";
        case StatusCode::PolicyViolation: return "PolicyViolation";
        case StatusCode::EvictionUnsafe: return "EvictionUnsafe";
        case StatusCode::DedupRefcountUnderflow: return "DedupRefcountUnderflow";
        case StatusCode::IoError: return "IoError";
        case StatusCode::ProtocolError: return "ProtocolError";
        case StatusCode::TrailingGarbage: return "TrailingGarbage";
        case StatusCode::Reentrancy: return "Reentrancy";
        case StatusCode::Contention: return "Contention";
        case StatusCode::KernelError: return "KernelError";
        case StatusCode::Internal: return "Internal";
    }
    return "Unknown";
}

class Status {
public:
    Status() noexcept = default;
    /* implicit */ Status(StatusCode code) noexcept : code_(code) {}
    Status(StatusCode code, std::string message) noexcept
        : code_(code), message_(std::move(message)) {}

    static Status ok_status() noexcept { return Status(); }

    bool ok() const noexcept { return code_ == StatusCode::Ok; }
    bool failed() const noexcept { return !ok(); }
    explicit operator bool() const noexcept { return ok(); }

    StatusCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    const char* name() const noexcept { return status_name(code_); }

    // Produces a formatted "code: message" string.
    std::string to_string() const {
        if (message_.empty()) return std::string(name());
        return std::string(name()) + ": " + message_;
    }

    static Status from_code_message(StatusCode code, const std::string& msg) {
        return Status(code, msg);
    }

private:
    StatusCode code_{StatusCode::Ok};
    std::string message_;
};

// Result<T> models an operation that either succeeds with a value or fails
// with a Status. It is the preferred transport for fallible operations.
template <typename T>
class Result {
public:
    Result() : ok_(false), status_(StatusCode::Internal, "empty result") {}
    Result(const T& value) : ok_(true), value_(value) {}
    Result(T&& value) : ok_(true), value_(std::move(value)) {}
    Result(const Status& status) : ok_(false), status_(status) {}
    Result(Status&& status) : ok_(false), status_(std::move(status)) {}

    Result(const Result&) = default;
    Result& operator=(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(Result&&) = default;

    static Result ok_value(T value) { return Result(std::move(value)); }
    static Result failure(StatusCode code, std::string msg) {
        return Result(Status(code, std::move(msg)));
    }

    bool has_value() const noexcept { return ok_; }
    bool ok() const noexcept { return ok_; }
    bool failed() const noexcept { return !ok_; }
    explicit operator bool() const noexcept { return ok_; }

    const T& value() const& {
        if (!ok_) throw_failure();
        return value_;
    }
    T& value() & {
        if (!ok_) throw_failure();
        return value_;
    }
    T&& value() && {
        if (!ok_) throw_failure();
        return std::move(value_);
    }

    const Status& status() const noexcept { return status_; }
    StatusCode error_code() const noexcept { return status_.code(); }
    const std::string& error_message() const { return status_.message(); }

    // Adapters for range-style access.
    const T& operator*() const& { return value(); }
    T& operator*() & { return value(); }
    T&& operator*() && { return std::move(*this).value(); }

private:
    [[noreturn]] void throw_failure() const {
        throw std::runtime_error("Storage Fabric Result failure: " + status_.to_string());
    }
    T value_{};
    bool ok_{false};
    Status status_{StatusCode::Internal, "empty result"};
};

// StatusOr alias kept for readability in some call sites.
template <typename T>
using StatusOr = Result<T>;

// A cheap zero-overhead success Status value.
inline const Status kOk = Status::ok_status();

}  // namespace storagefabric
