#include "server.h"
#include "utils.h"
#include "parser.h"
#include "generator.h"
#include "safe_crt.h"
#include "subscription_params.h"
#include <thread>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <format>
#include <string_view>
#include <algorithm>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <curl/curl.h>
#define closesocket close
#define SD_SEND SHUT_WR
#endif

void copy_limited(char *dst, int cap, const char *src) {
    if (!dst || cap <= 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    safe_crt::strncpy_s_wrapper(dst, static_cast<size_t>(cap), src, static_cast<size_t>(cap - 1));
}

namespace {

bool send_all(SOCKET socket, std::string_view bytes) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    while (!bytes.empty()) {
        const auto sent = send(socket, bytes.data(), static_cast<int>(bytes.size()), flags);
        if (sent <= 0) return false;
        bytes.remove_prefix(sent);
    }
    return true;
}

std::string peer_name(SOCKET socket) {
    sockaddr_in addr{};
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (getpeername(socket, reinterpret_cast<sockaddr *>(&addr), &len) != 0 || addr.sin_family != AF_INET) {
        return "unknown";
    }
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip))) return "unknown";
    return std::format("{}:{}", ip, ntohs(addr.sin_port));
}

// Classify requests without logging raw bytes, URLs or credentials.
bool starts_with_tls(std::string_view bytes) {
    return bytes.starts_with("\x16\x03");
}

std::string_view request_kind(std::string_view bytes) {
    if (bytes.empty()) return "no data";
    if (bytes.starts_with("GET ")) return "GET";
    if (bytes.starts_with("HEAD ")) return "HEAD";
    if (bytes.starts_with("CONNECT ")) return "CONNECT";
    return "unrecognized data";
}

#ifdef _WIN32
struct WinHttpHandleCloser {
    void operator()(HINTERNET h) const noexcept {
        if (h) WinHttpCloseHandle(h);
    }
};

using ScopedHInternet = std::unique_ptr<void, WinHttpHandleCloser>;

void query_metadata(HINTERNET req, SubMetadata& meta) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes) return;

    std::wstring wide(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                             wide.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return;
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return;
    std::string utf8(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), length, nullptr, nullptr)) return;
    std::string_view headers(utf8.data(), length - 1);
    while (!headers.empty()) {
        const auto end = headers.find('\n');
        read_subscription_header(meta, headers.substr(0, end));
        if (end == headers.npos) break;
        headers.remove_prefix(end + 1);
    }
}
#else
struct MemoryStruct {
    char *memory;
    size_t size;
    size_t capacity;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    auto *mem = static_cast<MemoryStruct *>(userp);

    if (mem->size + realsize >= mem->capacity) {
        realsize = (mem->capacity > mem->size + 1) ? (mem->capacity - mem->size - 1) : 0;
    }
    if (realsize > 0) {
        std::memcpy(&(mem->memory[mem->size]), contents, realsize);
        mem->size += realsize;
        mem->memory[mem->size] = 0;
    }
    return size * nmemb;
}

static size_t HeaderCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    auto *meta = static_cast<SubMetadata *>(userdata);
    if (!meta) return total;

    std::string_view line(buffer, total);
    // libcurl also reports intermediate responses (redirects and proxy CONNECT).
    if (line.starts_with("HTTP/")) *meta = {};
    else read_subscription_header(*meta, line);
    return total;
}
#endif

void merge_metadata(SubMetadata& dst, const SubMetadata& src) {
    if (dst.content_type.empty()) dst.content_type = src.content_type;
    for (const auto& param : src.parameters) {
        set_subscription_parameter(dst, param.name, param.value, false);
    }
}

std::string lower_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) out.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    return out;
}

bool contains_ci(std::string_view value, std::string_view needle) {
    return lower_copy(value).find(lower_copy(needle)) != std::string::npos;
}

