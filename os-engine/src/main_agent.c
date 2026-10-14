#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#define CPU_COUNTER_COUNT 10

typedef struct {
    unsigned long long values[CPU_COUNTER_COUNT];
    size_t count;
} CpuCounters;

typedef struct {
    unsigned long long total_kib;
    unsigned long long available_kib;
} MemoryInfo;

typedef struct {
    uintmax_t total_bytes;
    uintmax_t available_bytes;
} DiskInfo;

static int read_cpu_counters(CpuCounters *counters)
{
    FILE *file = fopen("/proc/stat", "r");
    char *line = NULL;
    size_t line_capacity = 0;
    char *save_pointer;
    char *token;
    ssize_t line_length;

    if (file == NULL) {
        return -1;
    }

    line_length = getline(&line, &line_capacity, file);
    fclose(file);
    if (line_length == -1) {
        free(line);
        errno = EINVAL;
        return -1;
    }

    token = strtok_r(line, " \t\r\n", &save_pointer);
    if (token == NULL || strcmp(token, "cpu") != 0) {
        free(line);
        errno = EINVAL;
        return -1;
    }

    counters->count = 0;
    while (counters->count < CPU_COUNTER_COUNT &&
           (token = strtok_r(NULL, " \t\r\n", &save_pointer)) != NULL) {
        char *end_pointer;
        unsigned long long value;

        errno = 0;
        value = strtoull(token, &end_pointer, 10);
        if (errno != 0 || end_pointer == token || *end_pointer != '\0') {
            free(line);
            errno = EINVAL;
            return -1;
        }
        counters->values[counters->count++] = value;
    }

    free(line);
    if (counters->count < 4) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int read_memory_info(MemoryInfo *memory)
{
    FILE *file = fopen("/proc/meminfo", "r");
    char *line = NULL;
    size_t line_capacity = 0;
    int found_total = 0;
    int found_available = 0;

    if (file == NULL) {
        return -1;
    }

    while (getline(&line, &line_capacity, file) != -1) {
        char field_name[64];
        unsigned long long value;

        if (sscanf(line, "%63[^:]: %llu", field_name, &value) != 2) {
            continue;
        }
        if (strcmp(field_name, "MemTotal") == 0) {
            memory->total_kib = value;
            found_total = 1;
        } else if (strcmp(field_name, "MemAvailable") == 0) {
            memory->available_kib = value;
            found_available = 1;
        }
    }

    free(line);
    fclose(file);
    if (!found_total || !found_available) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int read_disk_info(DiskInfo *disk)
{
    struct statvfs filesystem;
    uintmax_t fragment_size;

    if (statvfs(".", &filesystem) == -1) {
        return -1;
    }

    fragment_size = filesystem.f_frsize != 0
                        ? (uintmax_t)filesystem.f_frsize
                        : (uintmax_t)filesystem.f_bsize;
    disk->total_bytes = (uintmax_t)filesystem.f_blocks * fragment_size;
    disk->available_bytes = (uintmax_t)filesystem.f_bavail * fragment_size;
    return 0;
}

static int read_uptime(double *uptime_seconds)
{
    FILE *file = fopen("/proc/uptime", "r");

    if (file == NULL) {
        return -1;
    }
    if (fscanf(file, "%lf", uptime_seconds) != 1) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    fclose(file);
    return 0;
}

/* Read the whole comm file because a process name may contain control bytes. */
static char *read_process_name(const char *path)
{
    FILE *file = fopen(path, "r");
    char *name = NULL;
    size_t length = 0;
    size_t capacity = 0;
    int character;

    if (file == NULL) {
        return NULL;
    }

    while ((character = fgetc(file)) != EOF) {
        if (length + 1 >= capacity) {
            size_t new_capacity = capacity == 0 ? 32 : capacity * 2;
            char *new_name;

            if (new_capacity < capacity) {
                free(name);
                fclose(file);
                errno = ENOMEM;
                return NULL;
            }
            new_name = realloc(name, new_capacity);
            if (new_name == NULL) {
                free(name);
                fclose(file);
                return NULL;
            }
            name = new_name;
            capacity = new_capacity;
        }
        name[length++] = (char)character;
    }

    if (ferror(file)) {
        free(name);
        fclose(file);
        return NULL;
    }
    fclose(file);

    /* /proc/[pid]/comm adds one final newline after the process name. */
    if (length > 0 && name[length - 1] == '\n') {
        length--;
    }
    if (name == NULL) {
        name = malloc(1);
        if (name == NULL) {
            return NULL;
        }
    }
    name[length] = '\0';
    return name;
}

/* Return the length of a valid UTF-8 sequence, or zero for an invalid byte. */
static size_t valid_utf8_length(const unsigned char *text, size_t remaining)
{
    unsigned char first = text[0];

    if (remaining >= 2 && first >= 0xc2 && first <= 0xdf &&
        (text[1] & 0xc0) == 0x80) {
        return 2;
    }
    if (remaining >= 3 && first >= 0xe0 && first <= 0xef &&
        (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80 &&
        !(first == 0xe0 && text[1] < 0xa0) &&
        !(first == 0xed && text[1] >= 0xa0)) {
        return 3;
    }
    if (remaining >= 4 && first >= 0xf0 && first <= 0xf4 &&
        (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80 &&
        (text[3] & 0xc0) == 0x80 &&
        !(first == 0xf0 && text[1] < 0x90) &&
        !(first == 0xf4 && text[1] >= 0x90)) {
        return 4;
    }
    return 0;
}

/* Escape JSON quotes, backslashes, controls, and invalid UTF-8 bytes. */
static void write_json_string(const char *value)
{
    const unsigned char *text = (const unsigned char *)value;
    size_t remaining = strlen(value);

    putchar('"');
    while (remaining > 0) {
        unsigned char character = *text;

        switch (character) {
        case '"':
            fputs("\\\"", stdout);
            text++;
            remaining--;
            break;
        case '\\':
            fputs("\\\\", stdout);
            text++;
            remaining--;
            break;
        case '\b':
            fputs("\\b", stdout);
            text++;
            remaining--;
            break;
        case '\f':
            fputs("\\f", stdout);
            text++;
            remaining--;
            break;
        case '\n':
            fputs("\\n", stdout);
            text++;
            remaining--;
            break;
        case '\r':
            fputs("\\r", stdout);
            text++;
            remaining--;
            break;
        case '\t':
            fputs("\\t", stdout);
            text++;
            remaining--;
            break;
        default:
            if (character < 0x20) {
                printf("\\u%04x", character);
                text++;
                remaining--;
            } else if (character < 0x80) {
                putchar(character);
                text++;
                remaining--;
            } else {
                size_t sequence_length = valid_utf8_length(text, remaining);

                if (sequence_length == 0) {
                    printf("\\u%04x", character);
                    text++;
                    remaining--;
                } else {
                    fwrite(text, 1, sequence_length, stdout);
                    text += sequence_length;
                    remaining -= sequence_length;
                }
            }
            break;
        }
    }
    putchar('"');
}

static int is_numeric_name(const char *name)
{
    if (*name == '\0') {
        return 0;
    }
    while (*name != '\0') {
        if (!isdigit((unsigned char)*name)) {
            return 0;
        }
        name++;
    }
    return 1;
}

/* Print processes that still exist when their comm file is read. */
static int write_processes_json(void)
{
    DIR *proc_directory = opendir("/proc");
    struct dirent *entry;
    int first_process = 1;

    if (proc_directory == NULL) {
        fprintf(stderr, "scagent: cannot open /proc: %s\n", strerror(errno));
        fputs("null", stdout);
        return -1;
    }

    putchar('[');
    while ((entry = readdir(proc_directory)) != NULL) {
        char *end_pointer;
        long process_id;
        char path[PATH_MAX];
        char *process_name;

        if (!is_numeric_name(entry->d_name)) {
            continue;
        }

        errno = 0;
        process_id = strtol(entry->d_name, &end_pointer, 10);
        if (errno != 0 || *end_pointer != '\0' || process_id <= 0) {
            continue;
        }

        if (snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name) >=
            (int)sizeof(path)) {
            continue;
        }
        process_name = read_process_name(path);
        if (process_name == NULL) {
            /* Processes can exit or become unreadable during the scan. */
            continue;
        }

        if (!first_process) {
            putchar(',');
        }
        printf("{\"pid\": %ld, \"name\": ", process_id);
        write_json_string(process_name);
        putchar('}');
        first_process = 0;
        free(process_name);
    }
    closedir(proc_directory);
    putchar(']');
    return 0;
}

static void write_cpu_json(const CpuCounters *counters, int available)
{
    static const char *counter_names[CPU_COUNTER_COUNT] = {
        "user", "nice", "system", "idle", "iowait",
        "irq", "softirq", "steal", "guest", "guest_nice"
    };

    if (!available) {
        fputs("null", stdout);
        return;
    }

    putchar('{');
    for (size_t index = 0; index < counters->count; index++) {
        if (index > 0) {
            fputs(", ", stdout);
        }
        printf("\"%s\": %llu", counter_names[index], counters->values[index]);
    }
    putchar('}');
}

int main(void)
{
    CpuCounters cpu_counters = {{0}, 0};
    MemoryInfo memory = {0, 0};
    DiskInfo disk = {0, 0};
    double uptime_seconds = 0.0;
    int have_cpu = read_cpu_counters(&cpu_counters) == 0;
    int have_memory = read_memory_info(&memory) == 0;
    int have_disk = read_disk_info(&disk) == 0;
    int have_uptime = read_uptime(&uptime_seconds) == 0;
    int had_error = !(have_cpu && have_memory && have_disk && have_uptime);

    if (!have_cpu) {
        fprintf(stderr, "scagent: cannot read CPU counters: %s\n", strerror(errno));
    }
    if (!have_memory) {
        fprintf(stderr, "scagent: cannot read memory information: %s\n",
                strerror(errno));
    }
    if (!have_disk) {
        fprintf(stderr, "scagent: cannot read current filesystem information: %s\n",
                strerror(errno));
    }
    if (!have_uptime) {
        fprintf(stderr, "scagent: cannot read uptime: %s\n", strerror(errno));
    }

    puts("{");
    fputs("  \"cpu_counters\": ", stdout);
    write_cpu_json(&cpu_counters, have_cpu);
    fputs(",\n  \"memory\": ", stdout);
    if (have_memory) {
        printf("{\"total_kib\": %llu, \"available_kib\": %llu}",
               memory.total_kib, memory.available_kib);
    } else {
        fputs("null", stdout);
    }
    fputs(",\n  \"disk\": ", stdout);
    if (have_disk) {
         printf("{\"path\": \".\", \"total_bytes\": %" PRIuMAX
             ", \"available_bytes\": %" PRIuMAX "}",
             disk.total_bytes, disk.available_bytes);
    } else {
        fputs("null", stdout);
    }
    fputs(",\n  \"uptime_seconds\": ", stdout);
    if (have_uptime) {
        printf("%.2f", uptime_seconds);
    } else {
        fputs("null", stdout);
    }
    fputs(",\n  \"processes\": ", stdout);
    if (write_processes_json() == -1) {
        had_error = 1;
    }
    puts("\n}");

    return had_error ? EXIT_FAILURE : EXIT_SUCCESS;
}