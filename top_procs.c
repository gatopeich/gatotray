// Track per process resource usage
// Goals:
// - Track top memory consumers
// - Track top CPU consumers
// - Track top file descriptor consumers (modern IDEs)
// - Track top thread consumers (multi-threaded apps)
// - Identify processes starving the system
// - Do OOM kill before it is too late?

// Loosely based on procps lib
#include <unistd.h>
#include <string.h>
#include <dirent.h>

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

// Count open file descriptors for a process
// Returns count, or 0 on error (lightweight check using readdir)
static unsigned count_fds(const char* pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/fd", pid);
    DIR* dir = opendir(path);
    if (!dir)
        return 0;
    
    unsigned count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        // Skip "." and ".." entries
        if (entry->d_name[0] != '.')
            count++;
    }
    closedir(dir);
    return count;
}

// Count threads for a process
// Returns count, or 0 on error (lightweight check using readdir)
static unsigned count_threads(const char* pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/task", pid);
    DIR* dir = opendir(path);
    if (!dir)
        return 0;
    
    unsigned count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        // Skip "." and ".." entries
        if (entry->d_name[0] != '.')
            count++;
    }
    closedir(dir);
    return count;
}


typedef unsigned long long ULL;
typedef struct ProcessInfo {
    struct ProcessInfo* next; // embedded single linked list
    unsigned pid, rss, fd_count, thread_count;
    ULL cpu_time, io_time, sample_time;
    float cpu, io_wait, average_cpu;
    char comm[32];
} ProcessInfo;

int procs_total=0, procs_active=0;
// Track top resource consumers:
// - top_cpu: highest current CPU usage (%)
// - top_avg: highest average CPU usage since process start (%)
// - top_cumulative: highest absolute CPU time consumed (ticks) - often long-running system processes
// - top_io: highest I/O wait time
// - top_mem: highest memory usage
// - top_fds: highest file descriptor count (modern IDEs often have many open files)
// - top_threads: highest thread count (modern applications often use many threads)
ProcessInfo *top_procs=NULL, *top_cpu=NULL, *top_mem=NULL, *top_avg=NULL, *top_io=NULL
    , *top_cumulative=NULL, *top_fds=NULL, *top_threads=NULL, *procs_self=NULL;

// Helper macro to format small values as 0 for cleaner display
#define max2decs(g) (g>.005?g:.0)

void ProcessInfo_to_GString(ProcessInfo* p, GString* out)
{
    float gb = p->rss * PAGE_GB();
    // Dynamic CPU icon based on usage (using raw value for accurate threshold comparison)
    const char* cpu_icon = p->cpu > CPU_HIGH_THRESHOLD ? "📈" : "📉";
    // Dynamic I/O icon based on wait percentage
    const char* io_icon = p->io_wait < IO_WAIT_THRESHOLD ? "🔄" : "⏳";
    
    g_string_append_printf(out, "%s: %s%.2g%%cpu %.2g%%avg %s%.2g%%io 💾%.2ggb 📂%d 🧵%d (%d)"
        , p->comm, cpu_icon, max2decs(p->cpu), max2decs(p->average_cpu), io_icon, p->io_wait, gb
        , p->fd_count, p->thread_count, p->pid);
    g_warn_if_fail(p->pid>0);
}

void ProcessInfo_to_GString_with_category(ProcessInfo* p, GString* out, const char* category_icon)
{
    float gb = p->rss * PAGE_GB();
    // Dynamic CPU icon based on usage (using raw value for accurate threshold comparison)
    const char* cpu_icon = p->cpu > CPU_HIGH_THRESHOLD ? "📈" : "📉";
    // Dynamic I/O icon based on wait percentage
    const char* io_icon = p->io_wait < IO_WAIT_THRESHOLD ? "🔄" : "⏳";
    
    g_string_append_printf(out, "%s %s: %s%.2g%%cpu %.2g%%avg %s%.2g%%io 💾%.2ggb 📂%d 🧵%d (%d)"
        , category_icon, p->comm, cpu_icon, max2decs(p->cpu), max2decs(p->average_cpu), io_icon, p->io_wait, gb
        , p->fd_count, p->thread_count, p->pid);
    g_warn_if_fail(p->pid>0);
}

void top_procs_append_summary(GString* summary)
{
    g_string_append_printf(summary, "\n📊  %d processes, %d active", procs_total, procs_active);
    if (top_cpu) {
        g_string_append(summary, "\n\n📊  Top consumers:\n");
        ProcessInfo_to_GString_with_category(top_cpu, summary, "🔥");
        if (top_avg && top_avg != top_cpu) {
            g_string_append(summary, "\n");
            ProcessInfo_to_GString_with_category(top_avg, summary, "🔥");
        }
        if (top_cumulative && top_cumulative != top_avg && top_cumulative != top_cpu) {
            g_string_append(summary, "\n");
            ProcessInfo_to_GString_with_category(top_cumulative, summary, "🔥");
        }
        if (top_io && top_io != top_cumulative && top_io != top_avg && top_io != top_cpu) {
            g_string_append(summary, "\n");
            ProcessInfo_to_GString_with_category(top_io, summary, "🔁");
        }
        if (top_mem && top_mem != top_io && top_mem != top_cumulative && top_mem != top_avg && top_mem != top_cpu) {
            g_string_append(summary, "\n");
            ProcessInfo_to_GString_with_category(top_mem, summary, "🧠");
        }
        if (top_fds && top_fds != top_mem && top_fds != top_io && top_fds != top_cumulative && top_fds != top_avg && top_fds != top_cpu) {
            g_string_append(summary, "\n");
            ProcessInfo_to_GString_with_category(top_fds, summary, "📂");
        }
        if (top_threads && top_threads != top_fds && top_threads != top_mem && top_threads != top_io && top_threads != top_cumulative && top_threads != top_avg && top_threads != top_cpu) {
            g_string_append(summary, "\n");
            ProcessInfo_to_GString_with_category(top_threads, summary, "🧵");
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

    // Count FDs and threads for all processes
    // This is lightweight (readdir) so we can do it for all processes
    pi.fd_count = count_fds(pid);
    pi.thread_count = count_threads(pid);

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
        // Track process with highest absolute cumulative CPU time (total ticks),
        // which is often long-running processes like systemd, regardless of current usage
        if (!top_cumulative || proc.cpu_time > top_cumulative->cpu_time)
            top_cumulative = p;
        
        // Track top FD and thread consumers
        if (!top_fds || proc.fd_count > top_fds->fd_count)
            top_fds = p;
        if (!top_threads || proc.thread_count > top_threads->thread_count)
            top_threads = p;

        p = *(it = &(p->next));
    }
}