std::string request_header(std::string_view req, std::string_view name) {
    std::string wanted = lower_copy(name);
    size_t start = 0;
    while (start < req.length()) {
        size_t end = req.find('\n', start);
        if (end == std::string_view::npos) end = req.length();
        std::string_view line = req.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        size_t colon = line.find(':');
        if (colon != std::string_view::npos && lower_copy(line.substr(0, colon)) == wanted) {
            size_t value_start = line.find_first_not_of(" \t", colon + 1);
            return value_start == std::string_view::npos ? "" : std::string(line.substr(value_start));
        }
        start = end + 1;
    }
    return "";
}

std::string target_from_user_agent(std::string_view ua) {
    if (contains_ci(ua, "clash")) return "clash";
    if (contains_ci(ua, "sing-box") || contains_ci(ua, "singbox")) {
        if (contains_ci(ua, "android")) return "singbox-android";
        return "singbox-pc";
    }
    if (contains_ci(ua, "v2ray") || contains_ci(ua, "xray") ||
        contains_ci(ua, "nekobox") || contains_ci(ua, "nekoray") ||
        contains_ci(ua, "hiddify") ||
        contains_ci(ua, "streisand") || contains_ci(ua, "shadowrocket")) {
        return "v2ray";
    }
    return "";
}

std::string quoted_filename(const char *name) {
    std::string out;
    for (const char *p = name; p && *p; p++) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c >= 0x7f) continue;
        if (*p == '"' || *p == '\\') out += '_';
        else out += *p;
    }
    return out.empty() ? "subscription" : out;
}

void apply_route_name(SubMetadata& meta, const char *name) {
    if (!name || !name[0]) return;

    std::string title = std::format("base64:{}", base64_encode(name));
    set_subscription_parameter(meta, "profile-title", title);

    std::string disp = std::format("attachment; filename=\"{}\"; filename*=UTF-8''{}",
                                   quoted_filename(name), url_encode(name));
    set_subscription_parameter(meta, "content-disposition", disp);
}

std::string metadata_headers(const SubMetadata& meta) {
    std::string headers;
    for (const auto& param : meta.parameters) {
        if (!param.value.empty()) headers += std::format("{}: {}\r\n", param.name, param.value);
    }
    return headers;
}

const char *converted_content_type(std::string_view target) {
    if (target == "clash") return "text/yaml; charset=utf-8";
    if (target == "singbox-pc" || target == "sing-box-pc" ||
        target == "singbox-android" || target == "sing-box-android" ||
        target == "xray-one" || target == "xray-json" || target == "v2ray-json" || target == "xray-jsons") {
        return "application/json; charset=utf-8";
    }
    return "text/plain; charset=utf-8";
}

bool preferred_metadata_source(std::string_view target, const Route& rt, int index, const SubMetadata& meta) {
    std::string ua = (index >= 0 && index < 8) ? rt.user_agents[index] : "";
    if (target == "clash") {
        return contains_ci(ua, "clash") || contains_ci(meta.content_type, "yaml");
    }
    if (target == "v2ray" || target == "xray") {
        return contains_ci(meta.content_type, "text/plain");
    }
    if (target == "singbox-pc" || target == "sing-box-pc" ||
        target == "singbox-android" || target == "sing-box-android") {
        return contains_ci(ua, "sing-box") || contains_ci(ua, "singbox") || contains_ci(meta.content_type, "json");
    }
    return false;
}

void merge_metadata_for_target(SubMetadata& dst, const SubMetadata& src, bool preferred, bool& have_preferred) {
    if (preferred && !have_preferred) {
        SubMetadata old = dst;
        dst = src;
        merge_metadata(dst, old);
        have_preferred = true;
        return;
    }
    if (!have_preferred || preferred) {
        merge_metadata(dst, src);
    } else {
        // A source of a different format can still supply a missing ID.
        for (const auto& param : src.parameters) {
            if (param.name == "providerid") set_subscription_parameter(dst, param.name, param.value, false);
        }
    }
}

} // namespace

