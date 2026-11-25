#include "generator.h"
#include "server.h"
#include "subscription_params.h"
#include "utils.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

// POSIX socket pairs exercise the HTTP handler without a listening server or
// external network access. All successful conversions use synthetic fixtures.
static std::string request(std::string_view target, std::string_view source,
                           std::string_view ua = "", const Route* listener = nullptr) {
    int sockets[2]{};
    const int status = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    assert(status == 0);
    // Put the UA beyond the server's previous 2 KiB read limit.
    const std::string padding = listener ? "X-Test-Padding: " + std::string(3000, 'a') + "\r\n" : "";
    const std::string req = "GET /sub?target=" + url_encode(target) + "&url=" +
                            url_encode(source) + " HTTP/1.1\r\n" + padding +
                            "User-Agent: " + std::string(ua) + "\r\n\r\n";
    if (listener) {
        assert(send(sockets[1], req.data(), req.size(), 0) == static_cast<ssize_t>(req.size()));
        handle_client(sockets[0], listener); // Closes its end of the socket.
    } else {
        handle_subconverter(sockets[0], req);
        close(sockets[0]);
    }
    std::string response;
    char buffer[4096]{};
    for (;;) {
        const auto length = recv(sockets[1], buffer, sizeof(buffer), 0);
        assert(length >= 0);
        if (length == 0) break;
        response.append(buffer, static_cast<std::size_t>(length));
    }
    close(sockets[1]);
    return response;
}

// Sends raw bytes to a route and returns whatever the server replies.
static std::string raw_exchange(std::string_view bytes, const Route& listener) {
    int sockets[2]{};
    const int status = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    assert(status == 0);
    if (!bytes.empty()) {
        assert(send(sockets[1], bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()));
    }
    shutdown(sockets[1], SHUT_WR);
    handle_client(sockets[0], &listener);
    std::string response;
    char buffer[256]{};
    for (ssize_t length; (length = recv(sockets[1], buffer, sizeof(buffer), 0)) > 0;) {
        response.append(buffer, static_cast<std::size_t>(length));
    }
    close(sockets[1]);
    return response;
}

int main() {
    g_IsCon = 1; // Exercise connection diagnostics, including peer lookup.
    // Connections that never form an HTTP GET end without a subscription
    // response or a crash (TLS to the HTTP port, early close, other methods).
    {
        Route probe{};
        probe.local_port = 25599;
        probe.url_count = 1;
        assert(raw_exchange(std::string("\x16\x03\x01\x02\x00", 5), probe).empty());
        assert(raw_exchange("", probe).empty());
        assert(raw_exchange("GET / HTTP/1.1\r\n", probe).empty());
        assert(raw_exchange("POST / HTTP/1.1\r\n\r\n", probe).starts_with("HTTP/1.1 405 "));
    }

    const auto rejected = request("unsupported-target", "missing-file.txt");
    assert(rejected.starts_with("HTTP/1.1 400 Bad Request\r\n"));
    assert(rejected.ends_with("Unsupported conversion target\n"));

    constexpr std::string_view fixture = "tests/reference/grpc_reality_uri.txt";
    for (const auto target : supported_targets) {
        const auto response = request(target, fixture);
        assert(response.starts_with("HTTP/1.1 200 OK\r\n"));
        assert(response.find('\0') == std::string::npos);
    }
    const auto raw = request("", fixture);
    assert(raw.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(raw.contains("vless://"));
    // Configured route through the subconverter: same parameters as the base
    // endpoint, with app-specific headers and provider identity after conversion.
    auto& route = g_Routes[g_RouteCount++];
    route.local_port = 25501;
    route.url_count = 1;
    route.strip_incy = true;
    copy_limited(route.name, sizeof(route.name), "MainBundle");
    copy_limited(route.section, sizeof(route.section), "Sub_Main");
    copy_limited(route.urls[0], sizeof(route.urls[0]), "tests/reference/subscription_params/links.txt");
    load_subscription_parameters("tests/reference/subscription_params");

    const auto happ = request("xray-jsons", "http://127.0.0.1:25501", "Happ/3.23.0");
    assert(happ.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(happ.contains("\r\nproviderid: local-provider\r\n"));
    assert(happ.contains("\r\nhide-settings: 1\r\n"));
    assert(!happ.contains("\r\nfallback-url:")); // Local file, not an HTTP(S) URL.
    assert(happ.contains("\r\nVary: User-Agent\r\n"));
    assert(happ.contains("\r\nCache-Control: no-store\r\n"));
    assert(!happ.contains("\r\nhide-url:"));
    assert(happ.contains("\r\nContent-Type: application/json; charset=utf-8\r\n"));
    assert(happ.contains("\r\n\r\n["));

    const auto incy = request("", "http://127.0.0.1:25501", "INCY/1.0");
    assert(incy.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(incy.contains("\r\nproviderid: body-provider\r\n"));
    assert(incy.contains("\r\nhide-url: 1\r\n"));
    assert(!incy.contains("\r\nfallback-url:"));
    const auto non_app = request("", "http://127.0.0.1:25501", "v2rayNG/1.9");
    assert(non_app.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(non_app.contains("\r\nproviderid: body-provider\r\n"));
    assert(!non_app.contains("\r\nfallback-url:"));
    const auto non_app_body = std::string_view(non_app).substr(non_app.find("\r\n\r\n") + 4);
    assert(!non_app.contains("\r\nhide-url:")); // No INCY override for other clients.
    assert(!non_app_body.contains("#hide-url:")); // Stripping is route-wide.
    assert(!non_app_body.contains("#announce:"));

    const auto body = std::string_view(incy).substr(incy.find("\r\n\r\n") + 4);
    assert(body.contains("#providerid body-provider"));
    assert(body.contains("#hide-settings: 0")); // Only INCY/shared settings were stripped.
    assert(!body.contains("#hide-url:"));
    assert(!body.contains("#announce:"));
    assert(body.contains("vless://"));
    assert(incy.contains(std::format("\r\nContent-Length: {}\r\n", body.size())));
    // A Happ request on the base port must preserve the upstream native Xray
    // profile, with the same INI overrides as an explicitly converted request.
    const char* json_path = "tests/reference/leastload_balancer.json";
    copy_limited(route.urls[0], sizeof(route.urls[0]), json_path);
    const auto base = request("", "", "Happ/4.11.2/Android/", &route);
    assert(base.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(base.contains("\r\nproviderid: local-provider\r\n"));
    assert(base.contains("\r\nhide-settings: 1\r\n"));
    std::ifstream input(json_path, std::ios::binary);
    assert(input);
    const std::string original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    assert(std::string_view(base).substr(base.find("\r\n\r\n") + 4) == original);
    auto& converted = g_Routes[g_RouteCount++];
    converted.is_convert = 1;
    converted.base_port = route.local_port;
    converted.local_port = route.local_port + 1;
    copy_limited(converted.target, sizeof(converted.target), "v2ray");
    const auto links = request("", "", "Happ/4.11.2/Android/", &converted);
    assert(links.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(links.contains("\r\nproviderid: local-provider\r\n"));
    assert(base64_decode(std::string_view(links).substr(links.find("\r\n\r\n") + 4)).contains("vless://"));

    std::println("HTTP target and subscription parameter validation passed.");
}
