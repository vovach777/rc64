/* =========================================================================
 * TEST DATA — наборы данных для тестирования range coder'а
 * =========================================================================
 *
 * Источник: rc_guillotine_bench.c (в котором лежат самые полные наборы).
 * 5 текстовых + 1 бинарный (генерируется в рантайме).
 *
 * Использование:
 *   #include "test_data.h"
 *   for (int s = 0; s < N_SOURCES; s++) {
 *       size_t len = sources[s].len;       // рекомендуемая длина
 *       uint8_t *buf = make_dataset(s, target_len);
 *       ...
 *       free(buf);
 *   }
 * ========================================================================= */

#ifndef TEST_DATA_H
#define TEST_DATA_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== */
/* ТЕКСТОВЫЕ ДАННЫЕ (статические строки)                                   */
/* ===================================================================== */

static const char LOREM[] =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod "
    "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim "
    "veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea "
    "commodo consequat. Duis aute irure dolor in reprehenderit in voluptate "
    "velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint "
    "occaecat cupidatat non proident, sunt in culpa qui officia deserunt "
    "mollit anim id est laborum. At vero eos et accusam et justo duo dolores "
    "et ea rebum. Stet clita kasd gubergren, no sea takimata sanctus est "
    "Lorem ipsum dolor sit amet. Lorem ipsum dolor sit amet, consetetur "
    "sadipscing elitr, sed diam nonumy eirmod tempor invidunt ut labore et "
    "dolore magna aliquyam erat, sed diam voluptua. ";

static const char CCODE[] =
    "int main(int argc, char** argv) {\n"
    "    if (argc < 2) { fprintf(stderr, \"Usage: %s <file>\\n\", argv[0]); return 1; }\n"
    "    FILE* f = fopen(argv[1], \"rb\");\n"
    "    if (!f) { perror(\"fopen\"); return 1; }\n"
    "    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);\n"
    "    char* buf = malloc(sz + 1);\n"
    "    fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);\n"
    "    for (long i = 0; i < sz; i++) {\n"
    "        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;\n"
    "        putchar(buf[i]);\n"
    "    }\n"
    "    free(buf);\n"
    "    return 0;\n"
    "}\n"
    "static inline uint32_t hash32(const void* key, size_t len) {\n"
    "    const uint8_t* p = (const uint8_t*)key;\n"
    "    uint32_t h = 2166136261u;\n"
    "    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }\n"
    "    return h;\n"
    "}\n";

static const char ENGLISH[] =
    "The quick brown fox jumps over the lazy dog. Pack my box with five dozen "
    "liquor jugs. How vexingly quick daft zebras jump! The five boxing wizards "
    "jump quickly. Sphinx of black quartz, judge my vow. Two driven jocks help "
    "fax my big quiz. A wizard's job is to vex chumps quickly in fog. Waltz, "
    "bad nymph, for quick jigs vex. Glib jocks quiz nymph to vex dwarf. Quick "
    "zephyrs blow, vexing daft Jim. Heavy boxes perform quick waltzes and jigs. "
    "The job requires extra pluck and zeal from every young wage earner. ";

static const char RUSSIAN[] =
    "В чащах юга жил бы цитрус? Да, но фальшивый экземпляр! Съешь же ещё "
    "этих мягких французских булок, да выпей чаю. Любя, съешь щипцы, "
    "взмахнёт призывно мэр. Флегматичная вялая дева ютилась в чулане, "
    "жуя хрящи, булку, джем, яйцо. Эй, ёж, приди и вынеси знойный кубок "
    "с молоком! Широкая электрификация южных губерний даст мощный толчок "
    "к подъёму сельского хозяйства. Встраиваемая в киберпространство "
    "система обнаружила несоответствие в формате кодирования данных. "
    "Москва — столица Российской Федерации, город федерального значения. "
    "Программисты всех стран, соединяйтесь! Алгоритмы сжатия данных "
    "проходят этап интенсивного развития уже более семидесяти лет подряд. ";

static const char REPEAT[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "abababababababababababababababababababababababababababababababababababab"
    "abababababababababababababababababababababababababababababababababababab"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

/* ===================================================================== */
/* ОПИСАНИЕ НАБОРОВ                                                        */
/* ===================================================================== */

typedef enum {
    DS_LOREM = 0,
    DS_CCODE,
    DS_ENGLISH,
    DS_RUSSIAN,
    DS_REPEAT,
    DS_RANDOM,           /* генерируется в рантайме (равномерный 0..255) */
    N_SOURCES
} dataset_id;

typedef struct {
    const char *name;
    const char *text;    /* NULL для DS_RANDOM */
    size_t       len;    /* длина текста (для DS_RANDOM игнорируется) */
} dataset_desc;

static const dataset_desc sources[] = {
    { "Lorem Ipsum",     LOREM,   sizeof(LOREM)   - 1 },
    { "C source",       CCODE,   sizeof(CCODE)   - 1 },
    { "English",         ENGLISH, sizeof(ENGLISH) - 1 },
    { "Russian (UTF-8)", RUSSIAN, sizeof(RUSSIAN) - 1 },
    { "Repetitive",      REPEAT,  sizeof(REPEAT)  - 1 },
    { "Random binary",   NULL,    0                       },
};

/* ===================================================================== */
/* ГЕНЕРАТОР: циклически повторяет текст до target_len                     */
/* ===================================================================== */

/* PRNG (Numerical Recipes LCG) для DS_RANDOM */
static uint64_t _td_rng_state = 0x123456789ABCDEF0ULL;
static inline uint32_t _td_rng_next(void) {
    _td_rng_state = 6364136223846793005ULL * _td_rng_state + 1442695040888963407ULL;
    return (uint32_t)(_td_rng_state >> 32);
}

/* Вернуть malloc'нутый буфер размером target_len байт.
   Для текстовых — циклическое повторение src.
   Для DS_RANDOM — равномерный поток 0..255. */
static uint8_t *make_dataset(dataset_id id, size_t target_len) {
    uint8_t *out = (uint8_t *)malloc(target_len);
    if (!out) return NULL;

    if (id == DS_RANDOM) {
        for (size_t i = 0; i < target_len; i++)
            out[i] = (uint8_t)(_td_rng_next() & 0xFF);
        return out;
    }

    const char *src = sources[id].text;
    size_t src_len  = sources[id].len;
    size_t i = 0;
    while (i < target_len) {
        size_t chunk = src_len;
        if (i + chunk > target_len) chunk = target_len - i;
        memcpy(out + i, src, chunk);
        i += chunk;
    }
    return out;
}

#endif /* TEST_DATA_H */