# Temperature Sensor Selection - UI Mockup

## Preferences Dialog - Before Feature

```
┌─────────────────────────────────────────────────────────┐
│ gatotray Settings                          ┌───┐ ┌───┐  │
│                                            │ X │ │ + │  │
│  ┌─────────┐  ┌──────────────────────────┐└───┘ └───┘  │
│  │ Colors  │  │ Options                  │             │
│  │         │  │                          │             │
│  │ [...]   │  │ ☑ Transparent background │             │
│  │         │  │ ☑ Show Thermometer       │             │
│  │         │  │                          │             │
│  │         │  │ Basic refresh    [1000]  │             │
│  │         │  │ Top refresh      [3000]  │             │
│  │         │  │ High temp alarm  [85]    │             │
│  │         │  │                          │             │
│  │         │  │ Custom command:          │             │
│  │         │  │ [xterm -geometry ...]    │             │
│  └─────────┘  └──────────────────────────┘             │
│                                                         │
│                                 [Close]  [Save]         │
└─────────────────────────────────────────────────────────┘
```

## Preferences Dialog - After Feature (NEW)

```
┌─────────────────────────────────────────────────────────────────┐
│ gatotray Settings                               ┌───┐ ┌───┐     │
│                                                 │ X │ │ + │     │
│  ┌─────────┐  ┌───────────────────────────────┐└───┘ └───┘     │
│  │ Colors  │  │ Options                       │                │
│  │         │  │                               │                │
│  │ Free    │  │ ☑ Transparent background      │                │
│  │ memory  │  │ ☑ Show Thermometer            │                │
│  │ [Teal]  │  │                               │                │
│  │         │  │ Basic refresh interval   [1000]  ms           │
│  │ Fore-   │  │ Top refresh interval     [3000]  ms           │
│  │ ground  │  │ High temperature alarm   [85]    °C           │
│  │ [Black] │  │                               │                │
│  │         │  │ Custom command:               │                │
│  │ ...     │  │ [xterm -geometry 75x13{position} -e top]     │
│  │         │  │                               │                │
│  │         │  │ Temperature sensor:           │  ◄━━━ NEW     │
│  │         │  │ [coretemp (hwmon0 temp1) ▼]   │                │
│  └─────────┘  └───────────────────────────────┘                │
│                                                                 │
│                                      [Close]  [Save]            │
└─────────────────────────────────────────────────────────────────┘
```

## Dropdown Menu - Expanded

When user clicks the dropdown:

```
┌─────────────────────────────────────────────────────────────────┐
│ gatotray Settings                               ┌───┐ ┌───┐     │
│                                                 │ X │ │ + │     │
│  ┌─────────┐  ┌───────────────────────────────┐└───┘ └───┘     │
│  │ Colors  │  │ Options                       │                │
│  │         │  │                               │                │
│  │ ...     │  │ ☑ Transparent background      │                │
│  │         │  │ ☑ Show Thermometer            │                │
│  │         │  │                               │                │
│  │         │  │ ...                           │                │
│  │         │  │                               │                │
│  │         │  │ Temperature sensor:           │                │
│  │         │  │ ┌─────────────────────────────────────────┐   │
│  │         │  │ │ Auto (first available)                  │   │
│  │         │  │ ├─────────────────────────────────────────┤   │
│  │         │  │ │ coretemp (hwmon0 temp1 (new))    ◄─ CPU │   │
│  │         │  │ ├─────────────────────────────────────────┤   │
│  │         │  │ │ nvme (hwmon1 temp1 (new))        ◄─ SSD │   │
│  │         │  │ ├─────────────────────────────────────────┤   │
│  │         │  │ │ thermal_zone0                           │   │
│  │         │  │ └─────────────────────────────────────────┘   │
│  └─────────┘  └───────────────────────────────────────────────┘│
│                                                                 │
│                                      [Close]  [Save]            │
└─────────────────────────────────────────────────────────────────┘
```

## Interaction Flow

### Step 1: User Opens Preferences
```
User action: Right-click gatotray icon
              ↓
          [Context Menu]
              ↓
       Click "Preferences"
              ↓
      [Preferences Dialog Opens]
              ↓
    Temperature sensor dropdown visible
```

### Step 2: User Views Available Sensors
```
User action: Click dropdown arrow
              ↓
      [Dropdown expands]
              ↓
    Shows all available sensors:
    • Auto (first available)
    • coretemp (hwmon0 temp1)  ← CPU sensor
    • nvme (hwmon1 temp1)      ← SSD sensor
    • thermal_zone0            ← Generic thermal
```

