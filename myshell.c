#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>

#define MAX_ARGS 128
#define MAX_CMDS 64

static char *g_line = NULL;

/**
 * Strips leading and trailing whitespace characters (' ', '\t', '\r', '\n').
 * In-place modification using memmove.
 */
static void trim(char *s)
{
    if (!s)
    {
        return;
    }
    char *start = s;
    while (*start && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n'))
    {
        start++;
    }
    char *end = s + strlen(s) - 1;
    while (end >= start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
    {
        *end-- = '\0';
    }
    if (start != s)
    {
        memmove(s, start, strlen(start) + 1);
    }
}

/**
 * Returns 1 if s is NULL, empty, or consists only of whitespace characters.
 * Returns 0 otherwise.
 */
static int is_empty(const char *s)
{
    if (!s)
    {
        return 1;
    }
    for (int i = 0; s[i] != '\0'; i++)
    {
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
        {
            return 0;
        }
    }
    return 1;
}

/**
 * Signal handler for SIGINT (Ctrl+C) and SIGTSTP (Ctrl+Z).
 * The shell process ignores terminal interruption and suspension so that it
 * continues running until the 'exit' command is issued.
 */
static void sig_handler(int sig)
{
    (void)sig;
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
}

/**
 * Resets signal handling in child processes to SIG_DFL so that child commands
 * respond naturally to Ctrl+C and Ctrl+Z.
 */
static void reset_child_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
}

static void cleanup_main_line(void)
{
    if (g_line)
    {
        free(g_line);
        g_line = NULL;
    }
}

/**
 * Tokenizes a command string into arguments using strsep().
 * Populates argv array with pointers into cmd_str.
 * Returns argument count, or -1 if the number of arguments exceeds max_args - 1.
 */
static int parse_arguments(char *cmd_str, char **argv, int max_args)
{
    int argc = 0;
    char *tok;
    char *ptr = cmd_str;

    while ((tok = strsep(&ptr, " \t")) != NULL)
    {
        trim(tok);
        if (strlen(tok) > 0)
        {
            if (argc < max_args - 1)
            {
                argv[argc++] = tok;
            }
            else
            {
                return -1;
            }
        }
    }
    argv[argc] = NULL;
    return argc;
}

static int is_builtin(const char *cmd)
{
    if (!cmd)
    {
        return 0;
    }
    return (strcmp(cmd, "exit") == 0 || strcmp(cmd, "cd") == 0);
}

/**
 * Executes shell built-in commands ('cd' and 'exit').
 * Returns 0 on success, 1 on failure.
 */
static int execute_builtin(char **argv, int argc)
{
    if (strcmp(argv[0], "exit") == 0)
    {
        if (argc == 1)
        {
            printf("Exiting shell...\n");
            fflush(stdout);
            cleanup_main_line();
            exit(0);
        }
        else
        {
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            return 1;
        }
    }
    else if (strcmp(argv[0], "cd") == 0)
    {
        if (argc == 1)
        {
            const char *home = getenv("HOME");
            if (!home || chdir(home) != 0)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                return 1;
            }
        }
        else if (argc == 2)
        {
            if (chdir(argv[1]) != 0)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                return 1;
            }
        }
        else
        {
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            return 1;
        }
        return 0;
    }
    return -1;
}

/**
 * Executes a single command.
 */
static void execute_simple(char *line)
{
    char *line_copy = strdup(line);
    if (!line_copy)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        return;
    }

    char *argv[MAX_ARGS];
    int argc = parse_arguments(line_copy, argv, MAX_ARGS);

    if (argc < 0)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(line_copy);
        return;
    }

    if (argc == 0)
    {
        free(line_copy);
        return;
    }

    if (is_builtin(argv[0]))
    {
        execute_builtin(argv, argc);
        free(line_copy);
        return;
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        reset_child_signals();
        execvp(argv[0], argv);
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(line_copy);
        cleanup_main_line();
        exit(1);
    }
    else if (pid > 0)
    {
        int status;
        while (waitpid(pid, &status, 0) < 0)
        {
            if (errno != EINTR)
            {
                break;
            }
        }
    }
    else
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
    }

    free(line_copy);
}

/**
 * Executes sequential commands separated by '##'.
 * Waits for each command to complete before proceeding to the next.
 */
