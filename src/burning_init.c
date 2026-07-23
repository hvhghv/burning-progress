#include "burning.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void console_error(const char *message)
{
    int descriptor = open("/dev/console", O_WRONLY | O_CLOEXEC);
    if (descriptor >= 0) {
        dprintf(descriptor, "burning-init: %s\n", message);
        close(descriptor);
    }
}

static int exact_token(const char *text, const char *token)
{
    size_t token_length = strlen(token);
    const char *cursor = text;

    while (*cursor != '\0') {
        const char *start;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
        start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\r' && *cursor != '\n') {
            ++cursor;
        }
        if ((size_t)(cursor - start) == token_length &&
            memcmp(start, token, token_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int command_line_triggered(void)
{
    char buffer[8192];
    int descriptor = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    ssize_t count;

    if (descriptor < 0) {
        return 0;
    }
    count = read(descriptor, buffer, sizeof(buffer) - 1U);
    close(descriptor);
    if (count <= 0) {
        return 0;
    }
    buffer[count] = '\0';
    return exact_token(buffer, BP_TRIGGER);
}

static void exec_original(int argc, char **argv)
{
    char **arguments = calloc((size_t)argc + 1U, sizeof(*arguments));
    int source;
    int destination = 1;

    if (arguments == NULL) {
        console_error("out of memory preparing original init");
        _exit(127);
    }
    arguments[0] = (char *)BP_INIT_PATH;
    for (source = 1; source < argc; ++source) {
        if (strcmp(argv[source], BP_TRIGGER) != 0) {
            arguments[destination++] = argv[source];
        }
    }
    arguments[destination] = NULL;
    execv(BP_ORIGINAL_INIT_PATH, arguments);
    dprintf(STDERR_FILENO, "burning-init: exec %s: %s\n",
            BP_ORIGINAL_INIT_PATH, strerror(errno));
    _exit(127);
}

int main(int argc, char **argv)
{
    int index;
    int triggered = access(BP_ENABLE_PATH, F_OK) == 0 || command_line_triggered();

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], BP_TRIGGER) == 0) {
            triggered = 1;
        }
    }
    if (triggered) {
        char **arguments = calloc((size_t)argc + 3U, sizeof(*arguments));
        if (arguments != NULL) {
            arguments[0] = (char *)BP_PROGRESS_PATH;
            arguments[1] = (char *)"--stage1";
            arguments[2] = (char *)"--";
            for (index = 1; index < argc; ++index) {
                arguments[index + 2] = argv[index];
            }
            execv(BP_PROGRESS_PATH, arguments);
            console_error("cannot start burning-progress; continuing normal boot");
            free(arguments);
        } else {
            console_error("out of memory; continuing normal boot");
        }
    }
    exec_original(argc, argv);
    return 127;
}
