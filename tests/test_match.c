#define main cursor_barrier_main
#include "../cursor-barrier.c"
#undef main

#include <stdio.h>

static int expect_match(const char *name, const char *haystack,
                        const char pats[][256], int npatterns, int expected) {
    int actual = match_pattern_idx(haystack, pats, npatterns);
    if (actual != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
        return 1;
    }
    return 0;
}

int main(void) {
    const char pats[2][256] = { "war thunder", "call of duty" };
    int failures = 0;

    failures += expect_match(
        "youtube title should not match game pattern",
        "chrome-youtube.com__-Default,War Thunder May Air Sales 2026 - YouTube",
        pats, 2, -1);

    failures += expect_match(
        "game class should match pattern",
        "war thunder,War Thunder",
        pats, 2, 0);

    failures += expect_match(
        "activewindow class line should match pattern",
        "Window abc -> War Thunder:\n\tclass: war thunder\n\ttitle: War Thunder\n",
        pats, 2, 0);

    failures += expect_match(
        "activewindow title line should not match pattern",
        "Window abc -> War Thunder May Air Sales 2026 - YouTube:\n\tclass: chrome-youtube.com__-Default\n\ttitle: War Thunder May Air Sales 2026 - YouTube\n",
        pats, 2, -1);

    return failures ? 1 : 0;
}
