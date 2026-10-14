#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#define MAX_SUBJECTS 32
#define MAX_STUDENTS 128

typedef struct {
    int student_id;
    double average;
    int minimum;
    int maximum;
    int subject_marks[MAX_SUBJECTS];
} student_stats;

struct thread_params {
    int thread_id;
    int start_index;
    int segment_size;
};

static int num_subjects = 0;
static int num_students = 0;
static int raw_marks[MAX_SUBJECTS][MAX_STUDENTS];
static student_stats students[MAX_STUDENTS];
static student_stats sorted_students[MAX_STUDENTS];
static int active_sort_key = -1;

static double get_key_value(const student_stats *s, int key) {
    if (key == -3) return (double)s->maximum;
    if (key == -2) return (double)s->minimum;
    if (key == -1) return s->average;
    if (key >= 0 && key < num_subjects) return (double)s->subject_marks[key];
    return 0.0;
}

static int compare_students(const student_stats *a, const student_stats *b, int key) {
    double valA = get_key_value(a, key);
    double valB = get_key_value(b, key);
    if (valA > valB) return -1;
    if (valA < valB) return 1;
    if (a->student_id < b->student_id) return -1;
    if (a->student_id > b->student_id) return 1;
    return 0;
}

void* aggregate_worker(void *arg) {
    int s_id = *(int*)arg;
    free(arg);

    students[s_id].student_id = s_id;
    int sum = 0;
    int min_m = 100000;
    int max_m = -100000;

    for (int sub = 0; sub < num_subjects; sub++) {
        int mark = raw_marks[sub][s_id];
        students[s_id].subject_marks[sub] = mark;
        sum += mark;
        if (mark < min_m) min_m = mark;
        if (mark > max_m) max_m = mark;
    }
    students[s_id].minimum = min_m;
    students[s_id].maximum = max_m;
    students[s_id].average = (double)sum / num_subjects;

    pthread_exit(NULL);
}

void* sort_worker(void *arg) {
    struct thread_params *params = (struct thread_params*)arg;
    int start = params->start_index;
    int end = start + params->segment_size;

    for (int i = start; i < end - 1; i++) {
        for (int j = i + 1; j < end; j++) {
            if (compare_students(&students[j], &students[i], active_sort_key) < 0) {
                student_stats temp = students[i];
                students[i] = students[j];
                students[j] = temp;
            }
        }
    }
    pthread_exit(NULL);
}

