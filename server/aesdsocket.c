#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../aesd-char-driver/aesd_ioctl.h"

#define PORT 9000
#define DATA_FILE "/dev/aesdchar"
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define IOCTL_PREFIX "AESDCHAR_IOCSEEKTO:"

static volatile sig_atomic_t exit_requested = 0;
static int server_fd = -1;

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
    if (server_fd != -1) shutdown(server_fd, SHUT_RDWR);
}

static int send_from_fd(int client_fd, int fd)
{
    char buffer[BUFFER_SIZE];
    ssize_t rd;

    while ((rd = read(fd, buffer, sizeof(buffer))) > 0) {
        ssize_t sent_total = 0;
        while (sent_total < rd) {
            ssize_t sent = send(client_fd, buffer + sent_total, rd - sent_total, 0);
            if (sent < 0) return -1;
            sent_total += sent;
        }
    }

    return rd < 0 ? -1 : 0;
}

static int send_file_to_client(int client_fd)
{
    int fd = open(DATA_FILE, O_RDONLY);
    int rc;

    if (fd < 0) return -1;

    rc = send_from_fd(client_fd, fd);
    close(fd);
    return rc;
}

static bool parse_ioctl(const char *packet, struct aesd_seekto *seekto)
{
    unsigned int x, y;

    if (strncmp(packet, IOCTL_PREFIX, strlen(IOCTL_PREFIX)) != 0) {
        return false;
    }

    if (sscanf(packet + strlen(IOCTL_PREFIX), "%u,%u", &x, &y) != 2) {
        return false;
    }

    seekto->write_cmd = x;
    seekto->write_cmd_offset = y;
    return true;
}

static int handle_client(int client_fd)
{
    char buffer[BUFFER_SIZE];
    char *packet = NULL;
    size_t packet_size = 0;

    while (!exit_requested) {
        ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);

        if (received <= 0) break;

        char *new_packet = realloc(packet, packet_size + received + 1);
        if (!new_packet) {
            free(packet);
            return -1;
        }

        packet = new_packet;
        memcpy(packet + packet_size, buffer, received);
        packet_size += received;
        packet[packet_size] = '\0';

        if (memchr(buffer, '\n', received)) {
            struct aesd_seekto seekto;

            if (parse_ioctl(packet, &seekto)) {
                int fd = open(DATA_FILE, O_RDWR);
                if (fd >= 0) {
                    if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) == 0) {
                        send_from_fd(client_fd, fd);
                    }
                    close(fd);
                }
            } else {
                int fd = open(DATA_FILE, O_WRONLY);
                if (fd >= 0) {
                    write(fd, packet, packet_size);
                    close(fd);
                    send_file_to_client(client_fd);
                }
            }

            break;
        }
    }

    free(packet);
    return 0;
}

static void daemonize(void)
{
    pid_t pid = fork();

    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int main(int argc, char *argv[])
{
    bool daemon_mode = argc == 2 && strcmp(argv[1], "-d") == 0;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) return -1;

    if (daemon_mode) daemonize();

    if (listen(server_fd, BACKLOG) < 0) return -1;

    while (!exit_requested) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    closelog();
    return 0;
}
