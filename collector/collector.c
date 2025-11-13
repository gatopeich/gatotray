/*
 * collector.c - Zero-dependency process monitoring daemon for gatotray
 * 
 * (c) 2024 by gatopeich, licensed under a Creative Commons Attribution 3.0
 * Unported License: http://creativecommons.org/licenses/by/3.0/
 * Briefly: Use it however suits you better and just give me due credit.
 *
 * This daemon:
 * - Samples /proc every second to collect process statistics
 * - Computes per-process CPU% (delta utime+stime against system jiffies)
 * - Tracks VmRSS and command name
 * - Builds Top-N snapshots
 * - Stores snapshots in a memory-mapped ring buffer cache file
 * - Exposes data via an abstract Unix domain socket (Linux-specific)
 * - Accepts ASCII line protocol commands (no JSON)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/select.h>

/* Configuration defaults */
#define DEFAULT_SOCKET_NAME "gatotray_collector"
#define DEFAULT_CACHE_FILE "/tmp/gatotray_top.cache"
#define TOP_N_PROCESSES 10
#define SAMPLE_INTERVAL_SEC 1
#define MAX_CLIENTS 10
#define RING_BUFFER_SLOTS 60  /* Keep 1 minute of history */
#define MAX_PROCESSES 2048

/* Process statistics structure */
typedef struct {
    unsigned int pid;
    unsigned int rss_kb;
    unsigned long long utime;
    unsigned long long stime;
    unsigned long long prev_utime;
    unsigned long long prev_stime;
    unsigned long long prev_total_jiffies;
    float cpu_percent;
    char comm[256];
} ProcessStats;

/* Top-N snapshot entry */
typedef struct {
    unsigned int pid;
    unsigned int rss_kb;
    float cpu_percent;
    char comm[256];
} TopEntry;

/* Snapshot structure */
typedef struct {
    time_t timestamp;
    unsigned long long total_jiffies;
    int num_entries;
    TopEntry entries[TOP_N_PROCESSES];
} Snapshot;

/* Memory-mapped cache file header */
typedef struct {
    unsigned int magic;
    unsigned int version;
    unsigned int slot_size;
    unsigned int num_slots;
    unsigned int write_index;
} CacheHeader;

#define CACHE_MAGIC 0x47415443  /* "GATC" */
#define CACHE_VERSION 1

/* Global state */
static ProcessStats *processes = NULL;
static int num_processes = 0;
static int process_capacity = 0;
static unsigned long long prev_total_jiffies = 0;
static int cache_fd = -1;
static void *cache_map = NULL;
static size_t cache_size = 0;
static int server_socket = -1;
static int client_sockets[MAX_CLIENTS];
static int num_clients = 0;
static volatile sig_atomic_t running = 1;

/* Signal handler */
static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/* Read system jiffies from /proc/stat */
static unsigned long long read_total_jiffies(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("fopen /proc/stat");
        return 0;
    }
    
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) != 8) {
        fclose(fp);
        return 0;
    }
    
    fclose(fp);
    return user + nice + system + idle + iowait + irq + softirq + steal;
}

/* Read process statistics from /proc/[pid]/stat */
static int read_process_stat(unsigned int pid, ProcessStats *ps) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    
    int ret = fscanf(fp, "%*d (%255[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                     ps->comm, &ps->utime, &ps->stime);
    fclose(fp);
    
    if (ret != 3) {
        return -1;
    }
    
    ps->pid = pid;
    return 0;
}

/* Read process memory statistics from /proc/[pid]/status */
static int read_process_status(unsigned int pid, ProcessStats *ps) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/status", pid);
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    
    char line[256];
    ps->rss_kb = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%u", &ps->rss_kb);
            break;
        }
    }
    
    fclose(fp);
    return 0;
}

/* Find process by PID */
static ProcessStats *find_process(unsigned int pid) {
    for (int i = 0; i < num_processes; i++) {
        if (processes[i].pid == pid) {
            return &processes[i];
        }
    }
    return NULL;
}

