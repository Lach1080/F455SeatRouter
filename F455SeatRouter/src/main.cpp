// main.cpp
// Multi-ROI, seat-aware RealSense -> SYNKROS CMS bridge with auto-enrol
//
// IMPORTANT:
// 1) C++17 or later required (/std:c++17)
// 2) Link winhttp.lib and rsid.lib
// 3) Uses repeated Authenticate() calls, not AuthenticateLoop()
// 4) All runtime config is read from config.json next to the executable.
//    Edit config.json - do NOT hard-code values here.

#include <RealSenseID/FaceAuthenticator.h>
#include <RealSenseID/AuthenticationCallback.h>
#include <RealSenseID/EnrollmentCallback.h>
#include <RealSenseID/DeviceConfig.h>
#include <RealSenseID/Status.h>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// CONFIG STRUCTS
// ============================================================

struct SeatConfig
{
    std::string    seat_id;
    bool           enabled;
    unsigned short x;
    unsigned short y;
    unsigned short width;
    unsigned short height;
};

struct AssetRoute
{
    std::string seat_id;           // seat resolved from ROI logic
    std::string asset_id;          // used for logout payload
    std::string login_token_type;  // e.g. "assetId"
    std::string login_token_data;  // e.g. "8003"
};

struct CmsConfig
{
    std::wstring  host                     = L"10.16.0.12";
    INTERNET_PORT port                     = 9001;
    std::wstring  path_players             = L"/ssb/v1/players";
    std::wstring  path_logins              = L"/ssb/v1/egm-logins";
    std::wstring  path_logouts             = L"/ssb/v1/egm-logouts";
    std::string   card_create_path_template = "/ssb/v1/players/%d/cards";
    bool          ignore_cert_errors       = true;
};

struct EnrollmentDefaults
{
    std::string first_name    = "Guest";
    std::string last_name     = "Guest";
    std::string birth_date    = "1980-09-10";
    std::string address_line1 = "12 Bay Lane";
    std::string postal_code   = "1001";
    std::string country_code  = "AU";
    std::string player_pin    = "1111";
};

struct AppConfig
{
    std::string port = "COM3";
    RealSenseID::DeviceConfig::CameraRotation rotation =
        RealSenseID::DeviceConfig::CameraRotation::Rotation_90_Deg;

    int  idle_timeout_s              = 5;
    int  switch_delay_s              = 15;
    int  auto_enrol_min_interval_s   = 5;
    bool forbidden_causes_logout_when_locked = false;

    CmsConfig           cms;
    EnrollmentDefaults  enrol_defaults;
    std::vector<SeatConfig>  seats;
    std::vector<AssetRoute>  routes;
};

// ============================================================
// CONFIG LOADING  (reads config.json next to the .exe)
// ============================================================

static std::wstring GetExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto slash = path.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? path.substr(0, slash) : L".";
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

static RealSenseID::DeviceConfig::CameraRotation ParseRotation(const std::string& s)
{
    using R = RealSenseID::DeviceConfig::CameraRotation;
    if (s == "Rotation_0_Deg")   return R::Rotation_0_Deg;
    if (s == "Rotation_180_Deg") return R::Rotation_180_Deg;
    if (s == "Rotation_270_Deg") return R::Rotation_270_Deg;
    return R::Rotation_90_Deg; // default
}