static void flush_stdin(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

int main(void) {
    FILE *fp = fopen("input.txt", "r");
    if (!fp) {
        perror("Error opening input.txt");
        return 1;
    }

    if (fscanf(fp, "%d", &num_subjects) != 1 || fscanf(fp, "%d", &num_students) != 1) {
        fprintf(stderr, "Error reading matrix dimensions from input.txt\n");
        fclose(fp);
        return 1;
    }

    if (num_subjects <= 0 || num_subjects > MAX_SUBJECTS || num_students <= 0 || num_students > MAX_STUDENTS) {
        fprintf(stderr, "Error: Invalid dimensions in input.txt\n");
        fclose(fp);
        return 1;
    }

    for (int i = 0; i < num_subjects; i++) {
        for (int j = 0; j < num_students; j++) {
            if (fscanf(fp, "%d", &raw_marks[i][j]) != 1) {
                fprintf(stderr, "Error reading mark at (%d, %d)\n", i, j);
                fclose(fp);
                return 1;
            }
        }
    }
    fclose(fp);

    pthread_t agg_threads[MAX_STUDENTS];
    for (int i = 0; i < num_students; i++) {
        int *s_id = malloc(sizeof(int));
        if (!s_id) {
            perror("malloc failed");
            return 1;
        }
        *s_id = i;
        if (pthread_create(&agg_threads[i], NULL, aggregate_worker, s_id) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }

    for (int i = 0; i < num_students; i++) {
        pthread_join(agg_threads[i], NULL);
    }

    int sort_key, k;
    while (1) {
        printf("Enter sort key (-3=max, -2=min, -1=avg, 0..%d subject index): ", num_subjects - 1);
        int res = scanf("%d", &sort_key);
        if (res == EOF) {
            printf("\nEOF reached. Exiting.\n");
            return 0;
        }
        if (res != 1) {
            flush_stdin();
            printf("Invalid input. Enter an integer.\n");
            continue;
        }
        if (sort_key < -3 || sort_key >= num_subjects) {
            printf("Sort key out of range. Try again.\n");
            continue;
        }
        active_sort_key = sort_key;
        break;
    }

    while (1) {
        printf("Enter number of sorting threads (k): ");
        int res = scanf("%d", &k);
        if (res == EOF) {
            printf("\nEOF reached. Exiting.\n");
            return 0;
        }
        if (res != 1) {
            flush_stdin();
            printf("Invalid input. Enter an integer.\n");
            continue;
        }
        if (k < 1 || k > num_students || (num_students % k != 0)) {
            printf("Invalid k. Must be 1..%d and divide %d.\n", num_students, num_students);
            continue;
        }
        break;
    }

    pthread_t sort_threads[MAX_STUDENTS];
    struct thread_params params[MAX_STUDENTS];
    int seg_size = num_students / k;

    for (int i = 0; i < k; i++) {
        params[i].thread_id = i;
        params[i].start_index = i * seg_size;
        params[i].segment_size = seg_size;
        pthread_create(&sort_threads[i], NULL, sort_worker, &params[i]);
    }

    for (int i = 0; i < k; i++) {
        pthread_join(sort_threads[i], NULL);
    }

    int indices[MAX_STUDENTS];
    for (int i = 0; i < k; i++) indices[i] = i * seg_size;

    for (int count = 0; count < num_students; count++) {
        int best_seg = -1;
        for (int i = 0; i < k; i++) {
            if (indices[i] < (i + 1) * seg_size) {
                if (best_seg == -1 || compare_students(&students[indices[i]], &students[indices[best_seg]], active_sort_key) < 0) {
                    best_seg = i;
                }
            }
        }
        sorted_students[count] = students[indices[best_seg]];
        indices[best_seg]++;
    }

    printf("\nReport Card Menu\n");
    int cmd = -1;
    while (1) {
        printf("Enter command (1 <id> = view student, 2 = view all, 0 = exit): ");
        int res = scanf("%d", &cmd);
        if (res == EOF) {
            break;
        }
        if (res != 1) {
            flush_stdin();
            printf("Invalid command.\n");
            continue;
        }
        if (cmd == 0) {
            break;
        }

        if (cmd == 1) {
            int req_id;
            int r_id = scanf("%d", &req_id);
            if (r_id == EOF) break;
            if (r_id != 1) {
                flush_stdin();
                printf("Invalid student ID.\n");
                continue;
            }
            int found = -1;
            for (int i = 0; i < num_students; i++) {
                if (sorted_students[i].student_id == req_id) {
                    found = i;
                    break;
                }
            }
            if (found != -1) {
                student_stats *s = &sorted_students[found];
                printf("Student ID   : %d\n", s->student_id);
                printf("Average      : %.2f\n", s->average);
                printf("Minimum      : %d\n", s->minimum);
                printf("Maximum      : %d\n", s->maximum);
                printf("Subject Marks: [");
                for (int m = 0; m < num_subjects; m++) {
                    printf("%d%s", s->subject_marks[m], (m == num_subjects - 1) ? "" : ", ");
                }
                printf("]\n");
            } else {
                printf("Student ID %d not found.\n", req_id);
            }
        } else if (cmd == 2) {
            for (int i = 0; i < num_students; i++) {
                double val = get_key_value(&sorted_students[i], active_sort_key);
                printf("s%-3d %.2f\n", sorted_students[i].student_id, val);
            }
        } else {
            printf("Unknown command %d.\n", cmd);
        }
    }

    return 0;
}