/* Add or update process */
static void update_process(ProcessStats *new_ps, unsigned long long total_jiffies) {
    ProcessStats *ps = find_process(new_ps->pid);
    
    if (!ps) {
        /* Add new process */
        if (num_processes >= process_capacity) {
            process_capacity = process_capacity ? process_capacity * 2 : 256;
            processes = realloc(processes, process_capacity * sizeof(ProcessStats));
            if (!processes) {
                perror("realloc");
                exit(1);
            }
        }
        ps = &processes[num_processes++];
        memcpy(ps, new_ps, sizeof(ProcessStats));
        ps->prev_utime = new_ps->utime;
        ps->prev_stime = new_ps->stime;
        ps->prev_total_jiffies = total_jiffies;
        ps->cpu_percent = 0.0;
    } else {
        /* Update existing process */
        unsigned long long delta_time = new_ps->utime + new_ps->stime - 
                                       (ps->prev_utime + ps->prev_stime);
        unsigned long long delta_total = total_jiffies - ps->prev_total_jiffies;
        
        if (delta_total > 0) {
            ps->cpu_percent = (100.0 * delta_time) / delta_total;
        } else {
            ps->cpu_percent = 0.0;
        }
        
        ps->prev_utime = new_ps->utime;
        ps->prev_stime = new_ps->stime;
        ps->prev_total_jiffies = total_jiffies;
        ps->utime = new_ps->utime;
        ps->stime = new_ps->stime;
        ps->rss_kb = new_ps->rss_kb;
        strncpy(ps->comm, new_ps->comm, sizeof(ps->comm) - 1);
        ps->comm[sizeof(ps->comm) - 1] = '\0';
    }
}

/* Compare functions for qsort */
static int compare_by_cpu(const void *a, const void *b) {
    const ProcessStats *pa = (const ProcessStats *)a;
    const ProcessStats *pb = (const ProcessStats *)b;
    if (pb->cpu_percent > pa->cpu_percent) return 1;
    if (pb->cpu_percent < pa->cpu_percent) return -1;
    return 0;
}

/* Scan /proc and update process statistics */
static void scan_processes(void) {
    unsigned long long total_jiffies = read_total_jiffies();
    if (total_jiffies == 0) {
        return;
    }
    
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        perror("opendir /proc");
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        /* Check if directory name is a number (PID) */
        if (entry->d_type != DT_DIR) {
            continue;
        }
        
        char *endptr;
        unsigned long pid = strtoul(entry->d_name, &endptr, 10);
        if (*endptr != '\0' || pid == 0) {
            continue;
        }
        
        ProcessStats ps;
        if (read_process_stat((unsigned int)pid, &ps) < 0) {
            continue;
        }
        
        if (read_process_status((unsigned int)pid, &ps) < 0) {
            continue;
        }
        
        update_process(&ps, total_jiffies);
    }
    
    closedir(proc_dir);
    prev_total_jiffies = total_jiffies;
}

/* Build Top-N snapshot */
static void build_snapshot(Snapshot *snapshot) {
    snapshot->timestamp = time(NULL);
    snapshot->total_jiffies = prev_total_jiffies;
    
    /* Sort processes by CPU usage */
    qsort(processes, num_processes, sizeof(ProcessStats), compare_by_cpu);
    
    /* Copy top N processes */
    int count = num_processes < TOP_N_PROCESSES ? num_processes : TOP_N_PROCESSES;
    snapshot->num_entries = count;
    
    for (int i = 0; i < count; i++) {
        snapshot->entries[i].pid = processes[i].pid;
        snapshot->entries[i].rss_kb = processes[i].rss_kb;
        snapshot->entries[i].cpu_percent = processes[i].cpu_percent;
        strncpy(snapshot->entries[i].comm, processes[i].comm, 
                sizeof(snapshot->entries[i].comm) - 1);
        snapshot->entries[i].comm[sizeof(snapshot->entries[i].comm) - 1] = '\0';
    }
}

/* Initialize memory-mapped cache file */
static int init_cache(const char *cache_file) {
    cache_size = sizeof(CacheHeader) + RING_BUFFER_SLOTS * sizeof(Snapshot);
    
    /* Create or open cache file */
    cache_fd = open(cache_file, O_RDWR | O_CREAT, 0644);
    if (cache_fd < 0) {
        perror("open cache file");
        return -1;
    }
    
    /* Set file size */
    if (ftruncate(cache_fd, cache_size) < 0) {
        perror("ftruncate");
        close(cache_fd);
        return -1;
    }
    
    /* Memory map the file */
    cache_map = mmap(NULL, cache_size, PROT_READ | PROT_WRITE, MAP_SHARED, cache_fd, 0);
    if (cache_map == MAP_FAILED) {
        perror("mmap");
        close(cache_fd);
        return -1;
    }
    
    /* Initialize header */
    CacheHeader *header = (CacheHeader *)cache_map;
    header->magic = CACHE_MAGIC;
    header->version = CACHE_VERSION;
    header->slot_size = sizeof(Snapshot);
    header->num_slots = RING_BUFFER_SLOTS;
    header->write_index = 0;
    
    /* Sync to disk */
    msync(cache_map, sizeof(CacheHeader), MS_SYNC);
    
    return 0;
}