AppConfig LoadAppConfig()
{
    AppConfig cfg; // populated with defaults above

    // Locate config.json next to the executable
    std::wstring exe_dir = GetExeDir();
    std::wstring config_path_w = exe_dir + L"\\config.json";
    std::string  config_path(config_path_w.begin(), config_path_w.end());

    std::ifstream f(config_path);
    if (!f.is_open())
    {
        std::printf("[CONFIG] WARNING: config.json not found at %s\n", config_path.c_str());
        std::printf("[CONFIG] Using built-in defaults. Copy config.json next to the exe.\n");
        return cfg;
    }

    try
    {
        auto j = nlohmann::json::parse(f, nullptr, /*exceptions=*/true, /*ignore_comments=*/true);

        if (j.contains("port"))
            cfg.port = j["port"].get<std::string>();

        if (j.contains("rotation"))
            cfg.rotation = ParseRotation(j["rotation"].get<std::string>());

        if (j.contains("idle_timeout_s"))
            cfg.idle_timeout_s = j["idle_timeout_s"].get<int>();

        if (j.contains("switch_delay_s"))
            cfg.switch_delay_s = j["switch_delay_s"].get<int>();

        if (j.contains("auto_enrol_min_interval_s"))
            cfg.auto_enrol_min_interval_s = j["auto_enrol_min_interval_s"].get<int>();

        if (j.contains("forbidden_causes_logout_when_locked"))
            cfg.forbidden_causes_logout_when_locked = j["forbidden_causes_logout_when_locked"].get<bool>();

        // CMS
        if (j.contains("cms"))
        {
            auto& jc = j["cms"];
            if (jc.contains("host"))
                cfg.cms.host = Utf8ToWide(jc["host"].get<std::string>());
            if (jc.contains("port"))
                cfg.cms.port = static_cast<INTERNET_PORT>(jc["port"].get<int>());
            if (jc.contains("path_players"))
                cfg.cms.path_players = Utf8ToWide(jc["path_players"].get<std::string>());
            if (jc.contains("path_logins"))
                cfg.cms.path_logins = Utf8ToWide(jc["path_logins"].get<std::string>());
            if (jc.contains("path_logouts"))
                cfg.cms.path_logouts = Utf8ToWide(jc["path_logouts"].get<std::string>());
            if (jc.contains("card_create_path_template"))
                cfg.cms.card_create_path_template = jc["card_create_path_template"].get<std::string>();
            if (jc.contains("ignore_cert_errors"))
                cfg.cms.ignore_cert_errors = jc["ignore_cert_errors"].get<bool>();
        }

        // Enrollment defaults
        if (j.contains("enrollment_defaults"))
        {
            auto& je = j["enrollment_defaults"];
            if (je.contains("first_name"))    cfg.enrol_defaults.first_name    = je["first_name"].get<std::string>();
            if (je.contains("last_name"))     cfg.enrol_defaults.last_name     = je["last_name"].get<std::string>();
            if (je.contains("birth_date"))    cfg.enrol_defaults.birth_date    = je["birth_date"].get<std::string>();
            if (je.contains("address_line1")) cfg.enrol_defaults.address_line1 = je["address_line1"].get<std::string>();
            if (je.contains("postal_code"))   cfg.enrol_defaults.postal_code   = je["postal_code"].get<std::string>();
            if (je.contains("country_code"))  cfg.enrol_defaults.country_code  = je["country_code"].get<std::string>();
            if (je.contains("player_pin"))    cfg.enrol_defaults.player_pin    = je["player_pin"].get<std::string>();
        }

        // Seats
        if (j.contains("seats") && j["seats"].is_array())
        {
            for (const auto& js : j["seats"])
            {
                SeatConfig sc{};
                if (js.contains("seat_id")) sc.seat_id = js["seat_id"].get<std::string>();
                if (js.contains("enabled")) sc.enabled = js["enabled"].get<bool>();
                if (js.contains("x"))       sc.x       = js["x"].get<unsigned short>();
                if (js.contains("y"))       sc.y       = js["y"].get<unsigned short>();
                if (js.contains("width"))   sc.width   = js["width"].get<unsigned short>();
                if (js.contains("height"))  sc.height  = js["height"].get<unsigned short>();
                cfg.seats.push_back(sc);
            }
        }

        // Routes
        if (j.contains("routes") && j["routes"].is_array())
        {
            for (const auto& jr : j["routes"])
            {
                AssetRoute ar{};
                if (jr.contains("seat_id"))          ar.seat_id          = jr["seat_id"].get<std::string>();
                if (jr.contains("asset_id"))         ar.asset_id         = jr["asset_id"].get<std::string>();
                if (jr.contains("login_token_type")) ar.login_token_type = jr["login_token_type"].get<std::string>();
                if (jr.contains("login_token_data")) ar.login_token_data = jr["login_token_data"].get<std::string>();
                cfg.routes.push_back(ar);
            }
        }

        std::printf("[CONFIG] Loaded config.json successfully.\n");
    }
    catch (const std::exception& ex)
    {
        std::printf("[CONFIG] ERROR parsing config.json: %s\n", ex.what());
        std::printf("[CONFIG] Falling back to built-in defaults.\n");
    }

    return cfg;
}

// ============================================================
// GLOBAL STATE
// ============================================================

