#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <ctype.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>

#define STATS_FILE "/proc/fork_guard"
#define CTL_FILE   "/proc/fork_guard_ctl"

static void print_usage(void) {
    printf("Usage:\n");
    printf("  tier set <username> <gold|silver|bronze>\n");
    printf("  tier reset\n");
    printf("  tier show\n");
}

/* Write one command to the control file; the kernel's verdict arrives at fclose(). */
static int send_cmd(const char *cmd) {
    FILE *fp = fopen(CTL_FILE, "w");
    if (!fp) {
        if (errno == EACCES || errno == EPERM)
            fprintf(stderr, "Error: only root can change tiers (try sudo).\n");
        else
            perror("Error opening " CTL_FILE);
        return -1;
    }
    int failed = fputs(cmd, fp) == EOF;
    if (fclose(fp) != 0 || failed) {
        perror("Error: kernel rejected the command");
        return -1;
    }
    return 0;
}

/* Print the stats file, adding the username next to each per-user UID. */
static int show(void) {
    FILE *fp = fopen(STATS_FILE, "r");
    if (!fp) {
        perror("Error opening " STATS_FILE " (is this the -forkguard kernel?)");
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (isdigit((unsigned char)line[0])) {
            uid_t uid = (uid_t)strtoul(line, NULL, 10);
            struct passwd *pw = getpwuid(uid);
            line[strcspn(line, "\n")] = '\0';
            printf("%s %s\n", line, pw ? pw->pw_name : "?");
        } else if (strncmp(line, "UID", 3) == 0) {
            line[strcspn(line, "\n")] = '\0';
            printf("%s USER\n", line);
        } else {
            fputs(line, stdout);
        }
    }
    fclose(fp);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "show") == 0 && argc == 2)
        return show();

    if (strcmp(argv[1], "reset") == 0 && argc == 2) {
        if (send_cmd("reset\n") != 0)
            return 1;
        printf("All users reset to SILVER tier.\n");
        return 0;
    }

    if (strcmp(argv[1], "set") == 0 && argc == 4) {
        const char *username = argv[2];
        const char *tier = argv[3];

        if (strcasecmp(tier, "gold") != 0 && strcasecmp(tier, "silver") != 0 &&
            strcasecmp(tier, "bronze") != 0) {
            fprintf(stderr, "Error: tier must be gold, silver or bronze.\n");
            return 1;
        }

        struct passwd *pw = getpwnam(username);
        if (!pw) {
            fprintf(stderr, "Error: user '%s' not found.\n", username);
            return 1;
        }

        char cmd[64];
        snprintf(cmd, sizeof(cmd), "set %u %s\n", (unsigned)pw->pw_uid, tier);
        if (send_cmd(cmd) != 0)
            return 1;

        printf("User %s (UID %u) set to %s.\n", username, (unsigned)pw->pw_uid, tier);
        return 0;
    }

    print_usage();
    return 1;
}