/* Write snapshot to cache */
static void write_snapshot_to_cache(const Snapshot *snapshot) {
    if (!cache_map) {
        return;
    }
    
    CacheHeader *header = (CacheHeader *)cache_map;
    Snapshot *slots = (Snapshot *)((char *)cache_map + sizeof(CacheHeader));
    
    /* Write to current slot */
    memcpy(&slots[header->write_index], snapshot, sizeof(Snapshot));
    
    /* Update write index (ring buffer) */
    header->write_index = (header->write_index + 1) % header->num_slots;
    
    /* Sync to disk */
    msync(cache_map, cache_size, MS_ASYNC);
}

/* Initialize abstract Unix domain socket */
static int init_socket(const char *socket_name) {
    struct sockaddr_un addr;
    
    server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return -1;
    }
    
    /* Set socket to non-blocking */
    int flags = fcntl(server_socket, F_GETFL, 0);
    fcntl(server_socket, F_SETFL, flags | O_NONBLOCK);
    
    /* Setup abstract socket address (Linux-specific) */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Abstract socket: first byte is \0, rest is name */
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, socket_name, sizeof(addr.sun_path) - 2);
    
    /* Bind socket */
    if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_socket);
        return -1;
    }
    
    /* Listen for connections */
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_socket);
        return -1;
    }
    
    return 0;
}

/* Accept new client connection */
static void accept_client(void) {
    if (num_clients >= MAX_CLIENTS) {
        return;
    }
    
    int client_fd = accept(server_socket, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
        }
        return;
    }
    
    /* Set client socket to non-blocking */
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    
    client_sockets[num_clients++] = client_fd;
}

/* Send snapshot to client as ASCII lines */
static void send_snapshot_to_client(int client_fd, const Snapshot *snapshot) {
    char buffer[4096];
    int len = 0;
    ssize_t result;
    
    /* Header line */
    len = snprintf(buffer, sizeof(buffer), "TIMESTAMP %ld\n", snapshot->timestamp);
    result = write(client_fd, buffer, len);
    if (result < 0) {
        return;
    }
    
    len = snprintf(buffer, sizeof(buffer), "ENTRIES %d\n", snapshot->num_entries);
    result = write(client_fd, buffer, len);
    if (result < 0) {
        return;
    }
    
    /* Process entries */
    for (int i = 0; i < snapshot->num_entries; i++) {
        const TopEntry *entry = &snapshot->entries[i];
        len = snprintf(buffer, sizeof(buffer), "%d %.2f %u %s\n",
                      entry->pid, entry->cpu_percent, entry->rss_kb, entry->comm);
        result = write(client_fd, buffer, len);
        if (result < 0) {
            return;
        }
    }
    
    /* End marker */
    const char *end = "END\n";
    result = write(client_fd, end, strlen(end));
    (void)result; /* Ignore result for end marker */
}

/* Handle client command */
static void handle_client_command(int client_fd, const char *command) {
    if (strncmp(command, "TOP", 3) == 0) {
        /* Get latest snapshot from cache */
        if (cache_map) {
            CacheHeader *header = (CacheHeader *)cache_map;
            Snapshot *slots = (Snapshot *)((char *)cache_map + sizeof(CacheHeader));
            
            /* Get the most recent snapshot (one before write_index) */
            unsigned int read_index = (header->write_index + header->num_slots - 1) % header->num_slots;
            send_snapshot_to_client(client_fd, &slots[read_index]);
        }
    } else if (strncmp(command, "HISTORY", 7) == 0) {
        /* Send all snapshots in chronological order */
        if (cache_map) {
            CacheHeader *header = (CacheHeader *)cache_map;
            Snapshot *slots = (Snapshot *)((char *)cache_map + sizeof(CacheHeader));
            
            /* Start from oldest (write_index) and wrap around */
            for (unsigned int i = 0; i < header->num_slots; i++) {
                unsigned int idx = (header->write_index + i) % header->num_slots;
                if (slots[idx].timestamp > 0) {
                    send_snapshot_to_client(client_fd, &slots[idx]);
                }
            }
        }
    } else if (strncmp(command, "QUIT", 4) == 0) {
        /* Client wants to disconnect */
        return;
    } else {
        /* Unknown command */
        const char *error = "ERROR Unknown command\n";
        ssize_t result = write(client_fd, error, strlen(error));
        (void)result; /* Ignore result for error message */
    }
}

