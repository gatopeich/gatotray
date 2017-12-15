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
#include <error.h>
#include <errno.h>

#ifdef __G_LIB_H__
#define error(_s, _e, fmt, args...) do { \
    g_message("ERROR %d:" fmt, _e, ##args); \
    if (_s) exit(_s); \
} while(0)
#endif

typedef unsigned long long u64;
typedef struct { int usage, iowait;  } CPU_Usage;
CPU_Usage
cpu_usage(int scale)
{
    /* static stuff */
    static u64 cpu_busy_prev=0;
    static u64 cpu_iowait_prev=0;
    static u64 cpu_total_prev=0;

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

    if( busy > cpu_busy_prev )
        cpu.usage = (u64)scale * (busy - cpu_busy_prev) / (total - cpu_total_prev);
    else
        cpu.usage = 0;

    if( iowait > cpu_iowait_prev )
        cpu.iowait = (u64)scale * (iowait - cpu_iowait_prev) / (total - cpu_total_prev);
    else
        cpu.iowait = 0;

    cpu_busy_prev = busy;
    cpu_iowait_prev = iowait;
    cpu_total_prev = total;

    return cpu;
}

int
file_read_int(const char* file, int on_error)
{
    FILE* fp = fopen(file, "r");
    if (fp)
    {
        int i;
        if (fscanf(fp, "%d", &i))
            return i;
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
        if (fscanf(cur_freq_file, "%u", &scaling_cur_freq) == 1) {
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

int
cpu_temperature(void)
{
    static gboolean unavailable = FALSE;
    if (unavailable) return 0;

    int T = 0;
    static FILE* temperature_file = NULL;
    static const char* format = "temperature: %d C"; // ACPI format by default
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
MemInfo meminfo = {0};
MemInfo
mem_info(void)
{
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