#ifdef _WIN32
int fetch_url(const Route *rt, char *buf, int cap, const wchar_t* custom_ua, int url_index,
              SubMetadata *out_meta) {
    int total = -1;
    char full_url[4096]{};
    std::wstring object_name;

    if (out_meta) *out_meta = {};

    copy_limited(full_url, sizeof(full_url), rt->urls[url_index]);

    wchar_t wurl[4096]{};
    MultiByteToWideChar(CP_UTF8, 0, full_url, -1, wurl, 4096);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        FILE *f = nullptr;
        if (fopen_s(&f, full_url, "rb") == 0 && f) {
            int n = static_cast<int>(fread(buf, 1, cap - 1, f));
            fclose(f);
            if (n >= 0) {
                buf[n] = '\0';
                return n;
            }
        }
        return -1;
    }

    wchar_t host[256]{};
    wcsncpy_s(host, 256, uc.lpszHostName, uc.dwHostNameLength);

    const wchar_t* ua = custom_ua;
    wchar_t wua[128]{};
    if (!ua && rt && rt->user_agents[url_index][0]) {
        MultiByteToWideChar(CP_UTF8, 0, rt->user_agents[url_index], -1, wua, 128);
        ua = wua;
    }
    if (!ua) ua = UA_WIDE;

    HINTERNET raw_hS = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!raw_hS) return -1;
    ScopedHInternet hS(raw_hS);

    WinHttpSetTimeouts(hS.get(), UP_TIMEOUT, UP_TIMEOUT, UP_TIMEOUT, UP_TIMEOUT);

    HINTERNET raw_hC = WinHttpConnect(hS.get(), host, uc.nPort, 0);
    if (!raw_hC) return -1;
    ScopedHInternet hC(raw_hC);

    if (uc.lpszUrlPath && uc.dwUrlPathLength) {
        object_name.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    } else {
        object_name = L"/";
    }
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) {
        object_name.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    if (const auto fragment = object_name.find(L'#'); fragment != object_name.npos) {
        object_name.resize(fragment);
    }

    HINTERNET raw_hR = WinHttpOpenRequest(hC.get(), L"GET", object_name.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!raw_hR) return -1;
    ScopedHInternet hR(raw_hR);

    if (rt->use_hwid) {
        wchar_t hdr[1024]{}, wH[128]{}, wO[64]{}, wV[64]{}, wM[256]{};
        MultiByteToWideChar(CP_UTF8, 0, g_Dev.hwid, -1, wH, 128);
        MultiByteToWideChar(CP_UTF8, 0, g_Dev.os, -1, wO, 64);
        MultiByteToWideChar(CP_UTF8, 0, g_Dev.ver, -1, wV, 64);
        MultiByteToWideChar(CP_UTF8, 0, g_Dev.model, -1, wM, 256);
        _snwprintf_s(hdr, sizeof(hdr) / sizeof(hdr[0]), _TRUNCATE,
                   L"x-hwid: %s\r\n"
                   L"x-device-os: %s\r\n"
                   L"x-ver-os: %s\r\n"
                   L"x-device-model: %s",
                   wH, wO, wV, wM);
        WinHttpAddRequestHeaders(hR.get(), hdr, static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(hR.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return -1;
    if (!WinHttpReceiveResponse(hR.get(), nullptr)) return -1;

    {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(hR.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) { return -static_cast<int>(code); }
    }

    if (out_meta) query_metadata(hR.get(), *out_meta);

    total = 0;
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(hR.get(), &avail) || !avail) break;
        if (total + static_cast<int>(avail) >= cap) avail = static_cast<DWORD>(cap - 1 - total);
        if (!avail) break;
        if (!WinHttpReadData(hR.get(), buf + total, avail, &got)) break;
        total += static_cast<int>(got);
    }
    buf[total] = '\0';
    return total;
}
#else
int fetch_url(const Route *rt, char *buf, int cap, const wchar_t* custom_ua, int url_index,
              SubMetadata *out_meta) {
    if (out_meta) *out_meta = {};
    char full_url[4096]{};
    copy_limited(full_url, sizeof(full_url), rt->urls[url_index]);

    std::string_view url_sv(full_url);
    if (!url_sv.starts_with("http://") && !url_sv.starts_with("https://")) {
        FILE *f = nullptr;
        if (safe_crt::fopen_s_wrapper(&f, full_url, "rb") == 0 && f) {
            int n = static_cast<int>(fread(buf, 1, cap - 1, f));
            fclose(f);
            if (n >= 0) {
                buf[n] = '\0';
                return n;
            }
        }
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    MemoryStruct chunk{ buf, 0, static_cast<size_t>(cap) };
    buf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void *>(&chunk));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(UP_TIMEOUT));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(UP_TIMEOUT));

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    std::string ua_str;
    if (custom_ua) {
        std::wstring ws(custom_ua);
        ua_str = std::string(ws.begin(), ws.end());
    } else if (rt && rt->user_agents[url_index][0]) {
        ua_str = rt->user_agents[url_index];
    } else {
        ua_str = "Happ/3.23.0";
    }
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua_str.c_str());

    struct curl_slist *headers = nullptr;
    if (rt && rt->use_hwid) {
        headers = curl_slist_append(headers, std::format("x-hwid: {}", std::string_view(g_Dev.hwid)).c_str());
        headers = curl_slist_append(headers, std::format("x-device-os: {}", std::string_view(g_Dev.os)).c_str());
        headers = curl_slist_append(headers, std::format("x-ver-os: {}", std::string_view(g_Dev.ver)).c_str());
        headers = curl_slist_append(headers, std::format("x-device-model: {}", std::string_view(g_Dev.model)).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (out_meta) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, static_cast<void *>(out_meta));
    }

    CURLcode res = curl_easy_perform(curl);
    if (headers) curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return -1;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code != 200) {
        return -static_cast<int>(http_code);
    }

    return static_cast<int>(chunk.size);
}
#endif

