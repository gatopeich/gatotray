// Network statistics: per-interface bandwidth, per-process TCP bandwidth & RTT

#include <linux/netlink.h>
#include <linux/inet_diag.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <dirent.h>
#include <unistd.h>

#ifdef NDEBUG
#undef  g_debug
#define g_debug(...) do{}while(0)
#endif

typedef struct { char name[16]; u64 rx, tx; } IfaceStat;
static IfaceStat iface_prev[8];
static int n_ifaces = 0;
int net_rx_KBps = 0, net_tx_KBps = 0;

static void net_dev_refresh(int elapsed_ms)
{
    static FILE* f = NULL;
    if (!f) f = fopen("/proc/net/dev", "r");
    if (!f) return;
    rewind(f);
    fflush(f); // /proc files: rewind alone leaves stale buffer

    char line[256];
    if (!fgets(line, sizeof(line), f)) return;
    if (!fgets(line, sizeof(line), f)) return;

    IfaceStat current[8];
    int n = 0;
    int total_rx = 0, total_tx = 0;

    while (n < 8 && fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ') p++;
        char* colon = strchr(p, ':');
        if (!colon) continue;
        int namelen = colon - p;
        if (namelen >= 16) namelen = 15;
        memcpy(current[n].name, p, namelen);
        current[n].name[namelen] = '\0';

        if (strcmp(current[n].name, "lo") == 0) continue;

        // /proc/net/dev fields after iface name:
        // rx_bytes packets errs drop fifo frame compressed multicast tx_bytes ...
        // To read rx_bytes and tx_bytes: 1 read, 7 skips, 1 read.
        u64 rx, tx;
        if (sscanf(colon + 1, " %llu %*u %*u %*u %*u %*u %*u %llu",
                   &rx, &tx) != 2) continue;
        current[n].rx = rx;
        current[n].tx = tx;

        if (n_ifaces && elapsed_ms > 0) {
            for (int i = 0; i < n_ifaces; i++) {
                if (strcmp(iface_prev[i].name, current[n].name) == 0) {
                    u64 drx = rx - iface_prev[i].rx;
                    u64 dtx = tx - iface_prev[i].tx;
                    total_rx += (int)(drx * 1000 / elapsed_ms / 1024);
                    total_tx += (int)(dtx * 1000 / elapsed_ms / 1024);
                    break;
                }
            }
        }
        n++;
    }

    memcpy(iface_prev, current, n * sizeof(IfaceStat));
    n_ifaces = n;
    net_rx_KBps = total_rx;
    net_tx_KBps = total_tx;
}

#define MAX_SOCKETS 4096

typedef struct {
    uint32_t inode;
    uint32_t rtt_us;
    uint64_t bytes_acked;
    uint64_t bytes_received;
} SockStat;

static SockStat sock_stats[MAX_SOCKETS];
static int n_socks = 0;

#define SOCK_HASH_SIZE 8192
#define SOCK_HASH_MASK (SOCK_HASH_SIZE - 1)
static int sock_hash[SOCK_HASH_SIZE];

static void sock_hash_clear(void)
{
    memset(sock_hash, -1, sizeof(sock_hash));
}

static void sock_hash_insert(uint32_t inode, int idx)
{
    uint32_t h = inode & SOCK_HASH_MASK;
    while (sock_hash[h] != -1)
        h = (h + 1) & SOCK_HASH_MASK;
    sock_hash[h] = idx;
}

static SockStat* sock_hash_lookup(uint32_t inode)
{
    uint32_t h = inode & SOCK_HASH_MASK;
    while (sock_hash[h] != -1) {
        if (sock_stats[sock_hash[h]].inode == inode)
            return &sock_stats[sock_hash[h]];
        h = (h + 1) & SOCK_HASH_MASK;
    }
    return NULL;
}

static void inet_diag_query(int fd, int family)
{
    struct {
        struct nlmsghdr nlh;
        struct inet_diag_req_v2 req;
    } msg = {
        .nlh = {
            .nlmsg_len = sizeof(msg),
            .nlmsg_type = SOCK_DIAG_BY_FAMILY,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
        },
        .req = {
            .sdiag_family = family,
            .sdiag_protocol = IPPROTO_TCP,
            .idiag_ext = (1 << (INET_DIAG_INFO - 1)),
            .idiag_states = ~0U,
        }
    };

    if (send(fd, &msg, sizeof(msg), 0) < 0) return;

    char buf[32768];
    for (;;) {
        int len = recv(fd, buf, sizeof(buf), 0);
        if (len <= 0) break;

        for (struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
             NLMSG_OK(nlh, (unsigned)len); nlh = NLMSG_NEXT(nlh, len))
        {
            if (nlh->nlmsg_type == NLMSG_DONE) return;
            if (nlh->nlmsg_type == NLMSG_ERROR) return;
            if (n_socks >= MAX_SOCKETS) return;

            struct inet_diag_msg* diag = NLMSG_DATA(nlh);
            if (!diag->idiag_inode) continue;

            SockStat* ss = &sock_stats[n_socks];
            ss->inode = diag->idiag_inode;
            ss->rtt_us = 0;
            ss->bytes_acked = 0;
            ss->bytes_received = 0;

            unsigned int attrlen = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*diag));
            struct rtattr* attr = (struct rtattr*)(diag + 1);
            for (; RTA_OK(attr, attrlen); attr = RTA_NEXT(attr, attrlen)) {
                if (attr->rta_type == INET_DIAG_INFO) {
                    struct tcp_info* ti = RTA_DATA(attr);
                    ss->rtt_us = ti->tcpi_rtt;
                    if (RTA_PAYLOAD(attr) >= offsetof(struct tcp_info, tcpi_bytes_received)
                            + sizeof(ti->tcpi_bytes_received)) {
                        ss->bytes_acked = ti->tcpi_bytes_acked;
                        ss->bytes_received = ti->tcpi_bytes_received;
                    }
                    break;
                }
            }

            sock_hash_insert(ss->inode, n_socks);
            n_socks++;
        }
    }
}

