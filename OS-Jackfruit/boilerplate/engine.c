/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 16384
#define CHILD_COMMAND_LEN 512
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define DEVICE_PATH "/dev/container_monitor"

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    int read_fd;
    bounded_buffer_t *buffer;
} producer_arg_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    char log_path[PATH_MAX];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int exit_code;
    int exit_signal;
    int stop_requested;
    int monitor_registered;
    int producer_started;
    pthread_t producer_thread;
    void *child_stack;
    child_config_t *child_cfg;
    char final_reason[64];
    struct container_record *next;
} container_record_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile sig_atomic_t should_stop;
    volatile sig_atomic_t reap_requested;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    char base_rootfs[PATH_MAX];
} supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int client_fd;
    control_request_t req;
} client_task_t;

static supervisor_ctx_t *g_supervisor_ctx;
static volatile sig_atomic_t g_client_forward_stop;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static void supervisor_signal_handler(int signo)
{
    if (!g_supervisor_ctx)
        return;

    if (signo == SIGCHLD)
        g_supervisor_ctx->reap_requested = 1;
    else
        g_supervisor_ctx->should_stop = 1;
}

static void client_signal_handler(int signo)
{
    (void)signo;
    g_client_forward_stop = 1;
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int is_container_finished(container_state_t state)
{
    return state == CONTAINER_STOPPED || state == CONTAINER_KILLED || state == CONTAINER_EXITED;
}

static void safe_snprintf(char *dest, size_t dest_size, const char *fmt, ...)
{
    va_list ap;

    if (dest_size == 0)
        return;

    va_start(ap, fmt);
    vsnprintf(dest, dest_size, fmt, ap);
    va_end(ap);
    dest[dest_size - 1] = '\0';
}

static void copy_cstr(char *dest, size_t dest_size, const char *src)
{
    safe_snprintf(dest, dest_size, "%s", src ? src : "");
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (strcmp(cur->id, id) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static int live_rootfs_in_use_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (!is_container_finished(cur->state) && strcmp(cur->rootfs, rootfs) == 0)
            return 1;
        cur = cur->next;
    }
    return 0;
}

static int send_response(int fd, int status, const char *fmt, ...)
{
    control_response_t resp;
    va_list ap;

    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    va_start(ap, fmt);
    vsnprintf(resp.message, sizeof(resp.message), fmt, ap);
    va_end(ap);

    if (write(fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp))
        return -1;
    return 0;
}

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return -1;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    copy_cstr(req.container_id, sizeof(req.container_id), container_id);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return -1;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    copy_cstr(req.container_id, sizeof(req.container_id), container_id);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

static void *producer_thread_main(void *arg)
{
    producer_arg_t *producer = arg;
    char buf[LOG_CHUNK_SIZE];

    for (;;) {
        ssize_t nread = read(producer->read_fd, buf, sizeof(buf));
        log_item_t item;

        if (nread == 0)
            break;
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        memset(&item, 0, sizeof(item));
        copy_cstr(item.container_id, sizeof(item.container_id), producer->container_id);
        item.length = (size_t)nread;
        memcpy(item.data, buf, (size_t)nread);
        fprintf(stdout, "[producer:%s] queued %zd bytes\n", producer->container_id, nread);
        fflush(stdout);
        if (bounded_buffer_push(producer->buffer, &item) != 0)
            break;
    }

    fprintf(stdout, "[producer:%s] exiting\n", producer->container_id);
    fflush(stdout);
    close(producer->read_fd);
    free(producer);
    return NULL;
}

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    for (;;) {
        container_record_t *record = NULL;
        int log_fd;

        if (bounded_buffer_pop(&ctx->log_buffer, &item) == 1)
            break;

        pthread_mutex_lock(&ctx->metadata_lock);
        record = find_container_locked(ctx, item.container_id);
        log_fd = (record != NULL) ? open(record->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_fd >= 0) {
            ssize_t ignored = write(log_fd, item.data, item.length);
            (void)ignored;
            close(log_fd);
            fprintf(stdout, "[logger:%s] flushed %zu bytes\n", item.container_id, item.length);
            fflush(stdout);
        }
    }

    fprintf(stdout, "[logger] exiting after draining buffer\n");
    fflush(stdout);
    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = arg;
    int devnull_fd;

    if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        dprintf(cfg->log_write_fd, "setpriority failed: %s\n", strerror(errno));

    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        dprintf(cfg->log_write_fd, "sethostname failed: %s\n", strerror(errno));

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        dprintf(cfg->log_write_fd, "mount private failed: %s\n", strerror(errno));

    if (chdir(cfg->rootfs) < 0) {
        dprintf(cfg->log_write_fd, "chdir(%s) failed: %s\n", cfg->rootfs, strerror(errno));
        return 1;
    }

    if (chroot(".") < 0) {
        dprintf(cfg->log_write_fd, "chroot failed: %s\n", strerror(errno));
        return 1;
    }

    if (chdir("/") < 0) {
        dprintf(cfg->log_write_fd, "chdir(/) failed: %s\n", strerror(errno));
        return 1;
    }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        dprintf(cfg->log_write_fd, "mount /proc failed: %s\n", strerror(errno));

    devnull_fd = open("/dev/null", O_RDONLY);
    if (devnull_fd >= 0) {
        dup2(devnull_fd, STDIN_FILENO);
        close(devnull_fd);
    }

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 || dup2(cfg->log_write_fd, STDERR_FILENO) < 0)
        return 1;

    close(cfg->log_write_fd);
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
    fprintf(stderr, "exec failed for command '%s': %s\n", cfg->command, strerror(errno));
    return 127;
}

