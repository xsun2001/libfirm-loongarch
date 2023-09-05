#define N 3

void matmul(int a[N][N], int b[N][N], int result[N][N]) {
    int i, j, k;
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            result[i][j] = 0;
            for (k = 0; k < N; k++)
                result[i][j] += a[i][k] * b[k][j];
        }
    }
}

int equal(int a[N][N], int b[N][N]) {
    int i, j;
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            if (a[i][j] != b[i][j]) {
                return 1;
            }
        }
    }
    return 0;
}

int main() {
    int a[N][N] = {{1, 1, 1}, {2, 2, 2}, {3, 3, 3}};
    int b[N][N] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    int result[N][N];
    int answer[N][N] = {{12, 15, 18}, {24, 30, 36}, {36, 45, 54}};

    matmul(a, b, result);
    return equal(answer, result);
}
