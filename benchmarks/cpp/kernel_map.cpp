#include <cstdio>

void double_arr(const double a[1000], double out[1000]) {
    for (int i = 0; i < 1000; i++)
        out[i] = a[i] * 2.0;
}

int main() {
    double a[1000];
    for (int i = 0; i < 1000; i++) a[i] = 1.0;
    double sum = 0.0;
    for (int i = 0; i < 10000; i++) {
        double b[1000];
        double_arr(a, b);
        sum += b[0];
    }
    printf("%f\n", sum);
    return 0;
}
