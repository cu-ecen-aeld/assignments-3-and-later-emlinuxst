#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 9000
#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define DATA_FILE "/dev/aesdchar"
#else
#define DATA_FILE "/var/tmp/aesdsocketdata"
#endif

#define BACKLOG 10
#define BUFFER_SIZE 1024

static volatile sig_atomic_t exit_requested = 0;
static int server_fd = -1;

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
    syslog(LOG_INFO, "Caught signal, exiting");
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
    }
}

static int send_file_to_client(int client_fd)
{
    int fd = open(DATA_FILE, O_RDONLY);
    if (fd < 0) {
        syslog(LOG_ERR, "open read failed: %s", strerror(errno));
        return -1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_sent = 0;
        while (total_sent < bytes_read) {
            ssize_t sent = send(client_fd, buffer + total_sent,
                                bytes_read - total_sent, 0);
            if (sent < 0) {
                syslog(LOG_ERR, "send failed: %s", strerror(errno));
                close(fd);
                return -1;
            }
            total_sent += sent;
        }
    }

    close(fd);
    return 0;
}

static int handle_client(int client_fd)
{
    char buffer[BUFFER_SIZE];
    char *packet = NULL;
    size_t packet_size = 0;

    while (!exit_requested) {
        ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
        if (received < 0) {
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            free(packet);
            return -1;
        }

        if (received == 0) {
            break;
        }

        char *new_packet = realloc(packet, packet_size + received);
        if (new_packet == NULL) {
            syslog(LOG_ERR, "realloc failed");
            free(packet);
            return -1;
        }

        packet = new_packet;
        memcpy(packet + packet_size, buffer, received);
        packet_size += received;

        if (memchr(buffer, '\n', received) != NULL) {
            int fd = open(DATA_FILE, O_WRONLY | O_APPEND);
            if (fd < 0) {
                syslog(LOG_ERR, "open write failed: %s", strerror(errno));
                free(packet);
                return -1;
            }

            ssize_t written = write(fd, packet, packet_size);
            if (written < 0 || (size_t)written != packet_size) {
                syslog(LOG_ERR, "write failed: %s", strerror(errno));
                close(fd);
                free(packet);
                return -1;
            }

            fsync(fd);
            close(fd);
            free(packet);
            packet = NULL;
            packet_size = 0;

            send_file_to_client(client_fd);
            break;
        }
    }

    free(packet);
    return 0;
}

static void daemonize(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    if (chdir("/") != 0) {
        exit(EXIT_FAILURE);
    }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    if (daemon_mode) {
        daemonize();
    }

    if (listen(server_fd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (exit_requested) {
                break;
            }
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        handle_client(client_fd);

        syslog(LOG_INFO, "Closed connection from %s", client_ip);

        close(client_fd);
    }

    if (server_fd != -1) {
        close(server_fd);
    }

#if !USE_AESD_CHAR_DEVICE
    remove(DATA_FILE);
#endif

    closelog();

    return 0;
}
