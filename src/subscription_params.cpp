#include "subscription_params.h"
#include "utils.h"
#include <algorithm>
#include <fstream>
#include <map>

namespace {

// App-management directives from the Happ and INCY documentation. The title,
// traffic quota, refill date and providerid are deliberately absent, so
// stripping app settings never hides the subscription's identity or usage.
constexpr std::string_view shared_keys[] = {
    "profile-update-interval", "support-url", "profile-web-page-url", "announce",
    "per-app-proxy-mode", "per-app-proxy-list",
    "fragmentation-enable", "fragmentation-packets", "fragmentation-length",
    "fragmentation-interval", "noises-enable", "noises-packet", "noises-delay",
    "server-address-resolve-enable", "server-address-resolve-dns-domain",
    "server-address-resolve-dns-ip", "no-limit-enabled", "routing"
};

constexpr std::string_view happ_keys[] = {
    "routing-enable", "custom-tunnel-config", "socks-auth-mode", "socks-auth-user",
    "socks-auth-password", "http-auth-mode", "http-auth-user", "http-auth-password",
    "tun-type", "sub-info-color", "sub-info-text", "sub-info-button-text",
    "sub-info-button-link", "sub-expire", "sub-expire-button-link", "no-limit-xhttp-enabled",
    "subscription-always-hwid-enable", "notification-subs-expire", "hide-settings",
    "subscription-autoconnect", "subscription-autoconnect-type",
    "subscription-ping-onopen-enabled", "subscription-auto-update-enable",
    "fragmentation-maxsplit", "noises-packet-type", "noises-rand", "noises-rand-range",
    "ping-type", "check-url-via-proxy", "change-user-agent", "app-auto-start",
    "subscription-auto-update-open-enable", "per-app-proxy-list-invert", "per-app-proxy-list-set",
    "sniffing-enable", "subscriptions-collapse", "subscriptions-expand-now", "ping-result",
    "mux-enable", "mux-tcp-connections", "mux-xudp-connections", "mux-quic", "proxy-enable",
    "tun-enable", "tun-mode", "exclude-routes", "exclude-routes-set", "color-profile",
    "include-all-networks-enable", "exclude-local-networks-enable", "exclude-apns-enable",
    "dont-use-filter", "autoswitch-on-wifi", "autoswitch-on-wifi-timeout", "subscription-pin",
    "manual-block-user-agent", "subscriptions-sort-type", "dns-from-json-enable",
    "user-agent-geo-files", "proxy-ping-timeout", "hide-vpn-icon", "subscription-request-timeout",
    "inbound-http-enable", "xray-tun-enable", "xray-tun-mtu", "block-bind-to-tunnel-enable",
    "subscription-alternative-hwid-enabled", "proxy-ping-mode", "per-app-proxy-mode-windows",
    "per-app-proxy-mode-macos", "per-app-proxy-mode-linux", "per-app-proxy-list-windows-add",
    "per-app-proxy-list-windows-set", "per-app-proxy-list-macos-add", "per-app-proxy-list-macos-set",
    "per-app-proxy-list-linux-add", "per-app-proxy-list-linux-set"
};

constexpr std::string_view incy_keys[] = {
    "subscription-name", "profile-description", "routing-geo-update-interval", "sort-order",
    "support-email", "homepage", "announce-url", "autorouting", "premium-url", "banner-text",
    "banner-button-text", "banner-button-url", "banner-url", "banner-bg-color",
    "banner-button-color", "expiry-banner-text", "expiry-banner-button-text",
    "expiry-banner-button-url", "expiry-banner-url", "expiry-banner-bg-color",
    "expiry-banner-button-color", "expiry-banner-text-color", "expiry-banner-button-text-color",
    "hide-url", "hide-check", "per-app-proxy-enable", "noises-type"
};

// Subscription identity rather than app settings. A client that converts the
// link list loses body directives, so these body forms become headers.
constexpr std::string_view identity_parameters[] = {
    "subscription-userinfo", "content-disposition", "profile-title",
    "subscription-refill-date", "providerid"
};

constexpr std::string_view base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/-_= \t\r\n";
constexpr std::string_view utf8_bom = "\xef\xbb\xbf";

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
}

