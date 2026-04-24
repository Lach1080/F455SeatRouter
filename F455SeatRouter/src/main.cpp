// main.cpp  —  v2.0  multi-seat concurrent sessions
// Multi-ROI, seat-aware RealSense -> SYNKROS CMS bridge with auto-enrol
//
// IMPORTANT:
// 1) C++17 or later required (/std:c++17)
// 2) Link winhttp.lib and rsid.lib
// 3) Uses repeated Authenticate() calls, not AuthenticateLoop()
// 4) All runtime config is read from config.json next to the executable.
//    Edit config.json - do NOT hard-code values here.
//
// v2.0 change: per-seat concurrent session management.
//   Each enabled seat/ROI has its own independent SessionState.
//   OnFaceDetected records the resolved seat for each face index.
//   OnResult matches each result to the face at the same index,
//   allowing simultaneous login/logout/enrol on different seats.

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
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
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
    std::string seat_id;
    std::string asset_id;           // slot mode
    std::string login_token_type;   // slot mode
    std::string login_token_data;   // slot mode
    int         device_id = 0;      // table mode
    int         game_id   = 0;      // table mode
};

// ── Application mode ──────────────────────────────────────────────────────
enum class AppMode { Slot, Table };

// ── Table-game specific global config ─────────────────────────────────────
struct TableConfig
{
    int employee_id   = 0;      // sent in every table login/logout
    int play_time     = 360;    // seconds, sent in table logout payload
    int average_wager = 0;      // sent in wagerDetails on table logout
};

struct CmsConfig
{
    std::wstring  host                      = L"10.16.0.12";
    INTERNET_PORT port                      = 9001;
    std::wstring  path_players              = L"/ssb/v1/players";
    std::wstring  path_logins               = L"/ssb/v1/egm-logins";
    std::wstring  path_logouts              = L"/ssb/v1/egm-logouts";
    std::string   card_create_path_template = "/ssb/v1/players/%d/cards";
    bool          ignore_cert_errors        = true;
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

    int  idle_timeout_s                      = 5;
    int  switch_delay_s                      = 15;
    int  auto_enrol_min_interval_s           = 5;
    bool forbidden_causes_logout_when_locked = false;

    RealSenseID::DeviceConfig::DumpMode dump_mode =
        RealSenseID::DeviceConfig::DumpMode::None;

    AppMode     mode       = AppMode::Slot;
    TableConfig table_cfg;

    CmsConfig           cms;
    EnrollmentDefaults  enrol_defaults;
    std::vector<SeatConfig>  seats;
    std::vector<AssetRoute>  routes;
};

// ============================================================
// CONFIG LOADING
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

static RealSenseID::DeviceConfig::DumpMode ParseDumpMode(const std::string& s)
{
    using D = RealSenseID::DeviceConfig::DumpMode;
    if (s == "CroppedFace") return D::CroppedFace;
    if (s == "FullFrame")   return D::FullFrame;
    if (s == "Debug")       return D::Debug;
    return D::None;
}

static RealSenseID::DeviceConfig::CameraRotation ParseRotation(const std::string& s)
{
    using R = RealSenseID::DeviceConfig::CameraRotation;
    if (s == "Rotation_0_Deg")   return R::Rotation_0_Deg;
    if (s == "Rotation_180_Deg") return R::Rotation_180_Deg;
    if (s == "Rotation_270_Deg") return R::Rotation_270_Deg;
    return R::Rotation_90_Deg;
}

