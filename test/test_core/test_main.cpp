#include <unity.h>

void run_flag_queue_tests();
void run_relay_url_tests();
void run_flag_name_tests();

void setUp() {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    run_flag_name_tests();
    run_flag_queue_tests();
    run_relay_url_tests();
    return UNITY_END();
}