static void serve_converted_or_raw(SOCKET c, const Route *source_rt, const std::string& target, int log_port,
                                   SubscriptionApp app) {
    if (!target.empty() && !is_supported_target(target)) {
        constexpr std::string_view message = "Unsupported conversion target\n";
        const std::string response = std::format(
            "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: {}\r\nConnection: close\r\n\r\n{}", message.size(), message);
        send(c, response.data(), static_cast<int>(response.size()), 0);
        logm("  [FAIL] Port %d -> 400 Unsupported Conversion Target\n", log_port);
        return;
    }
    char *body = static_cast<char *>(mem_alloc(BODY_CAP));
    if (!body) {
        constexpr std::string_view e = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 3\r\nConnection: close\r\n\r\nOOM";
        send(c, e.data(), static_cast<int>(e.length()), 0);
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [started] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    };
    std::string out_payload;
    std::vector<Proxy> all_proxies;
    std::vector<Balancer> all_balancers;
    std::vector<Rule> all_rules;
    int success_count = 0;

    SubMetadata meta{};
    const auto overrides = select_app_parameters(*source_rt, app);
    bool have_preferred_meta = false;

    for (int i = 0; i < source_rt->url_count; i++) {
        SubMetadata temp_meta{};
        int blen = fetch_url(source_rt, body, static_cast<int>(BODY_CAP), nullptr, i, &temp_meta);
        if (blen >= 0) {
            success_count++;
            std::string filtered;
            const auto payload = filter_subscription_payload(
                std::string_view(body, blen), *source_rt, overrides, temp_meta, filtered, !target.empty());
            read_provider_id_fragment(temp_meta, source_rt->urls[i]);
            if (!target.empty()) {
                merge_metadata_for_target(meta, temp_meta, preferred_metadata_source(target, *source_rt, i, temp_meta), have_preferred_meta);
            } else {
                merge_metadata(meta, temp_meta);
            }

            if (!target.empty()) {
                std::vector<Proxy> p;
                std::vector<Balancer> b;
                std::vector<Rule> r;
                parse_subscription(payload, p, b, r);
                all_proxies.insert(all_proxies.end(), p.begin(), p.end());
                all_balancers.insert(all_balancers.end(), b.begin(), b.end());
                all_rules.insert(all_rules.end(), r.begin(), r.end());
            } else {
                if (!out_payload.empty()) out_payload += "\n";
                out_payload.append(payload);
            }
        }
    }

    if (success_count > 0) {
        std::string content_type;
        try {
            if (target == "clash") {
                out_payload = gen_clash(all_proxies, all_rules, all_balancers, source_rt->force_balancer);
                content_type = converted_content_type(target);
            } else if (target == "singbox-pc" || target == "sing-box-pc") {
                out_payload = gen_singbox(all_proxies, "pc", all_rules, all_balancers, source_rt->force_balancer);
                content_type = converted_content_type(target);
            } else if (target == "singbox-android" || target == "sing-box-android") {
                out_payload = gen_singbox(all_proxies, "android", all_rules, all_balancers, source_rt->force_balancer);
                content_type = converted_content_type(target);
            } else if (target == "xray-one" || target == "xray-json" || target == "v2ray-json") {
                out_payload = gen_xray(all_proxies, source_rt->name, all_rules);
                content_type = converted_content_type(target);
            } else if (target == "xray-jsons") {
                out_payload = gen_xray_jsons(all_proxies, all_balancers, all_rules);
                content_type = converted_content_type(target);
            } else if (target == "xray" || target == "v2ray") {
                out_payload = gen_v2ray(all_proxies);
                content_type = converted_content_type(target);
            } else {
                content_type = meta.content_type.empty() ? "text/plain; charset=utf-8" : meta.content_type;
            }
        } catch (const std::invalid_argument& e) {
            const std::string message = std::string(e.what()) + "\n";
            const std::string response = std::format(
                "HTTP/1.1 422 Unprocessable Content\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: {}\r\nConnection: close\r\n\r\n{}", message.size(), message);
            send(c, response.data(), static_cast<int>(response.size()), 0);
            logm("  [FAIL] Port %d (target=%s) -> 422 Conversion Failed: %s\n\n",
                 log_port, target.c_str(), e.what());
            mem_free(body);
            return;
        }

        apply_route_name(meta, source_rt->name);
        apply_subscription_parameters(meta, *source_rt, overrides, app);
        // Both format and metadata can depend on User-Agent. Subscription
        // responses can include credentials (including the fallback URL).
        const std::string extra_hdrs = metadata_headers(meta) +
            "Vary: User-Agent\r\nCache-Control: no-store\r\n";

        std::string hdr = std::format("HTTP/1.1 200 OK\r\n"
                                      "Content-Type: {}\r\n"
                                      "Content-Length: {}\r\n"
                                      "{}"
                                      "Connection: close\r\n\r\n",
                                      content_type, out_payload.length(), extra_hdrs);
        if (!send_all(c, hdr) || !send_all(c, out_payload)) {
            logm_fmt("  [FAIL] Port {} -> response write failed ({} ms)\n\n", log_port, elapsed_ms());
        } else {
            logm_fmt("  [OK] Port {} (target={}) -> 200 OK ({} bytes, {} ms)\n\n",
                log_port, target.empty() ? "raw" : target, out_payload.size(), elapsed_ms());
        }
    } else {
        constexpr std::string_view e = "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nContent-Length: 15\r\nConnection: close\r\n\r\nupstream failed";
        send(c, e.data(), static_cast<int>(e.length()), 0);
        logm_fmt("  [FAIL] Port {} -> 502 Upstream Failed ({} ms)\n\n", log_port, elapsed_ms());
    }
    mem_free(body);
}

