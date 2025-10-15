/* CPU usage and temperature functions.
 *
 * (c) 2011 by gatopeich, licensed under a Creative Commons Attribution 3.0
 * Unported License: http://creativecommons.org/licenses/by/3.0/
 * Briefly: Use it however suits you better and just give me due credit.
 *
 * Changelog:
 * v1.1:    Added support for /sys/class/thermal/thermal_zone0/temp
 *          available since Linux 2.6.26.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <error.h>
#include <errno.h>

#include <glib.h>

#define error(_s, _e, fmt, args...) do { \
    g_message("ERROR %d:" fmt, _e, ##args); \
    if (_s) exit(_s); \
} while(0)

typedef unsigned long long u64;
typedef struct { int usage, iowait;  } CPU_Usage;

u64 cpu_busy_ticks=0;
u64 cpu_iowait_ticks=0;
u64 cpu_total_ticks=0;

CPU_Usage
cpu_usage(int scale)
{
    /* static stuff */

    static FILE *proc_stat = NULL;
    if (!proc_stat) {
        if (!(proc_stat = fopen("/proc/stat", "r")))
            error(1, errno, "Could not open /proc/stat");
    }

    u64 busy, nice, system, idle, total;
    u64 iowait=0, irq=0, softirq=0; /* New in Linux 2.6 */
    if( 4 > fscanf(proc_stat, "cpu %Lu %Lu %Lu %Lu %Lu %Lu %Lu",
                    &busy, &nice, &system, &idle, &iowait, &irq, &softirq))
        error(1, errno, "Can't seem to read /proc/stat properly");
    rewind(proc_stat);
    fflush(proc_stat); // Otherwise rewind is not effective

    busy += nice+system+irq+softirq;
    total = busy+idle+iowait;

    CPU_Usage cpu;

    if( busy > cpu_busy_ticks )
        cpu.usage = (u64)scale * (busy - cpu_busy_ticks) / (total - cpu_total_ticks);
    else
        cpu.usage = 0;

    if( iowait > cpu_iowait_ticks )
        cpu.iowait = (u64)scale * (iowait - cpu_iowait_ticks) / (total - cpu_total_ticks);
    else
        cpu.iowait = 0;

    cpu_busy_ticks = busy;
    cpu_iowait_ticks = iowait;
    cpu_total_ticks = total;

    return cpu;
}

int
file_read_int(const char* file, int on_error)
{
    FILE* fp = fopen(file, "r");
    if (fp) {
        int i;
        if (fscanf(fp, "%d", &i)) {
            fclose(fp);
            return i;
        }
    }
    error(0, errno, "Can't read uint from %s", file);
    return on_error;
}

// All freqs in MHz
int scaling_min_freq = 0;
int scaling_cur_freq = 0;
int scaling_max_freq = 0;

int
cpu_freq(void)
{
    if (scaling_cur_freq < 0)
        return 0; // Do not insist

    static FILE *cur_freq_file = NULL;
    if (!cur_freq_file) {
        cur_freq_file = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq","r");
    }
    if (cur_freq_file) {
        rewind(cur_freq_file);
        fflush(cur_freq_file);
        if (fscanf(cur_freq_file, "%d", &scaling_cur_freq) == 1) {
            if (scaling_max_freq) {
                scaling_cur_freq /= 1000; // KHz -> MHz
                if (scaling_cur_freq < scaling_min_freq)
                    scaling_min_freq = scaling_cur_freq;
                if (scaling_cur_freq > scaling_max_freq)
                    scaling_max_freq = scaling_cur_freq;
            } else {
                scaling_min_freq =
                    file_read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", scaling_cur_freq) / 1000;
                scaling_max_freq =
                    file_read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", scaling_cur_freq) / 1000;
                scaling_cur_freq /= 1000; // KHz -> MHz
            }
            return scaling_cur_freq;
        }
        fclose(cur_freq_file);
        cur_freq_file = NULL;
    }
    scaling_cur_freq = -1; // Do not waste efforts retrying
    return 0;
}

