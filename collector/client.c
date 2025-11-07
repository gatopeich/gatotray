/*
 * client.c - Simple test client for gatotray collector
 * 
 * This is a minimal client library/program that demonstrates how to
 * communicate with the collector daemon via the abstract Unix socket.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define DEFAULT_SOCKET_NAME "gatotray_collector"

/* Connect to collector daemon */
int collector_connect(const char *socket_name) {
    struct sockaddr_un addr;
    int sock;
    
    if (!socket_name) {
        socket_name = getenv("GATOTRAY_SOCKET_NAME");
        if (!socket_name) {
            socket_name = DEFAULT_SOCKET_NAME;
        }
    }
    
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    /* Setup abstract socket address (Linux-specific) */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, socket_name, sizeof(addr.sun_path) - 2);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    
    return sock;
}

/* Send command and read response */
int collector_send_command(int sock, const char *command) {
    char buffer[8192];
    ssize_t bytes_written, bytes_read;
    
    /* Send command */
    snprintf(buffer, sizeof(buffer), "%s\n", command);
    bytes_written = write(sock, buffer, strlen(buffer));
    if (bytes_written < 0) {
        perror("write");
        return -1;
    }
    
    /* Read response line by line */
    char *buf_ptr = buffer;
    size_t buf_left = sizeof(buffer) - 1;
    
    while (buf_left > 0) {
        bytes_read = read(sock, buf_ptr, buf_left);
        if (bytes_read <= 0) {
            break;
        }
        
        buf_ptr[bytes_read] = '\0';
        
        /* Check if we got the END marker */
        if (strstr(buf_ptr, "END\n")) {
            break;
        }
        
        buf_ptr += bytes_read;
        buf_left -= bytes_read;
    }
    
    /* Print response */
    printf("%s", buffer);
    
    return 0;
}

/* Disconnect from collector */
void collector_disconnect(int sock) {
    if (sock >= 0) {
        close(sock);
    }
}

/* Auto-spawn collector if not running */
int collector_auto_spawn(const char *socket_name) {
    int sock = collector_connect(socket_name);
    if (sock >= 0) {
        return sock; /* Already running */
    }
    
    /* Try to spawn the collector */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    
    if (pid == 0) {
        /* Child: exec collector */
        if (socket_name) {
            execlp("gatotray-collector", "gatotray-collector", "-s", socket_name, NULL);
        } else {
            execlp("gatotray-collector", "gatotray-collector", NULL);
        }
        /* If exec fails, try from current directory */
        if (socket_name) {
            execlp("./collector", "collector", "-s", socket_name, NULL);
        } else {
            execlp("./collector", "collector", NULL);
        }
        perror("execlp");
        exit(1);
    }
    
    /* Parent: wait a bit and try to connect */
    sleep(1);
    
    sock = collector_connect(socket_name);
    if (sock < 0) {
        fprintf(stderr, "Failed to connect to spawned collector\n");
        return -1;
    }
    
    return sock;
}

/* Main program - demonstrates client usage */
int main(int argc, char *argv[]) {
    const char *socket_name = NULL;
    const char *command = "TOP";
    int auto_spawn = 0;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            socket_name = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0) {
            auto_spawn = 1;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else {
            fprintf(stderr, "Usage: %s [-s socket_name] [-a] [-c command]\n", argv[0]);
            fprintf(stderr, "  -s: Socket name (default: gatotray_collector)\n");
            fprintf(stderr, "  -a: Auto-spawn collector if not running\n");
            fprintf(stderr, "  -c: Command to send (default: TOP)\n");
            fprintf(stderr, "      Available commands: TOP, HISTORY, QUIT\n");
            return 1;
        }
    }
    
    int sock;
    if (auto_spawn) {
        sock = collector_auto_spawn(socket_name);
    } else {
        sock = collector_connect(socket_name);
    }
    
    if (sock < 0) {
        fprintf(stderr, "Failed to connect to collector\n");
        return 1;
    }
    
    /* Send command */
    if (collector_send_command(sock, command) < 0) {
        collector_disconnect(sock);
        return 1;
    }
    
    collector_disconnect(sock);
    return 0;
}