void handle_subconverter(SOCKET c, std::string_view req) {
    size_t q_pos = req.find('?');
    size_t space_pos = req.find(' ', q_pos);
    if (q_pos == std::string_view::npos || space_pos == std::string_view::npos) {
        constexpr std::string_view r = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        send(c, r.data(), static_cast<int>(r.length()), 0);
        return;
    }

    std::string_view qs = req.substr(q_pos + 1, space_pos - (q_pos + 1));
    std::string target, url;
    int force_balancer = -1;
    size_t start = 0;
    while (start < qs.length()) {
        size_t amp = qs.find('&', start);
        if (amp == std::string_view::npos) amp = qs.length();
        std::string_view kv = qs.substr(start, amp - start);
        size_t eq = kv.find('=');
        if (eq != std::string_view::npos) {
            std::string_view k = kv.substr(0, eq);
            std::string v = url_decode(kv.substr(eq + 1));
            if (k == "target") target = v;
            else if (k == "url") url = v;
            else if (k == "force_balancer") {
                force_balancer = (v == "true" || v == "1") ? 1 : 0;
            }
        }
        start = amp + 1;
    }

    if (url.empty()) {
        constexpr std::string_view r = "HTTP/1.1 400 Missing URL\r\nConnection: close\r\n\r\n";
        send(c, r.data(), static_cast<int>(r.length()), 0);
        return;
    }

    Route temp_rt{};
    int internal_port = 0;
    if (url.starts_with("http://127.0.0.1:")) {
        internal_port = atoi(url.c_str() + 17);
    }

    const Route *source_rt = &temp_rt;
    if (internal_port > 0) {
        for (int i = 0; i < g_RouteCount; i++) {
            if (g_Routes[i].local_port == internal_port) {
                temp_rt = g_Routes[i];
                source_rt = &temp_rt;
                break;
            }
        }
    }
    if (source_rt == &temp_rt && temp_rt.url_count == 0) {
        copy_limited(temp_rt.urls[0], sizeof(temp_rt.urls[0]), url.c_str());
        temp_rt.url_count = 1;
    }
    if (force_balancer != -1) {
        temp_rt.force_balancer = force_balancer;
    }

    const auto app = subscription_app_for_user_agent(request_header(req, "User-Agent"));
    serve_converted_or_raw(c, source_rt, target, 25500, app);
}

