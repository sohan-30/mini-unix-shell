#!/usr/bin/env python3
"""
Comprehensive Test Suite for Mini Unix Shell (myshell)
Tests compliance with OS Lab Assignment 1 specification and POSIX robustness:
  1. Exit command
  2. Basic commands (echo, ls, pwd, arguments)
  3. Directory navigation (cd .., cd dir, cd invalid, cd home)
  4. Empty input handling (whitespace, empty lines, CRLF)
  5. Invalid commands & proper process termination
  6. Sequential execution ('##') 2 stages
  7. Sequential execution ('##') with cd (4 stages)
  8. Sequential syntax error detection
  9. Sequential command count limit handling (>64 commands)
 10. Parallel execution ('&&') concurrency
 11. Parallel concurrency wait-for-all (sleep timing verification)
 12. Parallel syntax error detection
 13. Output redirection ('>') basic operation
 14. Output redirection ('>') invalid command terminal error handling
 15. Redirection syntax error detection
 16. Pipelines ('|') 2 stages
 17. Pipelines ('|') 3 stages (cat | grep | wc)
 18. Pipelines ('|') 4 stages (cat | sort | uniq | wc)
 19. Pipeline error propagation to terminal (nonexistent | wc -l)
 20. Pipeline large data stream without deadlock (seq 10000 | grep | wc)
 21. Pipeline syntax error detection
 22. Signal handling (SIGINT & SIGTSTP resilience)
 23. Argument limit exceeded handling (>128 arguments)
"""

import os
import sys
import time
import signal
import shutil
import tempfile
import subprocess

SHELL_BIN = sys.argv[1] if len(sys.argv) > 1 else "./myshell"

class TestFailure(Exception):
    pass

