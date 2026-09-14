#include <unity.h>
#include "flag_queue.h"

static void test_starts_idle_and_empty() {
    FlagQueue q;
    TEST_ASSERT_EQUAL(F1Flag::IDLE, q.current());
    TEST_ASSERT_EQUAL(F1Flag::IDLE, q.lastQueued());
    TEST_ASSERT_EQUAL(0, q.depth());
    TEST_ASSERT_FALSE(q.hasPending());
    TEST_ASSERT_EQUAL_UINT32(0, q.pendingInMs(1000, 45000));
}

static void test_applies_after_delay_not_before() {
    FlagQueue q;
    TEST_ASSERT_TRUE(q.push(F1Flag::SC, 1000));
    TEST_ASSERT_FALSE(q.tick(1000 + 44999, 45000));
    TEST_ASSERT_EQUAL(F1Flag::IDLE, q.current());
    TEST_ASSERT_TRUE(q.tick(1000 + 45000, 45000));
    TEST_ASSERT_EQUAL(F1Flag::SC, q.current());
    TEST_ASSERT_FALSE(q.hasPending());
}

static void test_zero_delay_applies_on_next_tick() {
    FlagQueue q;
    q.push(F1Flag::RED_FLAG, 500);
    TEST_ASSERT_TRUE(q.tick(500, 0));
    TEST_ASSERT_EQUAL(F1Flag::RED_FLAG, q.current());
}

static void test_dedups_repeated_target() {
    FlagQueue q;
    TEST_ASSERT_TRUE(q.push(F1Flag::YELLOW, 0));
    TEST_ASSERT_FALSE(q.push(F1Flag::YELLOW, 10));
    TEST_ASSERT_FALSE(q.push(F1Flag::YELLOW, 20));
    TEST_ASSERT_EQUAL(1, q.depth());
}

static void test_initial_idle_is_not_queued() {
    FlagQueue q;
    TEST_ASSERT_FALSE(q.push(F1Flag::IDLE, 0));
    TEST_ASSERT_EQUAL(0, q.depth());
}

static void test_short_lived_flag_is_not_swallowed() {
    // YELLOW at t=0, CLEAR at t=2000. With a 45 s delay both must show, in
    // order, 2 s apart.
    FlagQueue q;
    q.push(F1Flag::YELLOW, 0);
    q.push(F1Flag::CLEAR, 2000);
    TEST_ASSERT_EQUAL(2, q.depth());

    TEST_ASSERT_TRUE(q.tick(45000, 45000));
    TEST_ASSERT_EQUAL(F1Flag::YELLOW, q.current());
    TEST_ASSERT_EQUAL(1, q.depth());

    TEST_ASSERT_FALSE(q.tick(46999, 45000));
    TEST_ASSERT_EQUAL(F1Flag::YELLOW, q.current());

    TEST_ASSERT_TRUE(q.tick(47000, 45000));
    TEST_ASSERT_EQUAL(F1Flag::CLEAR, q.current());
}

static void test_multiple_due_entries_apply_in_one_tick() {
    FlagQueue q;
    q.push(F1Flag::YELLOW, 0);
    q.push(F1Flag::SC, 100);
    q.push(F1Flag::CLEAR, 200);
    TEST_ASSERT_TRUE(q.tick(100000, 45000));
    TEST_ASSERT_EQUAL(F1Flag::CLEAR, q.current());
    TEST_ASSERT_EQUAL(0, q.depth());
}

static void test_pending_reports_head_and_countdown() {
    FlagQueue q;
    q.push(F1Flag::SC, 1000);
    q.push(F1Flag::CLEAR, 5000);
    TEST_ASSERT_TRUE(q.hasPending());
    TEST_ASSERT_EQUAL(F1Flag::SC, q.pendingFlag());
    TEST_ASSERT_EQUAL_UINT32(45000, q.pendingInMs(1000, 45000));
    TEST_ASSERT_EQUAL_UINT32(30000, q.pendingInMs(16000, 45000));
    TEST_ASSERT_EQUAL_UINT32(0,     q.pendingInMs(99999, 45000));

    q.tick(46000, 45000);
    TEST_ASSERT_EQUAL(F1Flag::CLEAR, q.pendingFlag());
    TEST_ASSERT_EQUAL_UINT32(4000, q.pendingInMs(46000, 45000));
}

