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
    float cpu, io_wait;
    char comm[32];
} ProcessInfo;

ProcessInfo *top_procs=NULL, *top_cpu=NULL, *top_mem=NULL, *top_time=NULL, *top_io=NULL;

void ProcessInfo_to_GString(ProcessInfo* p, GString* out)
{
    float gb = p->rss * PAGE_GB();
    float time = p->cpu_time/TICKS_PER_SEC();
    g_string_append_printf(out, "%s %.2g%%cpu %.2g%%io %.2ggb %.4gs pid:%d"
        , p->comm, p->cpu, p->io_wait, gb, time, p->pid);
    g_warn_if_fail(p->pid>0);
}

void top_procs_append_summary(GString* summary)
{
    g_string_append(summary, "Top consumers of...\n·%CPU: ");
    ProcessInfo_to_GString(top_cpu, summary);
    g_string_append(summary, "\n·I/O: ");
    ProcessInfo_to_GString(top_io, summary);
    g_string_append(summary, "\n·RSS: ");
    ProcessInfo_to_GString(top_mem, summary);
    g_string_append(summary, "\n·TIME+: ");
    ProcessInfo_to_GString(top_time, summary);
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
    int l = comm_len<sizeof(pi.comm) ? comm_len : sizeof(pi.comm)-1;
    memcpy(pi.comm, comm, l);
    pi.comm[l] = '\0';

    // Hacky low level field parsing just for fun
    char *fp = comm + comm_len + 4; // skip parens and spaces around 1-char field #3 "state"
    int field = 4;
    #define move_to(n) while(field<n) { while(*fp++>' ') {} ++field; }
    #define read_field(n, name) ULL name=0; move_to(n); \
        do {name = name*10 + *fp - '0';} while (*++fp >= '0'); \
        ++fp; ++field // Finish pointing to next field
    read_field(14, utime);
    read_field(15, stime);
    read_field(16, cutime);
    read_field(17, cstime);
    pi.cpu_time = utime + stime + cutime + cstime;

    read_field(24, rss); pi.rss = rss; // TODO: Discount shared memory
    // printf("14:17 %llu %llu %llu %llu 24:%llu ~ %s\n", utime, stime, cutime, cstime, rss, buf);
    read_field(42, io); pi.io_time = io;

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
        if (!top_io || proc.io_wait > top_io->io_wait)
            top_io = p;

        p = *(it = &(p->next));
    }
    //g_info("PIDs: %s", debug->str);
}
