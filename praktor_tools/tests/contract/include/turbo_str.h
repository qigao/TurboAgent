#ifndef TURBO_STR_H
#define TURBO_STR_H
typedef char *tstr_t;
tstr_t tstr_dup(const char *text);
void tstr_free(tstr_t text);
#endif
