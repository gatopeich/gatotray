// Track per process resource usage
// Goals:
// - Track top memory consumers
// - Track top CPU consumers
// - Identify processes starving the system
// - Do OOM kill before it is too late?

// Loosely based on procps lib
#include <unistd.h>
#include <string.h>

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
    ULL cpu_time, io_time, sample_time;
    float cpu, io_wait, average_cpu;
    char comm[32];
} ProcessInfo;

int procs_total=0, procs_active=0;
ProcessInfo *top_procs=NULL, *top_cpu=NULL, *top_mem=NULL, *top_avg=NULL, *top_io=NULL
    , *top_cumulative=NULL, *procs_self=NULL;

void ProcessInfo_to_GString(ProcessInfo* p, GString* out)
{
    float gb = p->rss * PAGE_GB();
    #define max2decs(g) (g>.005?g:.0)
    g_string_append_printf(out, "%s: ⚙️  %.2g%%cpu %.2g%%avg ⏳  %.2g%%io 💾  %.2ggb (%d)"
        , p->comm, max2decs(p->cpu), max2decs(p->average_cpu), p->io_wait, gb, p->pid);
    g_warn_if_fail(p->pid>0);
}

void top_procs_append_summary(GString* summary)
{
    g_string_append_printf(summary, "\n⚡  %d processes, %d active", procs_total, procs_active);
    if (top_cpu) {
        g_string_append(summary, "\n\n⭐  Top consumers:\n· ");
        ProcessInfo_to_GString(top_cpu, summary);
        if (top_avg && top_avg != top_cpu) {
            g_string_append(summary, "\n· ");
            ProcessInfo_to_GString(top_avg, summary);
        }
        if (top_cumulative && top_cumulative != top_avg && top_cumulative != top_cpu) {
            g_string_append(summary, "\n· ");
            ProcessInfo_to_GString(top_cumulative, summary);
        }
        if (top_io && top_io != top_cumulative && top_io != top_avg && top_io != top_cpu) {
            g_string_append(summary, "\n· ");
            ProcessInfo_to_GString(top_io, summary);
        }
        if (top_mem && top_mem != top_io && top_mem != top_cumulative && top_mem != top_avg && top_mem != top_cpu) {
            g_string_append(summary, "\n· ");
            ProcessInfo_to_GString(top_mem, summary);
        }
    }
    if (procs_self) {
        g_string_append(summary, "\n\n");
        ProcessInfo_to_GString(procs_self, summary);
    }
}

ProcessInfo ProcessInfo_scan(const char* pid)
{
    ProcessInfo pi;
    pi.pid = atoi(pid);
    char buf[512];
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
    int l = 0;
    while (l < comm_len && l < (sizeof(pi.comm)-1)) {
        char c = comm[l];
        pi.comm[l++] = (c >= 32 && c <= 126) ? c : '?';  // remove non-ASCII characters
    }
    pi.comm[l] = '\0';

    // Hacky low level field parsing just for fun
    char *fp = comm + comm_len + 4; // skip parens and spaces around 1-char field #3 "state"
    int field = 4;
    #define move_to(n) while(field<n) { while(*fp++>' ') {} ++field; }
    #define read_field(n, name) ULL name=0; move_to(n); \
        do {name = name*10 + *fp - '0';} while (*++fp >= '0'); \
        ++fp; ++field // Finish pointing to next field

    // Add up CPU usage...
    read_field(14, utime);
    read_field(15, stime);
    // ...including reaped subprocesses:
    read_field(16, cutime);
    read_field(17, cstime);
    pi.cpu_time = utime + stime + cutime + cstime;

    // Calculate CPU average since process started
    read_field(22, starttime);
    pi.average_cpu = pi.cpu_time * 100.0 / (cpu_total_ticks - starttime);

    read_field(24, rss); pi.rss = rss; // TODO: Discount shared memory
    // printf("14:17 %llu %llu %llu %llu 24:%llu ~ %s\n", utime, stime, cutime, cstime, rss, buf);
    read_field(42, delayacct_blkio_ticks); pi.io_time = delayacct_blkio_ticks;

    pi.sample_time = cpu_total_ticks;
    return pi;
}

void ProcessInfo_update(ProcessInfo* pi, ProcessInfo* update)
{
    float percent_time = 100.0 / (update->sample_time - pi->sample_time);
    update->cpu = (update->cpu_time - pi->cpu_time) * percent_time;
    update->io_wait = (update->io_time - pi->io_time) * percent_time;
    void* next = pi->next;
    *pi = *update;
    pi->next = next;
}

void top_procs_refresh(void)
{
    static int delay = 0;
    if (--delay > 0)
        return;
    delay = top_refresh_ms/refresh_interval_ms;
    static GDir* proc_dir = NULL;
    int find_my_pid = 0;
    if (proc_dir) {
        g_dir_rewind(proc_dir);
    } else {
        find_my_pid = getpid();
        proc_dir = g_dir_open ("/proc", 0, NULL);
    }

    // iterator pointers
    ProcessInfo **it = &top_procs, *p = *it;

    const gchar* pid;
    procs_total = procs_active = 0;
    while ((pid = g_dir_read_name(proc_dir)))
    {
        if (pid[0] < '0' || pid[0] > '9')
            continue;
        ++procs_total;
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
            procs_active += !!p->cpu;
        } else {
            // reached end of the list, add new
            *it = p = malloc(sizeof(proc));
            *p = proc;
            p->next = NULL;
            p->cpu = p->io_wait = 0;
            g_debug("Added process %d (%s)", p->pid, p->comm);
            if (find_my_pid && p->pid == find_my_pid)
                procs_self = p;
        }

        if (!top_mem || proc.rss > top_mem->rss)
            top_mem = p;
        if (!top_avg || proc.average_cpu > top_avg->average_cpu)
            top_avg = p;
        if (!top_cpu || proc.cpu > top_cpu->cpu)
            top_cpu = p;
        if (!top_io || proc.io_wait > top_io->io_wait)
            top_io = p;
        if (!top_cumulative || proc.cpu_time > top_cumulative->cpu_time)
            top_cumulative = p;

        p = *(it = &(p->next));
    }
}