static int start_container(supervisor_ctx_t *ctx, const control_request_t *req, char *message, size_t message_size)
{
    container_record_t *record;
    producer_arg_t *producer;
    child_config_t *child_cfg;
    char *stack;
    int pipefd[2];
    pid_t child_pid;

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
        safe_snprintf(message, message_size, "failed to create logs directory: %s\n", strerror(errno));
        return 1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_locked(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        safe_snprintf(message, message_size, "container id '%s' already exists\n", req->container_id);
        return 1;
    }
    if (live_rootfs_in_use_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        safe_snprintf(message, message_size, "rootfs '%s' is already in use by a running container\n", req->rootfs);
        return 1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    record = calloc(1, sizeof(*record));
    producer = calloc(1, sizeof(*producer));
    child_cfg = calloc(1, sizeof(*child_cfg));
    stack = malloc(STACK_SIZE);
    if (!record || !producer || !child_cfg || !stack) {
        safe_snprintf(message, message_size, "allocation failure while starting container\n");
        free(record);
        free(producer);
        free(child_cfg);
        free(stack);
        return 1;
    }

    if (pipe(pipefd) < 0) {
        safe_snprintf(message, message_size, "pipe creation failed: %s\n", strerror(errno));
        free(record);
        free(producer);
        free(child_cfg);
        free(stack);
        return 1;
    }

    memset(record, 0, sizeof(*record));
    copy_cstr(record->id, sizeof(record->id), req->container_id);
    copy_cstr(record->rootfs, sizeof(record->rootfs), req->rootfs);
    copy_cstr(record->command, sizeof(record->command), req->command);
    safe_snprintf(record->log_path, sizeof(record->log_path), "%s/%s.log", LOG_DIR, req->container_id);
    record->soft_limit_bytes = req->soft_limit_bytes;
    record->hard_limit_bytes = req->hard_limit_bytes;
    record->nice_value = req->nice_value;
    record->state = CONTAINER_STARTING;
    strcpy(record->final_reason, "starting");

    memset(producer, 0, sizeof(*producer));
    copy_cstr(producer->container_id, sizeof(producer->container_id), req->container_id);
    producer->read_fd = pipefd[0];
    producer->buffer = &ctx->log_buffer;

    memset(child_cfg, 0, sizeof(*child_cfg));
    copy_cstr(child_cfg->id, sizeof(child_cfg->id), req->container_id);
    copy_cstr(child_cfg->rootfs, sizeof(child_cfg->rootfs), req->rootfs);
    copy_cstr(child_cfg->command, sizeof(child_cfg->command), req->command);
    child_cfg->nice_value = req->nice_value;
    child_cfg->log_write_fd = pipefd[1];

    if (pthread_create(&record->producer_thread, NULL, producer_thread_main, producer) != 0) {
        safe_snprintf(message, message_size, "failed to create producer thread\n");
        close(pipefd[0]);
        close(pipefd[1]);
        free(record);
        free(producer);
        free(child_cfg);
        free(stack);
        return 1;
    }
    record->producer_started = 1;

    child_pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD,
                      child_cfg);
    close(pipefd[1]);

    if (child_pid < 0) {
        safe_snprintf(message, message_size, "clone failed: %s\n", strerror(errno));
        pthread_join(record->producer_thread, NULL);
        free(child_cfg);
        free(stack);
        free(record);
        return 1;
    }

    record->child_cfg = child_cfg;
    record->child_stack = stack;
    record->host_pid = child_pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_RUNNING;
    strcpy(record->final_reason, "running");

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (register_with_monitor(ctx->monitor_fd,
                              record->id,
                              record->host_pid,
                              record->soft_limit_bytes,
                              record->hard_limit_bytes) == 0)
        record->monitor_registered = 1;

    safe_snprintf(message,
                  message_size,
                  "started container=%s pid=%d rootfs=%s command=\"%s\"\n",
                  record->id,
                  record->host_pid,
                  record->rootfs,
                  record->command);
    return 0;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    ctx->reap_requested = 0;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *record;

        pthread_mutex_lock(&ctx->metadata_lock);
        record = ctx->containers;
        while (record && record->host_pid != pid)
            record = record->next;

        if (record) {
            if (WIFEXITED(status)) {
                record->exit_code = WEXITSTATUS(status);
                record->exit_signal = 0;
                record->state = record->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                safe_snprintf(record->final_reason,
                              sizeof(record->final_reason),
                              "%s",
                              record->stop_requested ? "manual_stop" : "normal_exit");
            } else if (WIFSIGNALED(status)) {
                record->exit_code = 128 + WTERMSIG(status);
                record->exit_signal = WTERMSIG(status);
                if (record->stop_requested) {
                    record->state = CONTAINER_STOPPED;
                    strcpy(record->final_reason, "manual_stop");
                } else if (WTERMSIG(status) == SIGKILL) {
                    record->state = CONTAINER_KILLED;
                    strcpy(record->final_reason, "hard_limit_killed");
                } else {
                    record->state = CONTAINER_KILLED;
                    strcpy(record->final_reason, "signaled");
                }
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (record) {
            if (record->monitor_registered) {
                unregister_from_monitor(ctx->monitor_fd, record->id, record->host_pid);
                record->monitor_registered = 0;
            }
            if (record->producer_started) {
                pthread_join(record->producer_thread, NULL);
                record->producer_started = 0;
            }
            free(record->child_cfg);
            record->child_cfg = NULL;
            free(record->child_stack);
            record->child_stack = NULL;
        }
    }
}

static int stop_container(supervisor_ctx_t *ctx, const char *id, char *message, size_t message_size)
{
    container_record_t *record;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_locked(ctx, id);
    if (!record) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        safe_snprintf(message, message_size, "container '%s' not found\n", id);
        return 1;
    }

    if (is_container_finished(record->state)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        safe_snprintf(message, message_size, "container '%s' is already finished (%s)\n", id, state_to_string(record->state));
        return 0;
    }

    record->stop_requested = 1;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (kill(record->host_pid, SIGTERM) < 0) {
        safe_snprintf(message, message_size, "failed to signal container '%s': %s\n", id, strerror(errno));
        return 1;
    }

    safe_snprintf(message, message_size, "stop requested for container '%s' (pid=%d)\n", id, record->host_pid);
    return 0;
}