### Step 3: User Selects Sensor
```
User action: Click "coretemp (hwmon0 temp1)"
              ↓
      [Selection updates]
              ↓
    Dropdown shows: [coretemp (hwmon0 temp1) ▼]
              ↓
    on_temp_sensor_changed() callback fires
              ↓
    pref_temp_sensor_path updated
```

### Step 4: User Saves Preferences
```
User action: Click "Save" button
              ↓
      [pref_save() called]
              ↓
    Writes to ~/.config/gatotrayrc:
    "Temperature sensor=/sys/class/hwmon/hwmon0/temp1_input"
              ↓
      [Dialog closes]
              ↓
    Gatotray now monitors selected sensor
```

## Visual States

### Enabled State (Thermometer ON)
```
Temperature sensor: [coretemp (hwmon0 temp1) ▼]
                    └─────────────────────────┘
                         Can be clicked
                    Text is dark/normal color
```

### Disabled State (Thermometer OFF)
```
Temperature sensor: [coretemp (hwmon0 temp1) ▼]
                    └─────────────────────────┘
                         Cannot be clicked
                    Text is grayed out
                    When "Show Thermometer" is unchecked
```

### Auto Selected State
```
Temperature sensor: [Auto (first available) ▼]
                    └──────────────────────────┘
                    Uses original auto-detection
                    No preference saved
```

### Specific Sensor Selected State
```
Temperature sensor: [coretemp (hwmon0 temp1) ▼]
                    └─────────────────────────┘
                    Uses selected sensor only
                    Preference saved to config
```

## System Tray Display

Before and after selecting the correct sensor:

### BEFORE (Wrong sensor selected)
```
┌────────┐
│   38°C │  ← RAM temperature (not useful!)
│  [===] │  
│ ██████ │
└────────┘
```

### AFTER (Correct sensor selected)
```
┌────────┐
│   56°C │  ← CPU temperature (useful!)
│  [███] │  
│ ██████ │
└────────┘
```

## Configuration File

The selection is persisted in `~/.config/gatotrayrc`:

```ini
[Colors]
Free memory (top)=#008080
Foreground=#000000
Background=#ffffff
I/O wait (bottom)=#0000ff
Min frequency=#00ff00
Max frequency=#ff0000
Min temperature=#0000ff
Max temperature=#ff0000

[Options]
Transparent background=true
Show Thermometer=true
Basic refresh interval (ms)=1000
Top refresh interval (ms)=3000
High temperature alarm=85
Custom command=xterm -geometry 75x13{position} -e top
Temperature sensor=/sys/class/hwmon/hwmon0/temp1_input  ◄━━━ NEW SETTING
```

## Error Handling

### Scenario 1: Selected sensor becomes unavailable
```
┌─────────────────────────────────────────┐
│ Warning: Cannot open selected sensor   │
│ Path: /sys/class/hwmon/hwmon0/...      │
│ Falling back to automatic detection    │
└─────────────────────────────────────────┘

Result: Continues working with automatic detection
```

### Scenario 2: No sensors found at all
```
┌─────────────────────────────────────────┐
│ Temperature sensor: [No sensors found] │
│                     [Disabled]         │
└─────────────────────────────────────────┘

Result: Thermometer not displayed
```

### Scenario 3: Sensor disappeared from dropdown
```
Current selection: /sys/class/hwmon/hwmon5/temp1_input
Available sensors: hwmon0, hwmon1, thermal_zone0

Result: Dropdown shows selected path even if not in list
        Falls back to auto on next read failure
```

## Responsive Behavior

### When "Show Thermometer" is toggled:

1. Checked → Temperature dropdown ENABLED
   ```
   ☑ Show Thermometer
   Temperature sensor: [coretemp (hwmon0) ▼]  ← Dark/clickable
   ```

2. Unchecked → Temperature dropdown DISABLED
   ```
   ☐ Show Thermometer
   Temperature sensor: [coretemp (hwmon0) ▼]  ← Grayed out
   ```

The dropdown is immediately enabled/disabled without needing to save.

## Multi-language Support Ready

The feature is ready for internationalization:

```c
// Labels can be translated:
"Auto (first available)"       → "Automatique (premier disponible)"
"Temperature sensor:"          → "Capteur de température:"
"Show Thermometer"             → "Afficher le thermomètre"
```

Sensor names from hardware remain in English (coretemp, nvme, etc.)
