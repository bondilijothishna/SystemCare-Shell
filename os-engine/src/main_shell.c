#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Print the commands that are handled directly by this shell. */
static void print_help(void)
{
    puts("Built-in commands:");
    puts("  help       Show this list of commands");
    puts("  pwd        Print the current directory");
    puts("  cd [DIR]   Change directory (defaults to HOME)");
    puts("  exit       Exit the shell");
    puts("Use command1 | command2 to pipe two external commands.");
    puts("Add & after an external command to run it in the background.");
    puts("Other commands run as Linux programs, for example: ls, date, whoami.");
}

/* Change directory in this process so the new directory remains active. */
static void change_directory(char *const arguments[])
{
    const char *directory = arguments[1];

    if (arguments[1] != NULL && arguments[2] != NULL) {
        fprintf(stderr, "cd: expected at most one directory\n");
        return;
    }

    if (directory == NULL) {
        directory = getenv("HOME");
        if (directory == NULL) {
            fprintf(stderr, "cd: HOME is not set\n");
            return;
        }
    }

    if (chdir(directory) == -1) {
        perror("cd");
    }
}

/* Reap finished children whenever the operating system reports SIGCHLD. */
static void reap_finished_children(int signal_number)
{
    int saved_errno = errno;

    (void)signal_number;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* Keep reaping in case more than one child finished. */
    }
    errno = saved_errno;
}

