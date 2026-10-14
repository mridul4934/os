#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "workload_io.h"

#define NUM_CHILDREN 8

struct child_timing {
    struct timespec start_time;
    struct timespec end_time;
};

int main(void) {
    int pipes[NUM_CHILDREN][2];
    pid_t pids[NUM_CHILDREN];
    struct timespec ref_time;

    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe failed");
            exit(1);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ref_time);

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork failed");
            for (int k = 0; k < i; k++) kill(pids[k], SIGKILL);
            exit(1);
        }

        if (pids[i] == 0) {
            struct child_timing timing;
            clock_gettime(CLOCK_MONOTONIC, &timing.start_time);
            /* Close every pipe end except our own write end. Write ends of
               earlier pipes were already closed by the parent before this fork. */
            for (int k = 0; k < NUM_CHILDREN; k++) {
                close(pipes[k][0]);
                if (k > i) close(pipes[k][1]);
            }

            io_task(i);

            clock_gettime(CLOCK_MONOTONIC, &timing.end_time);
            if (write(pipes[i][1], &timing, sizeof(timing)) != sizeof(timing)) {
                perror("child pipe write failed");
            }
            close(pipes[i][1]);
            exit(0);
        } else {
            close(pipes[i][1]);
        }
    }

    struct child_timing timings[NUM_CHILDREN];
    int got[NUM_CHILDREN];
    int num_ok = 0;
    for (int i = 0; i < NUM_CHILDREN; i++) {
        got[i] = read(pipes[i][0], &timings[i], sizeof(struct child_timing)) == sizeof(struct child_timing);
        if (got[i]) {
            num_ok++;
        } else {
            fprintf(stderr, "Error reading timing from child %d\n", i);
        }
        close(pipes[i][0]);
        waitpid(pids[i], NULL, 0);
    }

    struct timespec parent_end_time;
    clock_gettime(CLOCK_MONOTONIC, &parent_end_time);

    struct rusage usage;
    getrusage(RUSAGE_CHILDREN, &usage);

    double total_elapsed = (parent_end_time.tv_sec - ref_time.tv_sec) +
                           (parent_end_time.tv_nsec - ref_time.tv_nsec) / 1e9;

    double sum_resp = 0.0, max_resp = 0.0;
    double sum_turn = 0.0, max_turn = 0.0;

    if (num_ok == 0) {
        fprintf(stderr, "No timing data received from any child\n");
        return 1;
    }

    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (!got[i]) continue;
        double resp = (timings[i].start_time.tv_sec - ref_time.tv_sec) * 1000.0 +
                      (timings[i].start_time.tv_nsec - ref_time.tv_nsec) / 1e6;
        double turn = (timings[i].end_time.tv_sec - ref_time.tv_sec) * 1000.0 +
                      (timings[i].end_time.tv_nsec - ref_time.tv_nsec) / 1e6;

        sum_resp += resp;
        if (resp > max_resp) max_resp = resp;

        sum_turn += turn;
        if (turn > max_turn) max_turn = turn;
    }

    printf("Involuntary Context Switches: %ld\n", usage.ru_nivcsw);
    printf("Voluntary Context Switches:   %ld\n", usage.ru_nvcsw);
    printf("Average Response Time:        %.2fms\n", sum_resp / num_ok);
    printf("Maximum Response Time:        %.2fms\n", max_resp);
    printf("Average Turnaround Time:      %.2fms\n", sum_turn / num_ok);
    printf("Maximum Turnaround Time:      %.2fms\n", max_turn);
    printf("Throughput:                   %.2f children/s\n", NUM_CHILDREN / total_elapsed);
    printf("Total Elapsed Time:           %.2fs\n", total_elapsed);

    return 0;
}
