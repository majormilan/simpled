#ifndef CONFIG_H
#define CONFIG_H

#include <sys/types.h>

typedef enum {
    TYPE_SIMPLE,
    TYPE_ONESHOT
} ServiceType;

typedef enum {
    RESTART_NO,
    RESTART_ALWAYS
} RestartPolicy;

typedef enum {
    STATE_STOPPED,
    STATE_STARTING,
    STATE_RUNNING,
    STATE_RESTARTING,
    STATE_FAILED
} ServiceState;

typedef struct Service {
    char *name;
    char *description;
    char *requires; // For simplicity, a comma-separated list or just one for now
    char *exec_start;
    char *user;
    char *group;
    char **env;
    size_t env_count;
    ServiceType type;
    RestartPolicy restart;
    
    // Runtime state
    pid_t pid;
    ServiceState state;
    int exit_status;
    
    struct Service *next;
} Service;

Service *parse_service_file(const char *path, const char *name);
void free_service(Service *s);

#endif
