#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_NODES 10
#define MAX_PIPES 10
#define MAX_NAME_LEN 50
#define MAX_CMD_LEN 100

typedef struct {
    char name[MAX_NAME_LEN];
    char command[MAX_CMD_LEN];
} Node;

typedef struct {
    char name[MAX_NAME_LEN];
    char from[MAX_NAME_LEN];
    char to[MAX_NAME_LEN];
} Pipe;

Node nodes[MAX_NODES];
int node_count = 0;

Pipe pipes[MAX_PIPES];
int pipe_count = 0;

void parse_flow_file(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Error opening .flow file");
        exit(EXIT_FAILURE);
    }

    char line[256];
    char current_section[MAX_NAME_LEN] = "";
    while (fgets(line, sizeof(line), file)) {
        char *pos;
        // Remove newline character
        if ((pos = strchr(line, '\n')) != NULL) {
            *pos = '\0';
        }
        // Skip empty lines
        if (strlen(line) == 0) {
            continue;
        }
        // Parse key and value
        char *equal_sign = strchr(line, '=');
        if (equal_sign == NULL) {
            continue;
        }
        *equal_sign = '\0';
        char *key = line;
        char *value = equal_sign + 1;

        if (strcmp(key, "node") == 0) {
            strncpy(nodes[node_count].name, value, MAX_NAME_LEN);
            // Initialize command to empty string
            nodes[node_count].command[0] = '\0';
            strcpy(current_section, "node");
        } else if (strcmp(key, "command") == 0 && strcmp(current_section, "node") == 0) {
            strncpy(nodes[node_count].command, value, MAX_CMD_LEN);
            node_count++;
            strcpy(current_section, "");
        } else if (strcmp(key, "pipe") == 0) {
            strncpy(pipes[pipe_count].name, value, MAX_NAME_LEN);
            strcpy(current_section, "pipe");
        } else if (strcmp(key, "from") == 0 && strcmp(current_section, "pipe") == 0) {
            strncpy(pipes[pipe_count].from, value, MAX_NAME_LEN);
        } else if (strcmp(key, "to") == 0 && strcmp(current_section, "pipe") == 0) {
            strncpy(pipes[pipe_count].to, value, MAX_NAME_LEN);
            pipe_count++;
            strcpy(current_section, "");
        }
    }
    fclose(file);
}

Node* find_node(const char *name) {
    for (int i = 0; i < node_count; i++) {
        if (strcmp(nodes[i].name, name) == 0) {
            return &nodes[i];
        }
    }
    return NULL;
}

Pipe* find_pipe(const char *name) {
    for (int i = 0; i < pipe_count; i++) {
        if (strcmp(pipes[i].name, name) == 0) {
            return &pipes[i];
        }
    }
    return NULL;
}

void execute_pipe(Pipe *flow_pipe) {
    Node *from_node = find_node(flow_pipe->from);
    Node *to_node = find_node(flow_pipe->to);

    if (from_node == NULL || to_node == NULL) {
        fprintf(stderr, "Error: Invalid node names in pipe\n");
        exit(EXIT_FAILURE);
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe failed");
        exit(EXIT_FAILURE);
    }

    pid_t pid1 = fork();
    if (pid1 == -1) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }

    if (pid1 == 0) {
        // First child process (from_node)
        close(pipefd[0]); // Close unused read end
        dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe write end
        close(pipefd[1]); // Close original write end

        // Execute command
        execlp("/bin/sh", "sh", "-c", from_node->command, (char *)NULL);
        perror("execlp failed");
        exit(EXIT_FAILURE);
    }

    pid_t pid2 = fork();
    if (pid2 == -1) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }

    if (pid2 == 0) {
        // Second child process (to_node)
        close(pipefd[1]); // Close unused write end
        dup2(pipefd[0], STDIN_FILENO); // Redirect stdin to pipe read end
        close(pipefd[0]); // Close original read end

        // Execute command
        execlp("/bin/sh", "sh", "-c", to_node->command, (char *)NULL);
        perror("execlp failed");
        exit(EXIT_FAILURE);
    }

    // Parent process
    close(pipefd[0]);
    close(pipefd[1]);

    waitpid(pid1, NULL, 0);
    waitpid(pid2, NULL, 0);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: ./flow <filename.flow> <action>\n");
        exit(EXIT_FAILURE);
    }

    const char *flow_filename = argv[1];
    const char *action_name = argv[2];

    parse_flow_file(flow_filename);

    Pipe *action_pipe = find_pipe(action_name);
    if (action_pipe != NULL) {
        execute_pipe(action_pipe);
    } else {
        fprintf(stderr, "Error: Action '%s' not found\n", action_name);
        exit(EXIT_FAILURE);
    }

    return 0;
}
