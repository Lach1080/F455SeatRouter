// main.cpp  —  v5.1  Preview windows + file logging
// Multi-ROI, seat-aware RealSense -> SYNKROS CMS bridge with auto-enrol
//
// IMPORTANT:
// 1) C++17 or later required (/std:c++17)
// 2) Link winhttp.lib and rsid.lib
// 3) All runtime config is read from config.json next to the executable.
//
// v5.0 changes: N-camera support (CameraWorker per camera, cross-enrolment, startup DB merge)
// v5.1 changes:
//   "preview": true  — OpenCV window per camera showing live feed with ROI boxes overlaid.
//                      Requires "camera_number" in each cameras[] entry when using multiple
//                      cameras (0-based Windows UVC device index). ESC closes preview & exits.
//   "log_to_file": true  — Appends all console output to F455SeatRouter.log in the exe folder,
//                           with a timestamp prefix on each line.

#include <RealSenseID/FaceAuthenticator.h>
#include <RealSenseID/AuthenticationCallback.h>
#include <RealSenseID/EnrollmentCallback.h>
#include <RealSenseID/DeviceConfig.h>
#include <RealSenseID/Faceprints.h>
#include <RealSenseID/Status.h>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// LOGGING
// ============================================================

static std::ofstream g_log_file;
static std::mutex    g_log_mutex;
static bool          g_log_to_file = false;

static void init_logging(const std::wstring& exe_dir, bool enabled)
{
    g_log_to_file = enabled;
    if (!enabled) return;
    std::wstring path = exe_dir + L"\\F455SeatRouter.log";
    g_log_file.open(path, std::ios::app);
    if (!g_log_file.is_open())
    {
        std::printf("[LOG] WARNING: Could not open F455SeatRouter.log — file logging disabled.\n");
        g_log_to_file = false;
    }
    else
        std::printf("[LOG] Logging to F455SeatRouter.log\n");
}

