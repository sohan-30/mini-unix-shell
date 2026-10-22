CC ?= gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -D_GNU_SOURCE -O2
SAN_FLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -D_GNU_SOURCE -fsanitize=address,undefined -g

TARGET = myshell
TARGET_ASAN = myshell_asan

.PHONY: all clean test test_asan asan

all: $(TARGET)

$(TARGET): myshell.c
	$(CC) $(CFLAGS) $< -o $@

asan: $(TARGET_ASAN)

$(TARGET_ASAN): myshell.c
	$(CC) $(SAN_FLAGS) $< -o $@

test: $(TARGET)
	python3 test_runner.py ./$(TARGET)

test_asan: $(TARGET_ASAN)
	python3 test_runner.py ./$(TARGET_ASAN)

clean:
	rm -f $(TARGET) $(TARGET_ASAN) test_redir_tmp.txt test_err_redir_tmp.txt test_sort_input.txt
