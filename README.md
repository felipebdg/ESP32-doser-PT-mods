# ESP32 Aquarium Dosing Controller v2.0

A 4-channel dosing pump controller with web interface, OTA updates, weekly scheduling, and configurable timezone.

## Features

- **Web Interface** — Control everything from your phone or computer
- **OTA Updates** — Upload new firmware over WiFi, no USB adapter needed
- **Weekly Scheduling** — Select specific days (e.g., Mon/Wed/Fri only)
- **Timezone Config** — Set your timezone from the web interface
- **Custom Pump Names** — Rename each channel (e.g., "KNO3", "Alkalinity")
- **Calibration System** — Guided calibration for accurate dosing
- **Persistent Settings** — Survives power loss and reboots

## Hardware Required

| Part | Approximate Cost |
|------|------------------|
| ESP32 4-channel relay board | $18 |
| 12V peristaltic pump(s) | $8-15 each |
| 12V DC power supply | $8-12 |
| Silicone tubing | $5 |
| Check valves (recommended) | $5 |

**Total: ~$25-50** (vs $200+ for commercial dosers)

## Wiring

### Relay Pin Mapping
| Relay | GPIO | Terminal |
|-------|------|----------|
| 1     | 32   | NO1/COM1 |
| 2     | 33   | NO2/COM2 |
| 3     | 25   | NO3/COM3 |
| 4     | 26   | NO4/COM4 |

### Pump Wiring (per channel)
```
12V PSU (+) ──────► COM terminal
NO terminal ──────► Pump (+) red wire
Pump (-) black ───► 12V PSU (-)
```

The relay acts as a switch on the positive wire. When activated, it connects COM to NO, completing the circuit.

### Why COM isn't pre-wired to power
The relay contacts are isolated from the board's power. This is intentional — it lets you switch any voltage (5V, 12V, 24V, even AC) safely. You must bring 12V from your PSU to the COM terminal yourself.

## First-Time Setup

### 1. Install Arduino IDE
Download from https://www.arduino.cc/en/software

### 2. Add ESP32 Board Support
1. Arduino IDE → Preferences
2. Add to "Additional Board Manager URLs":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Tools → Board → Board Manager → Search "ESP32" → Install

### 3. Configure the Sketch
Edit these lines:
```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

Optional — change the OTA password:
```cpp
const char* OTA_PASSWORD = "doser123";  // Change this!
```

### 4. Upload via USB-TTL Adapter
First upload must be done via USB. After that, use OTA.

**Wiring:**
| USB-TTL Adapter | ESP32 Board |
|-----------------|-------------|
| TX              | RX          |
| RX              | TX          |
| GND             | GND         |
| 5V              | Leave disconnected |

Power the board separately with 12V.

**Upload sequence:**
1. Hold IO0/BOOT button
2. Press and release EN/RST
3. Release IO0
4. Click Upload in Arduino IDE

**Board settings:**
- Board: "ESP32 Dev Module"
- Upload Speed: 115200 (if 921600 fails)
- Port: Your COM port

### 5. Find the IP Address
Open Serial Monitor (115200 baud) after upload. Look for:
```
WiFi Connected!
IP Address: 192.168.x.x
```

Navigate to that IP in your browser.

## Using OTA Updates

After the first USB upload, you can update wirelessly:

1. Tools → Port → Select "dosing-controller at 192.168.x.x"
2. Click Upload
3. Enter OTA password when prompted (default: `doser123`)

**If the network port doesn't appear:**
- Verify your computer is on the same WiFi network
- Restart Arduino IDE
- Check Serial Monitor to confirm ESP32 is connected to WiFi

## Web Interface Guide

### Status Bar
Shows current time (confirms NTP sync is working) and IP address.

### Pump Names
Click directly on the pump name to rename it. Changes save automatically.
Examples: "KNO3", "Alkalinity", "Calcium", "Magnesium"

### Dose Amount
How many mL to dispense per scheduled dose.

### Dose Time
24-hour format (e.g., 14:30 = 2:30 PM).
Leave blank to disable scheduled dosing for that channel.

### Dose Days
Click day buttons to toggle:
- **Cyan/Blue** = Dose on this day
- **Gray** = Skip this day

Changes save immediately when you click a day button.

### Dose Now
Manually trigger an immediate dose (uses the configured mL amount).
Only available after calibration.

### Save Schedule
Saves the dose amount and time. Day selections save automatically.

### Calibration Section
1. Set seconds (10 is a good starting point)
2. Click "Run Pump" — pump runs for that duration
3. Measure mL dispensed in a graduated cylinder
4. Enter the mL value
5. Click "Save Cal"

The system calculates mL/second and uses this for all future doses.

### Timezone Settings (bottom of page)
- **GMT Offset** — Your timezone in hours (e.g., -5 for EST, -8 for PST)
- **Daylight Saving** — Select "+1 Hour DST" if applicable

## Common Timezone Offsets

| Timezone | Offset | Example Cities |
|----------|--------|----------------|
| PST      | -8     | Los Angeles, Seattle |
| MST      | -7     | Denver, Phoenix |
| CST      | -6     | Chicago, Dallas |
| EST      | -5     | New York, Miami |
| GMT/UTC  | 0      | London (winter) |
| CET      | +1     | Paris, Berlin |
| IST      | +5.5   | Mumbai, Delhi |
| JST      | +9     | Tokyo |
| AEST     | +10    | Sydney |

## API Endpoints

For Home Assistant, Node-RED, or custom integrations:

| Endpoint | Parameters | Description |
|----------|------------|-------------|
| `GET /status` | — | JSON with all channel data |
| `GET /dose` | `ch`, `ml` | Trigger immediate dose |
| `GET /stop` | `ch` | Emergency stop pump |
| `GET /calibrate` | `ch`, `sec` | Run pump for calibration |
| `GET /save` | `ch`, `name`, `mlps`, `target`, `hour`, `min`, `days` | Save settings |
| `GET /timezone` | `gmt`, `dst` | Set timezone |

### Examples
```
# Dose 50mL on channel 0
http://192.168.1.100/dose?ch=0&ml=50

