#include <gtest/gtest.h>

#include "core/order_lifecycle.hpp"

namespace {

TEST(OrderLifecycleTest, ValidTransitionsApply) {
    core::OrdStatus status = core::OrdStatus::New;
    ASSERT_TRUE(core::apply_status_transition(status, core::OrdStatus::Working));
    EXPECT_EQ(status, core::OrdStatus::Working);

    ASSERT_TRUE(core::apply_status_transition(status, core::OrdStatus::PartiallyFilled));
    EXPECT_EQ(status, core::OrdStatus::PartiallyFilled);

    ASSERT_TRUE(core::apply_status_transition(status, core::OrdStatus::Filled));
    EXPECT_EQ(status, core::OrdStatus::Filled);
}

TEST(OrderLifecycleTest, InvalidTransitionsRejected) {
    core::OrdStatus status = core::OrdStatus::Filled;
    EXPECT_FALSE(core::apply_status_transition(status, core::OrdStatus::New));
    EXPECT_EQ(status, core::OrdStatus::Filled);

    status = core::OrdStatus::Canceled;
    EXPECT_FALSE(core::apply_status_transition(status, core::OrdStatus::Working));
    EXPECT_EQ(status, core::OrdStatus::Canceled);

    status = core::OrdStatus::PartiallyFilled;
    EXPECT_FALSE(core::apply_status_transition(status, core::OrdStatus::Working));
    EXPECT_EQ(status, core::OrdStatus::PartiallyFilled);
}

TEST(OrderLifecycleTest, UnknownAcceptsFirstStatus) {
    core::OrdStatus status = core::OrdStatus::Unknown;
    ASSERT_TRUE(core::is_valid_transition(status, core::OrdStatus::New));
    ASSERT_TRUE(core::apply_status_transition(status, core::OrdStatus::New));
    EXPECT_EQ(status, core::OrdStatus::New);
}

// ============================================================================
// Wire-event transitions: Direct cancels (no CancelPending intermediate)
// ============================================================================

TEST(OrderLifecycleTest, DirectCancel_FromNew) {
    core::OrdStatus s = core::OrdStatus::New;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

TEST(OrderLifecycleTest, DirectCancel_FromPendingNew) {
    core::OrdStatus s = core::OrdStatus::PendingNew;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

TEST(OrderLifecycleTest, DirectCancel_FromWorking) {
    core::OrdStatus s = core::OrdStatus::Working;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

TEST(OrderLifecycleTest, DirectCancel_FromPartiallyFilled) {
    core::OrdStatus s = core::OrdStatus::PartiallyFilled;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

TEST(OrderLifecycleTest, DirectCancel_FromReplaced) {
    core::OrdStatus s = core::OrdStatus::Replaced;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

// ============================================================================
// Wire-event transitions: Direct replaces (skip intermediate states)
// ============================================================================

TEST(OrderLifecycleTest, DirectReplace_FromNew) {
    core::OrdStatus s = core::OrdStatus::New;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_EQ(s, core::OrdStatus::Replaced);
}

TEST(OrderLifecycleTest, DirectReplace_FromPendingNew) {
    core::OrdStatus s = core::OrdStatus::PendingNew;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_EQ(s, core::OrdStatus::Replaced);
}

TEST(OrderLifecycleTest, DirectReplace_FromWorking) {
    core::OrdStatus s = core::OrdStatus::Working;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_EQ(s, core::OrdStatus::Replaced);
}

TEST(OrderLifecycleTest, DirectReplace_FromPartiallyFilled) {
    core::OrdStatus s = core::OrdStatus::PartiallyFilled;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_EQ(s, core::OrdStatus::Replaced);
}

TEST(OrderLifecycleTest, IdempotentReplace_FromReplaced) {
    core::OrdStatus s = core::OrdStatus::Replaced;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_EQ(s, core::OrdStatus::Replaced);
}

// ============================================================================
// Wire-event transitions: Rejection from non-standard states
// ============================================================================

TEST(OrderLifecycleTest, Rejected_FromPartiallyFilled) {
    core::OrdStatus s = core::OrdStatus::PartiallyFilled;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Rejected));
    EXPECT_EQ(s, core::OrdStatus::Rejected);
}

TEST(OrderLifecycleTest, Rejected_FromReplaced) {
    core::OrdStatus s = core::OrdStatus::Replaced;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Rejected));
    EXPECT_EQ(s, core::OrdStatus::Rejected);
}

TEST(OrderLifecycleTest, Rejected_FromNew) {
    core::OrdStatus s = core::OrdStatus::New;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Rejected));
    EXPECT_EQ(s, core::OrdStatus::Rejected);
}

