#include "stdio.h"
#include "stdlib.h"
int main() {
    int n, *p;
    scanf("%d", &n);
    p = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        scanf("%d", &p[i]);
    }

    int max = p[0];
    for (int i = 1; i < n; i++) {
        if (p[i] > max) {
            max = p[i];
        }
    }
    printf("Max: %d\n", max);

    free(p);
}