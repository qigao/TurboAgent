#include "tinytest.h"
#include "sds.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define STRICMP _stricmp
#else
  #define STRICMP strcasecmp
#endif

#define SDS_NEW_LITERAL(dst, str)                                                                   \
    do {                                                                                            \
        sds __s = sdsnew((str));                                                                    \
        check_not_null(__s);                                                                        \
        check_str_eq(__s, (str));                                                                   \
        check_size_eq(sdslen(__s), strlen((str)));                                                  \
        (dst) = __s;                                                                                \
    } while (0)

#define SDS_NEW_FILLED(dst, ch, nbytes)                                                             \
    do {                                                                                            \
        size_t __n = (nbytes);                                                                      \
        char *__buf = (char *)malloc(__n);                                                          \
        check_not_null(__buf);                                                                      \
        memset(__buf, (ch), __n);                                                                   \
        sds __s = sdsnewlen(__buf, __n);                                                            \
        free(__buf);                                                                                \
        check_not_null(__s);                                                                        \
        check_size_eq(sdslen(__s), __n);                                                            \
        (dst) = __s;                                                                                \
    } while (0)

static void sds_free_all(sds a, sds b, sds c, sds d) {
    if (a) sdsfree(a);
    if (b) sdsfree(b);
    if (c) sdsfree(c);
    if (d) sdsfree(d);
}

