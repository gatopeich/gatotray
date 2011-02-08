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
    static enum { NOTHING = 0, SYS, PROC, UNKNOWN } found = UNKNOWN;
    if( NOTHING == found ) return 0;

    static FILE *acpi_temp = NULL;
    if( !acpi_temp ) {
        if( (acpi_temp = fopen("/sys/class/thermal/thermal_zone0/temp", "r")) )
        {
            found = SYS;
        }
        else {
            error(0, errno, "Can't open /sys/class/thermal/thermal_zone0/temp");
            if( (acpi_temp = fopen("/proc/acpi/thermal_zone/THM/temperature", "r"))
            || (acpi_temp = fopen("/proc/acpi/thermal_zone/THM0/temperature", "r"))
            || (acpi_temp = fopen("/proc/acpi/thermal_zone/THRM/temperature", "r")) )
            {
                found = PROC;
            }
            else {
                error(0, errno, "Can't open /proc/acpi/thermal_zone/THxx/temperature");
                found = NOTHING;
                return 0;
            }
        }
    }

    int T = 0;
    if( SYS == found ) {
        if(1!=fscanf(acpi_temp, "%d", &T)) {
            error(0, errno, "Can't read temperature file.");
            fclose(acpi_temp);
            found = NOTHING;
            return 0;
        }
        T /= 1000;
    }
    else if( PROC == found ) {
        if(1!=fscanf(acpi_temp, "temperature: %d C", &T)) {
            error(0, errno, "Can't read temperature file, wrong format?");
            fclose(acpi_temp);
            found = NOTHING;
            return 0;
        }
    }
    rewind(acpi_temp);
    fflush(acpi_temp);

    return T;
}
