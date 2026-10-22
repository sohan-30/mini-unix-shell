# Mini Unix Shell (`myshell`)

A lightweight, robust Unix command interpreter implemented in C using core POSIX operating system calls (`fork`, `execvp`, `waitpid`, `pipe`, `dup2`, `chdir`, `sigaction`). Designed and engineered to production systems-programming standards for systems and hardware-software engineering applications.

---

## 1. Project Overview

`myshell` is an interactive command interpreter that processes user commands, manages process hierarchies, configures inter-process communication (IPC) channels, handles asynchronous terminal signals, and enforces strict POSIX file-descriptor semantics.

### Core Systems Engineering Highlights
- **Process Management:** Manages process creation, execution, and reaping using `fork()`, `execvp()`, and `waitpid()`. Handles interrupted system calls (`EINTR`) and prevents zombie process accumulation.
- **IPC & Pipelines:** Implements arbitrary $N$-stage pipelines via `pipe()` and `dup2()`, ensuring all unused read and write descriptors are closed across both parent and child processes to prevent deadlocks on `EOF`.
- **Descriptor & Buffer Hygiene:** Eliminates libc user-space stdio buffer duplication across `fork()` through systematic `fflush(stdout)`. Preserves terminal descriptors to guarantee error delivery when child stdout is redirected.
- **Asynchronous Signal Handling:** Employs `sigaction` with `SA_RESTART` to maintain shell interactivity on `SIGINT` (Ctrl+C) and `SIGTSTP` (Ctrl+Z), while restoring default signal actions (`SIG_DFL`) in child processes.
- **Memory Safety:** Enforces strict bounds checking (`MAX_ARGS = 128`, `MAX_CMDS = 64`), eliminates memory leaks across all success and error paths, and validates cleanly under GCC `-Wall -Wextra -Werror -pedantic -std=c11`, AddressSanitizer, and UndefinedBehaviorSanitizer.

---

## 2. Architecture & Process Lifecycle

```
                              +--------------------+
                              |     myshell.c      |
                              |  (Parent Process)  |
                              +---------+----------+
                                        |
        +-------------------------------+-------------------------------+
        |                               |                               |
        v                               v                               v
 [Built-in: cd, exit]          [External Commands]              [Command Pipelines]
        |                               |                               |
  Direct execution              fork() + execvp()             pipe() + fork() + dup2()
  in parent process             Reaped via waitpid()          All unused FDs closed
```

### Process Lifecycle Details

1. **Interactive Loop:**
   - Obtains current working directory via `getcwd()` and emits the prompt `printf("%s$", cwd)`.
   - Calls `fflush(stdout)` immediately to flush the stdio buffer to the OS kernel.
   - Reads input lines via `getline()`, stripping trailing `\r` and `\n` to prevent CRLF line-ending corruption.
2. **Simple Commands:**
   - Built-ins (`cd`, `exit`) execute directly inside the parent process to modify environment state.
   - External commands fork a child process. The child resets signal handlers to `SIG_DFL` and invokes `execvp()`. The parent waits synchronously using `waitpid(pid, &status, 0)` with retry logic for `EINTR`.
3. **Sequential Execution (`##`):**
   - Commands separated by `##` run left-to-right.
   - The parent waits for each stage to complete before starting the next.
   - Directory changes (`cd`) affect the parent process directly and persist across subsequent commands.
4. **Concurrent Execution (`&&`):**
   - Commands separated by `&&` are spawned concurrently.
   - The parent forks all child processes in rapid succession without blocking in the fork loop, recording child PIDs in an array.
   - Once all commands are forked, the parent loops through the PID array and reaps every child with `waitpid()`. The prompt is redisplayed only after all background children terminate.
   - Built-ins in concurrent mode run within an isolated child process to prevent race conditions in parent working directory state.

---

## 3. Pipelines & File Descriptor Flow

### Multi-Stage Pipelines (`|`)
For an $N$-stage pipeline (`cmd1 | cmd2 | ... | cmdN`), the shell allocates $N - 1$ unidirectional pipes (`pipefds[2 * (N - 1)]`).

```
 [cmd 0] --(pipe 0 write)--> [pipe 0 read]--+
                                            |
                                            v
                                         [cmd 1] --(pipe 1 write)--> [pipe 1 read]--+
                                                                                    |
                                                                                    v
                                                                                 [cmd 2] (stdout -> terminal)
```

