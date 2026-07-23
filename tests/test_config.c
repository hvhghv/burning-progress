#include "burning.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_host_config(void)
{
    struct bp_host_config config;
    char error[BP_ERROR_CAPACITY];
    char output[256];

    assert(bp_host_config_parse("version=1\ntimeout=12\ndefault=shell\n",
                                &config, error, sizeof(error)) == 0);
    assert(config.timeout == 12U);
    assert(config.default_choice == BP_BOOT_SHELL);
    assert(bp_host_config_format(&config, output, sizeof(output),
                                 error, sizeof(error)) == 0);
    assert(strstr(output, "default=shell") != NULL);
    assert(bp_host_config_parse("timeout=1\ntimeout=2\n",
                                &config, error, sizeof(error)) != 0);
    assert(bp_host_config_parse("unknown=value\n",
                                &config, error, sizeof(error)) != 0);
}

static void test_runtime_config(void)
{
    struct bp_runtime_config config;
    char error[BP_ERROR_CAPACITY];

    assert(bp_runtime_config_parse("entryMode=handoff\nentry=/sbin/init\n",
                                   &config, error, sizeof(error)) == 0);
    assert(config.entry_mode == BP_ENTRY_HANDOFF);
    assert(strcmp(config.entry, "/sbin/init") == 0);
    assert(bp_runtime_config_parse("entry=/../init\n",
                                   &config, error, sizeof(error)) != 0);
    assert(bp_runtime_config_parse("entry=relative.sh\n",
                                   &config, error, sizeof(error)) != 0);
}

int main(void)
{
    test_host_config();
    test_runtime_config();
    puts("test_config: OK");
    return 0;
}
