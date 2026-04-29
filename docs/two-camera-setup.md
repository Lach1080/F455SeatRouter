# Two-Camera Setup Guide

## 1. Find the COM Port for Each Camera

Each F455 camera connects over USB and is assigned a COM port by Windows. You need to find the correct port for every camera before editing the config.

### Method A — Device Manager (recommended)

1. Plug in **one camera at a time**.
2. Open **Device Manager** (`Win + X` → Device Manager).
3. Expand **Ports (COM & LPT)**.
4. Look for an entry such as:
   - `Intel(R) RealSense(TM) ID F45x (COM5)`  
   - or `USB Serial Device (COM5)`
5. Note the COM number (e.g. `COM5`).
6. Unplug that camera, plug in the **second camera**, and repeat — the new entry that appears is the second camera's port.

> **Tip:** If multiple COM ports are already listed, plug and unplug the camera while watching Device Manager to see which entry appears and disappears.

### Method B — PowerShell

Run the following in PowerShell to list all serial ports with their friendly names:

```powershell
Get-PnpDevice -Class Ports | Where-Object Status -eq 'OK' |
    Select-Object FriendlyName | Sort-Object FriendlyName
```

Example output:
```
FriendlyName
------------
Intel(R) RealSense(TM) ID F45x (COM5)
Intel(R) RealSense(TM) ID F45x (COM7)
```

The number in parentheses is the COM port to use in `config.json`.

---

## 2. Edit config.json for Two-Camera Mode

Open `config.json` (in the same folder as `F455SeatRouter.exe`) and add a `"cameras"` array. Each entry maps one physical camera to one or more seats.

### Minimal two-camera example

```json
{
  "port": "COM5",
  "mode": "table",
  ...

  "cameras": [
    {
      "camera_id": "cam_1",
      "port": "COM5",
      "seats": ["seat_1", "seat_2"]
    },
    {
      "camera_id": "cam_2",
      "port": "COM7",
      "seats": ["seat_3"]
    }
  ],

  "seats": [
    { "seat_id": "seat_1", "enabled": true,  "x":    0, "y": 0, "width": 640, "height": 1080 },
    { "seat_id": "seat_2", "enabled": true,  "x":  640, "y": 0, "width": 640, "height": 1080 },
    { "seat_id": "seat_3", "enabled": true,  "x": 1280, "y": 0, "width": 640, "height": 1080 },
    { "seat_id": "seat_4", "enabled": false, "x":    0, "y": 0, "width":   0, "height":    0 },
    { "seat_id": "seat_5", "enabled": false, "x":    0, "y": 0, "width":   0, "height":    0 }
  ],

  "routes": [
    { "seat_id": "seat_1", "device_id": 10000359, "game_id": 5017, ... },
    { "seat_id": "seat_2", "device_id": 10000360, "game_id": 5017, ... },
    { "seat_id": "seat_3", "device_id": 10000361, "game_id": 5017, ... }
  ]
}
```

### Key rules

| Rule | Detail |
|---|---|
| Every seat in a camera's `"seats"` list must also exist in the top-level `"seats"` array with `"enabled": true`. | Disabled seats are ignored even if listed under a camera. |
| Each seat_id must appear in **exactly one** camera's `"seats"` list. | Assigning the same seat to two cameras is not supported. |
| The `"cameras"` array must have at least one entry. | See *Single-camera fallback* below. |
| `"camera_id"` is a free-form label used in log output. | Use something descriptive, e.g. `"cam_left"`, `"cam_right"`. |
| The top-level `"port"` field is only used in single-camera fallback mode. | It is ignored when the `"cameras"` array is present. |

---

## 3. Single-Camera Fallback

If the `"cameras"` array is **omitted entirely**, the application runs in single-camera mode:

- Uses the top-level `"port"` value.
- Assigns all `enabled: true` seats to that one camera.

This is identical to v4.0 behaviour and requires no config changes from a v4.0 installation.

---

## 4. Startup Behaviour

When the application starts in two-camera mode it will:

1. Connect to both cameras in sequence.
2. **Merge face databases** — any patron enrolled on one camera is automatically imported into the other camera's database so both cameras recognise every patron from the first scan.
3. Start an independent authentication loop on each camera simultaneously.

If a camera fails to connect, the application will exit with an error. Verify the COM port and that no other application (e.g. Intel RealSense ID DevApp) has the port open.

---

## 5. Cross-Camera Patron Handling

| Scenario | Behaviour |
|---|---|
| Patron enrolls at cam_1 for the first time | Faceprints are silently imported into cam_2's database in the background. No interruption to cam_2's auth loop. |
| Patron moves from a cam_1 seat to a cam_2 seat | cam_2 recognises the patron, triggers logout on the cam_1 seat, then logs into the cam_2 seat. |
| cam_2 is mid-authentication when a cross-enrol arrives | The import is queued and retried after the current authentication cycle completes. |
