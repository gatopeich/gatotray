// Track per process resource usage
// Goals:
// - Track top memory consumers
// - Track top CPU consumers
// - Identify processes starving the system
// - Do OOM kill before it is too late?

// Loosely based on procps lib
#include <unistd.h>

#ifdef NDEBUG
#undef  g_debug
#define g_debug(...) do{}while(0)
#endif

float PAGE_GB(void) {
    static float cached = 0;
    if (!cached) {
        cached = sysconf(_SC_PAGESIZE)*1.0/(1<<30);
        g_info("_SC_PAGESIZE = %g", cached);
    }
    return cached;
}

float TICKS_PER_SEC(void) {
    static float cached = 0;
    if (!cached) {
        cached = sysconf(_SC_CLK_TCK);
        g_info("_SC_CLK_TCK = %g", cached);
    }
    return cached;
}


typedef unsigned long long ULL;
typedef struct ProcessInfo {
    struct ProcessInfo* next; // embedded single linked list
    unsigned pid, rss;
    float cpu;
    ULL cpu_time, sample_time;
    char comm[32];
} ProcessInfo;

ProcessInfo *top_procs = NULL, *top_cpu = NULL, *top_mem = NULL, *top_time = NULL;

void ProcessInfo_update(ProcessInfo* pi, ProcessInfo* update)
{
    update->cpu = (update->cpu_time - pi->cpu_time) * 100.0
                / (update->sample_time - pi->sample_time);
    void* next = pi->next;
    *pi = *update;
    pi->next = next;
}

void ProcessInfo_to_GString(ProcessInfo* p, GString* out)
{
    float gb = p->rss * PAGE_GB();
    float time = p->cpu_time/TICKS_PER_SEC();
    g_string_append_printf(out, "%s: %.1f%%CPU %.2gGB %.4gs pid:%d"
        , p->comm, p->cpu, gb, time, p->pid);
    g_warn_if_fail(p->pid>0);
}

void top_procs_append_summary(GString* summary)
{
    g_string_append(summary, "Top consumers of %CPU, MEM, TIME+:\n· ");
    ProcessInfo_to_GString(top_cpu, summary);
    g_string_append(summary, "\n· ");
    ProcessInfo_to_GString(top_mem, summary);
    g_string_append(summary, "\n· ");
    ProcessInfo_to_GString(top_time, summary);
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
    // TODO: pi.rss -= shr; // Do not count shared memory
    pi.sample_time = cpu_total_ticks;
    if (fields < 6)
        g_error("Only got %d valid fields from /proc/%s/stat: %s", fields, pid, buf);
    return pi;
}

void top_procs_refresh(void)
{
    static int delay = 0;
    if (delay-->0)
        return;
    delay = 3; // TODO: Configure delay
    static GDir* proc_dir = NULL;
    if (proc_dir) {
        g_dir_rewind(proc_dir);
    } else {
        proc_dir = g_dir_open ("/proc", 0, NULL);
    }

    // static GString* debug;
    //debug = debug ? g_string_assign(debug,"") : g_string_new(0);

    // iterator pointers
    ProcessInfo **it = &top_procs, *p = *it;

    const gchar* pid;
    while ((pid = g_dir_read_name(proc_dir)))
    {
        //g_string_append_printf(debug, "%s, ", pid);
        if (pid[0] < '0' || pid[0] > '9')
            continue;
        ProcessInfo proc = ProcessInfo_scan(pid);

        // /proc/stat/[pid] entries seem to be sorted by numeric pid
        // new PIDs are expected to appear at end of the list
        while (G_UNLIKELY(p && p->pid != proc.pid)) {
            g_debug("Process %d (%s) died", p->pid, p->comm);
            if (G_UNLIKELY(p->next && p->pid >= p->next->pid))
                g_critical("Broken assumption that /proc/[pid] are always sorted. Please report bug!");
            *it = p->next;
            free(p);
            p = *it;
        }
        if (p) {
            g_debug("Updating process %d (%s)", p->pid, p->comm);
            ProcessInfo_update(p, &proc);
        } else {
            // reached end of the list, add new
            *it = p = malloc(sizeof(proc));
            *p = proc;
            p->next = NULL;
            g_debug("Added process %d (%s)", p->pid, p->comm);
        }

        if (!top_mem || proc.rss > top_mem->rss)
            top_mem = p;
        if (!top_time || proc.cpu_time > top_time->cpu_time)
            top_time = p;
        if (!top_cpu || proc.cpu > top_cpu->cpu)
            top_cpu = p;

        p = *(it = &(p->next));
    }
    //g_info("PIDs: %s", debug->str);
}
