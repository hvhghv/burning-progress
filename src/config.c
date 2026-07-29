#include "burning.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *bp_trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static int bp_parse_uint(const char *text, unsigned int maximum, unsigned int *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed > maximum) {
        return -1;
    }
    *value = (unsigned int)parsed;
    return 0;
}

const char *bp_boot_choice_name(enum bp_boot_choice choice)
{
    switch (choice) {
    case BP_BOOT_NORMAL:
        return "normal";
    case BP_BOOT_SHELL:
        return "shell";
    case BP_BOOT_POWEROFF:
        return "poweroff";
    }
    return "invalid";
}

int bp_boot_choice_parse(const char *text, enum bp_boot_choice *choice)
{
    if (strcmp(text, "normal") == 0) {
        *choice = BP_BOOT_NORMAL;
    } else if (strcmp(text, "shell") == 0) {
        *choice = BP_BOOT_SHELL;
    } else if (strcmp(text, "poweroff") == 0) {
        *choice = BP_BOOT_POWEROFF;
    } else {
        return -1;
    }
    return 0;
}

const char *bp_entry_mode_name(enum bp_entry_mode mode)
{
    return mode == BP_ENTRY_HANDOFF ? "handoff" : "supervised";
}

void bp_host_config_default(struct bp_host_config *config)
{
    config->version = BP_CONFIG_VERSION;
    config->timeout = BP_DEFAULT_TIMEOUT;
    config->default_choice = BP_BOOT_NORMAL;
}

int bp_host_config_parse(const char *text, struct bp_host_config *config,
                         char *error, size_t error_size)
{
    char *copy;
    char *line;
    char *save = NULL;
    unsigned int seen = 0U;

    bp_host_config_default(config);
    copy = strdup(text);
    if (copy == NULL) {
        bp_error_set(error, error_size, "out of memory parsing configuration");
        return -1;
    }
    for (line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char *key;
        char *value;
        char *separator;
        unsigned int bit;

        line = bp_trim(line);
        if (*line == '\0' || *line == '#') {
            continue;
        }
        separator = strchr(line, '=');
        if (separator == NULL) {
            bp_error_set(error, error_size, "configuration line is not key=value");
            free(copy);
            return -1;
        }
        *separator = '\0';
        key = bp_trim(line);
        value = bp_trim(separator + 1);
        if (strcmp(key, "version") == 0) {
            bit = 1U;
            if (bp_parse_uint(value, UINT32_MAX, &config->version) != 0 ||
                config->version != BP_CONFIG_VERSION) {
                bp_error_set(error, error_size, "unsupported config version: %s", value);
                free(copy);
                return -1;
            }
        } else if (strcmp(key, "timeout") == 0) {
            bit = 2U;
            if (bp_parse_uint(value, BP_MAX_TIMEOUT, &config->timeout) != 0) {
                bp_error_set(error, error_size, "invalid timeout: %s", value);
                free(copy);
                return -1;
            }
        } else if (strcmp(key, "default") == 0) {
            bit = 4U;
            if (bp_boot_choice_parse(value, &config->default_choice) != 0) {
                bp_error_set(error, error_size, "invalid default option: %s", value);
                free(copy);
                return -1;
            }
        } else {
            bp_error_set(error, error_size, "unknown configuration key: %s", key);
            free(copy);
            return -1;
        }
        if ((seen & bit) != 0U) {
            bp_error_set(error, error_size, "duplicate configuration key: %s", key);
            free(copy);
            return -1;
        }
        seen |= bit;
    }
    free(copy);
    return 0;
}

int bp_host_config_format(const struct bp_host_config *config, char *output,
                          size_t output_size, char *error, size_t error_size)
{
    int length = snprintf(output, output_size,
                          "version=%u\ntimeout=%u\ndefault=%s\n",
                          config->version, config->timeout,
                          bp_boot_choice_name(config->default_choice));
    if (length < 0 || (size_t)length >= output_size) {
        bp_error_set(error, error_size, "configuration output buffer is too small");
        return -1;
    }
    return 0;
}

int bp_host_config_ensure(const char *root, int *created,
                          char *error, size_t error_size)
{
    char path[BP_PATH_CAPACITY];
    char text[256];
    struct bp_host_config config;
    struct stat status;

    if (created != NULL) {
        *created = 0;
    }
    if (bp_path(path, sizeof(path), root, BP_HOST_CONFIG_PATH,
                error, error_size) != 0) {
        return -1;
    }
    if (lstat(path, &status) == 0) {
        return 0;
    }
    if (errno != ENOENT) {
        bp_error_set(error, error_size, "inspect %s: %s", path, strerror(errno));
        return -1;
    }
    bp_host_config_default(&config);
    if (bp_host_config_format(&config, text, sizeof(text),
                              error, error_size) != 0 ||
        bp_atomic_write(path, text, strlen(text), 0600,
                        error, error_size) != 0) {
        return -1;
    }
    if (created != NULL) {
        *created = 1;
    }
    return 0;
}