TEST(OrderLifecycleTest, Rejected_FromWorking) {
    core::OrdStatus s = core::OrdStatus::Working;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Rejected));
    EXPECT_EQ(s, core::OrdStatus::Rejected);
}

// ============================================================================
// Replaced → further transitions (re-amend, fill, partial-fill, cancel-pending)
// ============================================================================

TEST(OrderLifecycleTest, Replaced_ToWorking) {
    core::OrdStatus s = core::OrdStatus::Replaced;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Working));
    EXPECT_EQ(s, core::OrdStatus::Working);
}

TEST(OrderLifecycleTest, Replaced_ToPartiallyFilled) {
    core::OrdStatus s = core::OrdStatus::Replaced;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::PartiallyFilled));
    EXPECT_EQ(s, core::OrdStatus::PartiallyFilled);
}

TEST(OrderLifecycleTest, Replaced_ToFilled) {
    core::OrdStatus s = core::OrdStatus::Replaced;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Filled));
    EXPECT_EQ(s, core::OrdStatus::Filled);
}

TEST(OrderLifecycleTest, Replaced_ToCancelPending) {
    core::OrdStatus s = core::OrdStatus::Replaced;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::CancelPending));
    EXPECT_EQ(s, core::OrdStatus::CancelPending);
}

// ============================================================================
// Terminal state enforcement: no transitions out of Filled, Canceled, Rejected
// ============================================================================

TEST(OrderLifecycleTest, TerminalFilled_RejectsAll) {
    const core::OrdStatus targets[] = {
        core::OrdStatus::New, core::OrdStatus::Working,
        core::OrdStatus::PartiallyFilled, core::OrdStatus::Canceled,
        core::OrdStatus::Replaced, core::OrdStatus::Rejected,
        core::OrdStatus::CancelPending, core::OrdStatus::PendingNew
    };
    for (auto next : targets) {
        core::OrdStatus s = core::OrdStatus::Filled;
        EXPECT_FALSE(core::apply_status_transition(s, next))
            << "Filled should reject transition to " << static_cast<int>(next);
        EXPECT_EQ(s, core::OrdStatus::Filled);
    }
}

TEST(OrderLifecycleTest, TerminalCanceled_RejectsAll) {
    const core::OrdStatus targets[] = {
        core::OrdStatus::New, core::OrdStatus::Working,
        core::OrdStatus::PartiallyFilled, core::OrdStatus::Filled,
        core::OrdStatus::Replaced, core::OrdStatus::Rejected,
        core::OrdStatus::CancelPending, core::OrdStatus::PendingNew
    };
    for (auto next : targets) {
        core::OrdStatus s = core::OrdStatus::Canceled;
        EXPECT_FALSE(core::apply_status_transition(s, next))
            << "Canceled should reject transition to " << static_cast<int>(next);
        EXPECT_EQ(s, core::OrdStatus::Canceled);
    }
}

TEST(OrderLifecycleTest, TerminalRejected_RejectsAll) {
    const core::OrdStatus targets[] = {
        core::OrdStatus::New, core::OrdStatus::Working,
        core::OrdStatus::PartiallyFilled, core::OrdStatus::Filled,
        core::OrdStatus::Canceled, core::OrdStatus::Replaced,
        core::OrdStatus::CancelPending, core::OrdStatus::PendingNew
    };
    for (auto next : targets) {
        core::OrdStatus s = core::OrdStatus::Rejected;
        EXPECT_FALSE(core::apply_status_transition(s, next))
            << "Rejected should reject transition to " << static_cast<int>(next);
        EXPECT_EQ(s, core::OrdStatus::Rejected);
    }
}

// ============================================================================
// Idempotent transitions (same status → same status)
// ============================================================================

TEST(OrderLifecycleTest, Idempotent_AllNonTerminal) {
    const core::OrdStatus states[] = {
        core::OrdStatus::New, core::OrdStatus::PendingNew,
        core::OrdStatus::Working, core::OrdStatus::PartiallyFilled,
        core::OrdStatus::CancelPending, core::OrdStatus::Replaced
    };
    for (auto st : states) {
        core::OrdStatus s = st;
        EXPECT_TRUE(core::apply_status_transition(s, st))
            << "Idempotent transition should be valid for "
            << static_cast<int>(st);
        EXPECT_EQ(s, st);
    }
}

TEST(OrderLifecycleTest, Idempotent_Terminal) {
    const core::OrdStatus states[] = {
        core::OrdStatus::Filled, core::OrdStatus::Canceled,
        core::OrdStatus::Rejected
    };
    for (auto st : states) {
        core::OrdStatus s = st;
        EXPECT_TRUE(core::apply_status_transition(s, st))
            << "Idempotent terminal transition should be valid for "
            << static_cast<int>(st);
        EXPECT_EQ(s, st);
    }
}

// ============================================================================
// Unknown → any first observation
// ============================================================================