void handle_client(SOCKET c, const Route *rt) {
#ifdef _WIN32
    DWORD tv = CL_TIMEOUT;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = CL_TIMEOUT / 1000;
    tv.tv_usec = (CL_TIMEOUT % 1000) * 1000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    // Log the connection before reading anything. A TCP connection can
    // succeed even if it never produces a complete HTTP request.
    const auto peer = g_IsCon ? peer_name(c) : std::string{};
    logm_fmt("  [TCP] Port {} connection from {}\n", rt->local_port, peer);

    // One TCP read is not necessarily one HTTP request. The User-Agent
    // (needed for app detection) may arrive in a later read.
    char req[16384];
    size_t used = 0;
    std::string_view req_view;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(CL_TIMEOUT);
    while (!req_view.contains("\r\n\r\n")) {
        if (used == sizeof(req)) {
            logm_fmt("  [TCP] Port {} {} headers too large\n", rt->local_port, peer);
            send_all(c, "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            closesocket(c);
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            logm_fmt("  [TCP] Port {} {} timed out waiting for HTTP headers ({})\n",
                     rt->local_port, peer, request_kind(req_view));
            closesocket(c);
            return;
        }
        const auto n = recv(c, req + used, static_cast<int>(sizeof(req) - used), 0);
        if (n <= 0) {
            if (n == 0) {
                logm_fmt("  [TCP] Port {} {} closed before HTTP headers ({})\n",
                         rt->local_port, peer, request_kind(req_view));
            } else {
#ifdef _WIN32
                const int error = WSAGetLastError();
#else
                const int error = errno;
#endif
                logm_fmt("  [TCP] Port {} {} could not read HTTP headers ({}, socket error {})\n",
                         rt->local_port, peer, request_kind(req_view), error);
            }
            closesocket(c);
            return;
        }
        used += n;
        req_view = std::string_view(req, used);
        if (starts_with_tls(req_view)) {
            logm_fmt("  [TCP] Port {} {} sent TLS to a plain HTTP listener\n", rt->local_port, peer);
            closesocket(c);
            return;
        }
    }
    req_view = req_view.substr(0, req_view.find("\r\n\r\n") + 2);

    if (!req_view.starts_with("GET ")) {
        logm_fmt("  [HTTP] Port {} {} unexpected {}\n", rt->local_port, peer,
                 request_kind(req_view));
        constexpr std::string_view r = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, r.data(), static_cast<int>(r.length()), 0);
        closesocket(c);
        return;
    }

    if (rt->is_subconverter) {
        handle_subconverter(c, req_view);
        shutdown(c, SD_SEND);
        closesocket(c);
        return;
    }

    logm("  [REQ] Port %d\n", rt->local_port);
    const auto user_agent = request_header(req_view, "User-Agent");
    const auto app = subscription_app_for_user_agent(user_agent);

    if (rt->is_convert) {
        const Route *source_rt = rt;
        for (int i = 0; i < g_RouteCount; i++) {
            if (!g_Routes[i].is_convert && !g_Routes[i].is_subconverter && g_Routes[i].local_port == rt->base_port) {
                source_rt = &g_Routes[i];
                break;
            }
        }
        serve_converted_or_raw(c, source_rt, rt->target, rt->local_port, app);
    } else {
        // Preserve native subscription formats for Happ/INCY with a single
        // upstream. Normalize multi-source app routes instead of concatenating
        // standalone JSON arrays or Base64 payloads.
        std::string auto_target;
        if (app != SubscriptionApp::other && rt->url_count > 1) auto_target = "v2ray";
        else if (app == SubscriptionApp::other) {
            auto_target = target_from_user_agent(user_agent);
        }
        if (!auto_target.empty()) {
            logm("  [AUTO] Port %d UA target=%s\n", rt->local_port, auto_target.c_str());
        }
        serve_converted_or_raw(c, rt, auto_target, rt->local_port, app);
    }

    shutdown(c, SD_SEND);
    closesocket(c);
}

