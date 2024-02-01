#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAX 1000

struct ps_info {
    char comm[16];
    pid_t pid;
    volatile long state;
    unsigned long long sum_exec_runtime;
};

struct ness_info {
    char comm[16];
    pid_t pid;
    double CPU_usage;
    double exec_runtime;
    bool isruning;
};



int main(int argc, char *argv[]) {
    struct ps_info all_ps_1[MAX];
    struct ps_info all_ps_2[MAX];
    struct ness_info all_info[MAX];

    int total_num_1;
    int total_num_2;
    int period;


    if (argc == 1) {
        period = 1;
    }
    else if (argc == 2) {
        sscanf(argv[1], "-%d", &period);
    }

    while (1) {
        syscall(332, &total_num_1, all_ps_1);

        sleep(period);

        syscall(332, &total_num_2, all_ps_2);


        int i, j;
        for (i = 0; i < total_num_2; i++) {
            bool flag = 0;
            for (j = 0; j < total_num_1; j++) {
                if (all_ps_1[j].pid == all_ps_2[i].pid) {
                    flag = 1;
                    break;
                }
            }

            all_info[i].pid = all_ps_2[i].pid;

            all_info[i].exec_runtime =
                ((double)all_ps_2[i].sum_exec_runtime) / 1000000000;

            if (all_ps_2[i].state == 0) {
                all_info[i].isruning = true;
            }
            else {
                all_info[i].isruning = false;
            }

            strcpy(all_info[i].comm, all_ps_2[i].comm);


            if (flag == 1) {
                all_info[i].CPU_usage =
                    ((double)(all_ps_2[i].sum_exec_runtime -
                              all_ps_1[j].sum_exec_runtime)) /
                    (1000000000 * period);
            }
            else {
                all_info[i].CPU_usage = ((double)all_ps_2[i].sum_exec_runtime) /
                                        (1000000000 * period);
            }
        }



        for (int i = 0; i < total_num_2 - 1; i++) {
            for (int j = 0; j < total_num_2 - i - 1; j++) {
                if (all_info[j].CPU_usage < all_info[j + 1].CPU_usage) {
                    struct ness_info temp;

                    strcpy(temp.comm, all_info[j].comm);
                    temp.pid = all_info[j].pid;
                    temp.exec_runtime = all_info[j].exec_runtime;
                    temp.isruning = all_info[j].isruning;
                    temp.CPU_usage = all_info[j].CPU_usage;

                    strcpy(all_info[j].comm, all_info[j + 1].comm);
                    all_info[j].pid = all_info[j + 1].pid;
                    all_info[j].exec_runtime = all_info[j + 1].exec_runtime;
                    all_info[j].isruning = all_info[j + 1].isruning;
                    all_info[j].CPU_usage = all_info[j + 1].CPU_usage;

                    strcpy(all_info[j + 1].comm, temp.comm);
                    all_info[j + 1].pid = temp.pid;
                    all_info[j + 1].exec_runtime = temp.exec_runtime;
                    all_info[j + 1].isruning = temp.isruning;
                    all_info[j + 1].CPU_usage = temp.CPU_usage;
                }
            }
        } //排序

        // printf("period is %d\n",period);
        system("clear");

        printf("PID    COMM              ISRUNNING   %%CPU      TIME\n");
        for (int i = 0; i < 20; i++) {
            printf("%-5d  %-16s  %-2d          %-6.5f   %-6.5f\n",
                   all_info[i].pid, all_info[i].comm, all_info[i].isruning,
                   all_info[i].CPU_usage, all_info[i].exec_runtime);
        }

        // printf("%.6f", interval);
    }



    return 0;
}