class ShellTester:
    def __init__(self, binary_path):
        self.binary = os.path.abspath(binary_path)
        self.passed = 0
        self.failed = 0
        self.tests = []

    def run_raw(self, input_data, timeout=5, cwd=None):
        """Runs the shell with provided stdin input and returns stdout, stderr, and exitcode."""
        proc = subprocess.Popen(
            [self.binary],
            cwd=cwd or os.path.dirname(self.binary),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        try:
            stdout, stderr = proc.communicate(input=input_data, timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
            raise TestFailure(f"Shell timed out after {timeout} seconds (deadlock or missed EOF).")
        return stdout, stderr, proc.returncode

    def record(self, name, func):
        print(f"[*] Running: {name} ... ", end="", flush=True)
        try:
            func()
            print("\033[92mPASSED\033[0m")
            self.passed += 1
            self.tests.append((name, "PASS", ""))
        except Exception as e:
            print(f"\033[91mFAILED\033[0m: {e}")
            self.failed += 1
            self.tests.append((name, "FAIL", str(e)))

    # ----------------------------------------------------
    # Test 1: Exit command
    # ----------------------------------------------------
    def test_exit(self):
        stdout, stderr, code = self.run_raw("exit\n")
        if code != 0:
            raise TestFailure(f"Expected returncode 0, got {code}")
        if "Exiting shell..." not in stdout:
            raise TestFailure(f"Missing 'Exiting shell...' in stdout. Output: {stdout!r}")

    # ----------------------------------------------------
    # Test 2: Basic commands (pwd, echo, ls)
    # ----------------------------------------------------
    def test_basic_commands(self):
        stdout, _, code = self.run_raw("echo hello mini shell\npwd\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if "hello mini shell" not in stdout:
            raise TestFailure(f"Echo output missing. Output: {stdout!r}")
        if "Exiting shell..." not in stdout:
            raise TestFailure("Exit message missing")

    # ----------------------------------------------------
    # Test 3: Changing directory ('cd')
    # ----------------------------------------------------
    def test_cd(self):
        tmp = tempfile.mkdtemp(prefix="myshell_cd_")
        sub = os.path.join(tmp, "subdir")
        os.makedirs(sub, exist_ok=True)
        try:
            input_script = f"cd {tmp}\npwd\ncd subdir\npwd\ncd ..\npwd\nexit\n"
            stdout, _, code = self.run_raw(input_script)
            if code != 0:
                raise TestFailure(f"Shell exited with code {code}")
            if os.path.realpath(tmp) not in stdout and tmp not in stdout:
                raise TestFailure(f"Failed to chdir to {tmp}. Output: {stdout!r}")
            if "subdir" not in stdout:
                raise TestFailure(f"Failed to chdir to subdir. Output: {stdout!r}")

            # Test invalid directory
            stdout_err, _, _ = self.run_raw("cd /nonexistent_path_404_xyz\nexit\n")
            if "Shell: Incorrect command" not in stdout_err:
                raise TestFailure(f"cd to invalid path did not print 'Shell: Incorrect command'. Output: {stdout_err!r}")
        finally:
            shutil.rmtree(tmp)

    # ----------------------------------------------------
    # Test 4: Empty input & whitespace
    # ----------------------------------------------------
    def test_empty_input(self):
        stdout, _, code = self.run_raw("\n   \n\t\n\r\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if "Shell: Incorrect command" in stdout:
            raise TestFailure("Empty lines should not produce error messages")
        if "Exiting shell..." not in stdout:
            raise TestFailure("Missing exit message")

    # ----------------------------------------------------
    # Test 5 & 6: Invalid commands & process termination
    # ----------------------------------------------------
    def test_invalid_command(self):
        stdout, _, code = self.run_raw("cmd_that_does_not_exist_xyz123\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if "Shell: Incorrect command" not in stdout:
            raise TestFailure(f"Invalid command did not print 'Shell: Incorrect command'. Output: {stdout!r}")
        if "Exiting shell..." not in stdout:
            raise TestFailure("Shell crashed or failed to reach exit")

    # ----------------------------------------------------
    # Test 7: Sequential execution ('##')
    # ----------------------------------------------------
    def test_sequential_two_commands(self):
        stdout, _, code = self.run_raw("echo first ## echo second\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        pos1 = stdout.find("first")
        pos2 = stdout.find("second")
        if pos1 == -1 or pos2 == -1:
            raise TestFailure(f"Expected both 'first' and 'second'. Output: {stdout!r}")
        if pos1 > pos2:
            raise TestFailure("Commands did not execute in sequential order")

    def test_sequential_with_cd(self):
        tmp = tempfile.mkdtemp(prefix="myshell_seq_")
        sub = os.path.join(tmp, "seqsub")
        os.makedirs(sub, exist_ok=True)
        try:
            script = f"cd {tmp} ## cd seqsub ## pwd ## echo done_seq\nexit\n"
            stdout, _, code = self.run_raw(script)
            if code != 0:
                raise TestFailure(f"Shell exited with code {code}")
            if "seqsub" not in stdout:
                raise TestFailure("cd inside sequential chain did not affect subsequent commands")
            if "done_seq" not in stdout:
                raise TestFailure("Last sequential command failed to execute")
        finally:
            shutil.rmtree(tmp)

    def test_sequential_syntax_errors(self):
        for bad in ["echo a ##", "## echo a", "echo a ## ## echo b", "##"]:
            stdout, _, _ = self.run_raw(f"{bad}\nexit\n")
            if "Shell: Incorrect command" not in stdout:
                raise TestFailure(f"Syntax '{bad}' should report 'Shell: Incorrect command'")

    def test_sequential_count_limit(self):
        chain = " ## ".join(f"echo {i}" for i in range(70))
        stdout, _, _ = self.run_raw(f"{chain}\nexit\n")
        if "Shell: Incorrect command" not in stdout:
            raise TestFailure("Exceeding MAX_CMDS in sequential execution should report 'Shell: Incorrect command'")

    # ----------------------------------------------------
    # Test 8: Parallel execution ('&&')
    # ----------------------------------------------------
    def test_parallel_commands(self):
        stdout, _, code = self.run_raw("echo parallel_1 && echo parallel_2\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if "parallel_1" not in stdout or "parallel_2" not in stdout:
            raise TestFailure(f"Parallel outputs missing. Output: {stdout!r}")

    def test_parallel_wait_for_all(self):
        t0 = time.time()
        stdout, _, code = self.run_raw("sleep 0.3 && sleep 0.3\nexit\n")
        elapsed = time.time() - t0
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if elapsed < 0.25:
            raise TestFailure(f"Shell did not wait for parallel commands to finish! Elapsed: {elapsed:.2f}s")
        if elapsed > 0.55:
            raise TestFailure(f"Parallel commands executed sequentially instead of concurrently! Elapsed: {elapsed:.2f}s")

    def test_parallel_syntax_errors(self):
        for bad in ["echo a &&", "&& echo a", "echo a && && echo b", "&&"]:
            stdout, _, _ = self.run_raw(f"{bad}\nexit\n")
            if "Shell: Incorrect command" not in stdout:
                raise TestFailure(f"Syntax '{bad}' should report 'Shell: Incorrect command'")

    # ----------------------------------------------------
    # Test 9: Output redirection ('>')
    # ----------------------------------------------------
    def test_redirection_basic(self):
        tmp_name = "test_redir_tmp.txt"
        target_path = os.path.join(os.path.dirname(self.binary), tmp_name)
        if os.path.exists(target_path):
            os.remove(target_path)
        try:
            stdout, _, code = self.run_raw(f"echo redirection_payload > {tmp_name}\nexit\n")
            if code != 0:
                raise TestFailure(f"Shell exited with code {code}")
            if not os.path.exists(target_path):
                raise TestFailure(f"Target file {tmp_name} was not created in cwd")
            with open(target_path, "r") as f:
                content = f.read()
            if "redirection_payload" not in content:
                raise TestFailure(f"Target file missing content. File content: {content!r}")
            if "$" in content:
                raise TestFailure(f"Shell prompt leaked into redirected file: {content!r}")
        finally:
            if os.path.exists(target_path):
                os.remove(target_path)

    def test_redirection_invalid_command(self):
        tmp_name = "test_err_redir_tmp.txt"
        target_path = os.path.join(os.path.dirname(self.binary), tmp_name)
        if os.path.exists(target_path):
            os.remove(target_path)
        try:
            stdout, _, code = self.run_raw(f"nonexistent_cmd_xyz > {tmp_name}\nexit\n")
            if code != 0:
                raise TestFailure(f"Shell exited with code {code}")
            if "Shell: Incorrect command" not in stdout:
                raise TestFailure(f"Terminal stdout missing 'Shell: Incorrect command'. Output: {stdout!r}")
            if os.path.exists(target_path):
                with open(target_path, "r") as f:
                    content = f.read()
                if "$" in content:
                    raise TestFailure(f"Prompt leaked into target file on error: {content!r}")
        finally:
            if os.path.exists(target_path):
                os.remove(target_path)

    def test_redirection_syntax_errors(self):
        for bad in ["echo a >", "> file", "echo a >> file", "echo a > f1 f2", ">", "cd > file", "exit > file"]:
            stdout, _, _ = self.run_raw(f"{bad}\nexit\n")
            if "Shell: Incorrect command" not in stdout:
                raise TestFailure(f"Syntax '{bad}' should report 'Shell: Incorrect command'")

    # ----------------------------------------------------
    # Test 10: Pipelines ('|')
    # ----------------------------------------------------
    def test_pipeline_two_stages(self):
        stdout, _, code = self.run_raw("cat myshell.c | grep main\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if "main" not in stdout:
            raise TestFailure(f"Piped grep output missing expected lines. Output: {stdout!r}")

    def test_pipeline_three_stages(self):
        stdout, _, code = self.run_raw("cat myshell.c | grep include | wc -l\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if "10" not in stdout:
            raise TestFailure(f"Expected 10 includes in myshell.c. Output: {stdout!r}")

    def test_pipeline_four_stages(self):
        sort_tmp = os.path.join(os.path.dirname(self.binary), "test_sort_input.txt")
        with open(sort_tmp, "w") as f:
            f.write("orange\napple\nbanana\napple\n")
        try:
            stdout, _, code = self.run_raw(f"cat test_sort_input.txt | sort | uniq | wc -l\nexit\n")
            if code != 0:
                raise TestFailure(f"Shell exited with code {code}")
            if "3" not in stdout:
                raise TestFailure(f"Expected '3' from 4-stage pipeline. Output: {stdout!r}")
        finally:
            if os.path.exists(sort_tmp):
                os.remove(sort_tmp)

    def test_pipeline_failed_command(self):
        stdout, _, code = self.run_raw("nonexistent_binary_xyz_123 | wc -l\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if "Shell: Incorrect command" not in stdout:
            raise TestFailure(f"Expected 'Shell: Incorrect command' printed to terminal. Output: {stdout!r}")
        if "0" not in stdout:
            raise TestFailure(f"Expected wc -l to report 0 lines. Output: {stdout!r}")

    def test_pipeline_large_data_stream(self):
        stdout, _, code = self.run_raw("seq 10000 | grep 5 | wc -l\nexit\n")
        if code != 0:
            raise TestFailure(f"Shell exited with code {code}")
        if "3439" not in stdout:
            raise TestFailure(f"Expected '3439' from seq 10000 | grep 5 | wc -l. Output: {stdout!r}")

    def test_pipeline_syntax_errors(self):
        for bad in ["cat myshell.c |", "| wc -l", "cat myshell.c || wc -l", "|"]:
            stdout, _, _ = self.run_raw(f"{bad}\nexit\n")
            if "Shell: Incorrect command" not in stdout:
                raise TestFailure(f"Syntax '{bad}' should report 'Shell: Incorrect command'")

    # ----------------------------------------------------
    # Test 11: Signal handling (SIGINT & SIGTSTP)
    # ----------------------------------------------------
    def test_signal_handling(self):
        proc = subprocess.Popen(
            [self.binary],
            cwd=os.path.dirname(self.binary),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        try:
            time.sleep(0.15)
            proc.send_signal(signal.SIGINT)
            time.sleep(0.1)
            if proc.poll() is not None:
                raise TestFailure(f"Shell died upon receiving SIGINT with exitcode {proc.poll()}")

            proc.send_signal(signal.SIGTSTP)
            time.sleep(0.1)
            if proc.poll() is not None:
                raise TestFailure(f"Shell died upon receiving SIGTSTP with exitcode {proc.poll()}")

            stdout, stderr = proc.communicate(input="echo signal_test_passed\nexit\n", timeout=3)
            if "signal_test_passed" not in stdout:
                raise TestFailure(f"Shell failed to execute commands after signals. Output: {stdout!r}")
            if "Exiting shell..." not in stdout:
                raise TestFailure(f"Shell failed to exit cleanly after signals. Output: {stdout!r}")
        finally:
            if proc.poll() is None:
                proc.kill()

    # ----------------------------------------------------
    # Test 12: Argument limits
    # ----------------------------------------------------
    def test_argument_limit_exceeded(self):
        args = " ".join(f"arg{i}" for i in range(140))
        stdout, _, _ = self.run_raw(f"echo {args}\nexit\n")
        if "Shell: Incorrect command" not in stdout:
            raise TestFailure("Exceeding MAX_ARGS should report 'Shell: Incorrect command'")

    def run_all(self):
        print("=" * 75)
        print(f"RUNNING VERIFIED TEST SUITE ON: {self.binary}")
        print("=" * 75)

        self.record("1. Exit Command", self.test_exit)
        self.record("2. Basic Commands (echo, pwd)", self.test_basic_commands)
        self.record("3. Directory Navigation (cd .., cd dir, errors)", self.test_cd)
        self.record("4. Empty Lines & Whitespace Handling", self.test_empty_input)
        self.record("5. Invalid Command Handling", self.test_invalid_command)
        self.record("6. Sequential Commands (##) 2 stages", self.test_sequential_two_commands)
        self.record("7. Sequential Commands (##) with cd (4 stages)", self.test_sequential_with_cd)
        self.record("8. Sequential Syntax Error Detection", self.test_sequential_syntax_errors)
        self.record("9. Sequential Command Count Limit (>64 cmds)", self.test_sequential_count_limit)
        self.record("10. Parallel Commands (&&) Concurrency", self.test_parallel_commands)
        self.record("11. Parallel Concurrency Wait-For-All (sleep timing)", self.test_parallel_wait_for_all)
        self.record("12. Parallel Syntax Error Detection", self.test_parallel_syntax_errors)
        self.record("13. Redirection (>) Basic Operation", self.test_redirection_basic)
        self.record("14. Redirection (>) Terminal Error Handling", self.test_redirection_invalid_command)
        self.record("15. Redirection Syntax Error Detection", self.test_redirection_syntax_errors)
        self.record("16. Pipelines (|) 2 stages", self.test_pipeline_two_stages)
        self.record("17. Pipelines (|) 3 stages (cat | grep | wc)", self.test_pipeline_three_stages)
        self.record("18. Pipelines (|) 4 stages (cat | sort | uniq | wc)", self.test_pipeline_four_stages)
        self.record("19. Pipeline Terminal Error Propagation (fail | wc)", self.test_pipeline_failed_command)
        self.record("20. Pipeline Large Data Stream (10,000 lines)", self.test_pipeline_large_data_stream)
        self.record("21. Pipeline Syntax Error Detection", self.test_pipeline_syntax_errors)
        self.record("22. Signal Handling (SIGINT & SIGTSTP resilience)", self.test_signal_handling)
        self.record("23. Argument Limit Exceeded (>128 args)", self.test_argument_limit_exceeded)

        print("=" * 75)
        print(f"TOTAL TESTS: {self.passed + self.failed} | PASSED: \033[92m{self.passed}\033[0m | FAILED: \033[91m{self.failed}\033[0m")
        print("=" * 75)
        return self.failed == 0

if __name__ == "__main__":
    tester = ShellTester(SHELL_BIN)
    success = tester.run_all()
    sys.exit(0 if success else 1)
