/* =========================================================================
 * GEN_DATA — генератор тестовых данных для roundtrip-тестов
 * =========================================================================
 *
 * Usage:
 *   gen_data <dataset_id> <length> <output_file>
 *
 * dataset_id: 0=LOREM 1=CCODE 2=ENGLISH 3=RUSSIAN 4=REPEAT 5=RANDOM
 *
 * Использует test_data.h для текстовых наборов, LCG для RANDOM.
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "test_data.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <dataset_id 0-5> <length> <output_file>\n", argv[0]);
        return 1;
    }

    int id = atoi(argv[1]);
    size_t len = (size_t)strtoull(argv[2], NULL, 10);

    if (id < 0 || id >= N_SOURCES) {
        fprintf(stderr, "dataset_id must be 0..%d\n", N_SOURCES - 1);
        return 1;
    }

    uint8_t *buf = make_dataset((dataset_id)id, len);
    if (!buf) { fprintf(stderr, "make_dataset failed\n"); return 1; }

    FILE *f = fopen(argv[3], "wb");
    if (!f) { perror("fopen output"); free(buf); return 1; }
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "fwrite failed\n"); fclose(f); free(buf); return 1;
    }
    fclose(f);
    free(buf);
    return 0;
}