static void append_ps_line(char *buf,
                           size_t buf_size,
                           const char *id,
                           pid_t pid,
                           const char *state,
                           const char *reason,
                           unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes,
                           int nice_value,
                           int exit_code)
{
    size_t used = strlen(buf);

    if (used >= buf_size)
        return;

    snprintf(buf + used,
             buf_size - used,
             "%-10s %-8d %-10s %-18s %-8lu %-8lu %-6d %-6d\n",
             id,
             pid,
             state,
             reason,
             soft_limit_bytes >> 20,
             hard_limit_bytes >> 20,
             nice_value,
             exit_code);
}

static void format_ps(supervisor_ctx_t *ctx, char *message, size_t message_size)
{
    container_record_t *cur;

    safe_snprintf(message,
                  message_size,
                  "%-10s %-8s %-10s %-18s %-8s %-8s %-6s %-6s\n",
                  "ID",
                  "PID",
                  "STATE",
                  "REASON",
                  "SOFT",
                  "HARD",
                  "NICE",
                  "EXIT");

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        append_ps_line(message,
                       message_size,
                       cur->id,
                       cur->host_pid,
                       state_to_string(cur->state),
                       cur->final_reason,
                       cur->soft_limit_bytes,
                       cur->hard_limit_bytes,
                       cur->nice_value,
                       cur->exit_code);
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static int format_logs(supervisor_ctx_t *ctx, const char *id, char *message, size_t message_size)
{
    container_record_t *record;
    int fd;
    ssize_t nread;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_locked(ctx, id);
    if (!record) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        safe_snprintf(message, message_size, "container '%s' not found\n", id);
        return 1;
    }
    fd = open(record->log_path, O_RDONLY);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (fd < 0) {
        safe_snprintf(message, message_size, "log file for '%s' not available yet\n", id);
        return 1;
    }

    nread = read(fd, message, message_size - 1);
    close(fd);
    if (nread < 0) {
        safe_snprintf(message, message_size, "failed to read log for '%s': %s\n", id, strerror(errno));
        return 1;
    }

    message[nread] = '\0';
    return 0;
}