// Temperature sensor paths to try
typedef struct {
    const char* path;
    const char* label;
} TempSensorPath;

static TempSensorPath temp_sensor_paths[] = {
    {"/sys/class/hwmon/hwmon0/device/temp1_input", "hwmon0 temp1"},
    {"/sys/class/hwmon/hwmon1/device/temp1_input", "hwmon1 temp1"},
    {"/sys/class/hwmon/hwmon2/device/temp1_input", "hwmon2 temp1"},
    {"/sys/class/hwmon/hwmon0/temp1_input", "hwmon0 temp1 (new)"},
    {"/sys/class/hwmon/hwmon1/temp1_input", "hwmon1 temp1 (new)"},
    {"/sys/class/hwmon/hwmon2/temp1_input", "hwmon2 temp1 (new)"},
    {"/sys/class/hwmon/hwmon0/device/temp2_input", "hwmon0 temp2"},
    {"/sys/class/hwmon/hwmon1/device/temp2_input", "hwmon1 temp2"},
    {"/sys/class/hwmon/hwmon2/device/temp2_input", "hwmon2 temp2"},
    {"/sys/class/hwmon/hwmon0/temp2_input", "hwmon0 temp2 (new)"},
    {"/sys/class/hwmon/hwmon1/temp2_input", "hwmon1 temp2 (new)"},
    {"/sys/class/hwmon/hwmon2/temp2_input", "hwmon2 temp2 (new)"},
    {"/sys/class/thermal/thermal_zone0/temp", "thermal_zone0"},
    {"/sys/class/thermal/thermal_zone1/temp", "thermal_zone1"},
    {"/sys/class/thermal/thermal_zone2/temp", "thermal_zone2"},
    {"/proc/acpi/thermal_zone/THM/temperature", "ACPI THM"},
    {"/proc/acpi/thermal_zone/THM0/temperature", "ACPI THM0"},
    {"/proc/acpi/thermal_zone/THRM/temperature", "ACPI THRM"},
};

// Discover available temperature sensors
// Returns a newly allocated array of available sensor paths (caller must free)
// Sets count to the number of available sensors
char** discover_temp_sensors(int* count, char*** labels) {
    *count = 0;
    int capacity = 10;
    char** paths = g_malloc(capacity * sizeof(char*));
    char** label_list = g_malloc(capacity * sizeof(char*));
    
    for (int i = 0; i < G_N_ELEMENTS(temp_sensor_paths); i++) {
        FILE* f = fopen(temp_sensor_paths[i].path, "r");
        if (f) {
            fclose(f);
            
            if (*count >= capacity) {
                capacity *= 2;
                paths = g_realloc(paths, capacity * sizeof(char*));
                label_list = g_realloc(label_list, capacity * sizeof(char*));
            }
            
            // Try to read the sensor name from the name file
            char name_path[512];
            char sensor_name[256] = {0};
            
            // Try to find the name file for hwmon sensors
            if (g_str_has_prefix(temp_sensor_paths[i].path, "/sys/class/hwmon/")) {
                // Extract hwmon directory
                const char* hwmon_start = strstr(temp_sensor_paths[i].path, "hwmon");
                if (hwmon_start) {
                    const char* slash = strchr(hwmon_start, '/');
                    if (slash) {
                        int hwmon_len = slash - hwmon_start;
                        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/%.*s/name", hwmon_len, hwmon_start);
                        FILE* name_file = fopen(name_path, "r");
                        if (name_file) {
                            if (fgets(sensor_name, sizeof(sensor_name), name_file)) {
                                // Remove trailing newline
                                sensor_name[strcspn(sensor_name, "\n")] = 0;
                            }
                            fclose(name_file);
                        }
                    }
                }
            }
            
            // Build the label
            if (sensor_name[0]) {
                label_list[*count] = g_strdup_printf("%s (%s)", sensor_name, temp_sensor_paths[i].label);
            } else {
                label_list[*count] = g_strdup(temp_sensor_paths[i].label);
            }
            
            paths[*count] = g_strdup(temp_sensor_paths[i].path);
            (*count)++;
        }
    }
    
    if (labels) *labels = label_list;
    else {
        for (int i = 0; i < *count; i++) g_free(label_list[i]);
        g_free(label_list);
    }
    
    return paths;
}