spec("SDS library tests") {
    describe("Basics") {
        it("creates and frees strings") {
            sds s;
            SDS_NEW_LITERAL(s, "Hello World");
            sdsfree(s);
        }

        it("handles empty strings") {
            sds s = sdsempty();
            check_not_null(s);
            check_size_eq(sdslen(s), 0);
            check_str_eq(s, "");
            sdsfree(s);
        }
    }

    describe("Concatenation") {
        it("concatenates C strings") {
            sds s;
            SDS_NEW_LITERAL(s, "Hello");
            s = sdscat(s, " World");
            check_str_eq(s, "Hello World");
            check_size_eq(sdslen(s), (size_t)11);
            sdsfree(s);
        }

        it("concatenates sds strings") {
            sds s1;
            sds s2;
            SDS_NEW_LITERAL(s1, "Part 1");
            SDS_NEW_LITERAL(s2, " & Part 2");
            s1 = sdscatsds(s1, s2);
            check_str_eq(s1, "Part 1 & Part 2");
            sds_free_all(s1, s2, NULL, NULL);
        }
    }

    describe("Comparison") {
        it("matches string.h sign behavior") {
            sds s1;
            sds s2;
            sds s3;
            sds s4;
            SDS_NEW_LITERAL(s1, "abc");
            SDS_NEW_LITERAL(s2, "abc");
            SDS_NEW_LITERAL(s3, "abd");
            SDS_NEW_LITERAL(s4, "ab");

            check_int_eq(sdscmp(s1, s2), 0);
            check_int_eq(sdscmp(s1, s2), strcmp("abc", "abc"));

            {
                int res = sdscmp(s1, s3);
                int sys = strcmp("abc", "abd");
                check((res < 0 && sys < 0) || (res > 0 && sys > 0) || (res == 0 && sys == 0));
            }

            {
                int res = sdscmp(s1, s4);
                int sys = strcmp("abc", "ab");
                check((res < 0 && sys < 0) || (res > 0 && sys > 0) || (res == 0 && sys == 0));
            }

            sds_free_all(s1, s2, s3, s4);
        }

        it("case-insensitive comparison") {
            check_int_eq(sdscasecmp("Hello", "hello"), 0);
            check_int_eq(sdscasecmp("ABC", "abc"), 0);
            check_int_eq(sdscasecmp("abc", "ABC"), 0);
            check(sdscasecmp("abc", "abd") < 0);
            check(sdscasecmp("abd", "abc") > 0);
            check_int_eq(sdscasecmp(NULL, NULL), 0);
            check(sdscasecmp(NULL, "a") < 0);
            check(sdscasecmp("a", NULL) > 0);
        }

        it("case-insensitive comparison with length") {
            check_int_eq(sdsncasecmp("Hello", "hello", 5), 0);
            check_int_eq(sdsncasecmp("HelloWorld", "HelloPlanet", 5), 0);
            check(sdsncasecmp("HelloWorld", "HelloPlanet", 6) != 0);
            check_int_eq(sdsncasecmp("abc", "abd", 0), 0);
            check_int_eq(sdsncasecmp(NULL, NULL, 5), 0);
        }

        it("starts with prefix") {
            check(sdsstartswith("Hello World", "Hello"));
            check(sdsstartswith("Hello", "Hello"));
            check(!sdsstartswith("Hello", "hello"));
            check(!sdsstartswith("Hi", "Hello"));
            check(sdsstartswith("abc", ""));
            check(!sdsstartswith(NULL, "a"));
            check(!sdsstartswith("a", NULL));
        }

        it("starts with prefix (case-insensitive)") {
            check(sdsistartswith("Hello World", "hello"));
            check(sdsistartswith("HELLO", "hello"));
            check(sdsistartswith("hello", "HELLO"));
            check(!sdsistartswith("Hi", "Hello"));
        }

        it("ends with suffix") {
            check(sdsendswith("Hello World", "World"));
            check(sdsendswith("Hello", "Hello"));
            check(!sdsendswith("Hello", "hello"));
            check(!sdsendswith("Hi", "Hello"));
            check(sdsendswith("abc", ""));
            check(!sdsendswith(NULL, "a"));
            check(!sdsendswith("a", NULL));
        }

        it("contains substring") {
            check(sdscontains("Hello World", "World"));
            check(sdscontains("Hello World", "o W"));
            check(sdscontains("Hello", "Hello"));
            check(!sdscontains("Hello", "world"));
            check(sdscontains("abc", ""));
            check(!sdscontains(NULL, "a"));
            check(!sdscontains("a", NULL));
        }
    }

    describe("Copying") {
        it("copies strings") {
            sds s;
            SDS_NEW_LITERAL(s, "initial");
            s = sdscpy(s, "new content");
            check_str_eq(s, "new content");
            check_size_eq(sdslen(s), (size_t)11);
            sdsfree(s);
        }
    }

    describe("Trimming") {
        it("trims characters") {
            sds s;
            SDS_NEW_LITERAL(s, "  hello  ");
            s = sdstrim(s, " ");
            check_str_eq(s, "hello");
            check_size_eq(sdslen(s), (size_t)5);
            sdsfree(s);
        }
    }

    describe("Header boundaries") {
        it("uses type5 for length 31 and type8+ for length 32") {
            sds s31;
            sds s32;
            SDS_NEW_FILLED(s31, 'a', 31);
            SDS_NEW_FILLED(s32, 'b', 32);

            check_int_eq((s31[-1] & SDS_TYPE_MASK), SDS_TYPE_5);
            check((s32[-1] & SDS_TYPE_MASK) != SDS_TYPE_5);

            sds_free_all(s31, s32, NULL, NULL);
        }

        it("allows type5 length increments up to 31") {
            sds s;
            SDS_NEW_FILLED(s, 'c', 30);
            check_int_eq((s[-1] & SDS_TYPE_MASK), SDS_TYPE_5);
            sdsinclen(s, 1);
            check_size_eq(sdslen(s), (size_t)31);
            check_int_eq((s[-1] & SDS_TYPE_MASK), SDS_TYPE_5);
            sdsfree(s);
        }
    }

    describe("Benchmarks") {
        const int iters = 10000;
        const int loops = 100;
        const char *to_append = "bench";
        const size_t append_len = 5;
        const int iters_long = 2000;
        const int loops_long = 50;
        const size_t long_chunk_len = 256;
        const size_t long_len = 8192;
        const size_t small_total = append_len * (size_t)loops;
        const size_t long_total = long_chunk_len * (size_t)loops_long;

        bench("concatenation performance") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            benchmark("sdscat 100 loops", iters, 1) {
                sds s = sdsempty();
                for (int i = 0; i < loops; i++) {
                    s = sdscatlen(s, to_append, append_len);
                }
                sdsfree(s);
            }

            benchmark("strcat 100 loops", iters, 1) {
                char buf[1024];
                buf[0] = '\0';
                for (int i = 0; i < loops; i++) {
                    strcat(buf, to_append);
                }
            }
        }

        bench("concatenation performance (prealloc)") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            benchmark("sdscat 100 loops (prealloc)", iters, 1) {
                sds s = sdsempty();
                s = sdsMakeRoomFor(s, small_total);
                for (int i = 0; i < loops; i++) {
                    s = sdscatlen(s, to_append, append_len);
                }
                sdsfree(s);
            }


            benchmark("strcat 100 loops (prealloc)", iters, 1) {
                size_t cap = small_total + 1;
                char *buf = (char *)malloc(cap);
                if (buf) {
                    buf[0] = '\0';
                    for (int i = 0; i < loops; i++) {
                        strcat(buf, to_append);
                    }
                    free(buf);
                }
            }
        }

        bench("concatenation performance (long chunks)") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            char *chunk = (char *)malloc(long_chunk_len + 1);
            check_not_null(chunk);
            memset(chunk, 'x', long_chunk_len);
            chunk[long_chunk_len] = '\0';

            benchmark("sdscatlen 50x256", iters_long, 1) {
                sds s = sdsempty();
                for (int i = 0; i < loops_long; i++) {
                    s = sdscatlen(s, chunk, long_chunk_len);
                }
                sdsfree(s);
            }

            benchmark("strcat 50x256", iters_long, 1) {
                size_t cap = long_chunk_len * (size_t)loops_long + 1;
                char *buf = (char *)malloc(cap);
                if (buf) {
                    buf[0] = '\0';
                    for (int i = 0; i < loops_long; i++) {
                        strcat(buf, chunk);
                    }
                    free(buf);
                }
            }

            free(chunk);
        }

        bench("concatenation performance (long chunks, prealloc)") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            char *chunk = (char *)malloc(long_chunk_len + 1);
            check_not_null(chunk);
            memset(chunk, 'x', long_chunk_len);
            chunk[long_chunk_len] = '\0';

            benchmark("sdscatlen 50x256 (prealloc)", iters_long, 1) {
                sds s = sdsempty();
                s = sdsMakeRoomFor(s, long_total);
                for (int i = 0; i < loops_long; i++) {
                    s = sdscatlen(s, chunk, long_chunk_len);
                }
                sdsfree(s);
            }

            benchmark("strcat 50x256 (prealloc)", iters_long, 1) {
                size_t cap = long_total + 1;
                char *buf = (char *)malloc(cap);
                if (buf) {
                    buf[0] = '\0';
                    for (int i = 0; i < loops_long; i++) {
                        strcat(buf, chunk);
                    }
                    free(buf);
                }
            }

            free(chunk);
        }

        bench("length performance") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            const char *cstr = "a long string just to test length operation frequently items";
            sds s;
            SDS_NEW_LITERAL(s, cstr);
 
            benchmark("sdslen", iters * 10, 1) {
                size_t l = sdslen(s);
                (void)l;
            }

            benchmark("strlen", iters * 10, 1) {
                size_t l = strlen(cstr);
                (void)l;
            }
            sdsfree(s);
         }

        bench("length performance (long)") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            char *cstr = (char *)malloc(long_len + 1);
            check_not_null(cstr);
            memset(cstr, 'a', long_len);
            cstr[long_len] = '\0';

            sds s = sdsnewlen(cstr, long_len);
            check_not_null(s); 

            benchmark("sdslen 8K", iters_long * 5, 1) {
                size_t l = sdslen(s);
                (void)l;
            }

            benchmark("strlen 8K", iters_long * 5, 1) {
                size_t l = strlen(cstr);
                (void)l;
            }

            sdsfree(s);
            free(cstr);
        }

        bench("copy performance") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            const char *src = "copy this string content";
            sds s = sdsnewlen(NULL, 100);
            check_not_null(s);

            benchmark("sdscpy", iters, 1) {
                s = sdscpy(s, src);
            }

            benchmark("strcpy", iters, 1) {
                char dst[100];
                strcpy(dst, src);
            }
            sdsfree(s);
        }

        bench("compare performance") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            sds s1;
            sds s2;
            sds s3;
            SDS_NEW_LITERAL(s1, "string one");
            SDS_NEW_LITERAL(s2, "string two");
            SDS_NEW_LITERAL(s3, "string onz");
            const char *c1 = "string one";
            const char *c2 = "string two";
            const char *c3 = "string onz";

            benchmark("sdscmp (full)", iters, 1) {
                int res = sdscmp(s1, s2);
                (void)res;
            }

            benchmark("strcmp (full)", iters, 1) {
                int res = strcmp(c1, c2);
                (void)res;
            }

            benchmark("sdscmp (prefix)", iters, 1) {
                int res = sdscmp(s1, s3);
                (void)res;
            }

            benchmark("strncmp (9 chars)", iters, 1) {
                int res = strncmp(c1, c3, 9);
                (void)res;
            }

            benchmark("sdscasecmp (tolower+cmp)", iters, 1) {
                sds sc1 = sdsdup(s1);
                sdstolower(sc1);
                int res = sdscmp(sc1, s2);
                (void)res;
                sdsfree(sc1);
            }

            benchmark("stricmp/strcasecmp", iters, 1) {
                int res = STRICMP(c1, c2);
                (void)res;
            }

            sds_free_all(s1, s2, s3, NULL);
        }

        bench("compare performance (long)") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            char *a = (char *)malloc(long_len + 1);
            char *b = (char *)malloc(long_len + 1);
            check_not_null(a);
            check_not_null(b);
            memset(a, 'a', long_len);
            memset(b, 'a', long_len);
            a[long_len] = '\0';
            b[long_len] = '\0';
            b[long_len - 1] = 'b';

            sds sa = sdsnewlen(a, long_len);
            sds sb = sdsnewlen(b, long_len);
            check_not_null(sa);
            check_not_null(sb);

            benchmark("sdscmp 8K (diff end)", iters_long * 5, 1) {
                int res = sdscmp(sa, sb);
                (void)res;
            }

            benchmark("strcmp 8K (diff end)", iters_long * 5, 1) {
                int res = strcmp(a, b);
                (void)res;
            }

            sds_free_all(sa, sb, NULL, NULL);
            free(a);
            free(b);
        }

        bench("case-insensitive compare performance") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            const char *c1 = "Hello World Test String";
            const char *c2 = "hello world test string";
            const char *c3 = "HELLO WORLD TEST STRING";

            benchmark("sdscasecmp (equal)", iters, 1) {
                int res = sdscasecmp(c1, c2);
                (void)res;
            }

            benchmark("STRICMP (equal)", iters, 1) {
                int res = STRICMP(c1, c2);
                (void)res;
            }

            benchmark("sdsncasecmp (10 chars)", iters, 1) {
                int res = sdsncasecmp(c1, c3, 10);
                (void)res;
            }
        }

        bench("startswith/endswith/contains performance") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            const char *s = "The quick brown fox jumps over the lazy dog";
            const char *prefix = "The quick";
            const char *suffix = "lazy dog";
            const char *substr = "brown fox";

            benchmark("sdsstartswith", iters, 1) {
                int res = sdsstartswith(s, prefix);
                (void)res;
            }

            benchmark("sdsistartswith", iters, 1) {
                int res = sdsistartswith(s, "THE QUICK");
                (void)res;
            }

            benchmark("sdsendswith", iters, 1) {
                int res = sdsendswith(s, suffix);
                (void)res;
            }

            benchmark("sdscontains", iters, 1) {
                int res = sdscontains(s, substr);
                (void)res;
            }

            benchmark("strstr (contains)", iters, 1) {
                int res = strstr(s, substr) != NULL;
                (void)res;
            }
        }

        bench("case conversion performance") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            sds s;
            SDS_NEW_LITERAL(s, "A Long String With Mixed Case To Be Converted Many Times In A Loop");
            char *cstr = strdup("A Long String With Mixed Case To Be Converted Many Times In A Loop");
            check_not_null(cstr);
            size_t len = strlen(cstr);

            benchmark("sdstolower", iters, 1) {
                sdstolower(s);
            }

            benchmark("manual tolower (loop)", iters, 1) {
                for (size_t i = 0; i < len; i++) {
                    cstr[i] = (char)tolower((unsigned char)cstr[i]);
                }
            }
            sdsfree(s);
            free(cstr);
        }

        bench("memory management (new/free)") {

            benchmark_titles("benchmark", "input", "iters", "avg(us)", NULL, "min(us)", "max(us)", "ops/s", NULL, NULL);
            const char *src = "some content to initialize strings";

            benchmark("sdsnew/sdsfree", iters, 1) {
                sds s = sdsnew(src);
                sdsfree(s);
            }

            benchmark("malloc/strcpy/free", iters, 1) {
                char *s = (char *)malloc(strlen(src) + 1);
                if (s) {
                    strcpy(s, src);
                    free(s);
                }
            }
        }
    }
}
