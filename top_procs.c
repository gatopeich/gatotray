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
double TICKS_PER_SEC(void) {
    static double ticks = 0;
    if (!ticks) {
        ticks = sysconf(_SC_CLK_TCK);
        g_message("_SC_CLK_TCK = %g", ticks);
    }
    return ticks;
}

typedef unsigned long long ULL;
typedef struct {
    unsigned pid, rss;
    ULL prev_cpu, cpu_time;
    char comm[32];
} ProcessInfo;

ProcessInfo top_mem, top_cpu;

void ProcessInfo_update(ProcessInfo* pi, ProcessInfo* update)
{
    update->prev_cpu = (pi->pid==update->pid)? pi->cpu_time : 0;
    *pi = *update;
}

void ProcessInfo_to_GString(ProcessInfo* p, GString* out)
{
    double gb = p->rss * PAGE_GB();
    //int cpu = (p->cpu_time - p->prev_cpu)*100/interval_jiffies;
    double time = p->cpu_time/TICKS_PER_SEC();
    g_string_append_printf(out, "%s %.1fgb cpu=%gs ", p->comm, gb, time);
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

void ProcessInfo_scan(ProcessInfo* p, const char* pid)
{
    p->pid = atoi(pid);
    char buf[256];
    sprintf(buf, "/proc/%s/stat", pid);
    FILE* f = fopen(buf, "r");
    if (!f) {
        strcpy(p->comm, "(defunct)");
        return;
    }
    int len = fread(buf, 1, sizeof(buf)-1, f);
    fclose(f);

    // Extract executable name, handling extra parentheses e.g. ((sd-pam)) 
    char* comm = strchr(buf, '(') + 1;
    int comm_len = len - (comm-buf) - 1;
    while (comm_len>0 && comm[comm_len] != ')') --comm_len;
    int l = comm_len<sizeof(p->comm) ? comm_len : sizeof(p->comm)-1;
    memcpy(p->comm, comm, l);
    p->comm[l] = '\0';

    ULL utime, stime, cutime, cstime;
    int fields = sscanf(comm + comm_len + 3, // skip ") S "
        "%*s %*s %*s %*s %*s " // -- ppid pgrp session tty tpgid
        "%*s %*s %*s %*s %*s " // -- flags min_flt cmin_flt maj_flt cmaj_flt
        "%llu %llu %llu %llu " // utime stime cutime cstime
        "%*s %*s %*s %*s %*s " // -- priority, nice, nlwp, alarm, start_time
        "%*s %u " // vsize, rss
        , &utime, &stime, &cutime, &cstime, &p->rss);
    p->cpu_time = utime + stime + cutime + cstime;
    if (fields < 5) {
        static int warned = 0;
        if (!warned++)
            g_message("Only got %d fields from /proc/%s/stat. comm=%s, rss=%u", fields, pid, p->comm, p->rss);
        p->cpu_time = p->rss = 0;
    }
}

void top_procs_refresh(void)
{
    static GDir* proc_dir = NULL;
    if (proc_dir) {
        g_dir_rewind(proc_dir);
    } else {
        proc_dir = g_dir_open ("/proc", 0, NULL);
    }
    top_cpu.cpu_time = top_mem.rss = 0;
    const gchar* pid;
    while ((pid = g_dir_read_name(proc_dir)))
    {
        if (pid[0] < '0' || pid[0] > '9')
            continue;
        ProcessInfo update;
        ProcessInfo_scan(&update, pid);
        if (update.rss > top_mem.rss)
            ProcessInfo_update (&top_mem, &update);
        if (update.cpu_time > top_cpu.cpu_time)
            ProcessInfo_update (&top_cpu, &update);
    }
}
