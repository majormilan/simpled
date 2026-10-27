#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

#include "config.h"
#include "util.h"

#define SOCKET_PATH "/run/simpled.sock"
#define SERVICE_DIR "/etc/simpled/enabled"
#define LOG_DIR "/var/log/simpled"

static Service *services = NULL;
static int running = 1;

void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        running = 0;
    }
}

void reap_children() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (Service *s = services; s; s = s->next) {
            if (s->pid == pid) {
                s->pid = 0;
                if (WIFEXITED(status)) {
                    s->exit_status = WEXITSTATUS(status);
                    log_info("Service %s exited with status %d", s->name, s->exit_status);
                } else if (WIFSIGNALED(status)) {
                    s->exit_status = -WTERMSIG(status);
                    log_info("Service %s killed by signal %d", s->name, WTERMSIG(status));
                }

                if (s->type == TYPE_SIMPLE && s->restart == RESTART_ALWAYS && running) {
                    s->state = STATE_RESTARTING;
                    log_info("Restarting service %s", s->name);
                } else {
                    s->state = s->exit_status == 0 ? STATE_STOPPED : STATE_FAILED;
                }
                break;
            }
        }
    }
}

static char **split_args(const char *cmd, int *argc) {
    int n = 0;
    char **argv = NULL;
    char *buf = strdup(cmd);
    char *p = strtok(buf, " ");
    while (p) {
        argv = realloc(argv, sizeof(char *) * (n + 1));
        argv[n++] = strdup(p);
        p = strtok(NULL, " ");
    }
    argv = realloc(argv, sizeof(char *) * (n + 1));
    argv[n] = NULL;
    *argc = n;
    free(buf);
    return argv;
}

void spawn_service(Service *s) {
    if (s->pid > 0) return;

    log_info("Starting service: %s", s->name);

    pid_t pid = fork();
    if (pid < 0) {
        log_err("fork failed for %s: %s", s->name, strerror(errno));
        s->state = STATE_FAILED;
        return;
    }

    if (pid == 0) {
        // Child
        // 1. Logging redirection
        char log_path[1024];
        mkdir(LOG_DIR, 0755);
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, s->name);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        // 2. Environment
        for (size_t i = 0; i < s->env_count; i++) {
            putenv(s->env[i]);
        }

        // 3. User/Group
        if (s->group) {
            struct group *gr = getgrnam(s->group);
            if (gr) setgid(gr->gr_gid);
        }
        if (s->user) {
            struct passwd *pw = getpwnam(s->user);
            if (pw) setuid(pw->pw_uid);
        }

        // 4. Exec
        int argc;
        char **argv = split_args(s->exec_start, &argc);
        execvp(argv[0], argv);

        // If execvp returns, it failed
        perror("execvp");
        exit(1);
    }

    // Parent
    s->pid = pid;
    s->state = STATE_RUNNING;
}

void load_services() {
    DIR *dir = opendir(SERVICE_DIR);
    if (!dir) {
        log_err("Could not open service directory %s: %s", SERVICE_DIR, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", SERVICE_DIR, entry->d_name);

        char name[256];
        strncpy(name, entry->d_name, sizeof(name));
        char *dot = strrchr(name, '.');
        if (dot && strcmp(dot, ".service") == 0) *dot = '\0';

        Service *s = parse_service_file(path, name);
        if (s) {
            s->next = services;
            services = s;
            log_info("Loaded service: %s", s->name);
        }
    }
    closedir(dir);
}

Service *find_service(const char *name) {
    for (Service *s = services; s; s = s->next) {
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

void handle_command(int fd) {
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    char *cmd = strtok(buf, " \n");
    char *arg = strtok(NULL, " \n");

    if (!cmd) return;

    if (strcmp(cmd, "START") == 0 && arg) {
        Service *s = find_service(arg);
        if (s) {
            spawn_service(s);
            write(fd, "OK\n", 3);
        } else {
            write(fd, "ERROR: Service not found\n", 25);
        }
    } else if (strcmp(cmd, "STOP") == 0 && arg) {
        Service *s = find_service(arg);
        if (s) {
            stop_service(s);
            write(fd, "OK\n", 3);
        } else {
            write(fd, "ERROR: Service not found\n", 25);
        }
    } else if (strcmp(cmd, "LIST") == 0) {
        for (Service *s = services; s; s = s->next) {
            char line[256];
            snprintf(line, sizeof(line), "%s [%d]\n", s->name, s->state);
            write(fd, line, strlen(line));
        }
    } else {
        write(fd, "ERROR: Unknown command\n", 23);
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGCHLD, SIG_IGN); // We'll use waitpid in a loop or handle it specifically
    // Actually, let's handle SIGCHLD properly to avoid zombies and react quickly.
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Re-setup SIGCHLD for reap_children
    struct sigaction sa_chld;
    sa_chld.sa_handler = (void (*)(int))reap_children;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    load_services();

    log_info("simpled started");

    while (running) {
        // Main loop logic:
        // 1. Check for services that need restarting
        for (Service *s = services; s; s = s->next) {
            if (s->state == STATE_RESTARTING) {
                // spawn_service(s);
            }
        }

        // 2. Handle socket commands (TBD)
        sleep(1); // Placeholder
    }

    log_info("simpled stopping");
    return 0;
}
