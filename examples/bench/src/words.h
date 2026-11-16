#ifndef BENCH_WORDS_H
#define BENCH_WORDS_H

#include <stddef.h>

// the text of a seed, `n` words of it, the same in every framework's app.
void words(char *out, size_t size, unsigned seed, int n);
double cell_value(int cell, int frame);

#endif