1. **Child Descriptor Mapping:**
   - If not the first stage ($i > 0$): `dup2(pipefds[(i - 1) * 2], STDIN_FILENO)`
   - If not the last stage ($i < N - 1$): `dup2(pipefds[i * 2 + 1], STDOUT_FILENO)`
2. **Child Descriptor Closure:**
   - Every child immediately closes all $2 \times (N - 1)$ pipe descriptors after redirection.
3. **Parent Descriptor Closure (Crucial POSIX Invariant):**
   - The parent closes **all** read and write pipe descriptors immediately after the fork loop.
   - *Why this matters:* If the parent keeps write descriptors open, downstream readers (e.g. `grep`, `wc`, `cat`) will never receive `EOF` on `STDIN_FILENO`, causing the pipeline to deadlock permanently.
4. **Error Propagation to Terminal:**
   - For stages where stdout is redirected ($i < N - 1$), the child duplicates the original stdout descriptor (`saved_stdout = dup(STDOUT_FILENO)`) with `FD_CLOEXEC`.
   - If `execvp()` fails, `saved_stdout` is restored to `STDOUT_FILENO` before printing `Shell: Incorrect command\n`. This ensures error messages reach the user's terminal and downstream readers see an immediate `EOF` (0 bytes) rather than consuming the error message as input data.

### Output Redirection (`>`)
- Form: `cmd args... > target_file`
- Target file validation ensures exactly one destination file.
- The child preserves terminal stdout via `saved_stdout = dup(STDOUT_FILENO)` with `FD_CLOEXEC`.
- Opens the target file with flags `O_CREAT | O_TRUNC | O_WRONLY, 0644`.
- Redirects stdout via `dup2(fd, STDOUT_FILENO)` and closes the original file descriptor.
- If `execvp()` fails, `saved_stdout` is restored via `dup2()` before printing `Shell: Incorrect command\n`. Error messages are delivered to the terminal and never pollute the destination file.

---

## 4. Signal Handling & Terminal Control

Keyboard signals (`SIGINT` / Ctrl+C, `SIGTSTP` / Ctrl+Z) require distinct handling across shell and child contexts:

| Context | `SIGINT` (Ctrl+C) | `SIGTSTP` (Ctrl+Z) | Implementation |
| :--- | :--- | :--- | :--- |
| **Parent Shell** | Handled (No Exit) | Handled (No Stop) | Registered via `sigaction` with `SA_RESTART` |
| **Foreground Children** | Default (`SIG_DFL`) | Default (`SIG_DFL`) | Reset in child immediately after `fork()` |

### Technical Rationale
1. When the user presses Ctrl+C or Ctrl+Z at the shell prompt, the terminal driver delivers the signal to the foreground process group. `sigaction` with `SA_RESTART` prevents blocking system calls like `getline()` from aborting with `EINTR`, keeping the shell alive until the user explicitly enters `exit`.
2. When an external foreground command (e.g. `sleep 10`) is running, resetting signals to `SIG_DFL` inside the child process ensures the child terminates or suspends as expected, returning prompt control to the parent shell.

---

## 5. Supported Command Syntax & Examples

| Command Syntax | Execution Semantics | Example |
| :--- | :--- | :--- |
| `cmd [args...]` | Executes command via `execvp` | `ls -la`, `echo hello world` |
| `cd <path>` | Changes working directory via `chdir()` | `cd /usr/bin`, `cd ..`, `cd` |
| `exit` | Displays `Exiting shell...\n` and terminates | `exit` |
| `cmd1 ## cmd2 [## ...]` | Sequential execution (left-to-right) | `mkdir test ## cd test ## pwd` |
| `cmd1 && cmd2 [&& ...]` | Concurrent execution (waits for all) | `sleep 1 && sleep 1` |
| `cmd > file` | Overwrites `file` with stdout of `cmd` | `ls -la > listing.txt` |
| `cmd1 \| cmd2 [\| ...]` | Multi-stage pipeline stdout $\to$ stdin | `cat myshell.c \| grep include \| wc -l` |

### Error Reporting
All syntax errors (e.g. missing operands, empty stages, consecutive operators) and failed command executions emit:
```text
Shell: Incorrect command
```

---

## 6. Build & Run Instructions

The codebase is written in standard C11 and compiles cleanly with zero warnings under GCC's strictest flags.

```bash
# Compile standard optimized binary
make

# Run interactive shell
./myshell

# Compile with AddressSanitizer and UndefinedBehaviorSanitizer
make asan

# Clean build artifacts
make clean
```

