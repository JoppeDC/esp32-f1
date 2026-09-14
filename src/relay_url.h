#pragma once
// Relay URL parsing. Pure C++ (no Arduino) so the native test env can cover
// it, and fixed-size buffers so a parsed URL can travel through a FreeRTOS
// queue without heap ownership questions.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// Longest accepted URL, scheme included. Generous for any hostname + path a
// relay behind a reverse proxy could need, and small enough to sit on the
// stack or in a queue item.
static constexpr size_t RELAY_URL_MAX_LEN = 127;

struct RelayUrl {
    bool     tls  = false;
    char     host[RELAY_URL_MAX_LEN + 1] = {};
    uint16_t port = 0;
    char     path[RELAY_URL_MAX_LEN + 1] = {};
};

namespace relay_url_detail {

// Hostname / IPv4 characters. Anything else — including the brackets of an
// IPv6 literal, which is unsupported — is rejected so the UI can say so.
inline bool isHostChar(char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_';
}

}  // namespace relay_url_detail

// Parses "ws://host[:port][/path]" or "wss://host[:port][/path]" and leaves
// `out` untouched on failure. Leading and trailing whitespace is ignored and
// the scheme is matched case-insensitively.
//
// The scheme is required. Guessing it fails in both directions — assume wss
// and a LAN user typing "192.168.1.5:8000" breaks, assume ws and anyone typing
// a public hostname breaks — so a rejection the UI can show beats a silent
// wrong guess. Default port is 443 for wss and 80 for ws; an omitted path
// becomes "/ws". IPv6 literals are not supported.
inline bool parseRelayUrl(const char* url, RelayUrl& out) {
    using relay_url_detail::isHostChar;
    if (!url) return false;

    // Trim.
    while (*url && isspace(static_cast<unsigned char>(*url))) url++;
    size_t len = strlen(url);
    while (len > 0 && isspace(static_cast<unsigned char>(url[len - 1]))) len--;
    if (len == 0 || len > RELAY_URL_MAX_LEN) return false;

    RelayUrl u;
    size_t hostStart;
    if (len >= 6 && strncasecmp(url, "wss://", 6) == 0) {
        u.tls = true;  u.port = 443; hostStart = 6;
    } else if (len >= 5 && strncasecmp(url, "ws://", 5) == 0) {
        u.tls = false; u.port = 80;  hostStart = 5;
    } else {
        return false;   // scheme is required
    }

    // Authority runs from the scheme to the first '/', which starts the path.
    size_t authEnd = hostStart;
    while (authEnd < len && url[authEnd] != '/') authEnd++;

    // A colon inside the authority separates host from port.
    size_t hostEnd = hostStart;
    while (hostEnd < authEnd && url[hostEnd] != ':') hostEnd++;

    const size_t hostLen = hostEnd - hostStart;
    if (hostLen == 0) return false;
    for (size_t i = hostStart; i < hostEnd; i++) {
        if (!isHostChar(url[i])) return false;
    }
    memcpy(u.host, url + hostStart, hostLen);
    u.host[hostLen] = '\0';

    if (hostEnd < authEnd) {
        // Explicit port: 1–5 digits, 1..65535. The digit cap keeps the value
        // in range before conversion, so no overflow can sneak a bad port in.
        const size_t portStart = hostEnd + 1;
        const size_t portLen   = authEnd - portStart;
        if (portLen == 0 || portLen > 5) return false;
        uint32_t p = 0;
        for (size_t i = portStart; i < authEnd; i++) {
            if (!isdigit(static_cast<unsigned char>(url[i]))) return false;
            p = p * 10 + static_cast<uint32_t>(url[i] - '0');
        }
        if (p < 1 || p > 65535) return false;
        u.port = static_cast<uint16_t>(p);
    }

    if (authEnd < len) {
        const size_t pathLen = len - authEnd;
        memcpy(u.path, url + authEnd, pathLen);
        u.path[pathLen] = '\0';
    } else {
        strcpy(u.path, "/ws");
    }

    out = u;
    return true;
}