std::string_view trim(std::string_view s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    return start == s.npos ? std::string_view{} :
        s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

bool is_app_parameter(std::string_view name, bool happ, bool incy) {
    return ((happ || incy) && std::ranges::contains(shared_keys, name)) ||
           (happ && std::ranges::contains(happ_keys, name)) ||
           (incy && std::ranges::contains(incy_keys, name));
}

bool bridge_managed(std::string_view name) {
    // Migration would replace the bridge address. The fallback is derived
    // from this route, never from an upstream response or override file.
    return name == "new-url" || name == "new-domain" || name == "fallback-url";
}

bool is_subscription_parameter(std::string_view name) {
    return std::ranges::contains(identity_parameters, name) || is_app_parameter(name, true, true);
}

// Stripping is route-wide: every endpoint and client of the subscription gets
// the same filtered upstream data. Shared keys go when either flag is set.
bool stripped(std::string_view name, const Route& route) {
    return is_app_parameter(name, route.strip_happ, route.strip_incy);
}

bool valid_header_value(std::string_view value) {
    return value.size() <= 8192 && std::ranges::all_of(value, [](unsigned char c) {
        return c == '\t' || (c >= ' ' && c != 0x7f);
    });
}

void set_parameter(std::vector<SubscriptionParameter>& parameters, std::string_view name,
                   std::string_view value, bool overwrite = true) {
    const auto it = std::ranges::find(parameters, name, &SubscriptionParameter::name);
    if (it == parameters.end()) parameters.push_back({std::string(name), std::string(value)});
    else if (overwrite) it->value = value;
}

bool accepted_key(std::string_view name, bool happ) {
    return name == "profile-title" || (happ && name == "providerid") ||
           is_app_parameter(name, happ, !happ);
}

void load_app_ini(const std::filesystem::path& path, bool happ) {
    std::ifstream file(path);
    if (!file) return;

    std::map<std::string, std::vector<SubscriptionParameter>> sections;
    std::vector<SubscriptionParameter>* current = nullptr;
    std::string raw;
    unsigned line_number = 0;
    while (std::getline(file, raw)) {
        auto line = trim(raw);
        if (++line_number == 1 && line.starts_with(utf8_bom)) line = trim(line.substr(utf8_bom.size()));
        if (line.empty() || line.starts_with('#') || line.starts_with(';')) continue;
        if (line.starts_with('[')) {
            const auto end = line.find(']');
            const auto section = end != line.npos ? trim(line.substr(1, end - 1)) : std::string_view{};
            current = section.empty() ? nullptr : &sections[ascii_lower(section)];
            continue;
        }
        const auto eq = line.find('=');
        if (!current || eq == line.npos) continue;
        const auto name = ascii_lower(trim(line.substr(0, eq)));
        const auto value = trim(line.substr(eq + 1));
        if (!accepted_key(name, happ) || !valid_header_value(value)) {
            logm("Warning: ignoring invalid or unsupported parameter in %s:%u\n",
                 happ ? "happ.ini" : "incy.ini", line_number);
            continue;
        }
        // A blank Provider ID must not hide the upstream identity.
        if (name == "providerid" && value.empty()) continue;
        // A blank value removes that parameter from the response.
        set_parameter(*current, name, value);
    }

    const std::span routes(g_Routes, g_RouteCount);
    for (auto& route : routes) {
        if (route.is_convert || route.is_subconverter) continue;
        auto it = sections.find(ascii_lower(route.section));
        if (it == sections.end() && route.name[0]) {
            const auto alias = ascii_lower(route.name);
            // Use the legacy display-name alias only if no config.ini section
            // owns that name. An explicit section always wins over its alias.
            if (!std::ranges::any_of(routes, [&](const Route& r) { return ascii_lower(r.section) == alias; })) {
                it = sections.find(alias);
            }
        }
        if (it == sections.end()) continue;
        auto& overrides = happ ? route.happ_parameters : route.incy_parameters;
        overrides = it->second;
        logm("Loaded %zu parameters from %s for port %d\n", overrides.size(),
             happ ? "happ.ini" : "incy.ini", route.local_port);
    }
}

} // namespace

