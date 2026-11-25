#pragma once
#include "types.h"
#include <filesystem>
#include <span>

enum class SubscriptionApp { other, happ, incy };

// Identifies the requesting client app from its User-Agent product token.
SubscriptionApp subscription_app_for_user_agent(std::string_view user_agent);

// happ.ini / incy.ini overrides for this route and client app. Empty for
// clients that are neither Happ nor INCY.
std::span<const SubscriptionParameter> select_app_parameters(const Route& route, SubscriptionApp app);

// Reads <directory>/happ.ini and incy.ini into the already loaded g_Routes.
void load_subscription_parameters(const std::filesystem::path& directory);

void read_subscription_header(SubMetadata& meta, std::string_view line);
void read_provider_id_fragment(SubMetadata& meta, std::string_view url);
void set_subscription_parameter(SubMetadata& meta, std::string_view name,
                                std::string_view value, bool overwrite = true);

// Removes the app directives selected by strip_happ / strip_incy from a link
// list, keeping the payload's bytes and encoding when nothing is removed.
// A body providerid is lifted into metadata. When converting, other supported
// body parameters are lifted too because the original link list is discarded.
std::string_view filter_subscription_payload(std::string_view payload, const Route& route,
    std::span<const SubscriptionParameter> overrides, SubMetadata& meta, std::string& filtered,
    bool converting = false);

// Applies strip flags and local overrides, then adds the original HTTP(S)
// upstream as Happ's fallback on single-source routes.
void apply_subscription_parameters(SubMetadata& meta, const Route& route,
    std::span<const SubscriptionParameter> overrides, SubscriptionApp app = SubscriptionApp::other);
