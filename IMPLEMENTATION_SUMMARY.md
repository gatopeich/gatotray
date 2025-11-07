# Implementation Summary: Temperature Sensor Selection

## Issue Addressed

**Original Issue**: "Wrong temperature selected"
- User reported that gatotray was selecting the wrong temperature sensor (RAM instead of CPU)
- No way to select which temperature sensor to monitor
- Required modifying source code to change sensor

## Solution Implemented

Added a **Temperature Sensor Selection Dropdown** in the preferences UI that:
1. Automatically discovers all available temperature sensors
2. Displays them with descriptive labels
3. Allows users to select their preferred sensor
4. Saves the selection to configuration

## How It Works

### For Users

1. **Open Preferences**: Right-click gatotray icon → Select "Preferences"
2. **Find Temperature Sensor Dropdown**: Located in the "Options" section
3. **Select Sensor**: Choose from:
   - "Auto (first available)" - default behavior
   - List of discovered sensors with names like:
     - "coretemp (hwmon0 temp1)" - CPU temperature
     - "nvme (hwmon1 temp1)" - SSD temperature
     - "thermal_zone0" - System thermal zone
4. **Save**: Click "Save" button to persist the selection

### For Developers

#### New Functions

**cpu_usage.c:**
```c
char** discover_temp_sensors(int* count, char*** labels)
```
- Scans system for available temperature sensors
- Reads sensor names from hwmon name files
- Returns arrays of paths and labels
- Searches: hwmon, thermal zones, ACPI

**settings.c:**
```c
void on_temp_sensor_changed(GtkComboBox *combo, gpointer user_data)
```
- Handles sensor selection changes
- Updates preference variables
- Triggers UI updates

#### Modified Functions

**cpu_usage.c:**
```c
int cpu_temperature(void)
```
- Now respects user-selected sensor via `pref_temp_sensor_path`
- Falls back to automatic detection if no preference set
- Reopens sensor file if preference changes

#### New Variables

```c
extern char* pref_temp_sensor_path;  // Current sensor path
static char* pref_temp_sensor;       // Saved sensor path
static GtkWidget* temp_sensor_combo_container; // UI widget reference
```

## Sensor Discovery

Searches in order:
1. `/sys/class/hwmon/hwmon[0-2]/device/temp[1-2]_input`
2. `/sys/class/hwmon/hwmon[0-2]/temp[1-2]_input`
3. `/sys/class/thermal/thermal_zone[0-2]/temp`
4. `/proc/acpi/thermal_zone/{THM,THM0,THRM}/temperature`

For hwmon sensors, reads `/sys/class/hwmon/hwmonN/name` for descriptive labels.

## Configuration Storage

Saved in: `~/.config/gatotrayrc`

```ini
[Options]
Temperature sensor=/sys/class/hwmon/hwmon0/temp1_input
```

Empty or unset = automatic selection (original behavior)

## UI Integration

- Dropdown added after "Custom command" field in Options section
- Enabled/disabled based on "Show Thermometer" checkbox
- Widget lifecycle properly managed (destroyed when dialog closes)
- Uses GTK ComboBoxText for dropdown functionality
- Stores sensor paths as object data on combo widget

## Testing

✅ Sensor discovery function tested with mock sensors
✅ Sensor name reading verified (coretemp, nvme, etc.)
✅ Multiple sensors properly detected and labeled
✅ Code compiles without syntax errors (verified with standalone tests)

## Backwards Compatibility

✅ No breaking changes
✅ If no sensor selected, uses original automatic detection
✅ Existing configurations continue to work
✅ New preference is optional

## Files Modified

1. **cpu_usage.c** (+144 lines)
   - Sensor discovery function
   - Sensor name reading
   - Modified temperature function

2. **settings.c** (+105 lines)
   - UI dropdown widget
   - Selection callback
   - Preference load/save

3. **README.md** (+25 lines)
   - Configuration section
   - Usage instructions

4. **TEMPERATURE_SENSOR_SELECTION.md** (+128 lines)
   - Complete feature documentation
   - UI mockups
   - Technical details

**Total Changes**: ~400 lines added

## Benefits

✅ **Solves Original Issue**: Users can now select correct sensor
✅ **User-Friendly**: Easy dropdown selection, no code modification
✅ **Flexible**: Works across different hardware configurations
✅ **Informative**: Shows sensor names for easy identification
✅ **Persistent**: Settings saved across sessions
✅ **Backwards Compatible**: Defaults to original behavior

## Future Enhancements

Potential improvements discussed in documentation:
- Show current temperature in dropdown
- Monitor multiple sensors simultaneously
- Per-sensor temperature ranges
- Graphical sensor overview
