#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>
#include <string.h>

#define GRID_SIZE 9
#define NUM_UNITS 27
#define NUM_BLOCKS 9

int grid[GRID_SIZE][GRID_SIZE];
int validity[NUM_UNITS];
int global_dup_mask[NUM_UNITS];
int global_missing_mask[NUM_UNITS];
int global_blank_count[NUM_UNITS];
int heatmap[GRID_SIZE][GRID_SIZE];

struct unit_params {
    int unit_id;
};

struct unit_result {
    int unit_id;
    int valid;
    int missing_mask;
    int dup_mask;
    int blank_count;
};

struct block_params {
    int block_id;
};

void* validate_unit(void *arg) {
    struct unit_params *params = (struct unit_params*)arg;
    int uid = params->unit_id;
    free(params);

    struct unit_result *res = malloc(sizeof(struct unit_result));
    if (!res) pthread_exit(NULL);

    res->unit_id = uid;
    res->missing_mask = 0;
    res->dup_mask = 0;
    res->blank_count = 0;
    res->valid = 1;

    int counts[10] = {0};

    for (int idx = 0; idx < GRID_SIZE; idx++) {
        int val = 0;
        if (uid < 9) {
            val = grid[uid][idx];
        } else if (uid < 18) {
            val = grid[idx][uid - 9];
        } else {
            int b_row = (uid - 18) / 3;
            int b_col = (uid - 18) % 3;
            int r = b_row * 3 + idx / 3;
            int c = b_col * 3 + idx % 3;
            val = grid[r][c];
        }

        if (val == 0) {
            res->blank_count++;
        } else if (val >= 1 && val <= 9) {
            counts[val]++;
        }
    }

    for (int d = 1; d <= 9; d++) {
        if (counts[d] == 0) {
            res->missing_mask |= (1 << d);
        } else if (counts[d] > 1) {
            res->dup_mask |= (1 << d);
        }
    }

    if (res->blank_count > 0 || res->dup_mask != 0) {
        res->valid = 0;
    }

    validity[uid] = res->valid;
    pthread_exit(res);
}