// Preference for selected temperature sensor path
char* pref_temp_sensor_path = NULL;

int
cpu_temperature(void)
{
    static gboolean unavailable = FALSE;
    if (unavailable) return 0;

    int T = 0;
    static FILE* temperature_file = NULL;
    static const char* format = "temperature: %d C"; // ACPI format by default
    static char* current_path = NULL;
    
    // Check if the preference changed
    if (current_path != pref_temp_sensor_path) {
        if (temperature_file) {
            fclose(temperature_file);
            temperature_file = NULL;
        }
        current_path = pref_temp_sensor_path;
    }
    
    if (!temperature_file) {
        if (pref_temp_sensor_path && pref_temp_sensor_path[0]) {
            // Try to open the user-selected sensor
            temperature_file = fopen(pref_temp_sensor_path, "r");
            if (temperature_file) {
                if (1 != fscanf(temperature_file, format, &T))
                    format = "%d"; // Fallback to simple int
            } else {
                g_message("Failed to open selected temperature sensor: %s", pref_temp_sensor_path);
            }
        }
        
        // If no preference set or failed to open, try defaults
        if (!temperature_file) {
            if ((temperature_file = fopen("/sys/class/hwmon/hwmon0/device/temp1_input", "r"))
             || (temperature_file = fopen("/sys/class/hwmon/hwmon1/device/temp1_input", "r"))
             || (temperature_file = fopen("/sys/class/hwmon/hwmon0/temp1_input", "r"))
             || (temperature_file = fopen("/sys/class/hwmon/hwmon1/temp1_input", "r"))
             || (temperature_file = fopen("/sys/class/thermal/thermal_zone0/temp", "r"))
             || (temperature_file = fopen("/proc/acpi/thermal_zone/THM/temperature", "r"))
             || (temperature_file = fopen("/proc/acpi/thermal_zone/THM0/temperature", "r"))
             || (temperature_file = fopen("/proc/acpi/thermal_zone/THRM/temperature", "r")))
            {
                if (1 != fscanf(temperature_file, format, &T))
                    format = "%d"; // Fallback to simple int
            } else {
                unavailable = TRUE;
                return 0;
            }
        }
    }

    rewind(temperature_file);
    fflush(temperature_file);
    if (1==fscanf(temperature_file, format, &T)) {
        if (T>1000) T=(T+500)/1000;
        return T;
    }

    unavailable = TRUE;
    return 0;
}

// Memory info in megabytes
typedef struct { int Total_MB, Free_MB, Available_MB; } MemInfo;
MemInfo
mem_info(void)
{
    static MemInfo meminfo = {0};
    static gboolean unavailable = FALSE;
    if (unavailable)
        return meminfo;

    static FILE* proc_meminfo = NULL;
    if (proc_meminfo || (proc_meminfo = fopen("/proc/meminfo", "r")))
    {
        int total, free;
        if (2==fscanf(proc_meminfo, "MemTotal: %d kB\nMemFree: %d kB\n", &total, &free))
        {
            meminfo.Total_MB = total >> 10;
            meminfo.Free_MB = free >> 10;
            if (1==fscanf(proc_meminfo, "MemAvailable: %d kB\n", &free))
                meminfo.Available_MB = free >> 10;
            else
                meminfo.Available_MB = meminfo.Free_MB; // Fallback on older kernels
            rewind(proc_meminfo);
            fflush(proc_meminfo);
            return meminfo;
        }
        fclose(proc_meminfo);
    }
    error(0, errno, "Can't read /proc/meminfo");
    unavailable = TRUE;
    return meminfo;
}
