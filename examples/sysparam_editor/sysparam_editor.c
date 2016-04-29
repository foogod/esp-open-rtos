#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysparam.h>

#define CMD_BUF_SIZE 5000

// Number of (4K) sectors that make up a sysparam area.  Total sysparam data
// cannot be larger than half this amount.
// Note that if there is already a sysparam area created with a different size,
// that will continue to be used (if it can be found).  This value is only used
// when creating/reformatting the sysparam area.
#define SYSPARAM_SECTORS 4

// This places the sysparam region just below the upper-4 sdk-reserved sectors
// for a 16mbit flash
#define FLASH_TOP 0x1fc000
#define SYSPARAM_ADDR (FLASH_TOP - (SYSPARAM_SECTORS * 4096))

const int status_base = -6;
const char *status_messages[] = {
    "SYSPARAM_ERR_NOMEM",
    "SYSPARAM_ERR_CORRUPT",
    "SYSPARAM_ERR_IO",
    "SYSPARAM_ERR_FULL",
    "SYSPARAM_ERR_BADVALUE",
    "SYSPARAM_ERR_NOINIT",
    "SYSPARAM_OK",
    "SYSPARAM_NOTFOUND",
    "SYSPARAM_PARSEFAILED",
};

void usage(void) {
    printf(
        "Available commands:\n"
        "  <key>?          -- Query the value of <key>\n"
        "  <key>=<value>   -- Set <key> to text <value>\n"
        "  <key>:<hexdata> -- Set <key> to binary value represented as hex\n"
        "  dump            -- Show all currently set keys/values\n"
        "  reformat        -- Reinitialize (clear) the sysparam area\n"
        "  help            -- Show this help screen\n"
        );
}

size_t tty_readline(char *buffer, size_t buf_size, bool echo) {
    size_t i = 0;
    int c;

    while (true) {
        c = getchar();
        if (c == '\r') {
            if (echo) putchar('\n');
            break;
        } else if (c == '\b' || c == 0x7f) {
            if (i) {
                if (echo) printf("\b \b");
                i--;
            }
        } else if (c < 0x20) {
            /* Ignore other control characters */
        } else if (i >= buf_size - 1) {
            if (echo) putchar('\a');
        } else {
            buffer[i++] = c;
            if (echo) putchar(c);
        }
    }

    buffer[i] = 0;
    return i;
}

void print_text_value(char *key, char *value) {
    printf("  '%s' = '%s'\n", key, value);
}

void print_binary_value(char *key, uint8_t *value, size_t len) {
    size_t i;

    printf("  %s:", key);
    for (i = 0; i < len; i++) {
        if (!(i & 0x0f)) {
            printf("\n   ");
        }
        printf(" %02x", value[i]);
    }
    printf("\n");
}

sysparam_status_t dump_params(void) {
    sysparam_status_t status;
    sysparam_iter_t iter;

    status = sysparam_iter_start(&iter);
    if (status < 0) return status;
    while (true) {
        status = sysparam_iter_next(&iter);
        if (status != SYSPARAM_OK) break;
        if (!iter.binary) {
            print_text_value(iter.key, (char *)iter.value);
        } else {
            print_binary_value(iter.key, iter.value, iter.value_len);
        }
    }
    sysparam_iter_end(&iter);

    if (status == SYSPARAM_NOTFOUND) {
        // This is the normal status when we've reached the end of all entries.
        return SYSPARAM_OK;
    } else {
        // Something apparently went wrong
        return status;
    }
}

uint8_t *parse_hexdata(char *string, size_t *result_length) {
    size_t string_len = strlen(string);
    uint8_t *buf = malloc(string_len / 2);
    uint8_t c;
    int i, j;
    bool digit = false;

    j = 0;
    for (i = 0; string[i]; i++) {
        c = string[i];
        if (c >= 0x30 && c <= 0x39) {
            c &= 0x0f;
        } else if (c >= 0x41 && c <= 0x46) {
            c -= 0x37;
        } else if (c >= 0x61 && c <= 0x66) {
            c -= 0x57;
        } else if (c == ' ') {
            continue;
        } else {
            free(buf);
            return NULL;
        }
        if (!digit) {
            buf[j] = c << 4;
        } else {
            buf[j++] |= c;
        }
        digit = !digit;
    }
    if (digit) {
        free(buf);
        return NULL;
    }
    *result_length = j;
    return buf;
}

