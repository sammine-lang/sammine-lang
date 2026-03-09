#include <cstdio>

double sum_arr(const double a[1000]) {
    double acc = 0.0;
    for (int i = 0; i < 1000; i++)
        acc += a[i];
    return acc;
}

int main() {
    double a[1000];
    for (int i = 0; i < 1000; i++) a[i] = 1.0;
    double total = 0.0;
    for (int i = 0; i < 10000; i++) {
        total += sum_arr(a);
    }
    printf("%f\n", total);
    return 0;
}
