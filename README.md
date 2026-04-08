# F455SeatRouter

Multi-ROI seat-aware facial recognition bridge between an Intel RealSense ID F455 camera and the Konami SYNKROS Casino Management System.

## What it does

- Detects faces via the F455 camera across up to 5 configurable seat zones (ROIs)
- Identifies returning patrons and logs them into the correct gaming seat asset in SYNKROS
- Auto-enrols new patrons (creates player + card in SYNKROS, enrols face on camera)
- Logs patrons out automatically after an idle timeout
- All settings are read from `config.json` — no recompile required for deployment changes

---

## Repository structure

```
F455SeatRouter/
├── config.json                  ← Edit this for each deployment
├── F455SeatRouter.sln
├── F455SeatRouter/
│   ├── F455SeatRouter.vcxproj
│   └── src/
│       └── main.cpp
├── include/
│   └── nlohmann/
│       └── json.hpp             ← Vendored (do not edit)
├── sdk/                         ← Populated manually (not in source control)
│   ├── include/                 ← Copy RealSenseID headers here
│   ├── lib/x64/Release/         ← Copy rsid.lib here
│   └── bin/x64/Release/         ← Copy rsid.dll + deps here
├── scripts/
│   └── deploy.ps1               ← Packaging script
└── dist/                        ← Created by deploy.ps1
```

---

## Build prerequisites

| Requirement | Version |
|---|---|
| Visual Studio | 2022 (MSVC v143) |
| Windows SDK | 10.0 |
| C++ standard | C++17 |
| Intel RealSense ID SDK | F450_9.11.0.2813_multiROI / SDK 3.3.0.2813 |

---

## First-time setup

### 1. Clone the repo
```
git clone https://github.com/Lach1080/F455SeatRouter.git
cd F455SeatRouter
```

### 2. Populate the SDK folder

Copy the following from your RealSense SDK build:

| Source (SDK machine) | Destination (repo) |
|---|---|
| `SDK_3.3.0.2813_9eb91c3\include\` | `sdk\include\` |
| `SDK_3.3.0.2813_9eb91c3\lib\x64\Release\rsid.lib` | `sdk\lib\x64\Release\rsid.lib` |
| `SDK_3.3.0.2813_9eb91c3\bin\x64\Release\rsid.dll` | `sdk\bin\x64\Release\rsid.dll` |

Add any additional runtime DLLs required by rsid.dll to `sdk\bin\x64\Release\`.

### 3. Open and build in Visual Studio 2022
- Open `F455SeatRouter.sln`
- Select **Release | x64**
- Build → Build Solution (`Ctrl+Shift+B`)
- The EXE is placed in `x64\Release\F455SeatRouter.exe`
- `config.json` is automatically copied next to the EXE by a post-build event

### 4. Edit config.json
See the [Configuration](#configuration) section below.

### 5. Run
```
x64\Release\F455SeatRouter.exe
```

---

## Configuration

All settings live in `config.json` at the repo root (and are copied next to the EXE at build time).

### Key settings

| Field | Description | Example |
|---|---|---|
| `port` | Camera COM port | `"COM3"` |
| `rotation` | Camera orientation | `"Rotation_90_Deg"` |
| `idle_timeout_s` | Seconds before logout on no detection | `5` |
| `switch_delay_s` | Cooldown seconds after logout | `15` |
| `auto_enrol_min_interval_s` | Min seconds between auto-enrol attempts | `5` |

### Rotation values
`Rotation_0_Deg` · `Rotation_90_Deg` · `Rotation_180_Deg` · `Rotation_270_Deg`

### Seats (ROIs)
Each seat defines a pixel rectangle in the camera frame (1920×1080).
```json
{ "seat_id": "seat_1", "enabled": true, "x": 0, "y": 0, "width": 640, "height": 1080 }
```
- Only enabled seats are sent to the camera as active ROIs
- Disabled seats are ignored; set `enabled: false` rather than deleting entries
- Maximum 5 enabled seats

### Routes
Maps each seat to a SYNKROS asset:
```json
{ "seat_id": "seat_1", "asset_id": "8003", "login_token_type": "assetId", "login_token_data": "8003" }
```
- For table seats use `"login_token_type": "seatId"`

### CMS connection
```json
"cms": {
  "host": "10.16.0.12",
  "port": 9001,
  "ignore_cert_errors": true
}
```
Set `ignore_cert_errors` to `false` in production with a valid TLS certificate.

---

## Deployment (copying to another machine)

After a successful Release build:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1
```

This creates `dist\F455SeatRouter\` containing:
- `F455SeatRouter.exe`
- `config.json`
- `rsid.dll` (and any other SDK DLLs from `sdk\bin\x64\Release\`)

Zip `dist\F455SeatRouter\` and extract it on the target machine. Edit `config.json`, then run the EXE.

> The target machine does **not** need Visual Studio or the SDK installed.

---

## Session state machine

```
Unlocked
  │  Face detected + recognised → CMS login → LockedToUser
  │  Face detected + unknown    → auto-enrol workflow
  ▼
LockedToUser
  │  Owner re-detected          → refresh idle timer
  │  Idle timeout               → CMS logout → Cooldown
  ▼
Cooldown (switch_delay_s)
  └─ Timer expires              → Unlocked
```

---

## Known limitations / future work

1. Config is read once at startup — restart the EXE to pick up config.json changes
2. No file-based audit log — console output only
3. `ignore_cert_errors` must be set to `false` for production TLS
4. Table seat CMS endpoints differ from EGM endpoints — update `path_logins`/`path_logouts` in config.json accordingly
