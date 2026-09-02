#pragma once
// Storage Fabric - authority envelopes and generation fencing.
// An AuthorityEnvelope identifies which process incarnation and generation is
// authorised to perform a mutating storage action. Authority compares over the
// triple (CoordinatorEpoch, WorkerBootId, AuthorityGeneration) lexicographically.
// This is what makes a higher generation from a stale worker boot unable to
// fence a fresh process incarnation: the fresh incarnation carries a newer
// epoch and boot, so the stale boot is always ordered first regardless of how
// large its local generation numbers are.

#include "storagefabric/core/strong.h"
#include "storagefabric/model/enums.h"
#include <string>
#include <cstdint>

namespace storagefabric {

struct AuthorityEnvelope {
    CoordinatorEpoch epoch;
    WorkerBootId boot;
    WorkerId worker;
    AuthorityGeneration generation;
    AuthorityOrigin origin = AuthorityOrigin::UNKNOWN;

    bool is_nil() const noexcept {
        return epoch.is_nil() && boot.is_nil() && worker.is_nil() && generation.is_nil();
    }

    // Lexicographic ordering: epoch, then boot, then generation. Explicit.
    static int compare(const AuthorityEnvelope& a, const AuthorityEnvelope& b) noexcept {
        if (a.epoch.value() != b.epoch.value()) {
            return a.epoch.value() < b.epoch.value() ? -1 : 1;
        }
        if (a.boot.value() != b.boot.value()) {
            return a.boot.value() < b.boot.value() ? -1 : 1;
        }
        if (a.generation.value() != b.generation.value()) {
            return a.generation.value() < b.generation.value() ? -1 : 1;
        }
        return 0;
    }

    // True when this authority is strictly newer than 'other' in the triple order.
    bool is_strictly_newer_than(const AuthorityEnvelope& other) const noexcept {
        return compare(*this, other) > 0;
    }

    // Explicit generation freshness within this exact authority context.
    bool generation_fresh_after(AuthorityGeneration limit) const noexcept {
        return generation.is_fresh_after(limit);
    }

    std::string describe() const {
        return "epoch=" + epoch.str() + " boot=" + boot.str() +
               " worker=" + worker.str() + " gen=" + generation.str() +
               " origin=" + std::string(to_string(origin));
    }
};

// A persistence-envelope also records which generation last produced it so that
// an incarnation-local generation can be fenced by a later boot.
struct StampedAuthority {
    AuthorityEnvelope authority;
    ObjectGeneration object_generation;
};

// Compares an incoming mutation against the currently authoritative one.
// Returns true when the incoming envelope is strictly newer and must be accepted.
inline bool is_authoritative_after(const AuthorityEnvelope& incoming,
                                    const AuthorityEnvelope& current) noexcept {
    return incoming.is_strictly_newer_than(current);
}

}  // namespace storagefabric