AppConfig LoadAppConfig()
{
    AppConfig cfg;

    std::wstring exe_dir       = GetExeDir();
    std::wstring config_path_w = exe_dir + L"\\config.json";

    // Open via wide path directly to avoid wchar_t->char narrowing
    std::ifstream f(config_path_w);
    if (!f.is_open())
    {
        std::printf("[CONFIG] WARNING: config.json not found next to exe.\n");
        std::printf("[CONFIG] Using built-in defaults.\n");
        return cfg;
    }

    try
    {
        auto j = nlohmann::json::parse(f, nullptr, true, true);

        if (j.contains("port"))                              cfg.port = j["port"].get<std::string>();
        if (j.contains("rotation"))                         cfg.rotation  = ParseRotation(j["rotation"].get<std::string>());
        if (j.contains("DumpMode"))                         cfg.dump_mode = ParseDumpMode(j["DumpMode"].get<std::string>());
        if (j.contains("mode"))
        {
            auto m = j["mode"].get<std::string>();
            cfg.mode = (m == "table") ? AppMode::Table : AppMode::Slot;
        }

        if (j.contains("table"))
        {
            auto& jt = j["table"];
            if (jt.contains("employee_id"))   cfg.table_cfg.employee_id   = jt["employee_id"].get<int>();
            if (jt.contains("play_time"))      cfg.table_cfg.play_time      = jt["play_time"].get<int>();
            if (jt.contains("average_wager")) cfg.table_cfg.average_wager = jt["average_wager"].get<int>();
        }
        if (j.contains("idle_timeout_s"))                   cfg.idle_timeout_s = j["idle_timeout_s"].get<int>();
        if (j.contains("switch_delay_s"))                   cfg.switch_delay_s = j["switch_delay_s"].get<int>();
        if (j.contains("auto_enrol_min_interval_s"))        cfg.auto_enrol_min_interval_s = j["auto_enrol_min_interval_s"].get<int>();
        if (j.contains("forbidden_causes_logout_when_locked")) cfg.forbidden_causes_logout_when_locked = j["forbidden_causes_logout_when_locked"].get<bool>();

        if (j.contains("cms"))
        {
            auto& jc = j["cms"];
            if (jc.contains("host"))                      cfg.cms.host = Utf8ToWide(jc["host"].get<std::string>());
            if (jc.contains("port"))                      cfg.cms.port = static_cast<INTERNET_PORT>(jc["port"].get<int>());
            if (jc.contains("path_players"))              cfg.cms.path_players = Utf8ToWide(jc["path_players"].get<std::string>());
            if (jc.contains("path_logins"))               cfg.cms.path_logins = Utf8ToWide(jc["path_logins"].get<std::string>());
            if (jc.contains("path_logouts"))              cfg.cms.path_logouts = Utf8ToWide(jc["path_logouts"].get<std::string>());
            if (jc.contains("card_create_path_template")) cfg.cms.card_create_path_template = jc["card_create_path_template"].get<std::string>();
            if (jc.contains("ignore_cert_errors"))        cfg.cms.ignore_cert_errors = jc["ignore_cert_errors"].get<bool>();
        }

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

        if (j.contains("routes") && j["routes"].is_array())
        {
            for (const auto& jr : j["routes"])
            {
                AssetRoute ar{};
                if (jr.contains("seat_id"))          ar.seat_id          = jr["seat_id"].get<std::string>();
                if (jr.contains("asset_id"))         ar.asset_id         = jr["asset_id"].get<std::string>();
                if (jr.contains("login_token_type")) ar.login_token_type = jr["login_token_type"].get<std::string>();
                if (jr.contains("login_token_data")) ar.login_token_data = jr["login_token_data"].get<std::string>();
                if (jr.contains("device_id"))        ar.device_id        = jr["device_id"].get<int>();
                if (jr.contains("game_id"))          ar.game_id          = jr["game_id"].get<int>();
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
// PER-SEAT SESSION STATE
// ============================================================

enum class SessionState { Unlocked, LockedToUser, Cooldown };

struct SeatSession
{
    SessionState state = SessionState::Unlocked;

    std::string current_user_id;
    std::string current_card_id;
    AssetRoute  current_route{};

    std::chrono::steady_clock::time_point last_seen_owner{};
    std::chrono::steady_clock::time_point cooldown_until{};

    bool        pending_auto_enrol = false;
    std::chrono::steady_clock::time_point last_auto_enrol{};

    // Two-phase enrolment:
    //   Phase 1 (pending_auto_enrol)    → create SYNKROS player+card records
    //   Phase 2 (pending_enrol_card_id) → enrol face on camera using stored cardId
    // Keeping them separate prevents orphaned SYNKROS records when camera enrol fails/retries.
    std::string pending_enrol_user_id;
    std::string pending_enrol_card_id;  // non-empty = Phase 2 active

    // Table mode: ratingId of the open rating (0 = none).
    // Persisted to ratings_state.json so it survives a crash/restart.
    int rating_id = 0;
};

// One session per enabled seat_id
std::unordered_map<std::string, SeatSession> g_seat_sessions;
std::mutex        g_state_mtx;
std::atomic<bool> g_running{true};

// Set once in main() before any threads start; used by ratings persistence helpers.
std::wstring g_exe_dir;

// Extracts the trailing integer from a seat_id string.
// "seat_1" -> 1,  "seat_2" -> 2.  Returns 1 on parse failure.
static int ExtractSeatNumber(const std::string& seat_id)
{
    auto pos = seat_id.rfind('_');
    if (pos == std::string::npos || pos + 1 >= seat_id.size()) return 1;
    try { return std::stoi(seat_id.substr(pos + 1)); }
    catch (...) { return 1; }
}

// ============================================================
// HELPERS
// ============================================================

size_t CountEnabledSeats(const AppConfig& cfg)
{
    size_t n = 0;
    for (const auto& s : cfg.seats) if (s.enabled) ++n;
    return n;
}

std::optional<AssetRoute> FindRouteForSeat(const AppConfig& cfg, const std::string& seat_id)
{
    for (const auto& r : cfg.routes)
        if (r.seat_id == seat_id) return r;
    return std::nullopt;
}

RealSenseID::DeviceConfig BuildDeviceConfig(const AppConfig& appCfg)
{
    const size_t n = CountEnabledSeats(appCfg);
    if (n < 1) throw std::runtime_error("At least one seat must be enabled in config.json.");
    if (n > RealSenseID::DeviceConfig::MAX_ROIS) throw std::runtime_error("Too many enabled seats. MAX_ROIS is 5.");

    RealSenseID::DeviceConfig cfg;
    cfg.security_level        = RealSenseID::DeviceConfig::SecurityLevel::Low;
    cfg.algo_flow             = RealSenseID::DeviceConfig::AlgoFlow::All;
    cfg.face_selection_policy = (n > 1)
        ? RealSenseID::DeviceConfig::FaceSelectionPolicy::All
        : RealSenseID::DeviceConfig::FaceSelectionPolicy::Single;
    cfg.rect_enable           = 0x01;
    cfg.landmarks_enable      = 0;
    cfg.camera_rotation       = appCfg.rotation;
    cfg.dump_mode             = appCfg.dump_mode;
    cfg.matcher_confidence_level = RealSenseID::DeviceConfig::MatcherConfidenceLevel::Low;
    cfg.frontal_face_policy   = RealSenseID::DeviceConfig::FrontalFacePolicy::None;
    cfg.person_motion_mode    = RealSenseID::DeviceConfig::PersonMotionMode::Static;
    cfg.distance_limit        = RealSenseID::DeviceConfig::DistanceLimit::NoLimit;
    cfg.distance_enabled      = false;
    cfg.num_rois              = static_cast<unsigned char>(n);

    size_t i = 0;
    for (const auto& seat : appCfg.seats)
    {
        if (!seat.enabled) continue;
        cfg.detection_rois[i].x      = seat.x;
        cfg.detection_rois[i].y      = seat.y;
        cfg.detection_rois[i].width  = seat.width;
        cfg.detection_rois[i].height = seat.height;
        ++i;
    }
    return cfg;
}

// Builds a DeviceConfig restricted to a single seat's ROI.
// Used during camera enrolment to prevent DuplicateFaceprints when
// multiple patrons are in frame simultaneously.
RealSenseID::DeviceConfig BuildSingleSeatDeviceConfig(const AppConfig& appCfg, const std::string& seat_id)
{
    RealSenseID::DeviceConfig cfg;
    cfg.security_level           = RealSenseID::DeviceConfig::SecurityLevel::Low;
    cfg.algo_flow                = RealSenseID::DeviceConfig::AlgoFlow::All;
    cfg.face_selection_policy    = RealSenseID::DeviceConfig::FaceSelectionPolicy::Single;
    cfg.rect_enable              = 0x01;
    cfg.landmarks_enable         = 0;
    cfg.camera_rotation          = appCfg.rotation;
    cfg.dump_mode                = appCfg.dump_mode;
    cfg.matcher_confidence_level = RealSenseID::DeviceConfig::MatcherConfidenceLevel::Low;
    cfg.frontal_face_policy      = RealSenseID::DeviceConfig::FrontalFacePolicy::None;
    cfg.person_motion_mode       = RealSenseID::DeviceConfig::PersonMotionMode::Static;
    cfg.distance_limit           = RealSenseID::DeviceConfig::DistanceLimit::NoLimit;
    cfg.distance_enabled         = false;
    cfg.num_rois                 = 1;

    for (const auto& seat : appCfg.seats)
    {
        if (!seat.enabled || seat.seat_id != seat_id) continue;
        cfg.detection_rois[0].x      = seat.x;
        cfg.detection_rois[0].y      = seat.y;
        cfg.detection_rois[0].width  = seat.width;
        cfg.detection_rois[0].height = seat.height;
        break;
    }
    return cfg;
}

std::optional<std::string> ResolveSeatForFace(const RealSenseID::FaceRect& face, const AppConfig& cfg)
{
    const int cx = face.x + face.w / 2;
    const int cy = face.y + face.h / 2;

    std::optional<std::string> match;
    for (const auto& seat : cfg.seats)
    {
        if (!seat.enabled) continue;
        const bool inside =
            (cx >= seat.x && cx < seat.x + seat.width) &&
            (cy >= seat.y && cy < seat.y + seat.height);
        if (inside)
        {
            if (match.has_value()) return std::nullopt; // ambiguous
            match = seat.seat_id;
        }
    }
    return match;
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
        g_http_session = WinHttpOpen(L"F455SeatRouter/2.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_http_session)
        {
            std::printf("[HTTP] WinHttpOpen failed (err=%lu)\n", GetLastError());
            return false;
        }
    }

    if (!g_http_connect)
    {
        g_http_connect = WinHttpConnect(g_http_session, cms.host.c_str(), cms.port, 0);
        if (!g_http_connect)
        {
            std::printf("[HTTP] WinHttpConnect failed (err=%lu)\n", GetLastError());
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
    if (!init_http(cms)) { std::printf("[HTTP] init_http() failed.\n"); return result; }

    auto t_start = steady_clock::now();
    HINTERNET h_request = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_http_mtx);
        LPCWSTR headers = L"Content-Type: application/json\r\n";

        h_request = WinHttpOpenRequest(g_http_connect, L"POST", path, nullptr,
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

        if (!WinHttpSendRequest(h_request, headers, -1L,
            json_body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)json_body.data(),
            (DWORD)json_body.size(), (DWORD)json_body.size(), 0)) goto cleanup;

        if (!WinHttpReceiveResponse(h_request, nullptr)) goto cleanup;

        {
            DWORD sc = 0, sc_sz = sizeof(sc);
            if (!WinHttpQueryHeaders(h_request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sc_sz, WINHTTP_NO_HEADER_INDEX)) goto cleanup;
            result.status = (int)sc;
        }

        {
            std::string body;
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(h_request, &avail) || avail == 0) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(h_request, buf.data(), avail, &read) || read == 0) break;
                body.append(buf.data(), read);
            }
            result.body = std::move(body);
        }

    cleanup:
        if (h_request) { WinHttpCloseHandle(h_request); }
    }

    auto ms = duration_cast<milliseconds>(steady_clock::now() - t_start).count();
    std::printf("[HTTP] POST %ls  %ldms  status=%d\n", path, (long)ms, result.status);
    result.ok = (result.status != 0);
    return result;
}

HttpResponse http_get_json(const CmsConfig& cms, const wchar_t* path)
{
    using namespace std::chrono;
    HttpResponse result;
    if (!init_http(cms)) { std::printf("[HTTP] init_http() failed.\n"); return result; }

    auto t_start = steady_clock::now();
    HINTERNET h_request = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_http_mtx);

        h_request = WinHttpOpenRequest(g_http_connect, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!h_request) goto cleanup_get;

        if (cms.ignore_cert_errors)
        {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(h_request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        }

        if (!WinHttpSendRequest(h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup_get;

        if (!WinHttpReceiveResponse(h_request, nullptr)) goto cleanup_get;

        {
            DWORD sc = 0, sc_sz = sizeof(sc);
            if (!WinHttpQueryHeaders(h_request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sc_sz, WINHTTP_NO_HEADER_INDEX)) goto cleanup_get;
            result.status = (int)sc;
        }

        {
            std::string body;
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(h_request, &avail) || avail == 0) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(h_request, buf.data(), avail, &read) || read == 0) break;
                body.append(buf.data(), read);
            }
            result.body = std::move(body);
        }

    cleanup_get:
        if (h_request) { WinHttpCloseHandle(h_request); }
    }

    auto ms = duration_cast<milliseconds>(steady_clock::now() - t_start).count();
    std::printf("[HTTP] GET  %ls  %ldms  status=%d\n", path, (long)ms, result.status);
    result.ok = (result.status != 0);
    return result;
}

