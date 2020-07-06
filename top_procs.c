// Track per process resource usage
// Goals:
// - Track top memory consumers
// - Track top CPU consumers
// - Identify processes starving the system
// - Do OOM kill before it is too late?

// Loosely based on procps lib
#include <unistd.h>

double PAGE_GB(void) {
    static double pkb = 0;
    return pkb ? pkb : (pkb = sysconf(_SC_PAGESIZE)/(double)(1<<30));
}
int TICKS_PER_SEC(void) {
    static int ticks = 0;
    if (!ticks) {
        ticks = sysconf(_SC_CLK_TCK);
        g_message("_SC_CLK_TCK = %d", ticks);
    }
    return ticks;
}

typedef unsigned long long ULL;
typedef struct {
    unsigned pid, rss;
    float cpu_usage;
    ULL cpu_time, sample_time;
    char comm[32];
} ProcessInfo;

ProcessInfo top_mem, top_cpu;

void ProcessInfo_update(ProcessInfo* pi, ProcessInfo* update)
{
    update->cpu_usage = (pi->pid != update->pid) ? 0 :
        (update->cpu_time - pi->cpu_time)*100.0/(update->sample_time - pi->sample_time);
    *pi = *update;
}

void ProcessInfo_to_GString(ProcessInfo* p, GString* out)
{
    double gb = p->rss * PAGE_GB();
    int time = p->cpu_time/TICKS_PER_SEC();
    g_string_append_printf(out, "%s %.1fgb cpu=%ds(%.3g%%) ", p->comm, gb, time, p->cpu_usage);
    char buf[200];
    sprintf(buf, "/proc/%u/cmdline", p->pid);
    FILE* f = fopen(buf, "r");
    int len;
    if (f && (len=fread(buf, 1, sizeof(buf), f))) {
        for (int i=0; i<len; i++)
            buf[i] = buf[i]>=' ' && buf[i]<='~' ? buf[i] : ' ';
        g_string_append_len(out, buf, len);
    }
    fclose(f);
}

ProcessInfo ProcessInfo_scan(const char* pid)
{
    ProcessInfo pi;
    pi.pid = atoi(pid);
    char buf[256];
    sprintf(buf, "/proc/%s/stat", pid);
    FILE* f = fopen(buf, "r");
    if (!f) {
        strcpy(pi.comm, "(defunct)");
        return pi;
    }
    int len = fread(buf, 1, sizeof(buf)-1, f);
    fclose(f);

    // Extract executable name, handling extra parentheses e.g. ((sd-pam)) 
    char* comm = strchr(buf, '(') + 1;
    int comm_len = len - (comm-buf) - 1;
    while (comm_len>0 && comm[comm_len] != ')') --comm_len;
    int l = comm_len<sizeof(pi.comm) ? comm_len : sizeof(pi.comm)-1;
    memcpy(pi.comm, comm, l);
    pi.comm[l] = '\0';

    ULL utime, stime, cutime, cstime;
    int fields = sscanf(comm + comm_len + 3, // skip ") S "
        "%*s %*s %*s %*s %*s " // -- ppid pgrp session tty tpgid
        "%*s %*s %*s %*s %*s " // -- flags min_flt cmin_flt maj_flt cmaj_flt
        "%llu %llu %llu %llu " // utime stime cutime cstime
        "%*s %*s %*s %*s %*s " // -- priority, nice, nlwp, alarm, start_time
        "%*s %u " // vsize, rss
        , &utime, &stime, &cutime, &cstime, &pi.rss);
    pi.cpu_time = utime + stime + cutime + cstime;
    pi.sample_time = cpu_total_ticks;
    if (fields < 5) {
        static int warned = 0;
        if (!warned++)
            g_message("Only got %d fields from /proc/%s/stat. comm=%s, rss=%u", fields, pid, pi.comm, pi.rss);
        pi.cpu_time = pi.rss = 0;
    }
    return pi;
}

void top_procs_refresh(void)
{
    static GDir* proc_dir = NULL;
    if (proc_dir) {
        g_dir_rewind(proc_dir);
    } else {
        proc_dir = g_dir_open ("/proc", 0, NULL);
    }
    ProcessInfo max_rss, max_cpu;
    max_cpu.cpu_time = max_rss.rss = 0;
    const gchar* pid;
    while ((pid = g_dir_read_name(proc_dir)))
    {
        if (pid[0] < '0' || pid[0] > '9')
            continue;
        ProcessInfo proc = ProcessInfo_scan(pid);
        if (proc.rss > max_rss.rss)
            max_rss = proc;
        if (proc.cpu_time > max_cpu.cpu_time)
            max_cpu = proc;
    }
    ProcessInfo_update (&top_mem, &max_rss);
    ProcessInfo_update (&top_cpu, &max_cpu);
}