static void execute_sequential(char *input)
{
    char *cmds[MAX_CMDS];
    int n = 0;
    char *input_copy = strdup(input);
    if (!input_copy)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        return;
    }

    char *curr = input_copy;
    while (curr != NULL)
    {
        char *next = strstr(curr, "##");
        if (next != NULL)
        {
            *next = '\0';
            trim(curr);
            if (is_empty(curr) || n >= MAX_CMDS)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                free(input_copy);
                return;
            }
            cmds[n++] = curr;
            curr = next + 2;
        }
        else
        {
            trim(curr);
            if (is_empty(curr) || n >= MAX_CMDS)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                free(input_copy);
                return;
            }
            cmds[n++] = curr;
            curr = NULL;
        }
    }

    if (n < 2)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(input_copy);
        return;
    }

    for (int i = 0; i < n; i++)
    {
        char *cmd_copy = strdup(cmds[i]);
        if (!cmd_copy)
        {
            continue;
        }

        char *argv[MAX_ARGS];
        int argc = parse_arguments(cmd_copy, argv, MAX_ARGS);

        if (argc < 0)
        {
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            free(cmd_copy);
            continue;
        }

        if (argc > 0)
        {
            if (is_builtin(argv[0]))
            {
                execute_builtin(argv, argc);
            }
            else
            {
                pid_t pid = fork();
                if (pid == 0)
                {
                    reset_child_signals();
                    execvp(argv[0], argv);
                    printf("Shell: Incorrect command\n");
                    fflush(stdout);
                    free(cmd_copy);
                    free(input_copy);
                    cleanup_main_line();
                    exit(1);
                }
                else if (pid > 0)
                {
                    int status;
                    while (waitpid(pid, &status, 0) < 0)
                    {
                        if (errno != EINTR)
                        {
                            break;
                        }
                    }
                }
                else
                {
                    printf("Shell: Incorrect command\n");
                    fflush(stdout);
                }
            }
        }
        free(cmd_copy);
    }

    free(input_copy);
}

/**
 * Executes parallel commands separated by '&&'.
 * Spawns all children concurrently and waits for all of them to terminate.
 */
static void execute_parallel(char *input)
{
    char *cmds[MAX_CMDS];
    int n = 0;
    char *input_copy = strdup(input);
    if (!input_copy)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        return;
    }

    char *curr = input_copy;
    while (curr != NULL)
    {
        char *next = strstr(curr, "&&");
        if (next != NULL)
        {
            *next = '\0';
            trim(curr);
            if (is_empty(curr) || n >= MAX_CMDS)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                free(input_copy);
                return;
            }
            cmds[n++] = curr;
            curr = next + 2;
        }
        else
        {
            trim(curr);
            if (is_empty(curr) || n >= MAX_CMDS)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                free(input_copy);
                return;
            }
            cmds[n++] = curr;
            curr = NULL;
        }
    }

    if (n < 2)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(input_copy);
        return;
    }

    pid_t pids[MAX_CMDS];

    for (int i = 0; i < n; i++)
    {
        char *cmd_copy = strdup(cmds[i]);
        if (!cmd_copy)
        {
            pids[i] = -1;
            continue;
        }

        char *argv[MAX_ARGS];
        int argc = parse_arguments(cmd_copy, argv, MAX_ARGS);

        if (argc <= 0)
        {
            if (argc < 0)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
            }
            free(cmd_copy);
            pids[i] = -1;
            continue;
        }

        if (is_builtin(argv[0]))
        {
            /* Run built-in in child process during parallel execution to prevent race conditions */
            pids[i] = fork();
            if (pids[i] == 0)
            {
                reset_child_signals();
                execute_builtin(argv, argc);
                free(cmd_copy);
                free(input_copy);
                cleanup_main_line();
                exit(0);
            }
            else if (pids[i] < 0)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                pids[i] = -1;
            }
        }
        else
        {
            pids[i] = fork();
            if (pids[i] == 0)
            {
                reset_child_signals();
                execvp(argv[0], argv);
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                free(cmd_copy);
                free(input_copy);
                cleanup_main_line();
                exit(1);
            }
            else if (pids[i] < 0)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                pids[i] = -1;
            }
        }
        free(cmd_copy);
    }

    /* Wait for ALL spawned parallel children */
    for (int i = 0; i < n; i++)
    {
        if (pids[i] > 0)
        {
            int status;
            while (waitpid(pids[i], &status, 0) < 0)
            {
                if (errno != EINTR)
                {
                    break;
                }
            }
        }
    }

    free(input_copy);
}

/**
 * Executes a command with standard output redirected using '>'.
 */
