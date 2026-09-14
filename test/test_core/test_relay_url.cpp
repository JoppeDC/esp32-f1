#include <unity.h>
#include "relay_url.h"

static RelayUrl parsed;

static bool parse(const char* s) {
    parsed = RelayUrl{};
    return parseRelayUrl(s, parsed);
}

static void test_wss_defaults() {
    TEST_ASSERT_TRUE(parse("wss://f1-relay.joppe.dev/ws"));
    TEST_ASSERT_TRUE(parsed.tls);
    TEST_ASSERT_EQUAL_STRING("f1-relay.joppe.dev", parsed.host);
    TEST_ASSERT_EQUAL_UINT16(443, parsed.port);
    TEST_ASSERT_EQUAL_STRING("/ws", parsed.path);
}

static void test_ws_defaults_and_omitted_path() {
    TEST_ASSERT_TRUE(parse("ws://192.168.1.50"));
    TEST_ASSERT_FALSE(parsed.tls);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", parsed.host);
    TEST_ASSERT_EQUAL_UINT16(80, parsed.port);
    TEST_ASSERT_EQUAL_STRING("/ws", parsed.path);
}

static void test_explicit_port_and_path() {
    TEST_ASSERT_TRUE(parse("ws://192.168.1.50:8000/ws"));
    TEST_ASSERT_EQUAL_UINT16(8000, parsed.port);
    TEST_ASSERT_EQUAL_STRING("/ws", parsed.path);

    TEST_ASSERT_TRUE(parse("wss://relay.example:8443/some/deep/path"));
    TEST_ASSERT_EQUAL_UINT16(8443, parsed.port);
    TEST_ASSERT_EQUAL_STRING("/some/deep/path", parsed.path);
}

static void test_root_path_is_kept_not_defaulted() {
    TEST_ASSERT_TRUE(parse("wss://relay.example/"));
    TEST_ASSERT_EQUAL_STRING("/", parsed.path);
}

static void test_colon_in_path_is_not_a_port() {
    TEST_ASSERT_TRUE(parse("ws://host/path:with:colons"));
    TEST_ASSERT_EQUAL_UINT16(80, parsed.port);
    TEST_ASSERT_EQUAL_STRING("/path:with:colons", parsed.path);
}

static void test_scheme_is_case_insensitive_and_whitespace_trimmed() {
    TEST_ASSERT_TRUE(parse("  WSS://Relay.Example:1234/ws \n"));
    TEST_ASSERT_TRUE(parsed.tls);
    TEST_ASSERT_EQUAL_STRING("Relay.Example", parsed.host);
    TEST_ASSERT_EQUAL_UINT16(1234, parsed.port);
}

static void test_scheme_is_required() {
    TEST_ASSERT_FALSE(parse("f1-relay.joppe.dev/ws"));
    TEST_ASSERT_FALSE(parse("192.168.1.5:8000"));
    TEST_ASSERT_FALSE(parse("https://f1-relay.joppe.dev/ws"));
    TEST_ASSERT_FALSE(parse("wss:/host"));
}

static void test_empty_and_null_rejected() {
    TEST_ASSERT_FALSE(parse(""));
    TEST_ASSERT_FALSE(parse("   "));
    TEST_ASSERT_FALSE(parse(nullptr));
    TEST_ASSERT_FALSE(parse("wss://"));
    TEST_ASSERT_FALSE(parse("wss:///ws"));
}

static void test_bad_host_characters_rejected() {
    TEST_ASSERT_FALSE(parse("wss://[::1]:8000/ws"));       // IPv6 unsupported
    TEST_ASSERT_FALSE(parse("wss://user@host/ws"));
    TEST_ASSERT_FALSE(parse("wss://host?query"));           // '?' without path
    TEST_ASSERT_FALSE(parse("wss://ho st/ws"));
}

static void test_port_validation() {
    TEST_ASSERT_FALSE(parse("wss://host:/ws"));             // empty
    TEST_ASSERT_FALSE(parse("wss://host:0/ws"));
    TEST_ASSERT_FALSE(parse("wss://host:65536/ws"));
    TEST_ASSERT_FALSE(parse("wss://host:4294967339/ws"));   // would wrap a 32-bit parse
    TEST_ASSERT_FALSE(parse("wss://host:80a/ws"));
    TEST_ASSERT_TRUE (parse("wss://host:65535/ws"));
    TEST_ASSERT_EQUAL_UINT16(65535, parsed.port);
    TEST_ASSERT_TRUE (parse("wss://host:1"));
    TEST_ASSERT_EQUAL_UINT16(1, parsed.port);
}

static void test_length_cap() {
    char url[RELAY_URL_MAX_LEN + 10];
    // "wss://" + (MAX-6) 'a' == exactly MAX characters → accepted.
    strcpy(url, "wss://");
    size_t i = 6;
    for (; i < RELAY_URL_MAX_LEN; i++) url[i] = 'a';
    url[i] = '\0';
    TEST_ASSERT_TRUE(parse(url));
    TEST_ASSERT_EQUAL(RELAY_URL_MAX_LEN - 6, strlen(parsed.host));

    // One more character → rejected.
    url[i] = 'a';
    url[i + 1] = '\0';
    TEST_ASSERT_FALSE(parse(url));
}

static void test_failure_leaves_out_untouched() {
    RelayUrl keep;
    TEST_ASSERT_TRUE(parseRelayUrl("ws://old.host:81/x", keep));
    TEST_ASSERT_FALSE(parseRelayUrl("garbage", keep));
    TEST_ASSERT_EQUAL_STRING("old.host", keep.host);
    TEST_ASSERT_EQUAL_UINT16(81, keep.port);
    TEST_ASSERT_EQUAL_STRING("/x", keep.path);
}

void run_relay_url_tests() {
    RUN_TEST(test_wss_defaults);
    RUN_TEST(test_ws_defaults_and_omitted_path);
    RUN_TEST(test_explicit_port_and_path);
    RUN_TEST(test_root_path_is_kept_not_defaulted);
    RUN_TEST(test_colon_in_path_is_not_a_port);
    RUN_TEST(test_scheme_is_case_insensitive_and_whitespace_trimmed);
    RUN_TEST(test_scheme_is_required);
    RUN_TEST(test_empty_and_null_rejected);
    RUN_TEST(test_bad_host_characters_rejected);
    RUN_TEST(test_port_validation);
    RUN_TEST(test_length_cap);
    RUN_TEST(test_failure_leaves_out_untouched);
}