TEST(OrderLifecycleTest, Unknown_AcceptsAnyStatus) {
    const core::OrdStatus targets[] = {
        core::OrdStatus::New, core::OrdStatus::PendingNew,
        core::OrdStatus::Working, core::OrdStatus::PartiallyFilled,
        core::OrdStatus::Filled, core::OrdStatus::Canceled,
        core::OrdStatus::Replaced, core::OrdStatus::Rejected,
        core::OrdStatus::CancelPending
    };
    for (auto next : targets) {
        core::OrdStatus s = core::OrdStatus::Unknown;
        EXPECT_TRUE(core::apply_status_transition(s, next))
            << "Unknown should accept transition to " << static_cast<int>(next);
        EXPECT_EQ(s, next);
    }
}

// ============================================================================
// Multi-step realistic wire-event flows
// ============================================================================

TEST(OrderLifecycleTest, Flow_NewWorkingPartialCancelPendingCanceled) {
    core::OrdStatus s = core::OrdStatus::New;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Working));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::PartiallyFilled));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::CancelPending));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

TEST(OrderLifecycleTest, Flow_DirectCancelFromWorking) {
    core::OrdStatus s = core::OrdStatus::New;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Working));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

TEST(OrderLifecycleTest, Flow_ReplaceChain) {
    core::OrdStatus s = core::OrdStatus::New;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Working));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::PartiallyFilled));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Filled));
    EXPECT_EQ(s, core::OrdStatus::Filled);
}

TEST(OrderLifecycleTest, Flow_PartialFillThenDirectCancel) {
    core::OrdStatus s = core::OrdStatus::New;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Working));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::PartiallyFilled));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

TEST(OrderLifecycleTest, Flow_ReplacedThenDirectCancel) {
    core::OrdStatus s = core::OrdStatus::New;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Canceled));
    EXPECT_EQ(s, core::OrdStatus::Canceled);
}

TEST(OrderLifecycleTest, Flow_ReplacedThenRejected) {
    core::OrdStatus s = core::OrdStatus::Working;
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Replaced));
    EXPECT_TRUE(core::apply_status_transition(s, core::OrdStatus::Rejected));
    EXPECT_EQ(s, core::OrdStatus::Rejected);
}

// ============================================================================
// Invalid backward transitions from non-terminal states
// ============================================================================

TEST(OrderLifecycleTest, Invalid_WorkingToNew) {
    core::OrdStatus s = core::OrdStatus::Working;
    EXPECT_FALSE(core::apply_status_transition(s, core::OrdStatus::New));
}

TEST(OrderLifecycleTest, Invalid_PartiallyFilledToNew) {
    core::OrdStatus s = core::OrdStatus::PartiallyFilled;
    EXPECT_FALSE(core::apply_status_transition(s, core::OrdStatus::New));
}

TEST(OrderLifecycleTest, Invalid_PartiallyFilledToWorking) {
    core::OrdStatus s = core::OrdStatus::PartiallyFilled;
    EXPECT_FALSE(core::apply_status_transition(s, core::OrdStatus::Working));
}

TEST(OrderLifecycleTest, Invalid_CancelPendingToWorking) {
    core::OrdStatus s = core::OrdStatus::CancelPending;
    EXPECT_FALSE(core::apply_status_transition(s, core::OrdStatus::Working));
}

TEST(OrderLifecycleTest, Invalid_CancelPendingToReplaced) {
    core::OrdStatus s = core::OrdStatus::CancelPending;
    EXPECT_FALSE(core::apply_status_transition(s, core::OrdStatus::Replaced));
}

// ============================================================================
// is_terminal_status coverage
// ============================================================================

TEST(OrderLifecycleTest, IsTerminal) {
    EXPECT_TRUE(core::is_terminal_status(core::OrdStatus::Filled));
    EXPECT_TRUE(core::is_terminal_status(core::OrdStatus::Canceled));
    EXPECT_TRUE(core::is_terminal_status(core::OrdStatus::Rejected));
}

TEST(OrderLifecycleTest, IsNotTerminal) {
    EXPECT_FALSE(core::is_terminal_status(core::OrdStatus::Unknown));
    EXPECT_FALSE(core::is_terminal_status(core::OrdStatus::New));
    EXPECT_FALSE(core::is_terminal_status(core::OrdStatus::PendingNew));
    EXPECT_FALSE(core::is_terminal_status(core::OrdStatus::Working));
    EXPECT_FALSE(core::is_terminal_status(core::OrdStatus::PartiallyFilled));
    EXPECT_FALSE(core::is_terminal_status(core::OrdStatus::CancelPending));
    EXPECT_FALSE(core::is_terminal_status(core::OrdStatus::Replaced));
}

} // namespace