static void execute_redirection(char *input)
{
    char *input_copy = strdup(input);
    if (!input_copy)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        return;
    }

    char *gt = strchr(input_copy, '>');
    if (!gt)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(input_copy);
        return;
    }

    /* Exactly one '>' operator is supported */
    if (strchr(gt + 1, '>') != NULL)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(input_copy);
        return;
    }

    *gt = '\0';
    char *left = input_copy;
    char *right = gt + 1;
    trim(left);
    trim(right);

    if (is_empty(left) || is_empty(right))
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(input_copy);
        return;
    }

    /* Validate destination file name: exactly one token */
    char *right_copy = strdup(right);
    if (!right_copy)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(input_copy);
        return;
    }

    char *target_argv[MAX_ARGS];
    int target_argc = parse_arguments(right_copy, target_argv, MAX_ARGS);
    if (target_argc != 1)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(right_copy);
        free(input_copy);
        return;
    }
    char target_file[PATH_MAX];
    strncpy(target_file, target_argv[0], sizeof(target_file) - 1);
    target_file[sizeof(target_file) - 1] = '\0';
    free(right_copy);

    /* Parse command to be executed */
    char *left_copy = strdup(left);
    if (!left_copy)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(input_copy);
        return;
    }

    char *argv[MAX_ARGS];
    int argc = parse_arguments(left_copy, argv, MAX_ARGS);
    if (argc <= 0 || is_builtin(argv[0]))
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(left_copy);
        free(input_copy);
        return;
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        reset_child_signals();

        /* Duplicate stdout descriptor so errors can still reach the terminal if execvp fails */
        int saved_stdout = dup(STDOUT_FILENO);
        if (saved_stdout >= 0)
        {
            fcntl(saved_stdout, F_SETFD, FD_CLOEXEC);
        }

        int fd = open(target_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0)
        {
            if (saved_stdout >= 0)
            {
                close(saved_stdout);
            }
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            free(left_copy);
            free(input_copy);
            cleanup_main_line();
            exit(1);
        }

        if (dup2(fd, STDOUT_FILENO) < 0)
        {
            close(fd);
            if (saved_stdout >= 0)
            {
                dup2(saved_stdout, STDOUT_FILENO);
                close(saved_stdout);
            }
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            free(left_copy);
            free(input_copy);
            cleanup_main_line();
            exit(1);
        }
        close(fd);

        execvp(argv[0], argv);

        /* On execvp failure, restore terminal stdout before printing error */
        if (saved_stdout >= 0)
        {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(left_copy);
        free(input_copy);
        cleanup_main_line();
        exit(1);
    }
    else if (pid > 0)
    {
        int status;
        while (waitpid(pid, &status, 0) < 0)
        {
            if (errno != EINTR)
            {
                break;
            }
        }
    }
    else
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
    }

    free(left_copy);
    free(input_copy);
}

/**
 * Executes a multi-stage command pipeline connected via '|'.
 * Correctly configures pipes, closes unused file descriptors, and reaps all children.
 */