void load_subscription_parameters(const std::filesystem::path& directory) {
    load_app_ini(directory / "happ.ini", true);
    load_app_ini(directory / "incy.ini", false);
}

SubscriptionApp subscription_app_for_user_agent(std::string_view user_agent) {
    // Match a product token: "Happ/3.23.0 (Linux)" and "INCY-Desktop/1.0" work,
    // but names such as "UnhappyClient" do not match.
    const auto ua = ascii_lower(user_agent);
    for (size_t pos = 0; pos < ua.size();) {
        pos = ua.find_first_not_of(" \t();,", pos);
        if (pos == ua.npos) break;
        const auto end = ua.find_first_of(" \t();,", pos);
        const auto token = std::string_view(ua).substr(pos, end == ua.npos ? end : end - pos);
        if (token == "happ" || token.starts_with("happ-") || token.starts_with("happ/")) {
            return SubscriptionApp::happ;
        }
        if (token == "incy" || token.starts_with("incy-") || token.starts_with("incy/")) {
            return SubscriptionApp::incy;
        }
        if (end == ua.npos) break;
        pos = end + 1;
    }
    return SubscriptionApp::other;
}

std::span<const SubscriptionParameter> select_app_parameters(const Route& route, SubscriptionApp app) {
    if (app == SubscriptionApp::happ) return route.happ_parameters;
    if (app == SubscriptionApp::incy) return route.incy_parameters;
    return {};
}

void read_subscription_header(SubMetadata& meta, std::string_view line) {
    const auto colon = line.find(':');
    if (colon == line.npos) return;
    const auto name = ascii_lower(line.substr(0, colon));
    const auto value = trim(line.substr(colon + 1));
    if (value.empty() || !valid_header_value(value)) return;
    if (name == "content-type") meta.content_type = value;
    // Subscription settings only; transport and proxy-control headers describe
    // the upstream response, not the one SubBridge is generating.
    else if (is_subscription_parameter(name)) {
        set_parameter(meta.parameters, name, value, false);
    }
}

void set_subscription_parameter(SubMetadata& meta, std::string_view name, std::string_view value, bool overwrite) {
    set_parameter(meta.parameters, name, value, overwrite);
}

void read_provider_id_fragment(SubMetadata& meta, std::string_view url) {
    if (!url.starts_with("http://") && !url.starts_with("https://")) return;
    const auto fragment = url.find('#');
    const auto query = fragment == url.npos ? url.npos : url.find('?', fragment + 1);
    if (query == url.npos) return;
    auto params = url.substr(query + 1);
    while (!params.empty()) {
        const auto end = params.find('&');
        const auto param = params.substr(0, end);
        const auto eq = param.find('=');
        if (eq != param.npos && ascii_lower(param.substr(0, eq)) == "providerid") {
            const auto value = url_decode(param.substr(eq + 1));
            if (!value.empty() && valid_header_value(value)) set_parameter(meta.parameters, "providerid", value, false);
            return;
        }
        if (end == params.npos) break;
        params.remove_prefix(end + 1);
    }
}