void* analyze_block(void *arg) {
    struct block_params *params = (struct block_params*)arg;
    int bid = params->block_id;
    free(params);

    int b_row = bid / 3;
    int b_col = bid % 3;

    for (int r_off = 0; r_off < 3; r_off++) {
        for (int c_off = 0; c_off < 3; c_off++) {
            int r = b_row * 3 + r_off;
            int c = b_col * 3 + c_off;
            int val = grid[r][c];

            if (val == 0) {
                heatmap[r][c] = 3;
            } else {
                int score = 0;
                int row_uid = r;
                int col_uid = 9 + c;
                int sub_uid = 18 + (b_row * 3 + b_col);

                if (global_dup_mask[row_uid] & (1 << val)) score++;
                if (global_dup_mask[col_uid] & (1 << val)) score++;
                if (global_dup_mask[sub_uid] & (1 << val)) score++;

                heatmap[r][c] = score;
            }
        }
    }
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <grid_file>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        perror("Error opening grid file");
        return 1;
    }

    int cell_count = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (isspace(ch)) continue;
        if (ch == '0' || ch == '.') {
            if (cell_count < 81) {
                grid[cell_count / 9][cell_count % 9] = 0;
            }
            cell_count++;
        } else if (isdigit(ch)) {
            if (cell_count < 81) {
                grid[cell_count / 9][cell_count % 9] = ch - '0';
            }
            cell_count++;
        } else {
            fprintf(stderr, "Error: Invalid character '%c' in grid file.\n", ch);
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);

    if (cell_count != 81) {
        fprintf(stderr, "Error: Expected 81 cells, found %d.\n", cell_count);
        return 1;
    }

    printf("Input Grid:\n");
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            printf("%d ", grid[r][c]);
        }
        printf("\n");
    }
    printf("\n");

    pthread_t val_threads[NUM_UNITS];
    int created_val = 0;
    for (int i = 0; i < NUM_UNITS; i++) {
        struct unit_params *p = malloc(sizeof(struct unit_params));
        if (!p) {
            perror("malloc failed");
            for (int k = 0; k < created_val; k++) pthread_join(val_threads[k], NULL);
            return 1;
        }
        p->unit_id = i;
        if (pthread_create(&val_threads[i], NULL, validate_unit, p) != 0) {
            fprintf(stderr, "Failed to create validation thread %d\n", i);
            free(p);
            for (int k = 0; k < created_val; k++) pthread_join(val_threads[k], NULL);
            return 1;
        }
        created_val++;
    }

    int valid_count = 0;
    for (int i = 0; i < NUM_UNITS; i++) {
        void *res_ptr;
        if (pthread_join(val_threads[i], &res_ptr) != 0 || !res_ptr) {
            fprintf(stderr, "Failed to join thread or null result for unit %d\n", i);
            return 1;
        }
        struct unit_result *res = (struct unit_result*)res_ptr;
        global_dup_mask[res->unit_id] = res->dup_mask;
        global_missing_mask[res->unit_id] = res->missing_mask;
        global_blank_count[res->unit_id] = res->blank_count;

        if (res->valid) {
            valid_count++;
        }
        free(res);
    }

    if (valid_count == NUM_UNITS) {
        printf("VALID\n");
        return 0;
    }

    printf("INVALID\n");
    double score_pct = ((double)valid_count / NUM_UNITS) * 100.0;
    printf("Validity Score: %.2f%%\n\n", score_pct);

    pthread_t block_threads[NUM_BLOCKS];
    int created_blocks = 0;
    for (int i = 0; i < NUM_BLOCKS; i++) {
        struct block_params *bp = malloc(sizeof(struct block_params));
        if (!bp) {
            perror("malloc failed");
            for (int k = 0; k < created_blocks; k++) pthread_join(block_threads[k], NULL);
            return 1;
        }
        bp->block_id = i;
        if (pthread_create(&block_threads[i], NULL, analyze_block, bp) != 0) {
            fprintf(stderr, "Failed to create block thread %d\n", i);
            free(bp);
            for (int k = 0; k < created_blocks; k++) pthread_join(block_threads[k], NULL);
            return 1;
        }
        created_blocks++;
    }

    for (int i = 0; i < NUM_BLOCKS; i++) {
        pthread_join(block_threads[i], NULL);
    }

    printf("Conflict Density Heatmap:\n");
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            printf("%d", heatmap[r][c]);
        }
        printf("\n");
    }
    printf("\n");

    printf("Hotspots (score 3):\n");
    int hotspot_count = 0;
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            if (heatmap[r][c] == 3) {
                printf("(%d,%d) value=%d\n", r, c, grid[r][c]);
                hotspot_count++;
            }
        }
    }
    printf("Total Hotspots: %d\n\n", hotspot_count);

    printf("Missing Digit Report:\n");
    for (int i = 0; i < NUM_UNITS; i++) {
        if (validity[i] == 0) {
            char type_str[16];
            int idx = 0;
            if (i < 9) {
                strcpy(type_str, "Row");
                idx = i;
            } else if (i < 18) {
                strcpy(type_str, "Column");
                idx = i - 9;
            } else {
                strcpy(type_str, "Sub-grid");
                idx = i - 18;
            }

            printf("%-8s %2d : missing (", type_str, idx);
            int first = 1;
            int missing_digit = 0;
            int missing_cnt = 0;
            for (int d = 1; d <= 9; d++) {
                if (global_missing_mask[i] & (1 << d)) {
                    printf("%s%d", first ? "" : ", ", d);
                    first = 0;
                    missing_digit = d;
                    missing_cnt++;
                }
            }
            printf(")   blanks=%d", global_blank_count[i]);

            if (global_blank_count[i] == 1 && missing_cnt == 1) {
                printf("  [Advisor: Place %d in blank cell]", missing_digit);
            }
            printf("\n");
        }
    }

    return 0;
}
