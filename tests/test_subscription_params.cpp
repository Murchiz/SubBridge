#include "subscription_params.h"
#include "utils.h"
#include <cassert>
#include <cstdio>
#include <string>

// Synthetic fixtures only. Exercises happ.ini / incy.ini loading, User-Agent
// selection, stripping and providerid retention without network access.
static std::string header(const SubMetadata& meta, std::string_view name) {
    for (const auto& param : meta.parameters) {
        if (param.name == name) return param.value;
    }
    return "<missing>";
}

static Route& add_route(const char* section, const char* name) {
    Route& route = g_Routes[g_RouteCount++];
    std::snprintf(route.name, sizeof(route.name), "%s", name);
    std::snprintf(route.section, sizeof(route.section), "%s", section);
    route.local_port = 25501 + g_RouteCount;
    route.url_count = 1;
    return route;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    g_RouteCount = 0;
    // Sections match the config.ini section (Sub_Main) or the display name.
    Route& main_route = add_route("Sub_Main", "MainBundle");
    Route& stripped_route = add_route("Sub_Strip", "Stripped");
    stripped_route.strip_happ = true;
    load_subscription_parameters(argv[1]);

    assert(main_route.happ_parameters.size() == 3);
    assert(main_route.incy_parameters.size() == 1);
    assert(stripped_route.happ_parameters.empty());

    // Client selection uses product tokens, not substrings.
    const auto happ_client = subscription_app_for_user_agent("Happ/3.23.0 (Android)");
    assert(happ_client == SubscriptionApp::happ);
    assert(select_app_parameters(main_route, happ_client).size() == 3);
    assert(subscription_app_for_user_agent("INCY-Desktop/1.4") == SubscriptionApp::incy);
    assert(subscription_app_for_user_agent("UnhappyClient/1.0") == SubscriptionApp::other);
    assert(subscription_app_for_user_agent("v2rayNG/1.9") == SubscriptionApp::other);
    assert(select_app_parameters(main_route, SubscriptionApp::other).empty());

    // Happ overrides: set, replace and blank-remove; providerid is retained.
    {
        SubMetadata meta{};
        read_subscription_header(meta, "Providerid: upstream-provider\r\n");
        read_subscription_header(meta, "Announce: upstream\r\n");
        read_subscription_header(meta, "color-profile: upstream-theme\r\n");
        read_subscription_header(meta, "sort-order: ping\r\n");
        read_subscription_header(meta, "Content-Encoding: gzip\r\n");
        read_subscription_header(meta, "new-url: https://moved.example\r\n");
        apply_subscription_parameters(meta, main_route, main_route.happ_parameters);
        assert(header(meta, "providerid") == "local-provider");
        assert(header(meta, "hide-settings") == "1");
        assert(header(meta, "announce").empty());
        // Provider settings pass through by default, except migration.
        assert(header(meta, "color-profile") == "upstream-theme");
        assert(header(meta, "sort-order") == "ping");
        assert(header(meta, "content-encoding") == "<missing>");
        assert(header(meta, "new-url") == "<missing>");
        assert(header(meta, "fallback-url") == "<missing>"); // No HTTP upstream.
    }

    // Happ gets the original upstream as fallback, never a provider/ini one,
    // and never for merged routes or other clients.
    {
        Route& route = add_route("Sub_Fallback", "Fallback");
        std::snprintf(route.urls[0], sizeof(route.urls[0]), "%s",
                      "https://provider.example/sub/token?x=1#Name?providerid=abc");
        const std::vector<SubscriptionParameter> overrides{{"fallback-url", "https://ini.example"}};
        route.strip_happ = true; // Fallback is a local route property, not an app directive.
        SubMetadata meta{};
        read_subscription_header(meta, "fallback-url: https://upstream.example");
        read_subscription_header(meta, "new-domain: moved.example");
        apply_subscription_parameters(meta, route, overrides, SubscriptionApp::happ);
        assert(header(meta, "fallback-url") == "https://provider.example/sub/token?x=1#Name?providerid=abc");
        assert(header(meta, "new-domain") == "<missing>");
        SubMetadata other{};
        apply_subscription_parameters(other, route, {}, SubscriptionApp::incy);
        assert(header(other, "fallback-url") == "<missing>");
        route.url_count = 2;
        SubMetadata merged = meta; // A previously selected fallback must not survive.
        apply_subscription_parameters(merged, route, overrides, SubscriptionApp::happ);
        assert(header(merged, "fallback-url") == "<missing>");
        g_RouteCount--;
    }

    // Converted output discards the link list, so body directives become
    // headers; migration directives are still dropped.
    {
        SubMetadata meta{};
        std::string filtered;
        filter_subscription_payload("#hide-settings: 1\n#new-url: https://x\nvless://a@h:1#n\n",
            main_route, {}, meta, filtered, true);
        assert(header(meta, "hide-settings") == "1");
        assert(header(meta, "new-url") == "<missing>");
    }

    // Stripping removes upstream app directives, but keeps the provider ID,
    // quota and links in both plain and Base64 link lists.
    constexpr std::string_view body =
        "#providerid body-provider\n"
        "#profile-title: Title\n"
        "#announce: upstream\n"
        "#hide-settings: 1\n"
        "#hide-url: 1\n"
        "#NEW-URL: https://moved.example/sub\n"
        "#new-domain: moved.example\n"
        "#fallback-url: https://upstream-fallback.example/sub\n"
        "happ://routing/onadd/abc\n"
        "://autorouting/onadd/https://example.com/profile.json\n"
        "vless://00000000-0000-0000-0000-000000000001@198.51.100.1:443#node\n";
    for (const bool encoded : {false, true}) {
        SubMetadata meta{};
        read_subscription_header(meta, "subscription-userinfo: upload=0; download=1");
        read_subscription_header(meta, "support-url: https://support.example");
        const std::string payload = encoded ? base64_encode(body) : std::string(body);
        std::string filtered;
        const auto out = filter_subscription_payload(payload, stripped_route,
            {}, meta, filtered);
        const std::string text = encoded ? base64_decode(out) : std::string(out);
        assert(text.contains("#providerid body-provider\n"));
        assert(text.contains("#profile-title: Title\n"));
        assert(!text.contains("#announce"));
        assert(!text.contains("#hide-settings"));
        assert(!text.contains("#NEW-URL"));
        assert(!text.contains("#new-domain"));
        assert(!text.contains("#fallback-url"));
        assert(text.contains("#hide-url: 1\n")); // INCY-only, untouched by strip_happ
        assert(text.contains("://autorouting/onadd/"));
        assert(!text.contains("happ://routing"));
        assert(text.contains("vless://"));
        apply_subscription_parameters(meta, stripped_route, {});
        assert(header(meta, "providerid") == "body-provider");
        assert(header(meta, "subscription-userinfo") == "upload=0; download=1");
        assert(header(meta, "support-url") == "<missing>"); // shared key, stripped
    }

    stripped_route.strip_incy = true;
    {
        SubMetadata meta{};
        std::string filtered;
        const auto out = filter_subscription_payload(body, stripped_route, {}, meta, filtered);
        assert(!out.contains("#hide-url:"));
        assert(!out.contains("://autorouting/onadd/"));
        assert(header(meta, "providerid") == "body-provider");
    }

    // Unchanged bodies keep their exact bytes; override keys are removed from
    // the body so the configured header is the only value the app sees.
    {
        SubMetadata meta{};
        std::string filtered;
        constexpr std::string_view untouched = "vless://a@198.51.100.1:443#n\n";
        assert(filter_subscription_payload(untouched, main_route, {}, meta, filtered).data() == untouched.data());
        const auto out = filter_subscription_payload("#hide-settings: 0\n#sub-info-text: x\nvless://a@h:1#n\n",
            main_route, main_route.happ_parameters, meta, filtered);
        assert(out == "#sub-info-text: x\nvless://a@h:1#n\n");
    }

    // Provider ID in the subscription URL fragment, and JSON is left alone.
    {
        SubMetadata meta{};
        read_provider_id_fragment(meta, "https://example.com/sub#Name?a=1&providerid=frag%2Did");
        assert(header(meta, "providerid") == "frag-id");
        std::string filtered;
        constexpr std::string_view json = "[{\"remarks\":\"#hide-settings: 1\"}]";
        assert(filter_subscription_payload(json, stripped_route, {}, meta, filtered) == json);
    }

    // Judging the reported case: a JSON upstream whose app settings arrive as
    // headers. strip_happ removes them, happ.ini replaces them, and the
    // upstream providerid survives.
    {
        Route& gru = add_route("Sub_Gru", "GruVPN");
        gru.strip_happ = true;
        gru.happ_parameters = {{"profile-update-interval", "6"}, {"profile-title", "Test"}};
        SubMetadata meta{};
        for (const auto line : {"providerid: upstream-provider", "announce: base64:dXA=",
                                "profile-update-interval: 3", "support-url: https://t.me/x",
                                "profile-title: base64:R3J1"}) {
            read_subscription_header(meta, line);
        }
        apply_subscription_parameters(meta, gru, select_app_parameters(gru, SubscriptionApp::happ));
        assert(header(meta, "providerid") == "upstream-provider");
        assert(header(meta, "announce") == "<missing>");
        assert(header(meta, "support-url") == "<missing>");
        assert(header(meta, "profile-update-interval") == "6");
        assert(header(meta, "profile-title") == "Test");
    }

    std::println("Subscription parameter tests passed.");
}