std::string_view filter_subscription_payload(std::string_view payload, const Route& route,
    std::span<const SubscriptionParameter> overrides, SubMetadata& meta, std::string& filtered,
    bool converting) {
    auto text = trim(payload);
    if (text.starts_with(utf8_bom)) text.remove_prefix(utf8_bom.size());
    const bool encoded = !text.empty() && text.find_first_not_of(base64_chars) == text.npos;
    const auto decoded = encoded ? base64_decode(text) : std::string{};
    text = encoded ? std::string_view(decoded) : payload;
    auto probe = trim(text);
    if (probe.starts_with(utf8_bom)) probe = trim(probe.substr(utf8_bom.size()));
    // JSON and Clash YAML are passed through untouched.
    if (probe.starts_with('{') || probe.starts_with('[') ||
        probe.starts_with("proxies:") || probe.contains("\nproxies:")) return payload;

    const auto overridden = [&](std::string_view name) {
        return std::ranges::contains(overrides, name, &SubscriptionParameter::name);
    };
    std::vector<SubscriptionParameter> body_parameters;
    bool has_link = false;
    size_t kept_start = 0;
    for (size_t pos = 0; pos < text.size();) {
        const auto newline = text.find('\n', pos);
        const auto end = newline == text.npos ? text.size() : newline + 1;
        auto line = trim(text.substr(pos, end - pos));
        if (pos == 0 && line.starts_with(utf8_bom)) line = trim(line.substr(utf8_bom.size()));
        bool remove = false;
        if (line.starts_with('#')) {
            line = trim(line.substr(1));
            const auto sep = line.find_first_of(": \t");
            if (sep != line.npos) {
                const auto name = ascii_lower(line.substr(0, sep));
                auto value = trim(line.substr(sep));
                if (value.starts_with(':')) value = trim(value.substr(1));
                // An override in the ini replaces the directive in the body, so
                // clients that prefer body lines still see the configured value.
                remove = bridge_managed(name) || overridden(name) || stripped(name, route);
                if ((name == "providerid" || (converting && !remove && is_subscription_parameter(name))) &&
                    !value.empty() && valid_header_value(value)) {
                    set_parameter(body_parameters, name, value, false);
                }
            }
        } else if (line.starts_with("happ://routing/")) {
            remove = route.strip_happ || overridden("routing");
        } else if (line.starts_with("incy://routing/") || line.starts_with("://routing/") ||
                   line.starts_with("incy://onadd/") || line.starts_with("://onadd/")) {
            remove = route.strip_incy || overridden("routing");
        } else if (line.starts_with("incy://autorouting/") || line.starts_with("://autorouting/")) {
            remove = route.strip_incy || overridden("autorouting");
        } else {
            const auto scheme = line.find("://");
            has_link |= scheme != line.npos && scheme != 0 &&
                line.substr(0, scheme).find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-") == line.npos;
        }
        if (remove) {
            if (pos == 0 && text.starts_with(utf8_bom)) filtered.append(utf8_bom);
            filtered.append(text.substr(kept_start, pos - kept_start));
            kept_start = end;
        }
        pos = end;
    }
    // Only link lists are rewritten; unrelated bodies stay byte-for-byte.
    if (!has_link) return payload;
    for (const auto& param : body_parameters) {
        set_parameter(meta.parameters, param.name, param.value, false);
    }
    if (kept_start == 0) return payload;
    filtered.append(text.substr(kept_start));
    if (encoded) filtered = base64_encode(filtered);
    return filtered;
}

void apply_subscription_parameters(SubMetadata& meta, const Route& route,
    std::span<const SubscriptionParameter> overrides, SubscriptionApp app) {
    std::erase_if(meta.parameters, [&](const SubscriptionParameter& p) {
        return bridge_managed(p.name) || stripped(p.name, route);
    });
    for (const auto& param : overrides) {
        if (!bridge_managed(param.name)) set_parameter(meta.parameters, param.name, param.value);
    }
    if (app == SubscriptionApp::happ && route.url_count == 1) {
        const std::string_view url(route.urls[0]);
        if ((url.starts_with("https://") || url.starts_with("http://")) && valid_header_value(url)) {
            set_parameter(meta.parameters, "fallback-url", url);
        }
    }
}