/* Process client requests */
static void process_clients(void) {
    char buffer[1024];
    
    for (int i = 0; i < num_clients; ) {
        int client_fd = client_sockets[i];
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        
        if (bytes_read <= 0) {
            /* Client disconnected or error */
            close(client_fd);
            /* Remove from array */
            for (int j = i; j < num_clients - 1; j++) {
                client_sockets[j] = client_sockets[j + 1];
            }
            num_clients--;
            continue;
        }
        
        buffer[bytes_read] = '\0';
        
        /* Process each line */
        char *line = buffer;
        char *newline;
        while ((newline = strchr(line, '\n')) != NULL) {
            *newline = '\0';
            handle_client_command(client_fd, line);
            line = newline + 1;
        }
        
        i++;
    }
}

/* Main daemon loop */
static void daemon_loop(void) {
    time_t last_sample = 0;
    
    while (running) {
        time_t now = time(NULL);
        
        /* Sample processes every second */
        if (now - last_sample >= SAMPLE_INTERVAL_SEC) {
            scan_processes();
            
            Snapshot snapshot;
            build_snapshot(&snapshot);
            write_snapshot_to_cache(&snapshot);
            
            last_sample = now;
        }
        
        /* Check for new clients */
        accept_client();
        
        /* Process client requests */
        process_clients();
        
        /* Sleep briefly to avoid busy loop */
        usleep(100000); /* 100ms */
    }
}

/* Cleanup function */
static void cleanup(void) {
    /* Close client connections */
    for (int i = 0; i < num_clients; i++) {
        close(client_sockets[i]);
    }
    
    /* Close server socket */
    if (server_socket >= 0) {
        close(server_socket);
    }
    
    /* Unmap and close cache */
    if (cache_map) {
        msync(cache_map, cache_size, MS_SYNC);
        munmap(cache_map, cache_size);
    }
    if (cache_fd >= 0) {
        close(cache_fd);
    }
    
    /* Free process list */
    free(processes);
}

/* Print usage information */
static void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -s <socket_name>  Set socket name (default: %s)\n", DEFAULT_SOCKET_NAME);
    fprintf(stderr, "  -c <cache_file>   Set cache file path (default: %s)\n", DEFAULT_CACHE_FILE);
    fprintf(stderr, "  -d                Daemonize (run in background)\n");
    fprintf(stderr, "  -h                Show this help\n");
}

/* Daemonize the process */
static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid > 0) {
        /* Parent exits */
        exit(0);
    }
    
    /* Child continues */
    if (setsid() < 0) {
        perror("setsid");
        exit(1);
    }
    
    /* Change working directory to root */
    if (chdir("/") < 0) {
        perror("chdir");
        exit(1);
    }
    
    /* Close standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    /* Redirect to /dev/null */
    open("/dev/null", O_RDONLY); /* stdin */
    open("/dev/null", O_WRONLY); /* stdout */
    open("/dev/null", O_WRONLY); /* stderr */
}

int main(int argc, char *argv[]) {
    const char *socket_name = NULL;
    const char *cache_file = DEFAULT_CACHE_FILE;
    int should_daemonize = 0;
    int opt;
    
    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "s:c:dh")) != -1) {
        switch (opt) {
            case 's':
                socket_name = optarg;
                break;
            case 'c':
                cache_file = optarg;
                break;
            case 'd':
                should_daemonize = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    /* Use environment variable if socket name not specified */
    if (!socket_name) {
        socket_name = getenv("GATOTRAY_SOCKET_NAME");
        if (!socket_name) {
            socket_name = DEFAULT_SOCKET_NAME;
        }
    }
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    /* Daemonize if requested */
    if (should_daemonize) {
        daemonize();
    }
    
    /* Initialize cache */
    if (init_cache(cache_file) < 0) {
        fprintf(stderr, "Failed to initialize cache\n");
        return 1;
    }
    
    /* Initialize socket */
    if (init_socket(socket_name) < 0) {
        fprintf(stderr, "Failed to initialize socket\n");
        cleanup();
        return 1;
    }
    
    fprintf(stderr, "Collector daemon started\n");
    fprintf(stderr, "Socket: %s (abstract)\n", socket_name);
    fprintf(stderr, "Cache: %s\n", cache_file);
    
    /* Run main loop */
    daemon_loop();
    
    fprintf(stderr, "Collector daemon shutting down\n");
    cleanup();
    
    return 0;
}
