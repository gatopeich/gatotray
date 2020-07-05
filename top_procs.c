// Track per process resource usage
// Goals:
// - Track top memory consumers
// - Track top CPU consumers
// - Identify processes starving the system
// - Do OOM kill before it is too late?

// See https://manpages.debian.org/procps/openproc
#include <proc/readproc.h>
#include <proc/sysinfo.h>
#include <unistd.h>

unsigned PAGE_KB(void) {
    static unsigned pkb = 0;
    return pkb ? pkb : (pkb = sysconf(_SC_PAGESIZE)/1024);
}
jiff JIFF_PER_SEC(void) {
    static jiff jps = 0;
    return jps ? jps : (jps=sysconf(_SC_CLK_TCK));
}

typedef struct {
    unsigned pid, rss_kb;
    jiff prev_cpu, cpu_time;
    char desc[200];
} ProcessInfo;

double uptime_jiffies=0, interval_jiffies;
ProcessInfo top_mem, top_cpu;

void ProcessInfo_update(ProcessInfo* pi, proc_t* p)
{
    if (pi->pid == p->tid) {
        pi->prev_cpu = pi->cpu_time;
    } else {
        pi->prev_cpu = 0;
    }
    pi->pid = p->tid;
    pi->cpu_time = p->cutime + p->cstime;
    pi->rss_kb = p->rss * PAGE_KB();
}

void ProcessInfo_to_GString(ProcessInfo* p, GString* out)
{
    double gb = p->rss_kb/(double)(1<<20);
    int cpu = (p->cpu_time - p->prev_cpu)*100/interval_jiffies;
    g_string_append_printf(out, "#%d %.1fGB %d%%CPU ", p->pid, gb, cpu);
    char buf[200];
    sprintf(buf, "/proc/%u/cmdline", p->pid);
    FILE* f = fopen(buf, "r");
    int len;
    if (f && (len=fread(buf, 1, sizeof(buf), f))) {
        for (int i=0; i<len; i++)
            buf[i] = buf[i]>=' ' && buf[i]<='~' ? buf[i] : ' ';
        g_string_append_len(out, buf, len);
    }
}

void top_procs_refresh(void)
{
    PROCTAB* pTable = openproc(PROC_FILLSTAT);
    top_cpu.cpu_time = top_mem.rss_kb = 0;
    proc_t proc = {0};
    while (readproc(pTable, &proc)) {
        jiff cpu_time = proc.cutime + proc.cstime;
        unsigned rss_kb = proc.rss * PAGE_KB();
        if (rss_kb > top_mem.rss_kb)
            ProcessInfo_update (&top_mem, &proc);
        if (cpu_time > top_cpu.cpu_time)
            ProcessInfo_update (&top_cpu, &proc);
    }

    double uptime_secs, idle_secs;
    uptime(&uptime_secs, &idle_secs);
    jiff jiffies = uptime_secs * JIFF_PER_SEC();
    interval_jiffies = jiffies - uptime_jiffies;
    uptime_jiffies = jiffies;

    closeproc(pTable);
}