enum class SessionState { Unlocked, LockedToUser, Cooldown };

struct AutoEnrolState
{
    SessionState state = SessionState::Unlocked;

    std::string  current_user_id;
    std::string  current_card_id;
    std::string  current_seat_id;
    AssetRoute   current_route{};

    std::chrono::steady_clock::time_point last_seen_owner{};
    std::chrono::steady_clock::time_point cooldown_until{};

    bool       pending_auto_enrol = false;
    std::string pending_seat_id;
    AssetRoute  pending_route{};
};

AutoEnrolState            g_state;
std::mutex                g_state_mtx;
std::atomic<bool>         g_running{true};
std::chrono::steady_clock::time_point g_last_auto_enrol{};

// ============================================================
// HELPERS
// ============================================================

size_t CountEnabledSeats(const AppConfig& cfg)
{
    size_t count = 0;
    for (const auto& seat : cfg.seats)
        if (seat.enabled) ++count;
    return count;
}

std::optional<AssetRoute> FindRouteForSeat(const AppConfig& cfg, const std::string& seat_id)
{
    for (const auto& route : cfg.routes)
        if (route.seat_id == seat_id) return route;
    return std::nullopt;
}

RealSenseID::DeviceConfig BuildDeviceConfig(const AppConfig& appCfg)
{
    const size_t enabled_count = CountEnabledSeats(appCfg);

    if (enabled_count < 1)
        throw std::runtime_error("At least one seat/ROI must be enabled in config.json.");
    if (enabled_count > RealSenseID::DeviceConfig::MAX_ROIS)
        throw std::runtime_error("Too many enabled seats in config.json. MAX_ROIS is 5.");

    RealSenseID::DeviceConfig cfg;
    cfg.security_level       = RealSenseID::DeviceConfig::SecurityLevel::Low;
    cfg.algo_flow            = RealSenseID::DeviceConfig::AlgoFlow::All;
    cfg.face_selection_policy = (enabled_count > 1)
        ? RealSenseID::DeviceConfig::FaceSelectionPolicy::All
        : RealSenseID::DeviceConfig::FaceSelectionPolicy::Single;
    cfg.rect_enable          = 0x01;
    cfg.landmarks_enable     = 0;
    cfg.camera_rotation      = appCfg.rotation;
    cfg.matcher_confidence_level = RealSenseID::DeviceConfig::MatcherConfidenceLevel::Low;
    cfg.frontal_face_policy  = RealSenseID::DeviceConfig::FrontalFacePolicy::None;
    cfg.person_motion_mode   = RealSenseID::DeviceConfig::PersonMotionMode::Static;
    cfg.distance_limit       = RealSenseID::DeviceConfig::DistanceLimit::NoLimit;
    cfg.distance_enabled     = false;
    cfg.num_rois             = static_cast<unsigned char>(enabled_count);

    size_t roi_index = 0;
    for (const auto& seat : appCfg.seats)
    {
        if (!seat.enabled) continue;
        cfg.detection_rois[roi_index].x      = seat.x;
        cfg.detection_rois[roi_index].y      = seat.y;
        cfg.detection_rois[roi_index].width  = seat.width;
        cfg.detection_rois[roi_index].height = seat.height;
        ++roi_index;
    }

    return cfg;
}

std::optional<std::string> ResolveSeatForFace(const RealSenseID::FaceRect& face, const AppConfig& appCfg)
{
    const int face_center_x = face.x + (face.w / 2);
    const int face_center_y = face.y + (face.h / 2);

    std::optional<std::string> matched_seat;

    for (const auto& seat : appCfg.seats)
    {
        if (!seat.enabled) continue;

        const int roi_left   = static_cast<int>(seat.x);
        const int roi_right  = static_cast<int>(seat.x) + static_cast<int>(seat.width);
        const int roi_top    = static_cast<int>(seat.y);
        const int roi_bottom = static_cast<int>(seat.y) + static_cast<int>(seat.height);

        const bool inside =
            (face_center_x >= roi_left  && face_center_x < roi_right) &&
            (face_center_y >= roi_top   && face_center_y < roi_bottom);

        if (inside)
        {
            if (matched_seat.has_value()) return std::nullopt; // ambiguous
            matched_seat = seat.seat_id;
        }
    }

    return matched_seat;
}