static void execute_pipeline(char *input)
{
    char *cmds[MAX_CMDS];
    int n = 0;
    char *input_copy = strdup(input);
    if (!input_copy)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        return;
    }

    char *curr = input_copy;
    while (curr != NULL)
    {
        char *next = strchr(curr, '|');
        if (next != NULL)
        {
            *next = '\0';
            trim(curr);
            if (is_empty(curr) || n >= MAX_CMDS)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                free(input_copy);
                return;
            }
            cmds[n++] = curr;
            curr = next + 1;
        }
        else
        {
            trim(curr);
            if (is_empty(curr) || n >= MAX_CMDS)
            {
                printf("Shell: Incorrect command\n");
                fflush(stdout);
                free(input_copy);
                return;
            }
            cmds[n++] = curr;
            curr = NULL;
        }
    }

    if (n < 2)
    {
        printf("Shell: Incorrect command\n");
        fflush(stdout);
        free(input_copy);
        return;
    }

    /* Validate that all stages have valid arguments before creating pipes or processes */
    for (int i = 0; i < n; i++)
    {
        char *stage_copy = strdup(cmds[i]);
        if (!stage_copy)
        {
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            free(input_copy);
            return;
        }
        char *temp_argv[MAX_ARGS];
        int temp_argc = parse_arguments(stage_copy, temp_argv, MAX_ARGS);
        free(stage_copy);
        if (temp_argc <= 0)
        {
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            free(input_copy);
            return;
        }
    }

    /* Create n - 1 pipes */
    int pipefds[2 * (MAX_CMDS - 1)];
    for (int i = 0; i < n - 1; i++)
    {
        if (pipe(pipefds + i * 2) < 0)
        {
            for (int j = 0; j < i * 2; j++)
            {
                close(pipefds[j]);
            }
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            free(input_copy);
            return;
        }
    }

    pid_t pids[MAX_CMDS];
    int spawned = 0;

    for (int i = 0; i < n; i++)
    {
        pids[i] = fork();
        if (pids[i] == 0)
        {
            reset_child_signals();

            /* If not last stage, save terminal stdout to report execvp errors to user */
            int saved_stdout = -1;
            if (i < n - 1)
            {
                saved_stdout = dup(STDOUT_FILENO);
                if (saved_stdout >= 0)
                {
                    fcntl(saved_stdout, F_SETFD, FD_CLOEXEC);
                }
            }

            /* If not first stage, redirect stdin from read end of previous pipe */
            if (i > 0)
            {
                if (dup2(pipefds[(i - 1) * 2], STDIN_FILENO) < 0)
                {
                    if (saved_stdout >= 0) close(saved_stdout);
                    exit(1);
                }
            }
            /* If not last stage, redirect stdout to write end of current pipe */
            if (i < n - 1)
            {
                if (dup2(pipefds[i * 2 + 1], STDOUT_FILENO) < 0)
                {
                    if (saved_stdout >= 0) close(saved_stdout);
                    exit(1);
                }
            }

            /* Close all pipe file descriptors in child */
            for (int j = 0; j < 2 * (n - 1); j++)
            {
                close(pipefds[j]);
            }

            char *cmd_copy = strdup(cmds[i]);
            if (!cmd_copy)
            {
                if (saved_stdout >= 0) close(saved_stdout);
                exit(1);
            }
            char *argv[MAX_ARGS];
            int argc = parse_arguments(cmd_copy, argv, MAX_ARGS);

            if (is_builtin(argv[0]))
            {
                execute_builtin(argv, argc);
                if (saved_stdout >= 0) close(saved_stdout);
                free(cmd_copy);
                free(input_copy);
                cleanup_main_line();
                exit(0);
            }

            execvp(argv[0], argv);

            /* On execvp failure, restore terminal stdout before printing error */
            if (saved_stdout >= 0)
            {
                dup2(saved_stdout, STDOUT_FILENO);
                close(saved_stdout);
            }
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            free(cmd_copy);
            free(input_copy);
            cleanup_main_line();
            exit(1);
        }
        else if (pids[i] < 0)
        {
            printf("Shell: Incorrect command\n");
            fflush(stdout);
            break;
        }
        else
        {
            spawned++;
        }
    }

    /* Parent must close ALL pipe descriptors so reading children receive EOF */
    for (int j = 0; j < 2 * (n - 1); j++)
    {
        close(pipefds[j]);
    }

    /* Wait for all spawned pipeline children */
    for (int i = 0; i < spawned; i++)
    {
        if (pids[i] > 0)
        {
            int status;
            while (waitpid(pids[i], &status, 0) < 0)
            {
                if (errno != EINTR)
                {
                    break;
                }
            }
        }
    }

    free(input_copy);
}

/**
 * Dispatches an input line based on shell syntax and operators.
 */
static void execute_line(char *line)
{
    trim(line);
    if (is_empty(line))
    {
        return;
    }

    if (strstr(line, "&&"))
    {
        execute_parallel(line);
    }
    else if (strstr(line, "##"))
    {
        execute_sequential(line);
    }
    else if (strchr(line, '|'))
    {
        execute_pipeline(line);
    }
    else if (strchr(line, '>'))
    {
        execute_redirection(line);
    }
    else
    {
        execute_simple(line);
    }
}

int main(void)
{
    setup_signals();
    atexit(cleanup_main_line);

    size_t cap = 0;

    while (1)
    {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)))
        {
            printf("%s$", cwd);
        }
        else
        {
            printf("$");
        }
        fflush(stdout);

        ssize_t nread = getline(&g_line, &cap, stdin);
        if (nread < 0)
        {
            break;
        }

        /* Strip trailing newline and carriage return */
        while (nread > 0 && (g_line[nread - 1] == '\n' || g_line[nread - 1] == '\r'))
        {
            g_line[--nread] = '\0';
        }

        if (is_empty(g_line))
        {
            continue;
        }

        execute_line(g_line);
    }

    cleanup_main_line();
    return 0;
}