static void inet_diag_refresh(void)
{
    sock_hash_clear();
    n_socks = 0;

    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_INET_DIAG);
    if (fd < 0) return;

    inet_diag_query(fd, AF_INET);
    inet_diag_query(fd, AF_INET6);

    close(fd);
    g_debug("inet_diag: %d sockets", n_socks);
}

#define MAX_INODE_MAP 8192

typedef struct { uint32_t inode; unsigned pid; } InodePid;
static InodePid inode_map[MAX_INODE_MAP];
static int n_inode_map = 0;

// Cheap path: just count fds via readdir, no readlinks
static int net_count_pid_fds(const char* pid_str)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/fd", pid_str);
    DIR* dir = opendir(path);
    if (!dir) return 0;
    int fd_count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)))
        if (ent->d_name[0] >= '0' && ent->d_name[0] <= '9')
            fd_count++;
    closedir(dir);
    return fd_count;
}

// Heavy path: counts fds AND populates inode->pid map for socket attribution
static int net_collect_pid_sockets(const char* pid_str, unsigned pid, int* socket_count)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/fd", pid_str);
    DIR* dir = opendir(path);
    if (!dir) { *socket_count = 0; return 0; }

    int fd_count = 0, socks = 0;
    struct dirent* ent;
    while ((ent = readdir(dir))) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        fd_count++;

        if (n_inode_map >= MAX_INODE_MAP) continue;

        char link[280];
        snprintf(link, sizeof(link), "/proc/%s/fd/%s", pid_str, ent->d_name);
        char target[64];
        int tlen = readlink(link, target, sizeof(target) - 1);
        if (tlen <= 0) continue;
        target[tlen] = '\0';

        if (strncmp(target, "socket:[", 8) != 0) continue;
        uint32_t inode = strtoul(target + 8, NULL, 10);
        if (!inode) continue;

        socks++;
        inode_map[n_inode_map].inode = inode;
        inode_map[n_inode_map].pid = pid;
        n_inode_map++;
    }
    closedir(dir);
    *socket_count = socks;
    return fd_count;
}

static void net_inode_map_clear(void) { n_inode_map = 0; }

#define MAX_NET_PROCS 512

typedef struct {
    unsigned pid;
    uint64_t prev_acked, prev_received;
    int rx_KBps, tx_KBps;
    unsigned min_rtt_us;
} ProcNetStat;

static ProcNetStat proc_net[MAX_NET_PROCS];
static int n_proc_net = 0;

static ProcNetStat* net_stat_by_pid(unsigned pid)
{
    for (int i = 0; i < n_proc_net; i++)
        if (proc_net[i].pid == pid)
            return &proc_net[i];
    return NULL;
}

static void net_stats_aggregate(int elapsed_ms)
{
    // Build temporary per-pid aggregates from inode_map × sock_stats
    typedef struct { unsigned pid; uint64_t acked, received; unsigned min_rtt; } Agg;
    Agg aggs[MAX_NET_PROCS];
    int n_aggs = 0;

    for (int i = 0; i < n_inode_map; i++) {
        SockStat* ss = sock_hash_lookup(inode_map[i].inode);
        if (!ss) continue;

        unsigned pid = inode_map[i].pid;
        Agg* a = NULL;
        for (int j = 0; j < n_aggs; j++) {
            if (aggs[j].pid == pid) { a = &aggs[j]; break; }
        }
        if (!a) {
            if (n_aggs >= MAX_NET_PROCS) continue;
            a = &aggs[n_aggs++];
            a->pid = pid;
            a->acked = a->received = 0;
            a->min_rtt = 0;
        }

        a->acked += ss->bytes_acked;
        a->received += ss->bytes_received;
        if (ss->rtt_us && (!a->min_rtt || ss->rtt_us < a->min_rtt))
            a->min_rtt = ss->rtt_us;
    }

    // Update proc_net[] with rates
    ProcNetStat new_net[MAX_NET_PROCS];
    int n_new = 0;

    for (int i = 0; i < n_aggs && n_new < MAX_NET_PROCS; i++) {
        ProcNetStat* prev = net_stat_by_pid(aggs[i].pid);
        ProcNetStat* ns = &new_net[n_new++];
        ns->pid = aggs[i].pid;
        ns->min_rtt_us = aggs[i].min_rtt;

        if (prev && elapsed_ms > 0 && aggs[i].received >= prev->prev_received) {
            uint64_t drx = aggs[i].received - prev->prev_received;
            uint64_t dtx = aggs[i].acked - prev->prev_acked;
            ns->rx_KBps = (int)(drx * 1000 / elapsed_ms / 1024);
            ns->tx_KBps = (int)(dtx * 1000 / elapsed_ms / 1024);
        } else {
            ns->rx_KBps = ns->tx_KBps = 0;
        }
        ns->prev_acked = aggs[i].acked;
        ns->prev_received = aggs[i].received;
    }

    memcpy(proc_net, new_net, n_new * sizeof(ProcNetStat));
    n_proc_net = n_new;
}

static void net_stats_refresh(gboolean heavy)
{
    if (heavy) inet_diag_refresh();
}

static void net_stats_append_summary(GString* out)
{
    g_string_append_printf(out, "\n🌐  Network ↓%d ↑%d KB/s",
        net_rx_KBps, net_tx_KBps);
}