// Writes to stdout always; also writes with timestamp to log file when enabled.
static void log_printf(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    fputs(buf, stdout);
    fflush(stdout);

    if (!g_log_to_file) return;

    auto  now   = std::chrono::system_clock::now();
    auto  t     = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info{};
    localtime_s(&tm_info, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log_file << '[' << ts << "] " << buf;
    g_log_file.flush();
}

// ============================================================
// CONFIG STRUCTS
// ============================================================

struct SeatConfig
{
    std::string    seat_id;
    bool           enabled = false;
    unsigned short x = 0, y = 0, width = 0, height = 0;
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

// Maps one physical camera to the seats it covers.
// "port" overrides AppConfig::port for this camera.
// "camera_number" is the Windows UVC device index used by the Preview class.
//   -1 = auto-detect (only safe when a single camera is connected).
//   For two cameras, set 0 and 1 explicitly (see docs/two-camera-setup.md).
// If config.json has no "cameras" array, one CameraConfig is synthesised
// from AppConfig::port + all enabled seat_ids (backward compat).
struct CameraConfig
{
    std::string              camera_id;     // e.g. "cam_1"
    std::string              port;          // e.g. "COM5"
    int                      camera_number = -1; // UVC index for Preview (-1 = auto)
    std::vector<std::string> seat_ids;      // seat_ids this camera covers
};

enum class AppMode { Slot, Table };

struct TableConfig
{
    int employee_id   = 0;
    int play_time     = 360;
    int average_wager = 0;
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
    std::string port = "COM3";  // used if cameras array is empty
    RealSenseID::DeviceConfig::CameraRotation rotation =
        RealSenseID::DeviceConfig::CameraRotation::Rotation_90_Deg;
    RealSenseID::DeviceConfig::DumpMode dump_mode =
        RealSenseID::DeviceConfig::DumpMode::None;

    int  idle_timeout_s            = 5;
    int  switch_delay_s            = 15;
    int  auto_enrol_min_interval_s = 5;
    bool forbidden_causes_logout_when_locked = false;

    bool preview_enabled = false; // "preview": true
    bool log_to_file     = false; // "log_to_file": true

    AppMode     mode      = AppMode::Slot;
    TableConfig table_cfg;

    CmsConfig           cms;
    EnrollmentDefaults  enrol_defaults;
    std::vector<SeatConfig>   seats;
    std::vector<AssetRoute>   routes;
    std::vector<CameraConfig> cameras;  // one entry per physical camera
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

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], n, nullptr, nullptr);
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

    std::ifstream f(config_path_w);
    if (!f.is_open())
    {
        log_printf("[CONFIG] WARNING: config.json not found next to exe. Using built-in defaults.\n");
        return cfg;
    }

    try
    {
        auto j = nlohmann::json::parse(f, nullptr, true, true);

        if (j.contains("port"))                    cfg.port      = j["port"].get<std::string>();
        if (j.contains("rotation"))                cfg.rotation  = ParseRotation(j["rotation"].get<std::string>());
        if (j.contains("DumpMode"))                cfg.dump_mode = ParseDumpMode(j["DumpMode"].get<std::string>());
        if (j.contains("idle_timeout_s"))          cfg.idle_timeout_s = j["idle_timeout_s"].get<int>();
        if (j.contains("switch_delay_s"))          cfg.switch_delay_s = j["switch_delay_s"].get<int>();
        if (j.contains("auto_enrol_min_interval_s")) cfg.auto_enrol_min_interval_s = j["auto_enrol_min_interval_s"].get<int>();
        if (j.contains("forbidden_causes_logout_when_locked"))
            cfg.forbidden_causes_logout_when_locked = j["forbidden_causes_logout_when_locked"].get<bool>();
        if (j.contains("preview"))     cfg.preview_enabled = j["preview"].get<bool>();
        if (j.contains("log_to_file")) cfg.log_to_file     = j["log_to_file"].get<bool>();
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

        if (j.contains("cms"))
        {
            auto& jc = j["cms"];
            if (jc.contains("host"))                      cfg.cms.host = Utf8ToWide(jc["host"].get<std::string>());
            if (jc.contains("port"))                      cfg.cms.port = static_cast<INTERNET_PORT>(jc["port"].get<int>());
            if (jc.contains("path_players"))              cfg.cms.path_players = Utf8ToWide(jc["path_players"].get<std::string>());
            if (jc.contains("path_logins"))               cfg.cms.path_logins  = Utf8ToWide(jc["path_logins"].get<std::string>());
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

        // Multi-camera config
        if (j.contains("cameras") && j["cameras"].is_array())
        {
            for (const auto& jcam : j["cameras"])
            {
                CameraConfig cc;
                if (jcam.contains("camera_id"))     cc.camera_id     = jcam["camera_id"].get<std::string>();
                if (jcam.contains("port"))           cc.port          = jcam["port"].get<std::string>();
                if (jcam.contains("camera_number")) cc.camera_number = jcam["camera_number"].get<int>();
                if (jcam.contains("seats") && jcam["seats"].is_array())
                    for (const auto& s : jcam["seats"])
                        cc.seat_ids.push_back(s.get<std::string>());
                cfg.cameras.push_back(cc);
            }
        }

        // Backward compat: no cameras array -> synthesise one from port + all enabled seats
        if (cfg.cameras.empty())
        {
            CameraConfig cc;
            cc.camera_id     = "cam_1";
            cc.port          = cfg.port;
            cc.camera_number = -1;
            for (const auto& seat : cfg.seats)
                if (seat.enabled) cc.seat_ids.push_back(seat.seat_id);
            cfg.cameras.push_back(cc);
            log_printf("[CONFIG] No 'cameras' array found — using single-camera mode on %s\n",
                cc.port.c_str());
        }

        log_printf("[CONFIG] Loaded config.json. Cameras=%zu preview=%s log_to_file=%s\n",
            cfg.cameras.size(),
            cfg.preview_enabled ? "ON" : "OFF",
            cfg.log_to_file     ? "ON" : "OFF");
    }
    catch (const std::exception& ex)
    {
        log_printf("[CONFIG] ERROR parsing config.json: %s\n", ex.what());
        log_printf("[CONFIG] Falling back to built-in defaults.\n");
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

    std::string camera_id;       // which camera owns this seat
    std::string current_user_id; // playerId
    std::string current_card_id; // slot: resolved cardId; table: unused
    AssetRoute  current_route{};

    std::chrono::steady_clock::time_point last_seen_owner{};
    std::chrono::steady_clock::time_point cooldown_until{};

    bool        pending_auto_enrol = false;
    std::chrono::steady_clock::time_point last_auto_enrol{};

    // Two-phase enrolment
    std::string pending_enrol_user_id;
    std::string pending_enrol_card_id;  // non-empty = Phase 2 active

    // Table mode
    int         rating_id        = 0;
    std::chrono::steady_clock::time_point session_locked_at{};
};

std::unordered_map<std::string, SeatSession> g_seat_sessions;
std::mutex        g_state_mtx;
std::atomic<bool> g_running{true};

std::wstring g_exe_dir;

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

std::optional<AssetRoute> FindRouteForSeat(const AppConfig& cfg, const std::string& seat_id)
{
    for (const auto& r : cfg.routes)
        if (r.seat_id == seat_id) return r;
    return std::nullopt;
}

// Builds a DeviceConfig for a specific camera using only its assigned seats.
RealSenseID::DeviceConfig BuildCameraDeviceConfig(const AppConfig& appCfg,
                                                   const CameraConfig& camCfg)
{
    // Collect enabled seats assigned to this camera, in seat_ids order
    std::vector<const SeatConfig*> active;
    for (const auto& sid : camCfg.seat_ids)
        for (const auto& seat : appCfg.seats)
            if (seat.seat_id == sid && seat.enabled)
                { active.push_back(&seat); break; }

    size_t n = active.size();
    if (n < 1)
        throw std::runtime_error("Camera " + camCfg.camera_id + ": no enabled seats assigned.");
    if (n > RealSenseID::DeviceConfig::MAX_ROIS)
        throw std::runtime_error("Camera " + camCfg.camera_id + ": too many ROIs (max 5).");

    RealSenseID::DeviceConfig cfg;
    cfg.security_level        = RealSenseID::DeviceConfig::SecurityLevel::Low;
    cfg.algo_flow             = RealSenseID::DeviceConfig::AlgoFlow::All;
    cfg.face_selection_policy = (n > 1)
        ? RealSenseID::DeviceConfig::FaceSelectionPolicy::All
        : RealSenseID::DeviceConfig::FaceSelectionPolicy::Single;
    cfg.rect_enable              = 0x01;
    cfg.landmarks_enable         = 0;
    cfg.camera_rotation          = appCfg.rotation;
    cfg.dump_mode                = appCfg.dump_mode;
    cfg.matcher_confidence_level = RealSenseID::DeviceConfig::MatcherConfidenceLevel::Low;
    cfg.frontal_face_policy      = RealSenseID::DeviceConfig::FrontalFacePolicy::None;
    cfg.person_motion_mode       = RealSenseID::DeviceConfig::PersonMotionMode::Static;
    cfg.distance_limit           = RealSenseID::DeviceConfig::DistanceLimit::NoLimit;
    cfg.distance_enabled         = false;
    cfg.num_rois                 = static_cast<unsigned char>(n);

    for (size_t i = 0; i < n; ++i)
    {
        cfg.detection_rois[i].x      = active[i]->x;
        cfg.detection_rois[i].y      = active[i]->y;
        cfg.detection_rois[i].width  = active[i]->width;
        cfg.detection_rois[i].height = active[i]->height;
    }
    return cfg;
}

// Builds a single-ROI DeviceConfig for Phase 2 enrolment (prevents DuplicateFaceprints).
RealSenseID::DeviceConfig BuildSingleSeatDeviceConfig(const AppConfig& appCfg,
                                                       const std::string& seat_id)
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

// Resolve which seat a detected face belongs to, filtering to the allowed set.
std::optional<std::string> ResolveSeatForFace(const RealSenseID::FaceRect& face,
                                               const AppConfig& cfg,
                                               const std::vector<std::string>& allowed_seat_ids)
{
    const int cx = face.x + face.w / 2;
    const int cy = face.y + face.h / 2;

    std::optional<std::string> match;
    for (const auto& seat : cfg.seats)
    {
        if (!seat.enabled) continue;
        bool allowed = false;
        for (const auto& id : allowed_seat_ids)
            if (id == seat.seat_id) { allowed = true; break; }
        if (!allowed) continue;

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
        g_http_session = WinHttpOpen(L"F455SeatRouter/5.1",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_http_session)
        {
            log_printf("[HTTP] WinHttpOpen failed (err=%lu)\n", GetLastError());
            return false;
        }
    }
    if (!g_http_connect)
    {
        g_http_connect = WinHttpConnect(g_http_session, cms.host.c_str(), cms.port, 0);
        if (!g_http_connect)
        {
            log_printf("[HTTP] WinHttpConnect failed (err=%lu)\n", GetLastError());
            WinHttpCloseHandle(g_http_session); g_http_session = nullptr;
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
    if (!init_http(cms)) { log_printf("[HTTP] init_http() failed.\n"); return result; }
    auto t0 = steady_clock::now();
    HINTERNET h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_http_mtx);
        LPCWSTR hdr = L"Content-Type: application/json\r\n";
        h = WinHttpOpenRequest(g_http_connect, L"POST", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!h) goto cleanup_post;
        if (cms.ignore_cert_errors)
        {
            DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof(f));
        }
        if (!WinHttpSendRequest(h, hdr, -1L,
            json_body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)json_body.data(),
            (DWORD)json_body.size(), (DWORD)json_body.size(), 0)) goto cleanup_post;
        if (!WinHttpReceiveResponse(h, nullptr)) goto cleanup_post;
        { DWORD sc=0,sz=sizeof(sc);
          if (WinHttpQueryHeaders(h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
              WINHTTP_HEADER_NAME_BY_INDEX,&sc,&sz,WINHTTP_NO_HEADER_INDEX)) result.status=(int)sc; }
        { std::string b; for(;;){ DWORD av=0;
            if(!WinHttpQueryDataAvailable(h,&av)||av==0) break;
            std::vector<char> buf(av); DWORD rd=0;
            if(!WinHttpReadData(h,buf.data(),av,&rd)||rd==0) break;
            b.append(buf.data(),rd); } result.body=std::move(b); }
    cleanup_post: if(h) WinHttpCloseHandle(h);
    }
    log_printf("[HTTP] POST %s  %ldms  status=%d\n", WideToUtf8(path).c_str(),
        (long)duration_cast<milliseconds>(steady_clock::now()-t0).count(), result.status);
    result.ok = (result.status != 0);
    return result;
}

HttpResponse http_get_json(const CmsConfig& cms, const wchar_t* path)
{
    using namespace std::chrono;
    HttpResponse result;
    if (!init_http(cms)) { log_printf("[HTTP] init_http() failed.\n"); return result; }
    auto t0 = steady_clock::now();
    HINTERNET h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_http_mtx);
        h = WinHttpOpenRequest(g_http_connect, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!h) goto cleanup_get;
        if (cms.ignore_cert_errors)
        {
            DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof(f));
        }
        if (!WinHttpSendRequest(h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup_get;
        if (!WinHttpReceiveResponse(h, nullptr)) goto cleanup_get;
        { DWORD sc=0,sz=sizeof(sc);
          if (WinHttpQueryHeaders(h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
              WINHTTP_HEADER_NAME_BY_INDEX,&sc,&sz,WINHTTP_NO_HEADER_INDEX)) result.status=(int)sc; }
        { std::string b; for(;;){ DWORD av=0;
            if(!WinHttpQueryDataAvailable(h,&av)||av==0) break;
            std::vector<char> buf(av); DWORD rd=0;
            if(!WinHttpReadData(h,buf.data(),av,&rd)||rd==0) break;
            b.append(buf.data(),rd); } result.body=std::move(b); }
    cleanup_get: if(h) WinHttpCloseHandle(h);
    }
    log_printf("[HTTP] GET  %s  %ldms  status=%d\n", WideToUtf8(path).c_str(),
        (long)duration_cast<milliseconds>(steady_clock::now()-t0).count(), result.status);
    result.ok = (result.status != 0);
    return result;
}