SOCKET make_listener(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;

    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof(on));

    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(static_cast<u_short>(port));

    if (bind(s, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) == SOCKET_ERROR || listen(s, 4) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

void server_loop() {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int active_sockets = 0;
    for (int i = 0; i < g_RouteCount; i++) {
        g_Routes[i].listen_sock = make_listener(g_Routes[i].local_port);
        if (g_Routes[i].listen_sock != INVALID_SOCKET) {
            logm("Listening on 0.0.0.0:%d%s\n",
                 g_Routes[i].local_port,
                 g_Routes[i].is_subconverter ? " (embedded subconverter)" : (g_Routes[i].is_convert ? " (convert)" : " (base)"));
            active_sockets++;
        } else {
            logm("FATAL: cannot bind 0.0.0.0:%d\n", g_Routes[i].local_port);
        }
    }

    if (active_sockets == 0) {
        logm("No valid ports to listen on.\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    for (;;) {
#ifdef _WIN32
        if (WaitForSingleObject(g_Stop, 0) == WAIT_OBJECT_0) break;
#else
        if (g_Stop) break;
#endif

        fd_set rfds;
        FD_ZERO(&rfds);
        SOCKET max_sd = 0;

        for (int i = 0; i < g_RouteCount; i++) {
            SOCKET s = g_Routes[i].listen_sock;
            if (s != INVALID_SOCKET) {
                FD_SET(s, &rfds);
                if (s > max_sd) max_sd = s;
            }
        }

        struct timeval tv = { 1, 0 };
        int r = select(static_cast<int>(max_sd + 1), &rfds, nullptr, nullptr, &tv);

        if (r > 0) {
            for (int i = 0; i < g_RouteCount; i++) {
                SOCKET s = g_Routes[i].listen_sock;
                if (s != INVALID_SOCKET && FD_ISSET(s, &rfds)) {
                    SOCKET c = accept(s, nullptr, nullptr);
                    if (c != INVALID_SOCKET) {
                        std::thread([c, i]() {
                            handle_client(c, &g_Routes[i]);
                        }).detach();
                    }
                }
            }
        }
    }

    for (int i = 0; i < g_RouteCount; i++) {
        if (g_Routes[i].listen_sock != INVALID_SOCKET) {
            closesocket(g_Routes[i].listen_sock);
        }
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

