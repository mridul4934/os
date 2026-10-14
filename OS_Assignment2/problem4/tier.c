#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>

void print_usage(void) {
    printf("Usage:\n");
    printf("  tier set <username> <gold|silver|bronze>\n");
    printf("  tier reset\n");
    printf("  tier show\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "show") == 0) {
        FILE *fp1 = fopen("/proc/sched_tier", "r");
        if (!fp1) {
            fprintf(stderr, "Error: /proc/sched_tier does not exist. Is module loaded?\n");
            return 1;
        }
        printf("--- User Scheduling Tiers (/proc/sched_tier) ---\n");
        char line[256];
        while (fgets(line, sizeof(line), fp1)) {
            printf("%s", line);
        }
        fclose(fp1);

        FILE *fp2 = fopen("/proc/fork_guard", "r");
        if (!fp2) {
            fprintf(stderr, "Error: /proc/fork_guard does not exist.\n");
            return 1;
        }
        printf("\n--- Containment Stats (/proc/fork_guard) ---\n");
        while (fgets(line, sizeof(line), fp2)) {
            printf("%s", line);
        }
        fclose(fp2);
        return 0;
    }

    if (strcmp(argv[1], "reset") == 0) {
        FILE *fp = fopen("/proc/sched_tier", "w");
        if (!fp) {
            perror("Error writing to /proc/sched_tier");
            return 1;
        }
        fprintf(fp, "RESET\n");
        fclose(fp);
        printf("All users reset to SILVER tier.\n");
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            print_usage();
            return 1;
        }
        char *username = argv[2];
        char *tier_str = argv[3];

        char upper_tier[16];
        strncpy(upper_tier, tier_str, sizeof(upper_tier) - 1);
        upper_tier[sizeof(upper_tier) - 1] = '\0';
        for (int i = 0; upper_tier[i]; i++) {
            if (upper_tier[i] >= 'a' && upper_tier[i] <= 'z')
                upper_tier[i] -= 32;
        }

        struct passwd *pw = getpwnam(username);
        if (!pw) {
            fprintf(stderr, "Error: User '%s' not found.\n", username);
            return 1;
        }

        FILE *fp = fopen("/proc/sched_tier", "w");
        if (!fp) {
            perror("Error writing to /proc/sched_tier");
            return 1;
        }
        fprintf(fp, "SET %u %s\n", pw->pw_uid, upper_tier);
        fclose(fp);

        printf("User %s (UID %u) set to %s tier.\n", username, pw->pw_uid, upper_tier);
        return 0;
    }

    print_usage();
    return 1;
}