static int wait_for_container(supervisor_ctx_t *ctx, const char *id, char *message, size_t message_size)
{
    for (;;) {
        container_record_t *record;
        int finished = 0;
        int exit_status = 0;

        if (ctx->reap_requested)
            reap_children(ctx);

        pthread_mutex_lock(&ctx->metadata_lock);
        record = find_container_locked(ctx, id);
        if (record && is_container_finished(record->state)) {
            finished = 1;
            exit_status = record->exit_signal ? 128 + record->exit_signal : record->exit_code;
            safe_snprintf(message,
                          message_size,
                          "container=%s state=%s reason=%s exit_status=%d\n",
                          record->id,
                          state_to_string(record->state),
                          record->final_reason,
                          exit_status);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (finished)
            return exit_status;

        usleep(100000);
    }
}

static void shutdown_all_containers(supervisor_ctx_t *ctx)
{
    container_record_t *cur;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        if (!is_container_finished(cur->state)) {
            cur->stop_requested = 1;
            kill(cur->host_pid, SIGTERM);
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    sleep(1);

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        if (!is_container_finished(cur->state))
            kill(cur->host_pid, SIGKILL);
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void cleanup_container_records(supervisor_ctx_t *ctx)
{
    container_record_t *cur;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    ctx->containers = NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);

    while (cur) {
        container_record_t *next = cur->next;

        if (cur->producer_started)
            pthread_join(cur->producer_thread, NULL);
        free(cur->child_cfg);
        free(cur->child_stack);
        free(cur);
        cur = next;
    }
}

static void *client_thread_main(void *arg)
{
    client_task_t *task = arg;
    char message[CONTROL_MESSAGE_LEN];
    int status = 0;

    memset(message, 0, sizeof(message));

    switch (task->req.kind) {
    case CMD_START:
        status = start_container(task->ctx, &task->req, message, sizeof(message));
        send_response(task->client_fd, status, "%s", message);
        break;
    case CMD_RUN:
        status = start_container(task->ctx, &task->req, message, sizeof(message));
        if (status != 0) {
            send_response(task->client_fd, status, "%s", message);
            break;
        }
        status = wait_for_container(task->ctx, task->req.container_id, message, sizeof(message));
        send_response(task->client_fd, status, "%s", message);
        break;
    case CMD_PS:
        format_ps(task->ctx, message, sizeof(message));
        send_response(task->client_fd, 0, "%s", message);
        break;
    case CMD_LOGS:
        status = format_logs(task->ctx, task->req.container_id, message, sizeof(message));
        send_response(task->client_fd, status, "%s", message);
        break;
    case CMD_STOP:
        status = stop_container(task->ctx, task->req.container_id, message, sizeof(message));
        send_response(task->client_fd, status, "%s", message);
        break;
    default:
        send_response(task->client_fd, 1, "unsupported command\n");
        break;
    }

    close(task->client_fd);
    free(task);
    return NULL;
}

static int handle_client_connection(supervisor_ctx_t *ctx, int client_fd)
{
    client_task_t *task;
    pthread_t tid;
    ssize_t nread;

    task = calloc(1, sizeof(*task));
    if (!task) {
        close(client_fd);
        return -1;
    }

    nread = read(client_fd, &task->req, sizeof(task->req));
    if (nread != (ssize_t)sizeof(task->req)) {
        close(client_fd);
        free(task);
        return -1;
    }

    task->ctx = ctx;
    task->client_fd = client_fd;

    if (pthread_create(&tid, NULL, client_thread_main, task) != 0) {
        send_response(client_fd, 1, "failed to create client worker thread\n");
        close(client_fd);
        free(task);
        return -1;
    }

    pthread_detach(tid);
    return 0;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sigaction sa;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    strncpy(ctx.base_rootfs, rootfs, sizeof(ctx.base_rootfs) - 1);

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST)
        perror("mkdir logs");

    ctx.monitor_fd = open(DEVICE_PATH, O_RDWR);
    if (ctx.monitor_fd < 0)
        perror("open /dev/container_monitor");

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(ctx.server_fd, 16) < 0) {
        perror("bind/listen");
        close(ctx.server_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = supervisor_signal_handler;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    g_supervisor_ctx = &ctx;

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    printf("Supervisor listening on %s with base rootfs %s\n", CONTROL_PATH, rootfs);
    fflush(stdout);

    while (!ctx.should_stop) {
        struct pollfd pfd;
        int client_fd;

        if (ctx.reap_requested)
            reap_children(&ctx);

        pfd.fd = ctx.server_fd;
        pfd.events = POLLIN;
        rc = poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (rc == 0)
            continue;

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        handle_client_connection(&ctx, client_fd);
    }

    shutdown_all_containers(&ctx);
    printf("[supervisor] shutdown requested, waiting for children\n");
    fflush(stdout);
    ctx.reap_requested = 1;
    reap_children(&ctx);
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    cleanup_container_records(&ctx);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    printf("[supervisor] clean shutdown complete\n");
    fflush(stdout);
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    struct sockaddr_un addr;
    control_response_t resp;
    int fd;
    ssize_t nread;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    memset(&resp, 0, sizeof(resp));
    for (;;) {
        struct pollfd pfd;
        int prc;

        pfd.fd = fd;
        pfd.events = POLLIN;
        prc = poll(&pfd, 1, 200);
        if (prc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            close(fd);
            return 1;
        }

        if (g_client_forward_stop && req->kind == CMD_RUN) {
            control_request_t stop_req;
            memset(&stop_req, 0, sizeof(stop_req));
            stop_req.kind = CMD_STOP;
            copy_cstr(stop_req.container_id, sizeof(stop_req.container_id), req->container_id);
            g_client_forward_stop = 0;
            send_control_request(&stop_req);
        }

        if (prc == 0)
            continue;

        nread = read(fd, &resp, sizeof(resp));
        if (nread <= 0) {
            fprintf(stderr, "Supervisor closed connection without response\n");
            close(fd);
            return 1;
        }
        break;
    }

    if (resp.message[0] != '\0')
        printf("%s", resp.message);
    close(fd);
    return resp.status == 0 ? 0 : resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    copy_cstr(req.container_id, sizeof(req.container_id), argv[2]);
    copy_cstr(req.rootfs, sizeof(req.rootfs), argv[3]);
    copy_cstr(req.command, sizeof(req.command), argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    copy_cstr(req.container_id, sizeof(req.container_id), argv[2]);
    copy_cstr(req.rootfs, sizeof(req.rootfs), argv[3]);
    copy_cstr(req.command, sizeof(req.command), argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    copy_cstr(req.container_id, sizeof(req.container_id), argv[2]);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    copy_cstr(req.container_id, sizeof(req.container_id), argv[2]);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    struct sigaction sa;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = client_signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
