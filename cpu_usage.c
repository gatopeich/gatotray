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
#include <error.h>
#include <errno.h>

typedef unsigned long long ull;

typedef struct {
    int usage;
    int iowait;
} CPU_Usage;

CPU_Usage
cpu_usage(int scale)
{
    /* static stuff */
    static ull cpu_busy_prev=0;
    static ull cpu_iowait_prev=0;
    static ull cpu_total_prev=0;

    static FILE *proc_stat = NULL;
    if( !proc_stat ) {
        if( !(proc_stat = fopen("/proc/stat", "r")))
            error(1, errno, "Could not open /proc/stat");
    }

    ull busy, nice, system, idle, total;
    ull iowait=0, irq=0, softirq=0; /* New in Linux 2.6 */
    if( 4 > fscanf(proc_stat, "cpu %Lu %Lu %Lu %Lu %Lu %Lu %Lu",
                    &busy, &nice, &system, &idle, &iowait, &irq, &softirq))
        error(1, errno, "Can't seem to read /proc/stat properly");
    fflush(proc_stat);
    rewind(proc_stat);

    busy += nice+system+irq+softirq;
    total = busy+idle+iowait;

    CPU_Usage cpu;

    if( busy > cpu_busy_prev )
        cpu.usage = (ull)scale * (busy - cpu_busy_prev)
                    / (total - cpu_total_prev);
    else
        cpu.usage = 0;

    if( iowait > cpu_iowait_prev )
        cpu.iowait = (ull)scale * (iowait - cpu_iowait_prev)
                    / (total - cpu_total_prev);
    else
        cpu.iowait = 0;

    cpu_busy_prev = busy;
    cpu_iowait_prev = iowait;
    cpu_total_prev = total;

    return cpu;
}

int scaling_max_freq = 1;
int scaling_min_freq = 0;
int scaling_cur_freq = 0;

int
cpu_freq(void)
{
    static int active = 0;
    static int errorstate = 0;
    FILE *fp;

    if( !active ) /* Init time, read max & min */
    {
        fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "r");
        if(!fp) {
            if(!errorstate) {
                error(0, errno, "Can't get scaling frequencies");
                errorstate = 1;
            }
            return 0;
        }
        if( 1 != fscanf(fp, "%u", &scaling_min_freq) ) {
            fclose(fp);
            scaling_min_freq = 0;
            error(0, 0, "Can't get min scaling frequency");
            return 0;
        }
        fclose(fp);
        fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "r");
        if(fp)
        {
            if( 1 != fscanf(fp, "%u", &scaling_max_freq) ) {
                fclose(fp);
                scaling_max_freq = 1;
                scaling_min_freq = 0;
                error(0, 0, "Can't get max scaling frequency");
                return 0;
            }
            fclose(fp);
            if( scaling_max_freq <= scaling_min_freq ) {
                scaling_max_freq = 1;
                scaling_min_freq = 0;
                error(0, 0, "Wrong scaling frequencies!?");
                return 0;
            }
        }
        active = 1;
        errorstate = 1;
    }

    fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if(!fp || 1 != fscanf(fp, "%u", &scaling_cur_freq) )
    {
        error(0, 0, "Can't get current processor frequency");
        scaling_max_freq = 1;
        scaling_min_freq = 0;
        scaling_cur_freq = 0;
        active = 0;
    }

    if(fp) fclose(fp);

    return scaling_cur_freq;
}

int
cpu_temperature(void)
{
    static gboolean unavailable = FALSE;
    if (unavailable) return 0;

    int T = 0;
    static FILE* temperature_file = NULL;
    static const char* format;
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
            format = "temperature: %d C"; // ACPI style
            if (!fscanf(temperature_file, format, &T)) {
                format = "%d";
                rewind(temperature_file);
                if (!fscanf(temperature_file, format, &T))
                    goto error;
            }
        } else goto error;
    }

    rewind(temperature_file);
    fflush(temperature_file);
    if (!fscanf(temperature_file, format, &T)) goto error;
    if (T>1000) T=(T+500)/1000;
    return T;
    
    error:
    unavailable = TRUE;
    return 0;
}