/* Wait for one child; the SIGCHLD handler may already have reaped it. */
static void wait_for_child(pid_t child)
{
    while (waitpid(child, NULL, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        if (errno != ECHILD) {
            perror("waitpid");
        }
        return;
    }
}

/* Start one external program, optionally returning immediately. */
static void run_external_command(char *const arguments[], int background)
{
    pid_t child;

    fflush(NULL);
    child = fork();
    if (child == -1) {
        perror("fork");
        return;
    }

    if (child == 0) {
        execvp(arguments[0], arguments);
        perror(arguments[0]);
        _exit(127);
    }

    if (background) {
        printf("Started background process %ld\n", (long)child);
        fflush(stdout);
    } else {
        wait_for_child(child);
    }
}

/* Connect two external programs with a pipe. */
static void run_pipeline(char *const left_arguments[],
                         char *const right_arguments[], int background)
{
    int file_descriptors[2];
    pid_t left_child;
    pid_t right_child;

    if (pipe(file_descriptors) == -1) {
        perror("pipe");
        return;
    }

    fflush(NULL);
    left_child = fork();
    if (left_child == -1) {
        perror("fork");
        close(file_descriptors[0]);
        close(file_descriptors[1]);
        return;
    }

    if (left_child == 0) {
        close(file_descriptors[0]);
        if (dup2(file_descriptors[1], STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(EXIT_FAILURE);
        }
        close(file_descriptors[1]);
        execvp(left_arguments[0], left_arguments);
        perror(left_arguments[0]);
        _exit(127);
    }

    right_child = fork();
    if (right_child == -1) {
        perror("fork");
        close(file_descriptors[0]);
        close(file_descriptors[1]);
        wait_for_child(left_child);
        return;
    }

    if (right_child == 0) {
        close(file_descriptors[1]);
        if (dup2(file_descriptors[0], STDIN_FILENO) == -1) {
            perror("dup2");
            _exit(EXIT_FAILURE);
        }
        close(file_descriptors[0]);
        execvp(right_arguments[0], right_arguments);
        perror(right_arguments[0]);
        _exit(127);
    }

    close(file_descriptors[0]);
    close(file_descriptors[1]);
    if (background) {
        printf("Started background pipeline (%ld, %ld)\n",
               (long)left_child, (long)right_child);
        fflush(stdout);
    } else {
        wait_for_child(left_child);
        wait_for_child(right_child);
    }
}

static int is_builtin(const char *command)
{
    return strcmp(command, "help") == 0 || strcmp(command, "pwd") == 0 ||
           strcmp(command, "cd") == 0 || strcmp(command, "exit") == 0;
}

int main(void)
{
    char *line = NULL;
    size_t line_capacity = 0;
    int show_prompt = isatty(STDIN_FILENO);
    struct sigaction child_action;

    memset(&child_action, 0, sizeof(child_action));
    child_action.sa_handler = reap_finished_children;
    child_action.sa_flags = SA_RESTART;
    sigemptyset(&child_action.sa_mask);
    if (sigaction(SIGCHLD, &child_action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    for (;;) {
        ssize_t line_length;
        char **arguments;
        char *token;
        char *save_pointer;
        size_t argument_count = 0;
        size_t argument_capacity;
        size_t pipe_index = 0;
        int has_pipe = 0;
        int background = 0;
        int invalid_syntax = 0;

        if (show_prompt) {
            fputs("scsh> ", stdout);
            fflush(stdout);
        }

        line_length = getline(&line, &line_capacity, stdin);
        if (line_length == -1) {
            if (ferror(stdin)) {
                perror("getline");
            }
            break;
        }

        /* A line cannot contain more tokens than half its character count. */
        argument_capacity = (size_t)line_length / 2 + 2;
        arguments = malloc(argument_capacity * sizeof(*arguments));
        if (arguments == NULL) {
            perror("malloc");
            free(line);
            return EXIT_FAILURE;
        }

        for (token = strtok_r(line, " \t\r\n", &save_pointer);
             token != NULL;
             token = strtok_r(NULL, " \t\r\n", &save_pointer)) {
            arguments[argument_count++] = token;
        }
        arguments[argument_count] = NULL;

        /* Ignore empty lines and lines containing only whitespace. */
        if (argument_count == 0) {
            free(arguments);
            continue;
        }

        /* Operators must be separate tokens, and '&' must be at the end. */
        for (size_t index = 0; index < argument_count; index++) {
            if ((strchr(arguments[index], '|') != NULL &&
                 strcmp(arguments[index], "|") != 0) ||
                (strchr(arguments[index], '&') != NULL &&
                 strcmp(arguments[index], "&") != 0)) {
                invalid_syntax = 1;
            }
        }

        for (size_t index = 0; index < argument_count; index++) {
            if (strcmp(arguments[index], "&") == 0) {
                if (index != argument_count - 1) {
                    invalid_syntax = 1;
                } else {
                    background = 1;
                    arguments[index] = NULL;
                    argument_count--;
                }
                break;
            }
        }

        /* Find one pipe and split the argument list into two commands. */
        for (size_t index = 0; index < argument_count; index++) {
            if (strcmp(arguments[index], "|") == 0) {
                if (has_pipe || index == 0 || index + 1 == argument_count) {
                    invalid_syntax = 1;
                } else {
                    has_pipe = 1;
                    pipe_index = index;
                    arguments[index] = NULL;
                }
            }
        }

        if (invalid_syntax || argument_count == 0) {
            fprintf(stderr, "scsh: invalid command syntax\n");
            free(arguments);
            continue;
        }

        if (has_pipe &&
            (is_builtin(arguments[0]) || is_builtin(arguments[pipe_index + 1]))) {
            fprintf(stderr, "scsh: built-in commands cannot be used in a pipe\n");
        } else if (!has_pipe && background && is_builtin(arguments[0])) {
            fprintf(stderr, "scsh: built-in commands cannot run in the background\n");
        } else if (has_pipe) {
            run_pipeline(arguments, &arguments[pipe_index + 1], background);
        } else if (strcmp(arguments[0], "help") == 0) {
            print_help();
        } else if (strcmp(arguments[0], "pwd") == 0) {
            char working_directory[PATH_MAX];

            if (getcwd(working_directory, sizeof(working_directory)) == NULL) {
                perror("pwd");
            } else {
                puts(working_directory);
            }
        } else if (strcmp(arguments[0], "cd") == 0) {
            change_directory(arguments);
        } else if (strcmp(arguments[0], "exit") == 0) {
            free(arguments);
            break;
        } else {
            run_external_command(arguments, background);
        }

        free(arguments);
    }

    free(line);
    return EXIT_SUCCESS;
}