// ============================================================
// HTTP
// ============================================================

HINTERNET g_http_session = nullptr;
HINTERNET g_http_connect = nullptr;
std::mutex g_http_mtx;

bool init_http(const CmsConfig& cms)
{
    std::lock_guard<std::mutex> lock(g_http_mtx);
    if (g_http_session && g_http_connect) return true;

    if (!g_http_session)
    {
        g_http_session = WinHttpOpen(
            L"F455SeatRouter/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

        if (!g_http_session)
        {
            std::printf("[HTTP] ERROR: WinHttpOpen failed (err=%lu)\n", GetLastError());
            return false;
        }
    }

    if (!g_http_connect)
    {
        g_http_connect = WinHttpConnect(g_http_session, cms.host.c_str(), cms.port, 0);
        if (!g_http_connect)
        {
            std::printf("[HTTP] ERROR: WinHttpConnect failed (err=%lu)\n", GetLastError());
            WinHttpCloseHandle(g_http_session);
            g_http_session = nullptr;
            return false;
        }
    }

    return true;
}

struct HttpResponse { bool ok = false; int status = 0; std::string body; };

HttpResponse http_post_json(const CmsConfig& cms, const wchar_t* path, const std::string& json_body)
{
    using namespace std::chrono;
    HttpResponse result;

    if (!init_http(cms)) { std::printf("[HTTP] ERROR: init_http() failed.\n"); return result; }

    auto t_start = steady_clock::now();
    HINTERNET h_request = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_http_mtx);
        BOOL bRet = FALSE;
        LPCWSTR headers = L"Content-Type: application/json\r\n";

        h_request = WinHttpOpenRequest(
            g_http_connect, L"POST", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

        if (!h_request) goto cleanup;

        if (cms.ignore_cert_errors)
        {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(h_request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        }

        bRet = WinHttpSendRequest(h_request, headers, -1L,
            json_body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)json_body.data(),
            (DWORD)json_body.size(), (DWORD)json_body.size(), 0);
        if (!bRet) goto cleanup;

        bRet = WinHttpReceiveResponse(h_request, nullptr);
        if (!bRet) goto cleanup;

        {
            DWORD status_code = 0, status_size = sizeof(status_code);
            bRet = WinHttpQueryHeaders(h_request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                WINHTTP_NO_HEADER_INDEX);
            if (!bRet) goto cleanup;
            result.status = (int)status_code;
        }

        {
            std::string body;
            DWORD dw_size = 0;
            for (;;)
            {
                dw_size = 0;
                if (!WinHttpQueryDataAvailable(h_request, &dw_size)) break;
                if (dw_size == 0) break;
                std::vector<char> buffer(dw_size);
                DWORD dw_read = 0;
                if (!WinHttpReadData(h_request, buffer.data(), dw_size, &dw_read)) break;
                if (dw_read == 0) break;
                body.append(buffer.data(), dw_read);
            }
            result.body = std::move(body);
        }

    cleanup:
        if (h_request) { WinHttpCloseHandle(h_request); h_request = nullptr; }
    }

    auto ms = duration_cast<milliseconds>(steady_clock::now() - t_start).count();
    std::printf("[HTTP] POST %ls took %.1f ms (status=%d)\n", path, (double)ms, result.status);
    result.ok = (result.status != 0);
    return result;
}

int extract_int_field(const std::string& body, const std::string& field_name)
{
    std::string key = "\"" + field_name + "\":";
    auto pos = body.find(key);
    if (pos == std::string::npos) return 0;
    pos += key.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    bool any = false; int value = 0;
    while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9')
    { any = true; value = value * 10 + (body[pos] - '0'); ++pos; }
    return any ? value : 0;
}

// ============================================================
// CMS HELPERS
// ============================================================

struct NewPatronInfo { std::string user_id; std::string card_id; };

