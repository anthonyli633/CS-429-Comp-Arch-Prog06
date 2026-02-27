#include "libtdmm/tdmm.h"

int main() {
    t_init(BEST_FIT);

    void *a = t_malloc(100);
    void *b = t_malloc(200);
    t_free(a);
    t_free(b);

    display_metrics();
    return 0;
}