void sysparam_editor_task(void *pvParameters) {
    char *cmd_buffer = malloc(CMD_BUF_SIZE);
    sysparam_status_t status;
    char *value;
    uint8_t *bin_value;
    size_t len;
    uint8_t *data;

    if (!cmd_buffer) {
        printf("ERROR: Cannot allocate command buffer!\n");
        return;
    }

    printf("\nWelcome to the system parameter editor!  Enter 'help' for more information.\n\n");

    // NOTE: Eventually, this initialization part will be done automatically on
    // system startup, so the app won't need to do it.
    printf("Initializing sysparam...\n");
    status = sysparam_init(SYSPARAM_ADDR, FLASH_TOP);
    printf("(status %d)\n", status);
    if (status == SYSPARAM_NOTFOUND) {
        printf("Trying to create new sysparam area...\n");
        status = sysparam_create_area(SYSPARAM_ADDR, SYSPARAM_SECTORS, false);
        printf("(status %d)\n", status);
        if (status == SYSPARAM_OK) {
            status = sysparam_init(SYSPARAM_ADDR, 0);
            printf("(status %d)\n", status);
        }
    }

    while (true) {
        printf("==> ");
        len = tty_readline(cmd_buffer, CMD_BUF_SIZE, true);
        status = 0;
        if (!len) continue;
        if (cmd_buffer[len - 1] == '?') {
            cmd_buffer[len - 1] = 0;
            printf("Querying '%s'...\n", cmd_buffer);
            status = sysparam_get_string(cmd_buffer, &value);
            if (status == SYSPARAM_OK) {
                print_text_value(cmd_buffer, value);
                free(value);
            } else if (status == SYSPARAM_PARSEFAILED) {
                // This means it's actually a binary value
                status = sysparam_get_data(cmd_buffer, &bin_value, &len, NULL);
                if (status == SYSPARAM_OK) {
                    print_binary_value(cmd_buffer, bin_value, len);
                    free(value);
                }
            }
        } else if ((value = strchr(cmd_buffer, '='))) {
            *value++ = 0;
            printf("Setting '%s' to '%s'...\n", cmd_buffer, value);
            status = sysparam_set_string(cmd_buffer, value);
        } else if ((value = strchr(cmd_buffer, ':'))) {
            *value++ = 0;
            data = parse_hexdata(value, &len);
            if (value) {
                printf("Setting '%s' to binary data...\n", cmd_buffer);
                status = sysparam_set_data(cmd_buffer, data, len, true);
                free(data);
            } else {
                printf("Error: Unable to parse hex data\n");
            }
        } else if (!strcmp(cmd_buffer, "dump")) {
            printf("Dumping all params:\n");
            status = dump_params();
        } else if (!strcmp(cmd_buffer, "reformat")) {
            printf("Re-initializing region...\n");
            status = sysparam_create_area(SYSPARAM_ADDR, SYSPARAM_SECTORS, true);
            if (status == SYSPARAM_OK) {
                // We need to re-init after wiping out the region we've been
                // using.
                status = sysparam_init(SYSPARAM_ADDR, 0);
            }
        } else if (!strcmp(cmd_buffer, "help")) {
            usage();
        } else {
            printf("Unrecognized command.\n\n");
            usage();
        }

        if (status != SYSPARAM_OK) {
            printf("! Operation returned status: %d (%s)\n", status, status_messages[status - status_base]);
        }
    }
}

void user_init(void)
{
    xTaskCreate(sysparam_editor_task, (signed char *)"sysparam_editor_task", 512, NULL, 2, NULL);
}
