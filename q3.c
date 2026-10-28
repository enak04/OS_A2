#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>

#define GRID_SIZE 9
#define TOTAL_CELLS 81
#define TOTAL_UNITS 27
#define TOTAL_BLOCKS 9

/* Global shared structures */
static int grid[GRID_SIZE][GRID_SIZE];
static int validity[TOTAL_UNITS];
static int global_dup_mask[TOTAL_UNITS];
static int heatmap[GRID_SIZE][GRID_SIZE];

/* Phase 1 thread parameters & result structures */
struct unit_params {
    int unit_id; /* 0-8: rows, 9-17: cols, 18-26: sub-grids */
};

struct unit_result {
    int unit_id;
    int valid;         /* 1 = VALID, 0 = INVALID */
    int missing_mask;  /* bit d set => digit d never appears */
    int dup_mask;      /* bit d set => digit d appears twice or more */
    int blank_count;   /* number of empty cells in this unit */
};

/* Phase 2 thread parameter structure */
struct block_params {
    int block_id; /* 0-8, row-major */
};

/* --- Phase 1: Validation Worker --- */
void *validate_unit(void *arg) {
    struct unit_params *params = (struct unit_params *)arg;
    int uid = params->unit_id;

    struct unit_result *res = malloc(sizeof(struct unit_result));
    if (!res) {
        perror("malloc failed");
        pthread_exit(NULL);
    }

    res->unit_id = uid;
    res->missing_mask = 0;
    res->dup_mask = 0;
    res->blank_count = 0;

    int counts[10] = {0};

    for (int i = 0; i < GRID_SIZE; i++) {
        int val = 0;
        if (uid >= 0 && uid <= 8) {
            /* Row uid */
            val = grid[uid][i];
        } else if (uid >= 9 && uid <= 17) {
            /* Column (uid - 9) */
            val = grid[i][uid - 9];
        } else {
            /* Sub-grid (uid - 18) */
            int block = uid - 18;
            int r = (block / 3) * 3 + (i / 3);
            int c = (block % 3) * 3 + (i % 3);
            val = grid[r][c];
        }

        if (val == 0) {
            res->blank_count++;
        } else if (val >= 1 && val <= 9) {
            counts[val]++;
        }
    }

    int is_valid = (res->blank_count == 0);
    for (int d = 1; d <= 9; d++) {
        if (counts[d] == 0) {
            res->missing_mask |= (1 << d);
            is_valid = 0;
        } else if (counts[d] > 1) {
            res->dup_mask |= (1 << d);
            is_valid = 0;
        }
    }

    res->valid = is_valid;
    /* Disjoint write: thread only writes its own assigned index */
    validity[uid] = is_valid;

    pthread_exit((void *)res);
}

/* --- Phase 2: Analytics Worker --- */
void *compute_block_heatmap(void *arg) {
    struct block_params *params = (struct block_params *)arg;
    int b = params->block_id;

    int start_r = (b / 3) * 3;
    int start_c = (b % 3) * 3;
    int grid_uid = 18 + b;

    for (int r = start_r; r < start_r + 3; r++) {
        for (int c = start_c; c < start_c + 3; c++) {
            int v = grid[r][c];
            if (v == 0) {
                heatmap[r][c] = 3;
            } else {
                int row_uid = r;
                int col_uid = 9 + c;

                int row_dup = (global_dup_mask[row_uid] & (1 << v)) ? 1 : 0;
                int col_dup = (global_dup_mask[col_uid] & (1 << v)) ? 1 : 0;
                int blk_dup = (global_dup_mask[grid_uid] & (1 << v)) ? 1 : 0;

                heatmap[r][c] = row_dup + col_dup + blk_dup;
            }
        }
    }

    pthread_exit(NULL);
}