HttpResponse http_patch_json(const CmsConfig& cms, const wchar_t* path, const std::string& json_body)
{
    using namespace std::chrono;
    HttpResponse result;
    if (!init_http(cms)) { std::printf("[HTTP] init_http() failed.\n"); return result; }

    auto t_start = steady_clock::now();
    HINTERNET h_request = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_http_mtx);
        LPCWSTR headers = L"Content-Type: application/json\r\n";

        h_request = WinHttpOpenRequest(g_http_connect, L"PATCH", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!h_request) goto cleanup_patch;

        if (cms.ignore_cert_errors)
        {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(h_request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        }

        if (!WinHttpSendRequest(h_request, headers, -1L,
            json_body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)json_body.data(),
            (DWORD)json_body.size(), (DWORD)json_body.size(), 0)) goto cleanup_patch;

        if (!WinHttpReceiveResponse(h_request, nullptr)) goto cleanup_patch;

        {
            DWORD sc = 0, sc_sz = sizeof(sc);
            if (!WinHttpQueryHeaders(h_request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sc_sz, WINHTTP_NO_HEADER_INDEX)) goto cleanup_patch;
            result.status = (int)sc;
        }

        {
            std::string body;
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(h_request, &avail) || avail == 0) break;
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (!WinHttpReadData(h_request, buf.data(), avail, &read) || read == 0) break;
                body.append(buf.data(), read);
            }
            result.body = std::move(body);
        }

    cleanup_patch:
        if (h_request) { WinHttpCloseHandle(h_request); }
    }

    auto ms = duration_cast<milliseconds>(steady_clock::now() - t_start).count();
    std::printf("[HTTP] PATCH %ls  %ldms  status=%d\n", path, (long)ms, result.status);
    result.ok = (result.status != 0);
    return result;
}