bool egm_login(const CmsConfig& cms, const AssetRoute& route, const std::string& card_id)
{
    std::string payload =
        "{\"cardId\":\"" + card_id +
        "\",\"loginTokenType\":\"" + route.login_token_type +
        "\",\"loginTokenData\":\"" + route.login_token_data + "\"}";

    std::wprintf(L"[EGM] LOGIN  POST https://%ls:%d%ls\n", cms.host.c_str(), (int)cms.port, cms.path_logins.c_str());
    std::printf("[EGM] LOGIN  payload = %s\n", payload.c_str());
    HttpResponse resp = http_post_json(cms, cms.path_logins.c_str(), payload);
    std::printf("[EGM] LOGIN  status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(), 300), resp.body.c_str());
    return resp.ok && resp.status >= 200 && resp.status < 300;
}

bool egm_logout(const CmsConfig& cms, const AssetRoute& route, const std::string& card_id)
{
    std::string payload =
        "{\"cardId\":\"" + card_id +
        "\",\"assetId\":\"" + route.asset_id + "\"}";

    std::wprintf(L"[EGM] LOGOUT POST https://%ls:%d%ls\n", cms.host.c_str(), (int)cms.port, cms.path_logouts.c_str());
    std::printf("[EGM] LOGOUT payload = %s\n", payload.c_str());
    HttpResponse resp = http_post_json(cms, cms.path_logouts.c_str(), payload);
    std::printf("[EGM] LOGOUT status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(), 300), resp.body.c_str());
    return resp.ok && resp.status >= 200 && resp.status < 300;
}

bool egm_auto_enrol(const AppConfig& cfg, NewPatronInfo& out_info)
{
    using namespace std::chrono;

    auto now = steady_clock::now();
    if (g_last_auto_enrol.time_since_epoch().count() != 0)
    {
        double since_last = duration_cast<milliseconds>(now - g_last_auto_enrol).count() / 1000.0;
        if (since_last < cfg.auto_enrol_min_interval_s)
        {
            std::printf("[EGM] AUTO-ENROL: Skipping; last attempt %.2fs ago.\n", since_last);
            return false;
        }
    }
    g_last_auto_enrol = now;

    std::string player_payload =
        "{\"firstName\":\"" + cfg.enrol_defaults.first_name +
        "\",\"lastName\":\"" + cfg.enrol_defaults.last_name +
        "\",\"birthDate\":\"" + cfg.enrol_defaults.birth_date +
        "\",\"address\":{\"addressLine1\":\"" + cfg.enrol_defaults.address_line1 +
        "\",\"postalCode\":\"" + cfg.enrol_defaults.postal_code +
        "\",\"countryCode\":\"" + cfg.enrol_defaults.country_code + "\"}}";

    std::wprintf(L"[EGM] AUTO-ENROL: POST https://%ls:%d%ls\n", cfg.cms.host.c_str(), (int)cfg.cms.port, cfg.cms.path_players.c_str());
    HttpResponse player_resp = http_post_json(cfg.cms, cfg.cms.path_players.c_str(), player_payload);
    std::printf("[EGM] AUTO-ENROL: /players status=%d body=%.*s\n", player_resp.status,
        (int)std::min<size_t>(player_resp.body.size(), 300), player_resp.body.c_str());

    if (!player_resp.ok || player_resp.status < 200 || player_resp.status >= 300)
    { std::printf("[EGM] AUTO-ENROL ERROR: /players call failed.\n"); return false; }

    int player_id = extract_int_field(player_resp.body, "playerId");
    if (player_id <= 0) { std::printf("[EGM] AUTO-ENROL ERROR: Could not parse playerId.\n"); return false; }

    char path_buf[256];
    std::snprintf(path_buf, sizeof(path_buf), cfg.cms.card_create_path_template.c_str(), player_id);
    std::wstring card_path_w = Utf8ToWide(path_buf);

    std::string card_payload = "{\"playerPin\":\"" + cfg.enrol_defaults.player_pin + "\"}";
    HttpResponse card_resp = http_post_json(cfg.cms, card_path_w.c_str(), card_payload);
    std::printf("[EGM] AUTO-ENROL: /cards status=%d body=%.*s\n", card_resp.status,
        (int)std::min<size_t>(card_resp.body.size(), 300), card_resp.body.c_str());

    if (!card_resp.ok || card_resp.status < 200 || card_resp.status >= 300)
    { std::printf("[EGM] AUTO-ENROL ERROR: /cards call failed.\n"); return false; }

    int card_id = extract_int_field(card_resp.body, "cardId");
    if (card_id <= 0) { std::printf("[EGM] AUTO-ENROL ERROR: Could not parse cardId.\n"); return false; }

    out_info.card_id = std::to_string(card_id);
    out_info.user_id = out_info.card_id; // camera user_id == CMS cardId for auto-enrolled patrons
    std::printf("[EGM] AUTO-ENROL SUCCESS: playerId=%d cardId=%d camera_user_id=%s\n",
        player_id, card_id, out_info.user_id.c_str());
    return true;
}

// ============================================================
// CALLBACKS
// ============================================================

class SimpleEnrolCallback : public RealSenseID::EnrollmentCallback
{
public:
    void OnResult(RealSenseID::EnrollStatus status) override
    { std::printf("[ENROL] OnResult: status=%d\n", (int)status); }

    void OnProgress(RealSenseID::FacePose pose) override
    { std::printf("[ENROL] OnProgress: pose=%d\n", (int)pose); }

    void OnHint(RealSenseID::EnrollStatus hint, float faceCoverage) override
    { std::printf("[ENROL] OnHint: hint=%d faceCoverage=%.2f\n", (int)hint, (double)faceCoverage); }
};

class AutoEnrolAuthCallback : public RealSenseID::AuthenticationCallback
{
public:
    explicit AutoEnrolAuthCallback(const AppConfig& cfg) : _cfg(cfg) {}

    void OnResult(RealSenseID::AuthenticateStatus status, const char* user_id, short score) override
    {
        using namespace std::chrono;
        using RealSenseID::AuthenticateStatus;

        const std::string uid = user_id ? user_id : "";

        std::optional<std::string> resolved_seat;
        size_t face_count = 0;
        {
            std::lock_guard<std::mutex> lock(_face_mtx);
            resolved_seat = _last_resolved_seat;
            face_count    = _last_face_count;
        }

        std::printf("[AUTH] OnResult: status=%d user_id=%s score=%d", (int)status, uid.c_str(), (int)score);
        if (face_count == 1 && resolved_seat.has_value())
            std::printf(" seat=%s", resolved_seat->c_str());
        else if (face_count > 1)
            std::printf(" seat=<multiple faces>");
        else
            std::printf(" seat=<none>");
        std::printf("\n");

        auto now = steady_clock::now();
        std::lock_guard<std::mutex> state_lock(g_state_mtx);

        const bool is_success  = (status == AuthenticateStatus::Success);
        const bool is_forbidden = (status == AuthenticateStatus::Forbidden);

        // LOCKED
        if (g_state.state == SessionState::LockedToUser && !g_state.current_card_id.empty())
        {
            if (is_success && uid == g_state.current_user_id)
            {
                g_state.last_seen_owner = now;
                std::printf("[SESSION] Owner %s re-confirmed on seat %s\n", uid.c_str(), g_state.current_seat_id.c_str());
                return;
            }
            if (is_success && uid != g_state.current_user_id)
            {
                std::printf("[SESSION] Other user %s seen while locked to %s - ignored\n", uid.c_str(), g_state.current_user_id.c_str());
                return;
            }
            if (is_forbidden)
            {
                std::printf("[SESSION] Forbidden while locked - %s\n",
                    _cfg.forbidden_causes_logout_when_locked ? "policy logout (not yet implemented)" : "ignored");
                return;
            }
            return;
        }

        // COOLDOWN
        if (g_state.state == SessionState::Cooldown)
        {
            if (is_success || is_forbidden)
                std::printf("[SESSION] In cooldown - ignoring auth result\n");
            return;
        }

        // UNLOCKED + Success
        if (is_success)
        {
            if (face_count != 1 || !resolved_seat.has_value())
            {
                std::printf("[SESSION] Success but no single resolved seat - ignored\n");
                return;
            }
            auto route = FindRouteForSeat(_cfg, *resolved_seat);
            if (!route.has_value())
            {
                std::printf("[SESSION] No route for seat %s\n", resolved_seat->c_str());
                return;
            }

            g_state.current_user_id  = uid;
            g_state.current_card_id  = uid; // camera user_id == cardId for auto-enrolled patrons
            g_state.current_seat_id  = *resolved_seat;
            g_state.current_route    = *route;
            g_state.last_seen_owner  = now;
            g_state.state            = SessionState::LockedToUser;

            std::string   card_id      = g_state.current_card_id;
            AssetRoute    current_route = g_state.current_route;
            CmsConfig     cms_copy     = _cfg.cms;

            std::printf("[SESSION] Locked: user=%s cardId=%s seat=%s asset=%s\n",
                uid.c_str(), card_id.c_str(), g_state.current_seat_id.c_str(), current_route.asset_id.c_str());

            std::thread([cms_copy, current_route, card_id]()
                { egm_login(cms_copy, current_route, card_id); }).detach();
            return;
        }

        // UNLOCKED + Forbidden -> pending auto-enrol
        if (is_forbidden)
        {
            if (face_count != 1 || !resolved_seat.has_value())
            {
                std::printf("[SESSION] Forbidden but no single resolved seat - not auto-enrolling\n");
                return;
            }
            auto route = FindRouteForSeat(_cfg, *resolved_seat);
            if (!route.has_value())
            {
                std::printf("[SESSION] Forbidden on seat %s but no route - not auto-enrolling\n", resolved_seat->c_str());
                return;
            }

            g_state.pending_auto_enrol = true;
            g_state.pending_seat_id    = *resolved_seat;
            g_state.pending_route      = *route;
            std::printf("[SESSION] Forbidden on seat=%s asset=%s -> pending auto-enrol\n",
                g_state.pending_seat_id.c_str(), g_state.pending_route.asset_id.c_str());
        }
    }

    void OnHint(RealSenseID::AuthenticateStatus hint, float frameScore) override
    { std::printf("[AUTH] OnHint: hint=%d frameScore=%.2f\n", (int)hint, (double)frameScore); }

    void OnFaceDetected(const std::vector<RealSenseID::FaceRect>& faces, const unsigned int timestamp) override
    {
        std::lock_guard<std::mutex> lock(_face_mtx);
        _last_face_count = faces.size();
        _last_resolved_seat.reset();

        std::printf("[FACE] faces=%zu timestamp=%u\n", faces.size(), timestamp);
        if (faces.empty()) return;

        for (size_t i = 0; i < faces.size(); ++i)
        {
            const auto& f = faces[i];
            auto seat = ResolveSeatForFace(f, _cfg);
            std::printf("  Face %zu x=%d y=%d w=%d h=%d center=(%d,%d) -> %s\n",
                i, f.x, f.y, f.w, f.h,
                f.x + f.w / 2, f.y + f.h / 2,
                seat.has_value() ? seat->c_str() : "none/ambiguous");
            if (faces.size() == 1) _last_resolved_seat = seat;
        }
    }

private:
    const AppConfig& _cfg;
    std::mutex _face_mtx;
    size_t _last_face_count = 0;
    std::optional<std::string> _last_resolved_seat;
};

// ============================================================
// IDLE MONITOR
// ============================================================

void idle_monitor(const AppConfig& cfg)
{
    using namespace std::chrono;
    std::printf("[BG] Idle monitor started: owner timeout=%ds cooldown=%ds\n",
        cfg.idle_timeout_s, cfg.switch_delay_s);

    while (g_running)
    {
        {
            std::lock_guard<std::mutex> lock(g_state_mtx);
            auto now = steady_clock::now();

            if (g_state.state == SessionState::LockedToUser && !g_state.current_card_id.empty())
            {
                auto elapsed = duration_cast<seconds>(now - g_state.last_seen_owner).count();
                if (elapsed >= cfg.idle_timeout_s)
                {
                    std::string card_id = g_state.current_card_id;
                    AssetRoute  route   = g_state.current_route;
                    std::printf("[BG] Idle timeout - logout cardId=%s seat=%s asset=%s\n",
                        card_id.c_str(), g_state.current_seat_id.c_str(), route.asset_id.c_str());

                    g_state.state = SessionState::Cooldown;
                    g_state.cooldown_until = now + seconds(cfg.switch_delay_s);
                    g_state.current_user_id.clear();
                    g_state.current_card_id.clear();
                    g_state.current_seat_id.clear();

                    std::thread([cms = cfg.cms, route, card_id]()
                        { egm_logout(cms, route, card_id); }).detach();
                }
            }

            if (g_state.state == SessionState::Cooldown && now >= g_state.cooldown_until)
            {
                std::printf("[BG] Cooldown finished - returning to Unlocked\n");
                g_state.state = SessionState::Unlocked;
            }
        }
        std::this_thread::sleep_for(milliseconds(200));
    }
}

// ============================================================
// MAIN
// ============================================================

int main()
{
    using namespace RealSenseID;

    try
    {
        AppConfig appCfg = LoadAppConfig();

        const size_t enabled_count = CountEnabledSeats(appCfg);
        std::printf("=== F455 Seat-Aware Auto-Enrol (F455SeatRouter) ===\n");
        std::printf("Enabled seats = %zu  |  Port = %s\n", enabled_count, appCfg.port.c_str());

        for (const auto& seat : appCfg.seats)
            if (seat.enabled)
                std::printf("  Seat %-8s ROI x=%4u y=%4u w=%4u h=%4u\n",
                    seat.seat_id.c_str(), seat.x, seat.y, seat.width, seat.height);

        for (const auto& route : appCfg.routes)
            std::printf("  Route %-8s -> asset=%-6s tokenType=%s tokenData=%s\n",
                route.seat_id.c_str(), route.asset_id.c_str(),
                route.login_token_type.c_str(), route.login_token_data.c_str());

        FaceAuthenticator     auth;
        AutoEnrolAuthCallback authCb(appCfg);

        std::printf("Connecting to camera on %s...\n", appCfg.port.c_str());
        auto st = auth.Connect({appCfg.port.c_str()});
        std::printf("Connect status = %d\n", (int)st);

        if (st != Status::Ok)
        {
            std::printf("ERROR: Failed to connect to camera.\n");
            std::printf("Press ENTER to exit.\n"); (void)getchar();
            return 1;
        }

        DeviceConfig device_cfg = BuildDeviceConfig(appCfg);
        auto cfgSt = auth.SetDeviceConfig(device_cfg);
        std::printf("SetDeviceConfig status = %d\n", (int)cfgSt);
        if (cfgSt != Status::Ok)
        {
            auth.Disconnect();
            std::printf("Press ENTER to exit.\n"); (void)getchar();
            return 1;
        }

        std::thread idleThread(idle_monitor, std::cref(appCfg));

        std::printf("Running. Close window or stop debugging to exit.\n");
        std::printf("  Unlocked + Success          -> CMS login for resolved seat\n");
        std::printf("  Idle timeout                -> CMS logout + cooldown\n");
        std::printf("  Unlocked + Forbidden + seat -> pending auto-enrol\n");

        while (g_running)
        {
            auto ast = auth.Authenticate(authCb);
            std::printf("Authenticate() returned Status=%d\n", (int)ast);

            if (ast != Status::Ok)
            {
                std::printf("[MAIN] Authenticate error; brief backoff.\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }

            bool       do_auto_enrol = false;
            std::string pending_seat;
            AssetRoute  pending_route{};

            {
                std::lock_guard<std::mutex> lock(g_state_mtx);
                if (g_state.pending_auto_enrol && g_state.state == SessionState::Unlocked)
                {
                    g_state.pending_auto_enrol = false;
                    do_auto_enrol  = true;
                    pending_seat   = g_state.pending_seat_id;
                    pending_route  = g_state.pending_route;
                }
            }

            if (do_auto_enrol)
            {
                std::printf("[MAIN] Running auto-enrol for seat=%s asset=%s\n",
                    pending_seat.c_str(), pending_route.asset_id.c_str());

                NewPatronInfo info;
                if (!egm_auto_enrol(appCfg, info))
                {
                    std::printf("[MAIN] Auto-enrol FAILED (CMS). Patron must try again.\n");
                }
                else
                {
                    SimpleEnrolCallback enrolCb;
                    std::printf("[MAIN] Camera enrol for cardId=%s seat=%s\n",
                        info.card_id.c_str(), pending_seat.c_str());

                    auto est = auth.Enroll(enrolCb, info.user_id.c_str());
                    std::printf("[MAIN] Enroll() returned Status=%d\n", (int)est);

                    if (est != Status::Ok)
                        std::printf("[MAIN] Camera enrol failed; patron must try again.\n");
                    else
                        std::printf("[MAIN] Camera enrol succeeded. Re-present face to log in.\n");
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        g_running = false;
        idleThread.join();
        auth.Disconnect();

        std::printf("=== Finished. Press ENTER to exit. ===\n");
        (void)getchar();
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::printf("Unhandled exception: %s\n", ex.what());
        std::printf("Press ENTER to exit.\n"); (void)getchar();
        return 1;
    }
}