void bp_runtime_config_default(struct bp_runtime_config *config)
{
    config->version = BP_CONFIG_VERSION;
    config->entry_mode = BP_ENTRY_SUPERVISED;
    strcpy(config->entry, BP_DEFAULT_ENTRY_PATH);
}

static int bp_validate_entry(const char *entry)
{
    const char *component;

    if (entry[0] != '/' || strlen(entry) >= BP_PATH_CAPACITY) {
        return -1;
    }
    component = entry + 1;
    while (*component != '\0') {
        const char *end = strchr(component, '/');
        size_t length = end == NULL ? strlen(component) : (size_t)(end - component);
        if (length == 0U || (length == 1U && component[0] == '.') ||
            (length == 2U && component[0] == '.' && component[1] == '.')) {
            return -1;
        }
        if (end == NULL) {
            break;
        }
        component = end + 1;
    }
    return 0;
}

int bp_runtime_config_parse(const char *text, struct bp_runtime_config *config,
                            char *error, size_t error_size)
{
    char *copy;
    char *line;
    char *save = NULL;
    unsigned int seen = 0U;

    bp_runtime_config_default(config);
    copy = strdup(text);
    if (copy == NULL) {
        bp_error_set(error, error_size, "out of memory parsing runtime configuration");
        return -1;
    }
    for (line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char *separator;
        char *key;
        char *value;
        unsigned int bit;

        line = bp_trim(line);
        if (*line == '\0' || *line == '#') {
            continue;
        }
        separator = strchr(line, '=');
        if (separator == NULL) {
            bp_error_set(error, error_size, "runtime config line is not key=value");
            free(copy);
            return -1;
        }
        *separator = '\0';
        key = bp_trim(line);
        value = bp_trim(separator + 1);
        if (strcmp(key, "version") == 0) {
            bit = 1U;
            if (bp_parse_uint(value, UINT32_MAX, &config->version) != 0 ||
                config->version != BP_CONFIG_VERSION) {
                bp_error_set(error, error_size, "unsupported runtime config version");
                free(copy);
                return -1;
            }
        } else if (strcmp(key, "entryMode") == 0) {
            bit = 2U;
            if (strcmp(value, "supervised") == 0) {
                config->entry_mode = BP_ENTRY_SUPERVISED;
            } else if (strcmp(value, "handoff") == 0) {
                config->entry_mode = BP_ENTRY_HANDOFF;
            } else {
                bp_error_set(error, error_size, "invalid entryMode: %s", value);
                free(copy);
                return -1;
            }
        } else if (strcmp(key, "entry") == 0) {
            bit = 4U;
            if (bp_validate_entry(value) != 0) {
                bp_error_set(error, error_size, "invalid entry path: %s", value);
                free(copy);
                return -1;
            }
            strcpy(config->entry, value);
        } else {
            bp_error_set(error, error_size, "unknown runtime configuration key: %s", key);
            free(copy);
            return -1;
        }
        if ((seen & bit) != 0U) {
            bp_error_set(error, error_size, "duplicate runtime configuration key: %s", key);
            free(copy);
            return -1;
        }
        seen |= bit;
    }
    free(copy);
    return 0;
}

int bp_runtime_config_format(const struct bp_runtime_config *config, char *output,
                             size_t output_size, char *error, size_t error_size)
{
    int length;

    if (config->version != BP_CONFIG_VERSION ||
        (config->entry_mode != BP_ENTRY_SUPERVISED &&
         config->entry_mode != BP_ENTRY_HANDOFF)) {
        bp_error_set(error, error_size, "invalid runtime configuration");
        return -1;
    }
    if (bp_validate_entry(config->entry) != 0) {
        bp_error_set(error, error_size, "invalid entry path: %s", config->entry);
        return -1;
    }
    length = snprintf(output, output_size,
                      "version=%u\nentryMode=%s\nentry=%s\n",
                      config->version, bp_entry_mode_name(config->entry_mode),
                      config->entry);
    if (length < 0 || (size_t)length >= output_size) {
        bp_error_set(error, error_size,
                     "runtime configuration output buffer is too small");
        return -1;
    }
    return 0;
}