/* Helper to parse grid input */
static int read_grid(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
        return -1;
    }

    int count = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (isspace(ch)) {
            continue;
        }

        int val = -1;
        if (ch == '0' || ch == '.') {
            val = 0;
        } else if (ch >= '1' && ch <= '9') {
            val = ch - '0';
        } else {
            fprintf(stderr, "Error: Invalid character '%c' in input file\n", ch);
            fclose(fp);
            return -1;
        }

        if (count < TOTAL_CELLS) {
            grid[count / GRID_SIZE][count % GRID_SIZE] = val;
        }
        count++;
    }

    fclose(fp);

    if (count != TOTAL_CELLS) {
        fprintf(stderr, "Error: Input must contain exactly 81 cells (found %d)\n", count);
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <sudoku_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (read_grid(argv[1]) != 0) {
        return EXIT_FAILURE;
    }

    /* 1. Input grid echoed */
    printf("Input Grid:\n");
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (grid[r][c] == 0) {
                printf(". ");
            } else {
                printf("%d ", grid[r][c]);
            }
        }
        printf("\n");
    }
    printf("\n");

    /* Phase 1: Validation */
    pthread_t val_threads[TOTAL_UNITS];
    struct unit_params val_params[TOTAL_UNITS];

    for (int i = 0; i < TOTAL_UNITS; i++) {
        val_params[i].unit_id = i;
        if (pthread_create(&val_threads[i], NULL, validate_unit, &val_params[i]) != 0) {
            perror("Failed to create validation thread");
            return EXIT_FAILURE;
        }
    }

    struct unit_result *results[TOTAL_UNITS];
    int valid_units_count = 0;

    for (int i = 0; i < TOTAL_UNITS; i++) {
        void *ret_val = NULL;
        pthread_join(val_threads[i], &ret_val);
        results[i] = (struct unit_result *)ret_val;
        if (results[i]->valid) {
            valid_units_count++;
        }
        global_dup_mask[i] = results[i]->dup_mask;
    }

    /* 2. VALID or INVALID */
    if (valid_units_count == TOTAL_UNITS) {
        printf("VALID\n");
        for (int i = 0; i < TOTAL_UNITS; i++) {
            free(results[i]);
        }
        return EXIT_SUCCESS;
    }

    printf("INVALID\n\n");

    /* Phase 2: Analytics */

    /* 3. Validity Score */
    double score = ((double)valid_units_count / TOTAL_UNITS) * 100.0;
    printf("Validity Score: %.2f%%\n\n", score);

    /* Spawn 9 block analytics threads */
    pthread_t analytics_threads[TOTAL_BLOCKS];
    struct block_params a_params[TOTAL_BLOCKS];

    for (int b = 0; b < TOTAL_BLOCKS; b++) {
        a_params[b].block_id = b;
        if (pthread_create(&analytics_threads[b], NULL, compute_block_heatmap, &a_params[b]) != 0) {
            perror("Failed to create analytics thread");
            return EXIT_FAILURE;
        }
    }

    for (int b = 0; b < TOTAL_BLOCKS; b++) {
        pthread_join(analytics_threads[b], NULL);
    }

    /* 4. Conflict Density Heatmap */
    printf("Conflict Density Heatmap:\n");
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            printf("%d ", heatmap[r][c]);
        }
        printf("\n");
    }
    printf("\n");

    /* 5. Hotspot Report */
    printf("Hotspot Report:\n");
    int hotspot_count = 0;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (heatmap[r][c] == 3) {
                printf("Cell (%d, %d) value=%d\n", r, c, grid[r][c]);
                hotspot_count++;
            }
        }
    }
    printf("Total hotspots: %d\n\n", hotspot_count);

    /* 6. Missing Digit Report */
    printf("Missing Digit Report:\n");
    for (int i = 0; i < TOTAL_UNITS; i++) {
        if (!results[i]->valid) {
            const char *type = "";
            int idx = 0;

            if (i <= 8) {
                type = "Row";
                idx = i;
            } else if (i <= 17) {
                type = "Column";
                idx = i - 9;
            } else {
                type = "Sub-grid";
                idx = i - 18;
            }

            printf("%s %d: missing { ", type, idx);
            int missing_count = 0;
            int last_missing_digit = 0;

            for (int d = 1; d <= 9; d++) {
                if (results[i]->missing_mask & (1 << d)) {
                    printf("%d ", d);
                    missing_count++;
                    last_missing_digit = d;
                }
            }
            printf("}, blanks: %d", results[i]->blank_count);

            /* Repair Advisor */
            if (results[i]->blank_count == 1 && missing_count == 1) {
                printf(" -> [Repair: cell needs digit %d]", last_missing_digit);
            }
            printf("\n");
        }
    }

    /* Clean up all heap-allocated results */
    for (int i = 0; i < TOTAL_UNITS; i++) {
        free(results[i]);
    }

    return EXIT_SUCCESS;
}