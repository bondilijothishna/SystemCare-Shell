CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
TARGET = scsh
SOURCE = src/main_shell.c
AGENT_TARGET = scagent
AGENT_SOURCE = src/main_agent.c

.PHONY: all clean

all: $(TARGET) $(AGENT_TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $@ $<

$(AGENT_TARGET): $(AGENT_SOURCE)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET) $(AGENT_TARGET)