---

## 7. Testing Instructions & Verified Results

A comprehensive automated test suite ([test_runner.py](file:///e:/academics/dspd%20projects/os%20cli/test_runner.py)) exercises 23 functional, edge-case, and systems-level tests:

```bash
# Run tests on standard binary
make test

# Run tests under AddressSanitizer and LeakSanitizer
make test_asan
```

### Verified Test Matrix (23/23 Passing)
```text
===========================================================================
RUNNING VERIFIED TEST SUITE ON: ./myshell
===========================================================================
[*] Running: 1. Exit Command ................................... PASSED
[*] Running: 2. Basic Commands (echo, pwd) ..................... PASSED
[*] Running: 3. Directory Navigation (cd .., cd dir, errors) ... PASSED
[*] Running: 4. Empty Lines & Whitespace Handling .............. PASSED
[*] Running: 5. Invalid Command Handling ....................... PASSED
[*] Running: 6. Sequential Commands (##) 2 stages .............. PASSED
[*] Running: 7. Sequential Commands (##) with cd (4 stages) .... PASSED
[*] Running: 8. Sequential Syntax Error Detection .............. PASSED
[*] Running: 9. Sequential Command Count Limit (>64 cmds) ...... PASSED
[*] Running: 10. Parallel Commands (&&) Concurrency ............ PASSED
[*] Running: 11. Parallel Concurrency Wait-For-All (sleep timing)  PASSED
[*] Running: 12. Parallel Syntax Error Detection ............... PASSED
[*] Running: 13. Redirection (>) Basic Operation ............... PASSED
[*] Running: 14. Redirection (>) Terminal Error Handling ....... PASSED
[*] Running: 15. Redirection Syntax Error Detection ............ PASSED
[*] Running: 16. Pipelines (|) 2 stages ........................ PASSED
[*] Running: 17. Pipelines (|) 3 stages (cat | grep | wc) ...... PASSED
[*] Running: 18. Pipelines (|) 4 stages (cat | sort | uniq | wc) PASSED
[*] Running: 19. Pipeline Terminal Error Propagation (fail | wc)   PASSED
[*] Running: 20. Pipeline Large Data Stream (10,000 lines) ..... PASSED
[*] Running: 21. Pipeline Syntax Error Detection ............... PASSED
[*] Running: 22. Signal Handling (SIGINT & SIGTSTP resilience) . PASSED
[*] Running: 23. Argument Limit Exceeded (>128 args) ........... PASSED
===========================================================================
TOTAL TESTS: 23 | PASSED: 23 | FAILED: 0
===========================================================================
```

---

## 8. Known Limitations

By design, this implementation focuses on operating system fundamentals and adheres strictly to assignment requirements rather than imitating a full POSIX shell (like Bash):
1. **No Quote Parsing:** Arguments are parsed strictly using `strsep()` on whitespace (`" \t"`). Quotes (`"..."` or `'...'`) are treated as literal characters.
2. **No Variable or Wildcard Expansion:** Environment variables (e.g. `$HOME`, `$PATH`) and file wildcards (`*.c`) are not expanded by the shell.
3. **No Append or Input Redirection:** Only standard output overwrite (`>`) is implemented. Append (`>>`) and standard input redirection (`<`) are not supported.
4. **No Operator Mixing:** Simultaneous use of distinct operators on the same line (e.g. `cmd > file ## cmd2` or `cmd1 && cmd2 | cmd3`) is not supported.
5. **Fixed Array Bounds:** Command arguments are bounded to `MAX_ARGS = 128` and concurrent/sequential commands to `MAX_CMDS = 64`. Lines exceeding these limits are safely rejected with `Shell: Incorrect command\n`.

---

## 9. Possible Future Improvements

1. **State-Machine Lexer:** Replace `strsep()` with a formal lexer/tokenizer supporting quotes (`""`, `''`) and escape sequences (`\`).
2. **Full Redirection Suite:** Implement input redirection (`<`), append redirection (`>>`), and stderr redirection (`2>`, `&>`).
3. **Dynamic Memory Allocation:** Dynamically reallocate argument and command stage arrays (`realloc`) to support unbounded inputs.
4. **Terminal Job Control:** Implement foreground and background process groups (`setpgid`, `tcsetpgrp`) with built-in `jobs`, `fg`, and `bg` commands.

---

## 10. License

This project is licensed under the [MIT License](file:///e:/academics/dspd%20projects/os%20cli/LICENSE).