HttpResponse http_patch_json(const CmsConfig& cms, const wchar_t* path, const std::string& json_body)
{
    using namespace std::chrono;
    HttpResponse result;
    if (!init_http(cms)) { log_printf("[HTTP] init_http() failed.\n"); return result; }
    auto t0 = steady_clock::now();
    HINTERNET h = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_http_mtx);
        LPCWSTR hdr = L"Content-Type: application/json\r\n";
        h = WinHttpOpenRequest(g_http_connect, L"PATCH", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!h) goto cleanup_patch;
        if (cms.ignore_cert_errors)
        {
            DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof(f));
        }
        if (!WinHttpSendRequest(h, hdr, -1L,
            json_body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)json_body.data(),
            (DWORD)json_body.size(), (DWORD)json_body.size(), 0)) goto cleanup_patch;
        if (!WinHttpReceiveResponse(h, nullptr)) goto cleanup_patch;
        { DWORD sc=0,sz=sizeof(sc);
          if (WinHttpQueryHeaders(h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
              WINHTTP_HEADER_NAME_BY_INDEX,&sc,&sz,WINHTTP_NO_HEADER_INDEX)) result.status=(int)sc; }
        { std::string b; for(;;){ DWORD av=0;
            if(!WinHttpQueryDataAvailable(h,&av)||av==0) break;
            std::vector<char> buf(av); DWORD rd=0;
            if(!WinHttpReadData(h,buf.data(),av,&rd)||rd==0) break;
            b.append(buf.data(),rd); } result.body=std::move(b); }
    cleanup_patch: if(h) WinHttpCloseHandle(h);
    }
    log_printf("[HTTP] PATCH %s  %ldms  status=%d\n", WideToUtf8(path).c_str(),
        (long)duration_cast<milliseconds>(steady_clock::now()-t0).count(), result.status);
    result.ok = (result.status != 0);
    return result;
}

