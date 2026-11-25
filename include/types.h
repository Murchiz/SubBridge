#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>
#include <signal.h>
#include <climits>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_PATH PATH_MAX
typedef int SOCKET;
inline constexpr int INVALID_SOCKET = -1;
inline constexpr int SOCKET_ERROR = -1;
typedef unsigned int DWORD;
#endif

#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <utility>

inline constexpr const char* SVC_NAME = "SubBridge";
inline constexpr const char* SVC_DISPLAY = "Subscription Converter Bridge";
inline constexpr const wchar_t* UA_WIDE = L"Happ/3.23.0";

inline constexpr std::size_t BODY_CAP = 2048 * 1024;
inline constexpr uint32_t UP_TIMEOUT = 15000;
inline constexpr uint32_t CL_TIMEOUT = 5000;

struct DevInfo {
    char hwid[128]{};
    char os[64]{};
    char ver[64]{};
    char model[256]{};
};

struct SubscriptionParameter {
    std::string name{};
    std::string value{};
};

struct SubMetadata {
    std::string content_type{};
    // Subscription headers forwarded to clients (subscription-userinfo,
    // profile-title, providerid, Happ/INCY app-management parameters, ...).
    // Names are lowercase; the first source for a name wins.
    std::vector<SubscriptionParameter> parameters{};
};

struct Route {
    int local_port{0};
    int is_convert{0};
    int base_port{0};
    char target[64]{};
    char name[128]{};
    char section[128]{}; // config.ini section, e.g. "Sub_Main"
    char urls[8][2048]{};
    int url_count{0};
    int use_hwid{0};
    int is_subconverter{0};
    char user_agents[8][128]{};
    int force_balancer{0};
    // Remove upstream Happ / INCY app-management parameters (config.ini).
    bool strip_happ{false};
    bool strip_incy{false};
    // Parameters added from happ.ini / incy.ini. An empty value removes it.
    std::vector<SubscriptionParameter> happ_parameters{};
    std::vector<SubscriptionParameter> incy_parameters{};
    SOCKET listen_sock{INVALID_SOCKET};
};

struct Balancer {
    char name[128]{};
    char type[32]{};
    std::vector<std::string> proxies{};
    // Retain upstream Xray configs intact for JSON output. The normalized
    // member list alone cannot represent strategies, selectors or routing.
    std::string xray_config{};
};

struct Rule {
    char outbound[64]{};
    std::vector<std::string> domains{};
    std::vector<std::string> ips{};
    std::vector<std::string> protocols{};
    std::string port{};
    std::string network{};
};

struct Proxy {
    char protocol[16]{};
    char name[128]{};
    char server[128]{};
    int port{0};
    char uuid[64]{};
    char type[32]{};
    char security[32]{};
    char sni[128]{};
    char fp[64]{};
    char pbk[128]{};
    char sid[64]{};
    char flow[64]{};
    char path[128]{};
    char host[128]{};
    char alterId[16]{};
    char cipher[4096]{};
    char alpn[64]{};
    char mode[32]{};
    char extra[4096]{};
    char obfs[32]{};
    char obfs_pass[128]{};
    char up[32]{};
    char down[32]{};
    // Native Xray Hysteria stream options that have no lossless generic mapping.
    std::string xray_stream_settings{};
};

extern Route g_Routes[64];
extern int g_RouteCount;
extern DevInfo g_Dev;
#ifdef _WIN32
extern SERVICE_STATUS g_Svc;
extern SERVICE_STATUS_HANDLE g_SvcH;
extern HANDLE g_Stop;
#else
extern volatile sig_atomic_t g_Stop;
#endif
extern int g_IsCon;
extern char g_ExeDir[MAX_PATH];

void logm(const char *fmt, ...);

template <typename... Args>
inline void logm_fmt(std::format_string<Args...> fmt, Args&&... args) {
    if (!g_IsCon) return;
    std::string s = std::format(fmt, std::forward<Args>(args)...);
    logm("%s", s.c_str());
}