# Set channel 1 to dose Mon/Wed/Fri only
http://192.168.1.100/save?ch=1&days=0,1,0,1,0,1,0

# Get current status
http://192.168.1.100/status
```

### Days Format
Comma-separated 1/0 for Sun, Mon, Tue, Wed, Thu, Fri, Sat.
Example: `0,1,0,1,0,1,0` = Monday, Wednesday, Friday only

## Troubleshooting

### Relays click but pump doesn't run
- Check wiring: 12V+ to COM, NO to pump+, pump- to 12V-
- Make sure you're using NO (normally open), not NC
- Test pump directly on 12V to confirm it works

### Pump runs when it shouldn't (inverted logic)
Change this line and re-upload:
```cpp
const bool RELAY_ACTIVE_LOW = false;  // Try true instead
```

### Can't upload via USB
- Verify TX→RX and RX→TX (crossed, not straight)
- Try the boot sequence: Hold IO0, tap RST, release IO0, then upload
- Reduce upload speed to 115200

### Time shows wrong
- Adjust timezone in Settings section
- Time resyncs automatically after changing timezone

### Schedule doesn't trigger
- Check that time is synced (shows in status bar)
- Verify today's day button is cyan/blue
- Confirm hour and minute are set

### Characters display as garbled text
- Hard refresh the page (Ctrl+F5 or Cmd+Shift+R)
- The UTF-8 charset should fix this in v2

### OTA upload fails
- Same WiFi network?
- Correct password?
- Try power cycling the ESP32
- Fallback: Use USB-TTL adapter

## Suggested Setups

### Freshwater Planted Tank
- **Channel 1**: KNO3 (nitrate) — adjust frequency based on plant uptake

### Reef Tank
- **Channel 1**: Alkalinity — daily
- **Channel 2**: Calcium — daily (stagger 30+ min from alk)
- **Channel 3**: Magnesium — weekly or as needed
- **Channel 4**: Trace elements — optional

**Important:** Don't dose alkalinity and calcium at the same time or in the same location — they can precipitate out. Schedule them at least 30 minutes apart.

## Backup Your Settings

Settings persist in ESP32 flash through reboots, but a full re-flash may reset them.

Before major updates, note:
- Calibration values (mL/sec)
- Dose amounts
- Schedule times and days
- Pump names

## Hardware Tips

- **Check valves** — Install on each dosing line to prevent siphoning
- **Tubing** — Replace every few months; peristaltic pumps wear the tubing
- **Prime before calibrating** — Remove air bubbles for accurate calibration
- **Container placement** — Keep dosing containers above pump level if possible

## License

Free to use, modify, and share. No warranty provided.