int extract_int_field(const std::string& body, const std::string& field)
{
    std::string key = "\"" + field + "\":";
    auto pos = body.find(key);
    if (pos == std::string::npos) return 0;
    pos += key.size();
    while (pos < body.size() && (body[pos]==' '||body[pos]=='\t')) ++pos;
    int v=0; bool any=false;
    while (pos < body.size() && body[pos]>='0' && body[pos]<='9')
        { any=true; v=v*10+(body[pos++]-'0'); }
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
    log_printf("[EGM] LOGIN  %s:%d%s\n",
        WideToUtf8(cms.host).c_str(),(int)cms.port,WideToUtf8(cms.path_logins).c_str());
    log_printf("[EGM] LOGIN  payload=%s\n", payload.c_str());
    auto resp = http_post_json(cms, cms.path_logins.c_str(), payload);
    log_printf("[EGM] LOGIN  status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(),300), resp.body.c_str());
    return resp.ok && resp.status >= 200 && resp.status < 300;
}

bool egm_logout(const CmsConfig& cms, const AssetRoute& route, const std::string& card_id)
{
    std::string payload =
        "{\"cardId\":\"" + card_id + "\",\"assetId\":\"" + route.asset_id + "\"}";
    log_printf("[EGM] LOGOUT %s:%d%s\n",
        WideToUtf8(cms.host).c_str(),(int)cms.port,WideToUtf8(cms.path_logouts).c_str());
    log_printf("[EGM] LOGOUT payload=%s\n", payload.c_str());
    auto resp = http_post_json(cms, cms.path_logouts.c_str(), payload);
    log_printf("[EGM] LOGOUT status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(),300), resp.body.c_str());
    return resp.ok && resp.status >= 200 && resp.status < 300;
}

bool egm_auto_enrol(const AppConfig& cfg, NewPatronInfo& out)
{
    std::string player_payload =
        "{\"firstName\":\"" + cfg.enrol_defaults.first_name +
        "\",\"lastName\":\"" + cfg.enrol_defaults.last_name +
        "\",\"birthDate\":\"" + cfg.enrol_defaults.birth_date +
        "\",\"address\":{\"addressLine1\":\"" + cfg.enrol_defaults.address_line1 +
        "\",\"postalCode\":\"" + cfg.enrol_defaults.postal_code +
        "\",\"countryCode\":\"" + cfg.enrol_defaults.country_code + "\"}}";

    log_printf("[EGM] AUTO-ENROL: POST %s\n", WideToUtf8(cfg.cms.path_players).c_str());
    auto pr = http_post_json(cfg.cms, cfg.cms.path_players.c_str(), player_payload);
    log_printf("[EGM] AUTO-ENROL: /players status=%d body=%.*s\n", pr.status,
        (int)std::min<size_t>(pr.body.size(),300), pr.body.c_str());
    if (!pr.ok || pr.status < 200 || pr.status >= 300)
        { log_printf("[EGM] AUTO-ENROL ERROR: /players failed.\n"); return false; }

    int player_id = extract_int_field(pr.body, "playerId");
    if (player_id <= 0)
        { log_printf("[EGM] AUTO-ENROL ERROR: could not parse playerId.\n"); return false; }

    char path_buf[256];
    std::snprintf(path_buf, sizeof(path_buf), cfg.cms.card_create_path_template.c_str(), player_id);
    std::wstring card_path_w = Utf8ToWide(path_buf);

    std::string card_payload = "{\"playerPin\":\"" + cfg.enrol_defaults.player_pin + "\"}";
    auto cr = http_post_json(cfg.cms, card_path_w.c_str(), card_payload);
    log_printf("[EGM] AUTO-ENROL: /cards status=%d body=%.*s\n", cr.status,
        (int)std::min<size_t>(cr.body.size(),300), cr.body.c_str());
    if (!cr.ok || cr.status < 200 || cr.status >= 300)
        { log_printf("[EGM] AUTO-ENROL ERROR: /cards failed.\n"); return false; }

    int card_id = extract_int_field(cr.body, "cardId");
    if (card_id <= 0)
        { log_printf("[EGM] AUTO-ENROL ERROR: could not parse cardId.\n"); return false; }

    out.card_id = std::to_string(card_id);
    out.user_id = std::to_string(player_id); // store playerId on camera
    log_printf("[EGM] AUTO-ENROL SUCCESS: playerId=%d cardId=%d\n", player_id, card_id);
    return true;
}

std::string cms_lookup_active_card(const CmsConfig& cms, int player_id)
{
    char path_buf[256];
    std::snprintf(path_buf, sizeof(path_buf), cms.card_create_path_template.c_str(), player_id);
    std::wstring path_w = Utf8ToWide(path_buf);
    log_printf("[CMS] GET cards for playerId=%d\n", player_id);
    auto resp = http_get_json(cms, path_w.c_str());
    log_printf("[CMS] GET cards status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(),400), resp.body.c_str());
    if (!resp.ok || resp.status < 200 || resp.status >= 300) return {};
    try
    {
        auto j = nlohmann::json::parse(resp.body);
        if (!j.is_array() || j.empty()) return {};
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
        return first_active;
    }
    catch (const std::exception& ex)
    { log_printf("[CMS] GET cards parse error: %s\n", ex.what()); return {}; }
}

// ============================================================
// TABLE GAME LOGIN / LOGOUT
// ============================================================

struct TableLoginResult { bool ok = false; int rating_id = 0; };

TableLoginResult egm_table_login(const CmsConfig& cms, const AssetRoute& route,
    const TableConfig& table_cfg, const std::string& player_id, int seat_number)
{
    std::string payload =
        "{\"status\":\"open\""
        ",\"deviceId\":"   + std::to_string(route.device_id) +
        ",\"gameId\":"     + std::to_string(route.game_id) +
        ",\"playerId\":"   + player_id +
        ",\"seat\":"       + std::to_string(seat_number) +
        ",\"employeeId\":" + std::to_string(table_cfg.employee_id) + "}";
    log_printf("[TABLE] LOGIN  POST %s:%d%s\n",
        WideToUtf8(cms.host).c_str(),(int)cms.port,WideToUtf8(cms.path_logins).c_str());
    log_printf("[TABLE] LOGIN  payload=%s\n", payload.c_str());
    auto resp = http_post_json(cms, cms.path_logins.c_str(), payload);
    log_printf("[TABLE] LOGIN  status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(),500), resp.body.c_str());
    if (!resp.ok || resp.status < 200 || resp.status >= 300)
        { log_printf("[TABLE] LOGIN FAILED (HTTP %d)\n", resp.status); return {}; }
    int rid = extract_int_field(resp.body, "ratingId");
    if (rid <= 0)
        { log_printf("[TABLE] LOGIN ERROR: could not parse ratingId\n"); return {}; }
    log_printf("[TABLE] LOGIN SUCCESS: ratingId=%d\n", rid);
    return { true, rid };
}

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
    std::wstring path = cms.path_logouts + std::to_wstring(rating_id);
    log_printf("[TABLE] LOGOUT PATCH %s:%d%s\n",
        WideToUtf8(cms.host).c_str(),(int)cms.port,WideToUtf8(path).c_str());
    log_printf("[TABLE] LOGOUT payload=%s\n", payload.c_str());
    auto resp = http_patch_json(cms, path.c_str(), payload);
    log_printf("[TABLE] LOGOUT status=%d body=%.*s\n", resp.status,
        (int)std::min<size_t>(resp.body.size(),500), resp.body.c_str());
    if (!resp.ok || resp.status < 200 || resp.status >= 300)
        { log_printf("[TABLE] LOGOUT FAILED (HTTP %d)\n", resp.status); return false; }
    bool closed = resp.body.find("\"status\":\"closed\"")  != std::string::npos ||
                  resp.body.find("\"status\": \"closed\"") != std::string::npos;
    if (closed) log_printf("[TABLE] LOGOUT SUCCESS: ratingId=%d closed\n", rating_id);
    else        log_printf("[TABLE] LOGOUT WARNING: ratingId=%d not confirmed closed\n", rating_id);
    return closed;
}

// ============================================================
// RATINGS STATE PERSISTENCE
// ============================================================

void save_ratings_state()
{
    nlohmann::json j = nlohmann::json::object();
    {
        std::lock_guard<std::mutex> lock(g_state_mtx);
        for (const auto& [seat_id, session] : g_seat_sessions)
            if (session.rating_id > 0)
                j[seat_id] = { {"rating_id", session.rating_id},
                               {"player_id", session.current_user_id} };
    }
    std::wstring path = g_exe_dir + L"\\ratings_state.json";
    std::ofstream f(path);
    if (f.is_open()) { f << j.dump(2);
        log_printf("[STATE] ratings_state.json saved (%zu open rating(s))\n",(size_t)j.size()); }
    else log_printf("[STATE] WARNING: could not write ratings_state.json\n");
}

void load_ratings_state()
{
    std::wstring path = g_exe_dir + L"\\ratings_state.json";
    std::ifstream f(path);
    if (!f.is_open()) { log_printf("[STATE] No ratings_state.json — starting fresh\n"); return; }
    try
    {
        auto j   = nlohmann::json::parse(f);
        auto now = std::chrono::steady_clock::now();
        int  recovered = 0;
        std::lock_guard<std::mutex> lock(g_state_mtx);
        for (auto& [seat_id, session] : g_seat_sessions)
        {
            if (!j.contains(seat_id)) continue;
            int rid = j[seat_id].value("rating_id", 0);
            std::string pid = j[seat_id].value("player_id", "");
            if (rid <= 0 || pid.empty()) continue;
            session.rating_id       = rid;
            session.current_user_id = pid;
            session.last_seen_owner = now;
            session.state           = SessionState::LockedToUser;
            ++recovered;
            log_printf("[STATE][%s] Recovered open rating %d playerId=%s\n",
                seat_id.c_str(), rid, pid.c_str());
        }
        log_printf("[STATE] Recovered %d open rating(s)\n", recovered);
    }
    catch (const std::exception& ex)
    { log_printf("[STATE] WARNING: load failed: %s\n", ex.what()); }
}

// ============================================================
// CROSS-ENROLMENT
// ============================================================

// A faceprint entry to be imported on another camera.
struct CrossEnrolJob
{
    std::string                 user_id;
    RealSenseID::UserFaceprints faceprints;
};

// Forward declaration — used in CameraWorker.
class CameraWorker;
std::vector<CameraWorker*> g_camera_workers;
std::mutex                 g_camera_workers_mtx;

// ============================================================
// PREVIEW CAPTURE
// Uses OpenCV VideoCapture to read frames from the F455's UVC
// (USB video) interface — a separate USB endpoint from the serial
// authentication channel, so both run simultaneously.
// A background thread continuously reads frames; TryGetFrame()
// is called from the main thread display loop to retrieve the latest.
// ============================================================

class CameraPreviewCapture
{
public:
    // Open the UVC device at `camera_number` and start the capture thread.
    bool Start(int camera_number, const std::string& camera_id)
    {
        _camera_id = camera_id;
        _cap.open(camera_number, cv::CAP_DSHOW); // DirectShow — native on Windows
        if (!_cap.isOpened())
        {
            log_printf("[PREVIEW][%s] VideoCapture(%d) failed to open — "
                       "check camera_number in config.json\n",
                       _camera_id.c_str(), camera_number);
            return false;
        }
        log_printf("[PREVIEW][%s] VideoCapture(%d) opened\n",
                   _camera_id.c_str(), camera_number);
        _running = true;
        _thread  = std::thread(&CameraPreviewCapture::Run, this);
        return true;
    }

    void Stop()
    {
        _running = false;
        if (_thread.joinable()) _thread.join();
        _cap.release();
    }

    // Returns true and fills `out` (BGR Mat with ROI boxes) if a new frame
    // is available. Called from the main thread — thread-safe.
    bool TryGetFrame(cv::Mat& out,
                     const AppConfig& app_cfg,
                     const CameraConfig& cam_cfg)
    {
        std::lock_guard<std::mutex> lk(_mtx);
        if (!_has_frame) return false;
        _has_frame = false;
        out = _latest.clone();

        // Overlay each seat's ROI as a labelled green rectangle.
        for (const auto& seat_id : cam_cfg.seat_ids)
        {
            for (const auto& seat : app_cfg.seats)
            {
                if (seat.seat_id != seat_id || !seat.enabled) continue;
                if (seat.width == 0 || seat.height == 0) continue;
                cv::Rect roi(seat.x, seat.y, seat.width, seat.height);
                cv::rectangle(out, roi, cv::Scalar(0, 255, 0), 2);
                cv::putText(out, seat.seat_id,
                    cv::Point(seat.x + 8, seat.y + 36),
                    cv::FONT_HERSHEY_SIMPLEX, 1.1,
                    cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
                break;
            }
        }
        return true;
    }

private:
    void Run()
    {
        cv::Mat frame;
        while (_running)
        {
            if (_cap.read(frame) && !frame.empty())
            {
                std::lock_guard<std::mutex> lk(_mtx);
                _latest    = frame.clone();
                _has_frame = true;
            }
            // Small sleep avoids spinning at full CPU when camera delivers ~30fps
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::string       _camera_id;
    cv::VideoCapture  _cap;
    std::thread       _thread;
    std::atomic<bool> _running{false};
    std::mutex        _mtx;
    cv::Mat           _latest;
    bool              _has_frame = false;
};

// ============================================================
// CALLBACKS
// ============================================================

class SimpleEnrolCallback : public RealSenseID::EnrollmentCallback
{
public:
    void OnResult(RealSenseID::EnrollStatus status) override
    { log_printf("[ENROL] OnResult: %d\n", (int)status); }
    void OnProgress(RealSenseID::FacePose pose) override
    { log_printf("[ENROL] OnProgress: pose=%d\n", (int)pose); }
    void OnHint(RealSenseID::EnrollStatus hint, float cov) override
    { log_printf("[ENROL] OnHint: %d cov=%.2f\n", (int)hint, (double)cov); }
};

// Authentication callback — one instance per camera.
class CameraAwareAuthCallback : public RealSenseID::AuthenticationCallback
{
public:
    explicit CameraAwareAuthCallback(const AppConfig& cfg, const CameraConfig& cam_cfg)
        : _cfg(cfg), _cam_cfg(cam_cfg) {}

    void OnFaceDetected(const std::vector<RealSenseID::FaceRect>& faces,
                        const unsigned int timestamp) override
    {
        std::lock_guard<std::mutex> lk(_face_mtx);
        _result_index = 0;
        _face_seats.clear();
        log_printf("[FACE][%s] faces=%zu timestamp=%u\n",
            _cam_cfg.camera_id.c_str(), faces.size(), timestamp);
        for (size_t i = 0; i < faces.size(); ++i)
        {
            const auto& f = faces[i];
            auto seat = ResolveSeatForFace(f, _cfg, _cam_cfg.seat_ids);
            log_printf("  Face %zu x=%d y=%d w=%d h=%d center=(%d,%d) -> %s\n",
                i, f.x, f.y, f.w, f.h, f.x+f.w/2, f.y+f.h/2,
                seat.has_value() ? seat->c_str() : "none/ambiguous");
            _face_seats.push_back(seat);
        }
    }

    void OnResult(RealSenseID::AuthenticateStatus status,
                  const char* user_id, short score) override
    {
        using namespace std::chrono;
        using RealSenseID::AuthenticateStatus;
        const std::string uid = user_id ? user_id : "";

        std::optional<std::string> resolved_seat;
        {
            std::lock_guard<std::mutex> lk(_face_mtx);
            if (_result_index < _face_seats.size())
                resolved_seat = _face_seats[_result_index];
            ++_result_index;
        }

        log_printf("[AUTH][%s] OnResult[%zu]: status=%d user_id=%s score=%d seat=%s\n",
            _cam_cfg.camera_id.c_str(), _result_index-1, (int)status, uid.c_str(), (int)score,
            resolved_seat.has_value() ? resolved_seat->c_str() : "<none>");

        if (!resolved_seat.has_value())
        {
            log_printf("[AUTH][%s] No resolved seat — ignored\n", _cam_cfg.camera_id.c_str());
            return;
        }

        const bool is_success  = (status == AuthenticateStatus::Success);
        const bool is_forbidden = (status == AuthenticateStatus::Forbidden);
        if (!is_success && !is_forbidden) return;

        auto route_opt = FindRouteForSeat(_cfg, *resolved_seat);
        if (!route_opt.has_value())
        {
            log_printf("[AUTH][%s] No route for seat %s — ignored\n",
                _cam_cfg.camera_id.c_str(), resolved_seat->c_str());
            return;
        }

        auto now = steady_clock::now();
        std::lock_guard<std::mutex> state_lock(g_state_mtx);
        auto& session = g_seat_sessions[*resolved_seat];

        // -- LOCKED --------------------------------------------------------------
        if (session.state == SessionState::LockedToUser)
        {
            if (is_success && uid == session.current_user_id)
            {
                session.last_seen_owner = now;
                log_printf("[SESSION][%s][%s] Owner %s re-confirmed\n",
                    _cam_cfg.camera_id.c_str(), resolved_seat->c_str(), uid.c_str());
                return;
            }
            if (is_success)
            {
                log_printf("[SESSION][%s][%s] Other user %s seen while locked to %s — ignored\n",
                    _cam_cfg.camera_id.c_str(), resolved_seat->c_str(),
                    uid.c_str(), session.current_user_id.c_str());
                return;
            }
            if (is_forbidden) return;
            return;
        }

        // -- COOLDOWN ------------------------------------------------------------
        if (session.state == SessionState::Cooldown)
        {
            log_printf("[SESSION][%s][%s] In cooldown — ignored\n",
                _cam_cfg.camera_id.c_str(), resolved_seat->c_str());
            return;
        }

        // -- UNLOCKED + Success --------------------------------------------------
        if (is_success)
        {
            // Check for this patron already logged in on another seat — auto-logout there first.
            for (auto& [other_seat_id, other_session] : g_seat_sessions)
            {
                if (other_seat_id == *resolved_seat) continue;
                if (other_session.state != SessionState::LockedToUser) continue;
                if (other_session.current_user_id != uid) continue;

                log_printf("[SESSION] Auto-logout: patron %s moving from %s to %s\n",
                    uid.c_str(), other_seat_id.c_str(), resolved_seat->c_str());

                auto old_route  = other_session.current_route;
                auto old_card   = other_session.current_card_id;
                auto old_rating = other_session.rating_id;
                auto old_mode   = _cfg.mode;
                auto old_tbl    = _cfg.table_cfg;
                auto old_cms    = _cfg.cms;

                other_session.state = SessionState::Unlocked;
                other_session.current_user_id.clear();
                other_session.current_card_id.clear();
                other_session.rating_id = 0;

                if (old_mode == AppMode::Slot)
                    std::thread([old_cms, old_route, old_card]()
                        { egm_logout(old_cms, old_route, old_card); }).detach();
                else
                    std::thread([old_cms, old_route, old_tbl, old_rating]()
                        { egm_table_logout(old_cms, old_route, old_tbl, old_rating); }).detach();
            }

            // Lock this seat
            session.current_user_id   = uid;
            session.current_card_id   = "";
            session.current_route     = *route_opt;
            session.last_seen_owner   = now;
            session.state             = SessionState::LockedToUser;
            session.rating_id         = 0;
            session.session_locked_at = now;
            session.camera_id         = _cam_cfg.camera_id;

            std::string seat_id_copy = *resolved_seat;
            AssetRoute  route        = session.current_route;
            CmsConfig   cms          = _cfg.cms;
            int         player_id_i  = std::atoi(uid.c_str());

            if (_cfg.mode == AppMode::Slot)
            {
                log_printf("[SESSION][%s][%s] Locked (slot): playerId=%s asset=%s\n",
                    _cam_cfg.camera_id.c_str(), resolved_seat->c_str(),
                    uid.c_str(), route.asset_id.c_str());
                std::thread([cms, route, seat_id_copy, player_id_i]()
                {
                    std::string card_id = cms_lookup_active_card(cms, player_id_i);
                    if (card_id.empty())
                    {
                        log_printf("[SESSION][%s] Card lookup failed — cannot login\n",
                            seat_id_copy.c_str());
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_state_mtx);
                        auto it = g_seat_sessions.find(seat_id_copy);
                        if (it != g_seat_sessions.end() &&
                            it->second.state == SessionState::LockedToUser)
                            it->second.current_card_id = card_id;
                    }
                    log_printf("[SESSION][%s] Resolved playerId=%d -> cardId=%s\n",
                        seat_id_copy.c_str(), player_id_i, card_id.c_str());
                    egm_login(cms, route, card_id);
                }).detach();
            }
            else // Table
            {
                int         seat_num = ExtractSeatNumber(seat_id_copy);
                TableConfig tbl      = _cfg.table_cfg;
                log_printf("[SESSION][%s][%s] Locked (table): playerId=%s deviceId=%d seat=%d\n",
                    _cam_cfg.camera_id.c_str(), resolved_seat->c_str(),
                    uid.c_str(), route.device_id, seat_num);
                std::thread([cms, route, seat_id_copy, uid, seat_num, tbl]()
                {
                    auto result = egm_table_login(cms, route, tbl, uid, seat_num);
                    if (!result.ok)
                    {
                        log_printf("[SESSION][%s] Table login FAILED\n", seat_id_copy.c_str());
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

        // -- UNLOCKED + Forbidden -> queue auto-enrol ----------------------------
        if (is_forbidden)
        {
            session.pending_auto_enrol = true;
            session.camera_id          = _cam_cfg.camera_id;
            log_printf("[SESSION][%s][%s] Forbidden -> pending auto-enrol\n",
                _cam_cfg.camera_id.c_str(), resolved_seat->c_str());
        }
    }

    void OnHint(RealSenseID::AuthenticateStatus hint, float fs) override
    { log_printf("[AUTH][%s] OnHint: %d fs=%.2f\n",
        _cam_cfg.camera_id.c_str(), (int)hint, (double)fs); }

private:
    const AppConfig&    _cfg;
    const CameraConfig& _cam_cfg;
    std::mutex          _face_mtx;
    std::vector<std::optional<std::string>> _face_seats;
    size_t              _result_index = 0;
};

// ============================================================
// CAMERA WORKER
// ============================================================

class CameraWorker
{
public:
    CameraWorker(const CameraConfig& cam_cfg, const AppConfig& app_cfg)
        : _cam_cfg(cam_cfg), _app_cfg(app_cfg) {}

    CameraWorker(const CameraWorker&)            = delete;
    CameraWorker& operator=(const CameraWorker&) = delete;

    // Connect to camera and apply DeviceConfig (called before Start() and by startup merge).
    bool Connect()
    {
        auto st = _auth.Connect({_cam_cfg.port.c_str()});
        log_printf("[CAM][%s] Connect status=%d (port=%s)\n",
            _cam_cfg.camera_id.c_str(), (int)st, _cam_cfg.port.c_str());
        return st == RealSenseID::Status::Ok;
    }

    bool ApplyDeviceConfig()
    {
        auto dcfg = BuildCameraDeviceConfig(_app_cfg, _cam_cfg);
        auto st   = _auth.SetDeviceConfig(dcfg);
        log_printf("[CAM][%s] SetDeviceConfig status=%d\n",
            _cam_cfg.camera_id.c_str(), (int)st);
        return st == RealSenseID::Status::Ok;
    }

    void Disconnect() { _auth.Disconnect(); }

    // Get all enrolled users from this camera's DB as UserFaceprints.
    // Call only when camera is connected and auth loop is NOT running.
    bool GetAllUserFaceprints(std::vector<RealSenseID::UserFaceprints>& out)
    {
        unsigned int n = 0;
        if (_auth.QueryNumberOfUsers(n) != RealSenseID::Status::Ok || n == 0)
        {
            log_printf("[CAM][%s] GetAllUserFaceprints: %u users\n",
                _cam_cfg.camera_id.c_str(), n);
            return true; // empty DB is valid
        }

        std::vector<std::vector<char>> id_bufs(n,
            std::vector<char>(RealSenseID::MAX_USERID_LENGTH + 1, '\0'));
        std::vector<char*> id_ptrs(n);
        for (unsigned int i = 0; i < n; ++i) id_ptrs[i] = id_bufs[i].data();

        if (_auth.QueryUserIds(id_ptrs.data(), n) != RealSenseID::Status::Ok)
        {
            log_printf("[CAM][%s] QueryUserIds failed\n", _cam_cfg.camera_id.c_str());
            return false;
        }

        std::vector<RealSenseID::Faceprints> fps(n);
        if (_auth.GetUsersFaceprints(fps.data(), n) != RealSenseID::Status::Ok)
        {
            log_printf("[CAM][%s] GetUsersFaceprints failed\n", _cam_cfg.camera_id.c_str());
            return false;
        }

        out.resize(n);
        for (unsigned int i = 0; i < n; ++i)
        {
            strncpy_s(out[i].user_id, RealSenseID::MAX_USERID_LENGTH + 1,
                      id_ptrs[i], RealSenseID::MAX_USERID_LENGTH);
            out[i].faceprints = fps[i];
        }
        log_printf("[CAM][%s] Exported %u user faceprint(s)\n", _cam_cfg.camera_id.c_str(), n);
        return true;
    }

    // Import faceprint entries. Entries already present are skipped (not an error).
    bool ImportFaceprints(std::vector<RealSenseID::UserFaceprints>& fps)
    {
        if (fps.empty()) return true;
        auto st = _auth.SetUsersFaceprints(fps.data(), (unsigned int)fps.size());
        log_printf("[CAM][%s] SetUsersFaceprints(%zu entries) status=%d\n",
            _cam_cfg.camera_id.c_str(), fps.size(), (int)st);
        return st == RealSenseID::Status::Ok;
    }

    // Queue a cross-enrolment job for import during the next auth loop gap.
    void QueueCrossEnrol(const CrossEnrolJob& job)
    {
        std::lock_guard<std::mutex> lk(_xq_mtx);
        _xq.push(job);
        log_printf("[CAM][%s] Cross-enrol queued for playerId=%s (queue depth=%zu)\n",
            _cam_cfg.camera_id.c_str(), job.user_id.c_str(), _xq.size());
    }

    const std::string& CameraId() const { return _cam_cfg.camera_id; }

    // Start preview (if enabled) and the auth loop thread.
    void Start()
    {
        if (_app_cfg.preview_enabled)
            _preview_cap.Start(_cam_cfg.camera_number, _cam_cfg.camera_id);

        _running = true;
        _thread  = std::thread(&CameraWorker::Run, this);
    }

    void Stop()
    {
        _running = false;
        if (_app_cfg.preview_enabled)
            _preview_cap.Stop();
        if (_thread.joinable()) _thread.join();
    }

    // Called from the main thread display loop to retrieve the latest preview frame.
    bool TryGetFrame(cv::Mat& out)
    {
        return _preview_cap.TryGetFrame(out, _app_cfg, _cam_cfg);
    }

private:
    // Drain the cross-enrol queue — called between Authenticate() calls.
    void DrainCrossEnrolQueue()
    {
        std::unique_lock<std::mutex> lk(_xq_mtx);
        if (_xq.empty()) return;

        std::queue<CrossEnrolJob> local;
        std::swap(local, _xq);
        lk.unlock();

        while (!local.empty())
        {
            auto& job = local.front();
            log_printf("[CAM][%s] Importing cross-enrol for playerId=%s\n",
                _cam_cfg.camera_id.c_str(), job.user_id.c_str());

            std::vector<RealSenseID::UserFaceprints> entry = { job.faceprints };
            auto st = _auth.SetUsersFaceprints(entry.data(), 1);
            log_printf("[CAM][%s] SetUsersFaceprints status=%d\n",
                _cam_cfg.camera_id.c_str(), (int)st);

            if (st != RealSenseID::Status::Ok)
            {
                std::lock_guard<std::mutex> re_lk(_xq_mtx);
                _xq.push(job);
                log_printf("[CAM][%s] Cross-enrol failed — will retry\n",
                    _cam_cfg.camera_id.c_str());
            }
            local.pop();
        }
    }

    // Export faceprints for a single user from this camera's DB.
    bool ExportUserFaceprints(const std::string& user_id,
                              RealSenseID::UserFaceprints& out)
    {
        std::vector<RealSenseID::UserFaceprints> all;
        if (!GetAllUserFaceprints(all)) return false;
        for (auto& ufp : all)
            if (std::string(ufp.user_id) == user_id)
                { out = ufp; return true; }
        log_printf("[CAM][%s] ExportUserFaceprints: playerId=%s not found\n",
            _cam_cfg.camera_id.c_str(), user_id.c_str());
        return false;
    }

    void Run()
    {
        log_printf("[CAM][%s] Auth loop started\n", _cam_cfg.camera_id.c_str());
        CameraAwareAuthCallback cb(_app_cfg, _cam_cfg);

        while (_running && g_running)
        {
            auto ast = _auth.Authenticate(cb);
            log_printf("[CAM][%s] Authenticate() Status=%d\n",
                _cam_cfg.camera_id.c_str(), (int)ast);
            if (ast != RealSenseID::Status::Ok)
                std::this_thread::sleep_for(std::chrono::milliseconds(300));

            // -- Phase 1: Create SYNKROS records for pending seats ---------------
            {
                using SeatEnrol = std::pair<std::string, AssetRoute>;
                std::vector<SeatEnrol> cms_pending;
                {
                    std::lock_guard<std::mutex> lock(g_state_mtx);
                    auto now = std::chrono::steady_clock::now();
                    for (auto& [seat_id, session] : g_seat_sessions)
                    {
                        if (session.camera_id != _cam_cfg.camera_id) continue;
                        if (!session.pending_auto_enrol) continue;
                        if (session.state != SessionState::Unlocked) continue;
                        if (!session.pending_enrol_card_id.empty()) continue;

                        if (session.last_auto_enrol.time_since_epoch().count() != 0)
                        {
                            double secs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - session.last_auto_enrol).count() / 1000.0;
                            if (secs < _app_cfg.auto_enrol_min_interval_s)
                            {
                                log_printf("[CAM][%s][%s] Throttled (%.1fs ago)\n",
                                    _cam_cfg.camera_id.c_str(), seat_id.c_str(), secs);
                                session.pending_auto_enrol = false;
                                continue;
                            }
                        }
                        auto route = FindRouteForSeat(_app_cfg, seat_id);
                        if (!route.has_value())
                            { session.pending_auto_enrol = false; continue; }
                        session.pending_auto_enrol = false;
                        session.last_auto_enrol    = now;
                        cms_pending.push_back({seat_id, *route});
                    }
                }
                for (auto& [seat_id, route] : cms_pending)
                {
                    log_printf("[CAM][%s][%s] Phase 1 — creating CMS records\n",
                        _cam_cfg.camera_id.c_str(), seat_id.c_str());
                    NewPatronInfo info;
                    if (egm_auto_enrol(_app_cfg, info))
                    {
                        std::lock_guard<std::mutex> lock(g_state_mtx);
                        g_seat_sessions[seat_id].pending_enrol_user_id = info.user_id;
                        g_seat_sessions[seat_id].pending_enrol_card_id = info.card_id;
                        log_printf("[CAM][%s][%s] CMS done — cardId=%s camera enrol pending\n",
                            _cam_cfg.camera_id.c_str(), seat_id.c_str(), info.card_id.c_str());
                    }
                    else
                        log_printf("[CAM][%s][%s] Phase 1 FAILED\n",
                            _cam_cfg.camera_id.c_str(), seat_id.c_str());
                }
            }

            // -- Phase 2: Camera enrolment with per-seat ROI restriction ---------
            {
                using SeatCam = std::tuple<std::string,std::string,std::string>;
                std::vector<SeatCam> cam_pending;
                {
                    std::lock_guard<std::mutex> lock(g_state_mtx);
                    for (auto& [seat_id, session] : g_seat_sessions)
                    {
                        if (session.camera_id != _cam_cfg.camera_id) continue;
                        if (session.pending_enrol_card_id.empty()) continue;
                        if (session.state != SessionState::Unlocked) continue;
                        cam_pending.emplace_back(seat_id,
                            session.pending_enrol_user_id,
                            session.pending_enrol_card_id);
                    }
                }
                for (auto& [seat_id, user_id, card_id] : cam_pending)
                {
                    log_printf("[CAM][%s][%s] Phase 2 — camera enrol playerId=%s\n",
                        _cam_cfg.camera_id.c_str(), seat_id.c_str(), user_id.c_str());

                    auto single_cfg = BuildSingleSeatDeviceConfig(_app_cfg, seat_id);
                    _auth.SetDeviceConfig(single_cfg);

                    SimpleEnrolCallback enrolCb;
                    auto est = _auth.Enroll(enrolCb, user_id.c_str());
                    log_printf("[CAM][%s][%s] Enroll() Status=%d\n",
                        _cam_cfg.camera_id.c_str(), seat_id.c_str(), (int)est);

                    // Restore full camera config
                    _auth.SetDeviceConfig(BuildCameraDeviceConfig(_app_cfg, _cam_cfg));

                    if (est == RealSenseID::Status::Ok)
                    {
                        {
                            std::lock_guard<std::mutex> lock(g_state_mtx);
                            g_seat_sessions[seat_id].pending_enrol_card_id.clear();
                            g_seat_sessions[seat_id].pending_enrol_user_id.clear();
                        }
                        log_printf("[CAM][%s][%s] Enrol succeeded — cross-enrolling on other cameras\n",
                            _cam_cfg.camera_id.c_str(), seat_id.c_str());

                        RealSenseID::UserFaceprints ufp;
                        if (ExportUserFaceprints(user_id, ufp))
                        {
                            CrossEnrolJob job;
                            job.user_id    = user_id;
                            job.faceprints = ufp;
                            std::lock_guard<std::mutex> wlk(g_camera_workers_mtx);
                            for (auto* w : g_camera_workers)
                                if (w->CameraId() != _cam_cfg.camera_id)
                                    w->QueueCrossEnrol(job);
                        }
                    }
                    else
                        log_printf("[CAM][%s][%s] Camera enrol failed — retrying next cycle\n",
                            _cam_cfg.camera_id.c_str(), seat_id.c_str());
                }
            }

            // -- Drain cross-enrol queue -----------------------------------------
            DrainCrossEnrolQueue();

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        _auth.Disconnect();
        log_printf("[CAM][%s] Auth loop stopped\n", _cam_cfg.camera_id.c_str());
    }

    CameraConfig                   _cam_cfg;
    const AppConfig&               _app_cfg;
    RealSenseID::FaceAuthenticator _auth;
    std::thread                    _thread;
    std::atomic<bool>              _running{false};
    std::queue<CrossEnrolJob>      _xq;
    std::mutex                     _xq_mtx;
    CameraPreviewCapture           _preview_cap;
};

// ============================================================
// IDLE MONITOR
// ============================================================

void idle_monitor(const AppConfig& cfg)
{
    using namespace std::chrono;
    log_printf("[BG] Idle monitor started: timeout=%ds cooldown=%ds\n",
        cfg.idle_timeout_s, cfg.switch_delay_s);

    while (g_running)
    {
        {
            std::lock_guard<std::mutex> lock(g_state_mtx);
            auto now = steady_clock::now();

            for (auto& [seat_id, session] : g_seat_sessions)
            {
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
                            log_printf("[BG][%s] Idle timeout — slot logout cardId=%s\n",
                                seat_id.c_str(), card_id.c_str());
                            std::thread([cms=cfg.cms, route, card_id]()
                                { egm_logout(cms, route, card_id); }).detach();
                        }
                        else
                        {
                            int         rid = session.rating_id;
                            TableConfig tbl = cfg.table_cfg;
                            session.rating_id = 0;
                            log_printf("[BG][%s] Idle timeout — table logout ratingId=%d\n",
                                seat_id.c_str(), rid);
                            std::thread([cms=cfg.cms, route, tbl, rid, seat_id_cp]()
                            {
                                bool ok = egm_table_logout(cms, route, tbl, rid);
                                if (!ok) log_printf("[BG][%s] Table logout FAILED ratingId=%d\n",
                                    seat_id_cp.c_str(), rid);
                                save_ratings_state();
                            }).detach();
                        }
                    }
                }

                if (session.state == SessionState::Cooldown && now >= session.cooldown_until)
                {
                    log_printf("[BG][%s] Cooldown finished — Unlocked\n", seat_id.c_str());
                    session.state = SessionState::Unlocked;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ============================================================
// STARTUP DB MERGE
// Ensures every camera's face DB contains every patron known
// to any camera before the auth loops start.
// ============================================================

void StartupDBMerge(std::vector<std::unique_ptr<CameraWorker>>& workers)
{
    if (workers.size() < 2)
    {
        log_printf("[MERGE] Single camera — no merge needed\n");
        return;
    }

    log_printf("[MERGE] Starting DB merge across %zu cameras...\n", workers.size());

    struct CamDB
    {
        std::string                              camera_id;
        std::vector<RealSenseID::UserFaceprints> fps;
    };
    std::vector<CamDB> dbs(workers.size());

    for (size_t i = 0; i < workers.size(); ++i)
    {
        dbs[i].camera_id = workers[i]->CameraId();
        workers[i]->GetAllUserFaceprints(dbs[i].fps);
        log_printf("[MERGE][%s] %zu users in DB\n",
            dbs[i].camera_id.c_str(), dbs[i].fps.size());
    }

    for (size_t target = 0; target < dbs.size(); ++target)
    {
        std::unordered_set<std::string> have;
        for (const auto& ufp : dbs[target].fps)
            have.insert(std::string(ufp.user_id));

        std::vector<RealSenseID::UserFaceprints> missing;
        for (size_t src = 0; src < dbs.size(); ++src)
        {
            if (src == target) continue;
            for (const auto& ufp : dbs[src].fps)
                if (have.find(std::string(ufp.user_id)) == have.end())
                {
                    missing.push_back(ufp);
                    have.insert(std::string(ufp.user_id));
                }
        }

        if (missing.empty())
        {
            log_printf("[MERGE][%s] Already up to date\n", dbs[target].camera_id.c_str());
            continue;
        }

        log_printf("[MERGE][%s] Importing %zu missing user(s)\n",
            dbs[target].camera_id.c_str(), missing.size());
        workers[target]->ImportFaceprints(missing);
    }

    log_printf("[MERGE] DB merge complete\n");
}

// ============================================================
// MAIN
// ============================================================

int main()
{
    try
    {
        g_exe_dir = GetExeDir();

        // Load config first (needed for log_to_file flag), then init logging.
        AppConfig appCfg = LoadAppConfig();
        init_logging(g_exe_dir, appCfg.log_to_file);

        log_printf("=== F455SeatRouter v5.1 ===\n");
        log_printf("Mode=%s  Cameras=%zu  Preview=%s  Logging=%s\n",
            appCfg.mode == AppMode::Table ? "TABLE" : "SLOT",
            appCfg.cameras.size(),
            appCfg.preview_enabled ? "ON" : "OFF",
            appCfg.log_to_file     ? "ON" : "OFF");

        for (const auto& cam : appCfg.cameras)
        {
            log_printf("  Camera %-8s port=%-6s cam_num=%-3d seats=",
                cam.camera_id.c_str(), cam.port.c_str(), cam.camera_number);
            for (const auto& s : cam.seat_ids) log_printf("%s ", s.c_str());
            log_printf("\n");
        }

        if (appCfg.mode == AppMode::Slot)
            for (const auto& r : appCfg.routes)
                log_printf("  Route %-8s asset=%-6s tokenType=%s\n",
                    r.seat_id.c_str(), r.asset_id.c_str(), r.login_token_type.c_str());
        else
            for (const auto& r : appCfg.routes)
                log_printf("  Route %-8s deviceId=%-8d gameId=%d\n",
                    r.seat_id.c_str(), r.device_id, r.game_id);

        if (appCfg.mode == AppMode::Table)
            log_printf("  Table: employeeId=%d playTime=%ds averageWager=%d\n",
                appCfg.table_cfg.employee_id,
                appCfg.table_cfg.play_time,
                appCfg.table_cfg.average_wager);

        if (appCfg.preview_enabled)
        {
            int n_auto = 0;
            for (const auto& cam : appCfg.cameras)
                if (cam.camera_number < 0) ++n_auto;
            if (n_auto > 1)
                log_printf("[PREVIEW] WARNING: %d cameras have camera_number=-1 (auto-detect).\n"
                           "[PREVIEW] With multiple cameras, set 'camera_number' explicitly in\n"
                           "[PREVIEW] the cameras[] config to avoid both windows showing the same feed.\n", n_auto);
        }

        // Initialise per-seat sessions
        {
            std::lock_guard<std::mutex> lock(g_state_mtx);
            for (const auto& cam : appCfg.cameras)
                for (const auto& seat_id : cam.seat_ids)
                    for (const auto& seat : appCfg.seats)
                        if (seat.seat_id == seat_id && seat.enabled)
                        {
                            SeatSession ss;
                            ss.camera_id = cam.camera_id;
                            g_seat_sessions[seat_id] = ss;
                        }
        }

        if (appCfg.mode == AppMode::Table)
            load_ratings_state();

        // Create and connect all camera workers
        std::vector<std::unique_ptr<CameraWorker>> workers;
        workers.reserve(appCfg.cameras.size());

        for (const auto& cam_cfg : appCfg.cameras)
        {
            auto w = std::make_unique<CameraWorker>(cam_cfg, appCfg);
            if (!w->Connect() || !w->ApplyDeviceConfig())
            {
                log_printf("ERROR: Could not connect camera %s on %s\n",
                    cam_cfg.camera_id.c_str(), cam_cfg.port.c_str());
                log_printf("Press ENTER to exit.\n"); (void)getchar();
                return 1;
            }
            workers.push_back(std::move(w));
        }

        // Populate global worker pointer list for cross-enrol broadcasts
        {
            std::lock_guard<std::mutex> wlk(g_camera_workers_mtx);
            for (auto& w : workers)
                g_camera_workers.push_back(w.get());
        }

        // Merge face DBs across all cameras before starting auth loops
        StartupDBMerge(workers);

        // Start idle monitor
        std::thread idleThread(idle_monitor, std::cref(appCfg));

        // Start all camera auth loop threads (and preview streams if enabled)
        log_printf("Running. All cameras active.\n");
        if (appCfg.preview_enabled)
            log_printf("[PREVIEW] Press ESC in any preview window to exit.\n");

        for (auto& w : workers) w->Start();

        // ── Main loop ──────────────────────────────────────────────────────────
        // When preview is enabled: drive the OpenCV display loop on the main thread
        // (imshow/waitKey must be called from the same thread that creates the windows).
        // When preview is off: simply wait for g_running to be cleared externally.

        if (appCfg.preview_enabled)
        {
            // Create a named window for each camera
            for (auto& w : workers)
                cv::namedWindow(w->CameraId(), cv::WINDOW_NORMAL);

            while (g_running)
            {
                for (auto& w : workers)
                {
                    cv::Mat frame;
                    if (w->TryGetFrame(frame))
                        cv::imshow(w->CameraId(), frame);
                }
                int key = cv::waitKey(33); // ~30fps, processes window events
                if (key == 27) // ESC
                {
                    log_printf("[PREVIEW] ESC pressed — shutting down.\n");
                    g_running = false;
                    break;
                }
            }
            cv::destroyAllWindows();
        }
        else
        {
            while (g_running)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Stop all workers
        for (auto& w : workers) w->Stop();
        idleThread.join();

        log_printf("=== Finished. Press ENTER to exit. ===\n");
        (void)getchar();
        return 0;
    }
    catch (const std::exception& ex)
    {
        log_printf("Unhandled exception: %s\n", ex.what());
        log_printf("Press ENTER to exit.\n"); (void)getchar();
        return 1;
    }
}
