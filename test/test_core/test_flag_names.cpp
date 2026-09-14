#include <unity.h>
#include "f1_flags.h"

static void test_flag_names_round_trip() {
    const F1Flag all[] = { F1Flag::IDLE, F1Flag::CLEAR, F1Flag::YELLOW, F1Flag::VSC,
                           F1Flag::SC, F1Flag::RED_FLAG, F1Flag::CHEQ };
    for (F1Flag f : all) {
        TEST_ASSERT_EQUAL(f, flagFromName(flagName(f)));
    }
    TEST_ASSERT_EQUAL_STRING("RED", flagName(F1Flag::RED_FLAG));
}

static void test_unknown_flag_name_is_idle() {
    TEST_ASSERT_EQUAL(F1Flag::IDLE, flagFromName("PURPLE"));
    TEST_ASSERT_EQUAL(F1Flag::IDLE, flagFromName(""));
    TEST_ASSERT_EQUAL(F1Flag::IDLE, flagFromName(nullptr));
    TEST_ASSERT_EQUAL(F1Flag::IDLE, flagFromName("red"));   // wire names are upper-case
}

static void test_session_status_round_trip() {
    const SessionStatus all[] = { SessionStatus::PRE, SessionStatus::LIVE, SessionStatus::SUSPENDED,
                                  SessionStatus::BREAK, SessionStatus::FINISHED,
                                  SessionStatus::FINALISED, SessionStatus::ENDED };
    for (SessionStatus s : all) {
        TEST_ASSERT_EQUAL(s, sessionStatusFromName(sessionStatusName(s)));
    }
    TEST_ASSERT_EQUAL(SessionStatus::UNKNOWN, sessionStatusFromName("unknown"));
    TEST_ASSERT_EQUAL(SessionStatus::UNKNOWN, sessionStatusFromName(nullptr));
}

void run_flag_name_tests() {
    RUN_TEST(test_flag_names_round_trip);
    RUN_TEST(test_unknown_flag_name_is_idle);
    RUN_TEST(test_session_status_round_trip);
}
