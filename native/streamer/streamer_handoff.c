/* v12: camera buffer handoff. Injected into the camera provider like v9/v10, but
   does no per-frame work. A thread waits on the abstract unix socket
   "@qpro-camera-buffers-v12"; for each client it finds the provider's camera
   dmabuf mappings (0xC4000-byte dmabuf maps, sorted by address) and sends the
   matching file descriptors with SCM_RIGHTS. The client maps them read-only and
   reads frames itself, outside this process. */
#define _GNU_SOURCE

#include <dirent.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CAMERA_MAP_BYTES ((size_t)0xC4000)
#define MAX_CAMERA_MAPS 16
#define LOG_PATH "/data/local/tmp/questpro-live-v12.log"
#define SOCKET_NAME "qpro-camera-buffers-v12"

typedef struct {
    unsigned long long start, inode;
} CameraMap;

static int g_started;

static void log_line(const char *format, ...) {
    int fd = open(LOG_PATH, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) return;
    char line[512];
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int used = snprintf(line, sizeof(line), "[%lld.%03lld] ", (long long)now.tv_sec, (long long)(now.tv_nsec / 1000000));
    if (used < 0 || (size_t)used >= sizeof(line)) { close(fd); return; }
    va_list args;
    va_start(args, format);
    int added = vsnprintf(line + used, sizeof(line) - (size_t)used, format, args);
    va_end(args);
    if (added > 0) {
        size_t length = strnlen(line, sizeof(line) - 2);
        line[length++] = '\n';
        (void)write(fd, line, length);
    }
    close(fd);
}

static int compare_start(const void *left, const void *right) {
    const CameraMap *a = left, *b = right;
    return a->start < b->start ? -1 : a->start > b->start;
}

/* Same selection as v9/v10: dmabuf mappings of exactly CAMERA_MAP_BYTES. */
static size_t find_camera_maps(CameraMap *maps) {
    FILE *file = fopen("/proc/self/maps", "r");
    if (!file) return 0;
    size_t count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        unsigned long long start = 0, end = 0, offset = 0, inode = 0;
        char permissions[5] = {0}, device[24] = {0};
        int path_offset = 0;
        if (sscanf(line, "%llx-%llx %4s %llx %23s %llu %n", &start, &end, permissions, &offset, device, &inode,
                   &path_offset) != 6 || permissions[0] != 'r' || end - start != CAMERA_MAP_BYTES)
            continue;
        if (!strstr(line + path_offset, "/dmabuf:dmabuf")) continue;
        if (count == MAX_CAMERA_MAPS) { count = 0; break; }
        maps[count].start = start;
        maps[count].inode = inode;
        ++count;
    }
    fclose(file);
    qsort(maps, count, sizeof(maps[0]), compare_start);
    return count;
}

/* The mapping's inode identifies the dmabuf; find an open fd for the same inode. */
static int fd_for_inode(unsigned long long inode) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int found = -1;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end;
        long fd = strtol(entry->d_name, &end, 10);
        if (*end || fd < 0) continue;
        struct stat status;
        if (fstat((int)fd, &status) == 0 && (unsigned long long)status.st_ino == inode) { found = (int)fd; break; }
    }
    closedir(dir);
    return found;
}

static void serve(int client) {
    CameraMap maps[MAX_CAMERA_MAPS];
    size_t count = find_camera_maps(maps);
    int fds[MAX_CAMERA_MAPS];
    size_t sent = 0;
    for (size_t i = 0; i < count; ++i) {
        int fd = fd_for_inode(maps[i].inode);
        if (fd < 0) { log_line("FD_NOT_FOUND map=%zu inode=%llu", i, maps[i].inode); sent = 0; break; }
        fds[sent++] = fd;
    }
    uint32_t header[2] = { 0x31424651u /* "QFB1" */, (uint32_t)sent };
    struct iovec iov = { header, sizeof(header) };
    union { struct cmsghdr align; char buffer[CMSG_SPACE(sizeof(int) * MAX_CAMERA_MAPS)]; } control;
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (sent) {
        memset(&control, 0, sizeof(control));
        message.msg_control = control.buffer;
        message.msg_controllen = CMSG_SPACE(sizeof(int) * sent);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * sent);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * sent);
    }
    ssize_t result = sendmsg(client, &message, MSG_NOSIGNAL);
    log_line("HANDOFF maps=%zu sent=%zu result=%zd error=%s", count, sent, result, result < 0 ? strerror(errno) : "none");
}

static void *server_main(void *unused) {
    (void)unused;
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    memcpy(address.sun_path + 1, SOCKET_NAME, strlen(SOCKET_NAME));
    socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCKET_NAME));
    if (server < 0 || bind(server, (struct sockaddr *)&address, length) != 0 || listen(server, 4) != 0) {
        log_line("SERVER_FAILED error=%s", strerror(errno));
        if (server >= 0) close(server);
        return NULL;
    }
    log_line("SERVER_LISTENING socket=@%s", SOCKET_NAME);
    for (;;) {
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            log_line("ACCEPT_FAILED error=%s", strerror(errno));
            sleep(1);
            continue;
        }
        serve(client);
        close(client);
    }
}

__attribute__((constructor)) static void start_v12(void) {
    if (__atomic_exchange_n(&g_started, 1, __ATOMIC_ACQ_REL)) return;
    pthread_t thread;
    int result = pthread_create(&thread, NULL, server_main, NULL);
    if (result != 0) { log_line("THREAD_CREATE_FAILED error=%s", strerror(result)); return; }
    pthread_detach(thread);
    log_line("STREAMER_STARTED version=12.0 mode=buffer-handoff per-frame-work=none");
}
