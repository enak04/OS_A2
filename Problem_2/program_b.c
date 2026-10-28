/*
 * Program-B (I/O-Bound): forks 8 children, each running the task in workload_io.h.
 *
 * The parent records one reference timestamp before forking. Each child records its
 * own start (first action after fork) and completion (before exit) and sends both to
 * the parent through a pipe created before fork(). The parent derives response and
 * turnaround times and prints one results block (format matches the sample output).
 *
 * Build:  gcc -O2 -Wall -o program_b program_b.c
 * Run:    sudo taskset -c 0 chrt -r 10 ./program_b          (SCHED_RR)
 *         sudo timeout 30 taskset -c 0 chrt -f 10 ./program_b   (SCHED_FIFO)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "workload_io.h"

#define NUM_CHILDREN 8

/* If the provided header uses a different function name, change it here only. */
#define RUN_WORKLOAD(int) io_task(int)

typedef struct {
    int id;
    struct timespec start;
    struct timespec end;
} report_t;

/* a - b in milliseconds */
static double diff_ms(struct timespec a, struct timespec b)
{
    return (a.tv_sec - b.tv_sec) * 1e3 + (a.tv_nsec - b.tv_nsec) / 1e6;
}

static int read_quantum_ms(void)
{
    int v = -1;
    FILE *f = fopen("/proc/sys/kernel/sched_rr_timeslice_ms", "r");
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

int main(void)
{
    int fd[2];
    pid_t pids[NUM_CHILDREN];
    struct timespec ref, done;

    if (pipe(fd) < 0) {
        perror("pipe");
        return 1;
    }

    fflush(stdout);
    fflush(stderr);

    /* One reference timestamp, taken before forking */
    clock_gettime(CLOCK_MONOTONIC, &ref);

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            report_t r;
            clock_gettime(CLOCK_MONOTONIC, &r.start); /* first action after fork() */
            close(fd[0]);

            RUN_WORKLOAD(pid);

            r.id = i;
            clock_gettime(CLOCK_MONOTONIC, &r.end);   /* just before exit */
            if (write(fd[1], &r, sizeof r) != (ssize_t)sizeof r)
                _exit(2);
            close(fd[1]);
            _exit(0);
        }
        pids[i] = pid;
    }

    close(fd[1]);

    /* Wait for every child, then stop the clock */
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (waitpid(pids[i], NULL, 0) < 0)
            perror("waitpid");
    }
    clock_gettime(CLOCK_MONOTONIC, &done);
    double elapsed_ms = diff_ms(done, ref);

    /* Aggregated context switches of all terminated children */
    struct rusage usage;
    if (getrusage(RUSAGE_CHILDREN, &usage) < 0) {
        perror("getrusage");
        return 1;
    }

    /* Collect the reports from the pipe */
    report_t reports[NUM_CHILDREN];
    int got = 0;
    while (got < NUM_CHILDREN) {
        ssize_t n = read(fd[0], &reports[got], sizeof(report_t));
        if (n == (ssize_t)sizeof(report_t))
            got++;
        else if (n <= 0)
            break;
    }
    close(fd[0]);

    if (got != NUM_CHILDREN) {
        fprintf(stderr, "Only received %d/%d child reports\n", got, NUM_CHILDREN);
        return 1;
    }

    double resp_sum = 0, resp_max = 0, turn_sum = 0, turn_max = 0;
    for (int i = 0; i < NUM_CHILDREN; i++) {
        double resp = diff_ms(reports[i].start, ref);
        double turn = diff_ms(reports[i].end, ref);
        resp_sum += resp;
        turn_sum += turn;
        if (resp > resp_max) resp_max = resp;
        if (turn > turn_max) turn_max = turn;
    }

    double elapsed_s = elapsed_ms / 1000.0;

    int policy = sched_getscheduler(0);
    if (policy == SCHED_FIFO)
        printf("Scheduling Policy: SCHED_FIFO\n");
    else
        printf("Time Quantum: %dms\n", read_quantum_ms());

    printf("  %-32s%ld\n",    "Involuntary Context Switches:", usage.ru_nivcsw);
    printf("  %-32s%ld\n",    "Voluntary Context Switches:",   usage.ru_nvcsw);
    printf("  %-32s%.2fms\n", "Average Response Time:",   resp_sum / NUM_CHILDREN);
    printf("  %-32s%.2fms\n", "Maximum Response Time:",   resp_max);
    printf("  %-32s%.2fms\n", "Average Turnaround Time:", turn_sum / NUM_CHILDREN);
    printf("  %-32s%.2fms\n", "Maximum Turnaround Time:", turn_max);
    printf("  %-32s%.2f children/s\n", "Throughput:", NUM_CHILDREN / elapsed_s);
    printf("  %-32s%.2fs\n",  "Total Elapsed Time:", elapsed_s);

    return 0;
}