int extract_int_field(const std::string& body, const std::string& field)
{
    std::string key = "\"" + field + "\":";
    auto pos = body.find(key);
    if (pos == std::string::npos) return 0;
    pos += key.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    int v = 0; bool any = false;
    while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9')
        { any = true; v = v * 10 + (body[pos++] - '0'); }
    return any ? v : 0;
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

    std::wprintf(L"[EGM] LOGIN  %ls:%d%ls\n", cms.host.c_str(), (int)cms.port, cms.path_logins.c_str());
    std::printf("[EGM] LOGIN  payload=%s\n", payload.c_str());
    auto resp = http_post_json(cms, cms.path_logins.c_str(), payload);
    std::printf("[EGM] LOGIN  status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(), 300), resp.body.c_str());
    return resp.ok && resp.status >= 200 && resp.status < 300;
}

bool egm_logout(const CmsConfig& cms, const AssetRoute& route, const std::string& card_id)
{
    std::string payload =
        "{\"cardId\":\"" + card_id +
        "\",\"assetId\":\"" + route.asset_id + "\"}";

    std::wprintf(L"[EGM] LOGOUT %ls:%d%ls\n", cms.host.c_str(), (int)cms.port, cms.path_logouts.c_str());
    std::printf("[EGM] LOGOUT payload=%s\n", payload.c_str());
    auto resp = http_post_json(cms, cms.path_logouts.c_str(), payload);
    std::printf("[EGM] LOGOUT status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(), 300), resp.body.c_str());
    return resp.ok && resp.status >= 200 && resp.status < 300;
}

// Note: throttle is handled per-seat in main() — not here.
bool egm_auto_enrol(const AppConfig& cfg, NewPatronInfo& out)
{
    std::string player_payload =
        "{\"firstName\":\"" + cfg.enrol_defaults.first_name +
        "\",\"lastName\":\"" + cfg.enrol_defaults.last_name +
        "\",\"birthDate\":\"" + cfg.enrol_defaults.birth_date +
        "\",\"address\":{\"addressLine1\":\"" + cfg.enrol_defaults.address_line1 +
        "\",\"postalCode\":\"" + cfg.enrol_defaults.postal_code +
        "\",\"countryCode\":\"" + cfg.enrol_defaults.country_code + "\"}}";

    std::wprintf(L"[EGM] AUTO-ENROL: POST %ls\n", cfg.cms.path_players.c_str());
    auto pr = http_post_json(cfg.cms, cfg.cms.path_players.c_str(), player_payload);
    std::printf("[EGM] AUTO-ENROL: /players status=%d body=%.*s\n", pr.status,
        (int)std::min<size_t>(pr.body.size(), 300), pr.body.c_str());

    if (!pr.ok || pr.status < 200 || pr.status >= 300)
        { std::printf("[EGM] AUTO-ENROL ERROR: /players failed.\n"); return false; }

    int player_id = extract_int_field(pr.body, "playerId");
    if (player_id <= 0)
        { std::printf("[EGM] AUTO-ENROL ERROR: could not parse playerId.\n"); return false; }

    char path_buf[256];
    std::snprintf(path_buf, sizeof(path_buf), cfg.cms.card_create_path_template.c_str(), player_id);
    std::wstring card_path_w = Utf8ToWide(path_buf);

    std::string card_payload = "{\"playerPin\":\"" + cfg.enrol_defaults.player_pin + "\"}";
    auto cr = http_post_json(cfg.cms, card_path_w.c_str(), card_payload);
    std::printf("[EGM] AUTO-ENROL: /cards status=%d body=%.*s\n", cr.status,
        (int)std::min<size_t>(cr.body.size(), 300), cr.body.c_str());

    if (!cr.ok || cr.status < 200 || cr.status >= 300)
        { std::printf("[EGM] AUTO-ENROL ERROR: /cards failed.\n"); return false; }

    int card_id = extract_int_field(cr.body, "cardId");
    if (card_id <= 0)
        { std::printf("[EGM] AUTO-ENROL ERROR: could not parse cardId.\n"); return false; }

    out.card_id = std::to_string(card_id);
    out.user_id = std::to_string(player_id); // store playerId on camera — not cardId
    std::printf("[EGM] AUTO-ENROL SUCCESS: playerId=%d cardId=%d\n", player_id, card_id);
    return true;
}

// Looks up the active primary card for a known patron by playerId.
// Called at login time: camera stores playerId, CMS may re-issue cards,
// so we always fetch the current active card rather than storing it at enrolment.
// Returns the cardId as a string, or empty string on failure.
std::string cms_lookup_active_card(const CmsConfig& cms, int player_id)
{
    char path_buf[256];
    std::snprintf(path_buf, sizeof(path_buf), cms.card_create_path_template.c_str(), player_id);
    std::wstring path_w = Utf8ToWide(path_buf);

    std::printf("[CMS] GET cards for playerId=%d  path=%s\n", player_id, path_buf);
    auto resp = http_get_json(cms, path_w.c_str());
    std::printf("[CMS] GET cards status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(), 400), resp.body.c_str());

    if (!resp.ok || resp.status < 200 || resp.status >= 300)
    {
        std::printf("[CMS] GET cards FAILED for playerId=%d\n", player_id);
        return {};
    }

    try
    {
        auto j = nlohmann::json::parse(resp.body);
        if (!j.is_array() || j.empty())
        {
            std::printf("[CMS] GET cards: empty or non-array response\n");
            return {};
        }

        // Prefer Active+primary card; fall back to first Active card
        std::string first_active;
        for (const auto& card : j)
        {
            std::string status   = card.value("status",   "");
            std::string cardType = card.value("cardType", "");
            int         cid      = card.value("cardId",   0);
            if (status != "Active" || cid <= 0) continue;
            if (cardType == "primary") return std::to_string(cid);
            if (first_active.empty())  first_active = std::to_string(cid);
        }

        if (!first_active.empty())
            std::printf("[CMS] GET cards: no primary card — using first active cardId=%s\n",
                first_active.c_str());
        else
            std::printf("[CMS] GET cards: no active card found for playerId=%d\n", player_id);

        return first_active;
    }
    catch (const std::exception& ex)
    {
        std::printf("[CMS] GET cards parse error: %s\n", ex.what());
        return {};
    }
}

// ============================================================
// TABLE GAME LOGIN / LOGOUT
// ============================================================

struct TableLoginResult { bool ok = false; int rating_id = 0; };

// POST /ssb/v1/ratings/table/  — opens a table rating.
// Returns the ratingId assigned by SYNKROS (> 0 on success).
TableLoginResult egm_table_login(const CmsConfig& cms, const AssetRoute& route,
    const TableConfig& table_cfg, const std::string& player_id, int seat_number)
{
    std::string payload =
        "{\"status\":\"open\""
        ",\"deviceId\":"    + std::to_string(route.device_id) +
        ",\"gameId\":"      + std::to_string(route.game_id) +
        ",\"playerId\":"    + player_id +
        ",\"seat\":"        + std::to_string(seat_number) +
        ",\"employeeId\":"  + std::to_string(table_cfg.employee_id) + "}";

    std::wprintf(L"[TABLE] LOGIN  POST %ls:%d%ls\n",
        cms.host.c_str(), (int)cms.port, cms.path_logins.c_str());
    std::printf("[TABLE] LOGIN  payload=%s\n", payload.c_str());
    auto resp = http_post_json(cms, cms.path_logins.c_str(), payload);
    std::printf("[TABLE] LOGIN  status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(), 500), resp.body.c_str());

    if (!resp.ok || resp.status < 200 || resp.status >= 300)
    {
        std::printf("[TABLE] LOGIN FAILED (HTTP status=%d)\n", resp.status);
        return {};
    }

    int rating_id = extract_int_field(resp.body, "ratingId");
    if (rating_id <= 0)
    {
        std::printf("[TABLE] LOGIN ERROR: could not parse ratingId from response\n");
        return {};
    }

    std::printf("[TABLE] LOGIN SUCCESS: ratingId=%d playerId=%s seat=%d\n",
        rating_id, player_id.c_str(), seat_number);
    return { true, rating_id };
}

// PATCH /ssb/v1/ratings/table/{ratingId}  — closes the table rating.
// Verifies "status":"closed" in the response body.
bool egm_table_logout(const CmsConfig& cms, const AssetRoute& route,
    const TableConfig& table_cfg, int rating_id)
{
    std::string payload =
        "{\"status\":\"closed\""
        ",\"gameId\":"      + std::to_string(route.game_id) +
        ",\"wagerDetails\":[{\"gameId\":" + std::to_string(route.game_id) +
        ",\"averageWager\":" + std::to_string(table_cfg.average_wager) + "}]"
        ",\"playTime\":"    + std::to_string(table_cfg.play_time) +
        ",\"employeeId\":"  + std::to_string(table_cfg.employee_id) + "}";

    // Path: base logouts path + ratingId  (e.g. /ssb/v1/ratings/table/10309404)
    std::wstring path = cms.path_logouts + std::to_wstring(rating_id);

    std::wprintf(L"[TABLE] LOGOUT PATCH %ls:%d%ls\n",
        cms.host.c_str(), (int)cms.port, path.c_str());
    std::printf("[TABLE] LOGOUT payload=%s\n", payload.c_str());
    auto resp = http_patch_json(cms, path.c_str(), payload);
    std::printf("[TABLE] LOGOUT status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(), 500), resp.body.c_str());

    if (!resp.ok || resp.status < 200 || resp.status >= 300)
    {
        std::printf("[TABLE] LOGOUT FAILED (HTTP status=%d)\n", resp.status);
        return false;
    }

    // Confirm SYNKROS closed the rating
    bool closed = resp.body.find("\"status\":\"closed\"")  != std::string::npos ||
                  resp.body.find("\"status\": \"closed\"") != std::string::npos;

    if (closed)
        std::printf("[TABLE] LOGOUT SUCCESS: ratingId=%d confirmed closed\n", rating_id);
    else
        std::printf("[TABLE] LOGOUT WARNING: ratingId=%d response did not confirm closed\n", rating_id);

    return closed;
}

// ============================================================
// RATINGS STATE PERSISTENCE
// Writes/reads ratings_state.json next to the EXE so that open
// table ratings survive an application crash or restart.
// ============================================================

// Snapshot all seats with an open rating to disk.
// Call after every table login and after every successful table logout.
void save_ratings_state()
{
    nlohmann::json j = nlohmann::json::object();

    {
        std::lock_guard<std::mutex> lock(g_state_mtx);
        for (const auto& [seat_id, session] : g_seat_sessions)
        {
            if (session.rating_id > 0)
            {
                j[seat_id] = {
                    { "rating_id", session.rating_id      },
                    { "player_id", session.current_user_id }
                };
            }
        }
    }

    std::wstring path = g_exe_dir + L"\\ratings_state.json";
    std::ofstream f(path);
    if (f.is_open())
    {
        f << j.dump(2);
        std::printf("[STATE] ratings_state.json saved (%zu open rating(s))\n",
            (size_t)j.size());
    }
    else
    {
        std::printf("[STATE] WARNING: could not write ratings_state.json\n");
    }
}

// On startup (table mode only): restore any open ratings from disk.
// Recovered sessions are put back into LockedToUser state — idle_monitor
// will close them if the patron doesn't re-present within idle_timeout_s.
void load_ratings_state()
{
    std::wstring path = g_exe_dir + L"\\ratings_state.json";
    std::ifstream f(path);
    if (!f.is_open())
    {
        std::printf("[STATE] No ratings_state.json found — starting fresh\n");
        return;
    }

    try
    {
        auto j   = nlohmann::json::parse(f);
        auto now = std::chrono::steady_clock::now();
        int  recovered = 0;

        std::lock_guard<std::mutex> lock(g_state_mtx);
        for (auto& [seat_id, session] : g_seat_sessions)
        {
            if (!j.contains(seat_id)) continue;
            auto& js = j[seat_id];

            int         rid = js.value("rating_id", 0);
            std::string pid = js.value("player_id", "");
            if (rid <= 0 || pid.empty()) continue;

            session.rating_id       = rid;
            session.current_user_id = pid;
            session.last_seen_owner = now;   // start idle clock from now
            session.state           = SessionState::LockedToUser;
            ++recovered;

            std::printf("[STATE][%s] Recovered open rating ratingId=%d playerId=%s\n",
                seat_id.c_str(), rid, pid.c_str());
        }
        std::printf("[STATE] Recovered %d open rating(s) from previous session\n", recovered);
    }
    catch (const std::exception& ex)
    {
        std::printf("[STATE] WARNING: failed to load ratings_state.json: %s\n", ex.what());
    }
}

// ============================================================
// CALLBACKS
// ============================================================

class SimpleEnrolCallback : public RealSenseID::EnrollmentCallback
{
public:
    void OnResult(RealSenseID::EnrollStatus status) override
    { std::printf("[ENROL] OnResult: %d\n", (int)status); }

    void OnProgress(RealSenseID::FacePose pose) override
    { std::printf("[ENROL] OnProgress: pose=%d\n", (int)pose); }

    void OnHint(RealSenseID::EnrollStatus hint, float cov) override
    { std::printf("[ENROL] OnHint: %d cov=%.2f\n", (int)hint, (double)cov); }
};

class AutoEnrolAuthCallback : public RealSenseID::AuthenticationCallback
{
public:
    explicit AutoEnrolAuthCallback(const AppConfig& cfg) : _cfg(cfg) {}

    // Called once per Authenticate() with all detected faces.
    // Stores the resolved seat for each face in order — OnResult fires in the same order.
    void OnFaceDetected(const std::vector<RealSenseID::FaceRect>& faces,
                        const unsigned int timestamp) override
    {
        std::lock_guard<std::mutex> lk(_face_mtx);
        _result_index = 0;
        _face_seats.clear();

        std::printf("[FACE] faces=%zu timestamp=%u\n", faces.size(), timestamp);
        for (size_t i = 0; i < faces.size(); ++i)
        {
            const auto& f = faces[i];
            auto seat = ResolveSeatForFace(f, _cfg);
            std::printf("  Face %zu x=%d y=%d w=%d h=%d center=(%d,%d) -> %s\n",
                i, f.x, f.y, f.w, f.h, f.x + f.w / 2, f.y + f.h / 2,
                seat.has_value() ? seat->c_str() : "none/ambiguous");
            _face_seats.push_back(seat);
        }
    }

    // Called once per recognised/rejected face, in the same order as OnFaceDetected.
    void OnResult(RealSenseID::AuthenticateStatus status,
                  const char* user_id, short score) override
    {
        using namespace std::chrono;
        using RealSenseID::AuthenticateStatus;

        const std::string uid = user_id ? user_id : "";

        // Match this result to its face via index
        std::optional<std::string> resolved_seat;
        {
            std::lock_guard<std::mutex> lk(_face_mtx);
            if (_result_index < _face_seats.size())
                resolved_seat = _face_seats[_result_index];
            ++_result_index;
        }

        std::printf("[AUTH] OnResult[%zu]: status=%d user_id=%s score=%d seat=%s\n",
            _result_index - 1, (int)status, uid.c_str(), (int)score,
            resolved_seat.has_value() ? resolved_seat->c_str() : "<none>");

        if (!resolved_seat.has_value())
        {
            std::printf("[AUTH] No resolved seat for result %zu — ignored\n", _result_index - 1);
            return;
        }

        const bool is_success  = (status == AuthenticateStatus::Success);
        const bool is_forbidden = (status == AuthenticateStatus::Forbidden);
        if (!is_success && !is_forbidden) return; // NoFaceDetected etc.

        auto route_opt = FindRouteForSeat(_cfg, *resolved_seat);
        if (!route_opt.has_value())
        {
            std::printf("[AUTH] No route for seat %s — ignored\n", resolved_seat->c_str());
            return;
        }

        auto now = steady_clock::now();
        std::lock_guard<std::mutex> state_lock(g_state_mtx);
        auto& session = g_seat_sessions[*resolved_seat];

        // ── LOCKED ──────────────────────────────────────────────────────────
        if (session.state == SessionState::LockedToUser)
        {
            if (is_success && uid == session.current_user_id)
            {
                session.last_seen_owner = now;
                std::printf("[SESSION][%s] Owner %s re-confirmed\n",
                    resolved_seat->c_str(), uid.c_str());
                return;
            }
            if (is_success)
            {
                std::printf("[SESSION][%s] Other user %s seen while locked to %s — ignored\n",
                    resolved_seat->c_str(), uid.c_str(), session.current_user_id.c_str());
                return;
            }
            if (is_forbidden)
            {
                std::printf("[SESSION][%s] Forbidden while locked — ignored\n", resolved_seat->c_str());
                return;
            }
            return;
        }

        // ── COOLDOWN ─────────────────────────────────────────────────────────
        if (session.state == SessionState::Cooldown)
        {
            std::printf("[SESSION][%s] In cooldown — ignored\n", resolved_seat->c_str());
            return;
        }

        // ── UNLOCKED + Success ───────────────────────────────────────────────
        if (is_success)
        {
            session.current_user_id = uid;   // always the playerId stored on camera
            session.current_card_id = "";    // slot: resolved async; table: unused
            session.current_route   = *route_opt;
            session.last_seen_owner = now;
            session.state           = SessionState::LockedToUser;
            session.rating_id       = 0;     // cleared; table login thread will set it

            std::string seat_id_copy = *resolved_seat;
            AssetRoute  route        = session.current_route;
            CmsConfig   cms          = _cfg.cms;
            int         player_id_i  = std::atoi(uid.c_str());

            if (_cfg.mode == AppMode::Slot)
            {
                // ── Slot: resolve active cardId from CMS, then login ──────────
                std::printf("[SESSION][%s] Locked (slot): playerId=%s asset=%s — resolving card...\n",
                    resolved_seat->c_str(), uid.c_str(), route.asset_id.c_str());

                std::thread([cms, route, seat_id_copy, player_id_i]()
                {
                    std::string card_id = cms_lookup_active_card(cms, player_id_i);
                    if (card_id.empty())
                    {
                        std::printf("[SESSION][%s] Card lookup failed for playerId=%d — cannot login\n",
                            seat_id_copy.c_str(), player_id_i);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_state_mtx);
                        auto it = g_seat_sessions.find(seat_id_copy);
                        if (it != g_seat_sessions.end() &&
                            it->second.state == SessionState::LockedToUser)
                            it->second.current_card_id = card_id;
                    }
                    std::printf("[SESSION][%s] Resolved: playerId=%d -> cardId=%s — logging in\n",
                        seat_id_copy.c_str(), player_id_i, card_id.c_str());
                    egm_login(cms, route, card_id);
                }).detach();
            }
            else // AppMode::Table
            {
                // ── Table: open a rating directly using playerId ───────────────
                int         seat_num  = ExtractSeatNumber(seat_id_copy);
                TableConfig tbl       = _cfg.table_cfg;

                std::printf("[SESSION][%s] Locked (table): playerId=%s deviceId=%d gameId=%d seat=%d\n",
                    resolved_seat->c_str(), uid.c_str(),
                    route.device_id, route.game_id, seat_num);

                std::thread([cms, route, seat_id_copy, uid, seat_num, tbl]()
                {
                    auto result = egm_table_login(cms, route, tbl, uid, seat_num);
                    if (!result.ok)
                    {
                        std::printf("[SESSION][%s] Table login FAILED\n", seat_id_copy.c_str());
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_state_mtx);
                        auto it = g_seat_sessions.find(seat_id_copy);
                        if (it != g_seat_sessions.end() &&
                            it->second.state == SessionState::LockedToUser)
                            it->second.rating_id = result.rating_id;
                    }
                    save_ratings_state();
                }).detach();
            }
            return;
        }

        // ── UNLOCKED + Forbidden → queue auto-enrol ──────────────────────────
        if (is_forbidden)
        {
            session.pending_auto_enrol = true;
            std::printf("[SESSION][%s] Forbidden → pending auto-enrol\n", resolved_seat->c_str());
        }
    }

    void OnHint(RealSenseID::AuthenticateStatus hint, float frameScore) override
    { std::printf("[AUTH] OnHint: %d frameScore=%.2f\n", (int)hint, (double)frameScore); }

private:
    const AppConfig& _cfg;
    std::mutex       _face_mtx;
    std::vector<std::optional<std::string>> _face_seats; // index = face order from OnFaceDetected
    size_t           _result_index = 0;
};

// ============================================================
// IDLE MONITOR  — checks every seat independently
// ============================================================

void idle_monitor(const AppConfig& cfg)
{
    using namespace std::chrono;
    std::printf("[BG] Idle monitor started: timeout=%ds cooldown=%ds\n",
        cfg.idle_timeout_s, cfg.switch_delay_s);

    while (g_running)
    {
        {
            std::lock_guard<std::mutex> lock(g_state_mtx);
            auto now = steady_clock::now();

            for (auto& [seat_id, session] : g_seat_sessions)
            {
                // Determine if the session is ready for idle-logout monitoring.
                // Slot mode: wait until cardId is resolved (set by login thread).
                // Table mode: wait until ratingId is set (set by table login thread).
                const bool ready =
                    (session.state == SessionState::LockedToUser) &&
                    (cfg.mode == AppMode::Slot
                        ? !session.current_card_id.empty()
                        : session.rating_id != 0);

                if (ready)
                {
                    auto elapsed = duration_cast<seconds>(now - session.last_seen_owner).count();
                    if (elapsed >= cfg.idle_timeout_s)
                    {
                        AssetRoute  route      = session.current_route;
                        std::string seat_id_cp = seat_id;

                        session.state          = SessionState::Cooldown;
                        session.cooldown_until = now + seconds(cfg.switch_delay_s);
                        session.current_user_id.clear();

                        if (cfg.mode == AppMode::Slot)
                        {
                            std::string card_id = session.current_card_id;
                            session.current_card_id.clear();

                            std::printf("[BG][%s] Idle timeout — slot logout cardId=%s\n",
                                seat_id.c_str(), card_id.c_str());

                            std::thread([cms = cfg.cms, route, card_id]()
                                { egm_logout(cms, route, card_id); }).detach();
                        }
                        else // Table
                        {
                            int         rating_id = session.rating_id;
                            TableConfig tbl       = cfg.table_cfg;
                            session.rating_id = 0;

                            std::printf("[BG][%s] Idle timeout — table logout ratingId=%d\n",
                                seat_id.c_str(), rating_id);

                            std::thread([cms = cfg.cms, route, tbl, rating_id, seat_id_cp]()
                            {
                                bool ok = egm_table_logout(cms, route, tbl, rating_id);
                                if (!ok)
                                    std::printf("[BG][%s] Table logout FAILED for ratingId=%d\n",
                                        seat_id_cp.c_str(), rating_id);
                                save_ratings_state(); // clears ratingId from state file
                            }).detach();
                        }
                    }
                }

                if (session.state == SessionState::Cooldown && now >= session.cooldown_until)
                {
                    std::printf("[BG][%s] Cooldown finished — Unlocked\n", seat_id.c_str());
                    session.state = SessionState::Unlocked;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
        g_exe_dir = GetExeDir();  // must be set before any thread or persistence call

        AppConfig appCfg = LoadAppConfig();

        const size_t enabled_count = CountEnabledSeats(appCfg);
        std::printf("=== F455SeatRouter v4.0 ===\n");
        std::printf("Mode = %s  |  Enabled seats = %zu  |  Port = %s\n",
            appCfg.mode == AppMode::Table ? "TABLE" : "SLOT",
            enabled_count, appCfg.port.c_str());

        for (const auto& seat : appCfg.seats)
            if (seat.enabled)
                std::printf("  Seat %-8s ROI x=%4u y=%4u w=%4u h=%4u\n",
                    seat.seat_id.c_str(), seat.x, seat.y, seat.width, seat.height);

        if (appCfg.mode == AppMode::Slot)
            for (const auto& route : appCfg.routes)
                std::printf("  Route %-8s -> asset=%-6s tokenType=%s tokenData=%s\n",
                    route.seat_id.c_str(), route.asset_id.c_str(),
                    route.login_token_type.c_str(), route.login_token_data.c_str());
        else
            for (const auto& route : appCfg.routes)
                std::printf("  Route %-8s -> deviceId=%-8d gameId=%d\n",
                    route.seat_id.c_str(), route.device_id, route.game_id);

        if (appCfg.mode == AppMode::Table)
            std::printf("  Table config: employeeId=%d playTime=%ds averageWager=%d\n",
                appCfg.table_cfg.employee_id,
                appCfg.table_cfg.play_time,
                appCfg.table_cfg.average_wager);

        // Initialise per-seat sessions
        {
            std::lock_guard<std::mutex> lock(g_state_mtx);
            for (const auto& seat : appCfg.seats)
                if (seat.enabled)
                    g_seat_sessions[seat.seat_id] = SeatSession{};
        }

        // Restore any open table ratings from the previous session
        if (appCfg.mode == AppMode::Table)
            load_ratings_state();

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
        std::printf("  Each seat is tracked independently and concurrently.\n");

        while (g_running)
        {
            auto ast = auth.Authenticate(authCb);
            std::printf("Authenticate() returned Status=%d\n", (int)ast);

            if (ast != Status::Ok)
            {
                std::printf("[MAIN] Authenticate error; brief backoff.\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }

            // ── Phase 1: Create SYNKROS records for newly-pending seats ─────────
            // Only runs when pending_auto_enrol is set and no CMS records exist yet.
            // Separating this from camera enrolment prevents orphaned records on retry.
            {
                using SeatEnrol = std::pair<std::string, AssetRoute>;
                std::vector<SeatEnrol> cms_pending;

                {
                    std::lock_guard<std::mutex> lock(g_state_mtx);
                    auto now = std::chrono::steady_clock::now();

                    for (auto& [seat_id, session] : g_seat_sessions)
                    {
                        if (!session.pending_auto_enrol) continue;
                        if (session.state != SessionState::Unlocked) continue;
                        if (!session.pending_enrol_card_id.empty()) continue; // CMS already done

                        // Per-seat throttle
                        if (session.last_auto_enrol.time_since_epoch().count() != 0)
                        {
                            double since_s = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - session.last_auto_enrol).count() / 1000.0;
                            if (since_s < appCfg.auto_enrol_min_interval_s)
                            {
                                std::printf("[MAIN][%s] Auto-enrol throttled (%.1fs ago)\n",
                                    seat_id.c_str(), since_s);
                                session.pending_auto_enrol = false; // re-arm on next Forbidden
                                continue;
                            }
                        }

                        auto route = FindRouteForSeat(appCfg, seat_id);
                        if (!route.has_value()) { session.pending_auto_enrol = false; continue; }

                        session.pending_auto_enrol = false;
                        session.last_auto_enrol    = now;
                        cms_pending.push_back({seat_id, *route});
                    }
                }

                for (auto& [seat_id, route] : cms_pending)
                {
                    std::printf("[MAIN][%s] Phase 1 — creating CMS records (asset=%s)\n",
                        seat_id.c_str(), route.asset_id.c_str());
                    NewPatronInfo info;
                    if (egm_auto_enrol(appCfg, info))
                    {
                        std::lock_guard<std::mutex> lock(g_state_mtx);
                        g_seat_sessions[seat_id].pending_enrol_user_id = info.user_id;
                        g_seat_sessions[seat_id].pending_enrol_card_id = info.card_id;
                        std::printf("[MAIN][%s] CMS done — cardId=%s. Camera enrol pending.\n",
                            seat_id.c_str(), info.card_id.c_str());
                    }
                    else
                    {
                        std::printf("[MAIN][%s] Phase 1 FAILED (CMS) — will retry on next Forbidden\n",
                            seat_id.c_str());
                    }
                }
            }

            // ── Phase 2: Camera enrolment, one seat at a time ────────────────
            // Temporarily restricts the camera to only the enrolling seat's ROI.
            // This prevents DuplicateFaceprints when multiple patrons are in frame:
            // the camera can only see (and enrol) the face in the target seat's zone.
            // After enrolment, the full multi-seat config is restored.
            {
                using SeatCamEnrol = std::tuple<std::string, std::string, std::string>;
                std::vector<SeatCamEnrol> cam_pending;

                {
                    std::lock_guard<std::mutex> lock(g_state_mtx);
                    for (auto& [seat_id, session] : g_seat_sessions)
                    {
                        if (session.pending_enrol_card_id.empty()) continue;
                        if (session.state != SessionState::Unlocked) continue;
                        cam_pending.emplace_back(seat_id,
                            session.pending_enrol_user_id,
                            session.pending_enrol_card_id);
                    }
                }

                for (auto& [seat_id, user_id, card_id] : cam_pending)
                {
                    std::printf("[MAIN][%s] Phase 2 — camera enrol for cardId=%s\n",
                        seat_id.c_str(), card_id.c_str());

                    // Restrict to this seat's ROI only — prevents grabbing already-enrolled face
                    auto single_cfg = BuildSingleSeatDeviceConfig(appCfg, seat_id);
                    auto sc_st = auth.SetDeviceConfig(single_cfg);
                    std::printf("[MAIN][%s] SetDeviceConfig (single-seat) status=%d\n",
                        seat_id.c_str(), (int)sc_st);

                    SimpleEnrolCallback enrolCb;
                    auto est = auth.Enroll(enrolCb, user_id.c_str());
                    std::printf("[MAIN][%s] Enroll() Status=%d\n", seat_id.c_str(), (int)est);

                    // Always restore full multi-seat config after enrolment attempt
                    auto full_cfg = BuildDeviceConfig(appCfg);
                    auth.SetDeviceConfig(full_cfg);

                    if (est == Status::Ok)
                    {
                        std::lock_guard<std::mutex> lock(g_state_mtx);
                        g_seat_sessions[seat_id].pending_enrol_card_id.clear();
                        g_seat_sessions[seat_id].pending_enrol_user_id.clear();
                        std::printf("[MAIN][%s] Enrol succeeded — patron must re-present face to log in.\n",
                            seat_id.c_str());
                    }
                    else
                    {
                        // CMS records are preserved (pending_enrol_card_id stays set).
                        // Camera enrol will be retried next cycle without creating new SYNKROS records.
                        std::printf("[MAIN][%s] Camera enrol failed (Status=%d) — retrying next cycle\n",
                            seat_id.c_str(), (int)est);
                    }
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
