# Temperature Sensor Selection Feature

## Overview

This feature adds a temperature sensor selection dropdown to the gatotray preferences dialog, allowing users to choose which temperature sensor to monitor.

## Problem Solved

Previously, gatotray would automatically select the first available temperature sensor it found. On systems with multiple sensors (CPU, GPU, SSD, RAM, etc.), this often resulted in monitoring the wrong sensor. Users had no way to select which sensor they wanted to monitor without modifying the source code.

## Solution

A new dropdown menu in the preferences dialog that:
1. Automatically discovers all available temperature sensors on the system
2. Displays them with descriptive labels (e.g., "coretemp (hwmon0 temp1)")
3. Allows users to select their preferred sensor
4. Saves the selection to the configuration file

## UI Changes

### Preferences Dialog

The temperature sensor dropdown is added to the "Options" section of the preferences dialog:

```
┌─────────────────────────────────────────────┐
│ gatotray Settings                           │
├─────────────────────────────────────────────┤
│ ┌─────────┐  ┌────────────────────────────┐ │
│ │ Colors  │  │ Options                    │ │
│ │         │  │                            │ │
│ │ ...     │  │ ☑ Transparent background   │ │
│ │         │  │ ☑ Show Thermometer         │ │
│ │         │  │                            │ │
│ │         │  │ Basic refresh ... [1000]   │ │
│ │         │  │ Top refresh ...   [3000]   │ │
│ │         │  │ High temp alarm   [85]     │ │
│ │         │  │                            │ │
│ │         │  │ Custom command:            │ │
│ │         │  │ [xterm -geometry ...]      │ │
│ │         │  │                            │ │
│ │         │  │ Temperature sensor:        │ │  <- NEW
│ │         │  │ [coretemp (hwmon0 temp1)▼] │ │  <- NEW
│ └─────────┘  └────────────────────────────┘ │
│                                             │
│                        [Close]  [Save]      │
└─────────────────────────────────────────────┘
```

### Dropdown Options

When clicked, the dropdown shows:
```
┌──────────────────────────────────────┐
│ Auto (first available)               │
│ coretemp (hwmon0 temp1 (new))       │ <- CPU sensor
│ nvme (hwmon1 temp1 (new))           │ <- SSD sensor
│ thermal_zone0                        │ <- Generic thermal zone
└──────────────────────────────────────┘
```

### Behavior

- The dropdown is **enabled** when "Show Thermometer" is checked
- The dropdown is **disabled** (grayed out) when "Show Thermometer" is unchecked
- The selected sensor is immediately applied when changed
- The selection is saved when the "Save" button is clicked

## Technical Implementation

### Code Changes

1. **cpu_usage.c**
   - Added `discover_temp_sensors()` function to scan and list available sensors
   - Modified `cpu_temperature()` to use the user-selected sensor
   - Added support for reading sensor names from `/sys/class/hwmon/hwmonN/name`

2. **settings.c**
   - Added temperature sensor dropdown widget
   - Added `on_temp_sensor_changed()` callback
   - Integrated with preference save/load system
   - Added enable/disable logic based on thermometer checkbox

### Configuration File

The selected sensor is stored in `~/.config/gatotrayrc`:

```ini
[Options]
Temperature sensor=/sys/class/hwmon/hwmon0/temp1_input
```

If not set or set to an empty value, gatotray falls back to automatic selection.

## Sensor Discovery

The feature searches for temperature sensors in the following order:

1. `/sys/class/hwmon/hwmon0-2/device/temp1-2_input`
2. `/sys/class/hwmon/hwmon0-2/temp1-2_input`
3. `/sys/class/thermal/thermal_zone0-2/temp`
4. `/proc/acpi/thermal_zone/THM*/temperature`

For hwmon sensors, it attempts to read the sensor name from the `name` file in the hwmon directory, providing more descriptive labels like "coretemp" (CPU), "nvme" (SSD), etc.

## User Benefits

1. **Correct Monitoring**: Users can select the correct temperature sensor (typically CPU)
2. **Multiple Systems**: Works across different hardware configurations
3. **Transparency**: Shows all available sensors with descriptive names
4. **Flexibility**: Easy to change if hardware configuration changes
5. **Backwards Compatible**: Defaults to automatic selection if no preference is set

## Testing

Tested with mock sensors demonstrating:
- Sensor discovery works correctly
- Sensor names are properly read and displayed
- Multiple sensors can be distinguished
- Fallback to automatic selection works

## Future Enhancements

Possible future improvements:
- Show current temperature value next to each sensor in the dropdown
- Support for monitoring multiple sensors simultaneously
- Graphical representation of all sensors in a tooltip
- Custom temperature ranges per sensor type