static void test_survives_millis_wrap() {
    FlagQueue q;
    const uint32_t justBeforeWrap = 0xFFFFFFF0u;
    q.push(F1Flag::RED_FLAG, justBeforeWrap);
    // 16 ms later millis() has wrapped to 0; 45 s later it is 44984.
    TEST_ASSERT_FALSE(q.tick(0, 45000));
    TEST_ASSERT_EQUAL_UINT32(45000 - 16, q.pendingInMs(0, 45000));
    TEST_ASSERT_FALSE(q.tick(45000 - 17, 45000));
    TEST_ASSERT_TRUE(q.tick(45000 - 16, 45000));
    TEST_ASSERT_EQUAL(F1Flag::RED_FLAG, q.current());
}

static void test_full_queue_drops_oldest() {
    FlagQueue q;
    // Alternate so dedup never kicks in; CAPACITY + 1 pushes.
    for (uint8_t i = 0; i <= FlagQueue::CAPACITY; i++) {
        q.push((i % 2) ? F1Flag::YELLOW : F1Flag::CLEAR, i);
    }
    TEST_ASSERT_EQUAL(FlagQueue::CAPACITY, q.depth());
    // Oldest (i=0, CLEAR) was dropped, so the head is i=1, YELLOW.
    TEST_ASSERT_EQUAL(F1Flag::YELLOW, q.pendingFlag());
    // Draining applies everything; the final target is i=16, CLEAR.
    TEST_ASSERT_TRUE(q.tick(100000, 1000));
    TEST_ASSERT_EQUAL(F1Flag::CLEAR, q.current());
    TEST_ASSERT_EQUAL(0, q.depth());
}

static void test_reset_clears_everything_and_reenables_dedup_target() {
    FlagQueue q;
    q.push(F1Flag::SC, 0);
    q.tick(50000, 45000);
    q.push(F1Flag::RED_FLAG, 51000);
    TEST_ASSERT_EQUAL(F1Flag::SC, q.current());
    TEST_ASSERT_EQUAL(1, q.depth());

    q.reset();
    TEST_ASSERT_EQUAL(F1Flag::IDLE, q.current());
    TEST_ASSERT_EQUAL(F1Flag::IDLE, q.lastQueued());
    TEST_ASSERT_EQUAL(0, q.depth());
    TEST_ASSERT_FALSE(q.hasPending());

    // After a reset the relay re-sending SC must queue again: it was reset,
    // not shown, so dedup against the old target would lose it.
    TEST_ASSERT_TRUE(q.push(F1Flag::SC, 60000));
    TEST_ASSERT_FALSE(q.tick(60000, 45000));
    TEST_ASSERT_EQUAL(F1Flag::IDLE, q.current());
    TEST_ASSERT_TRUE(q.tick(105000, 45000));
    TEST_ASSERT_EQUAL(F1Flag::SC, q.current());
}

void run_flag_queue_tests() {
    RUN_TEST(test_starts_idle_and_empty);
    RUN_TEST(test_applies_after_delay_not_before);
    RUN_TEST(test_zero_delay_applies_on_next_tick);
    RUN_TEST(test_dedups_repeated_target);
    RUN_TEST(test_initial_idle_is_not_queued);
    RUN_TEST(test_short_lived_flag_is_not_swallowed);
    RUN_TEST(test_multiple_due_entries_apply_in_one_tick);
    RUN_TEST(test_pending_reports_head_and_countdown);
    RUN_TEST(test_survives_millis_wrap);
    RUN_TEST(test_full_queue_drops_oldest);
    RUN_TEST(test_reset_clears_everything_and_reenables_dedup_target);
}
