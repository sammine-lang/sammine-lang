#include <cstdio>

long long is_prime_h(long long n, long long d) {
    if (d * d > n) {
        return 1;
    } else if (n % d == 0) {
        return 0;
    } else {
        return is_prime_h(n, d + 1);
    }
}

long long is_prime(long long n) {
    if (n < 2) {
        return 0;
    } else {
        return is_prime_h(n, 2);
    }
}

long long sum_primes(long long n, long long limit, long long acc) {
    if (n >= limit) {
        return acc;
    } else if (is_prime(n) > 0) {
        return sum_primes(n + 1, limit, acc + n);
    } else {
        return sum_primes(n + 1, limit, acc);
    }
}

int main() {
    long long result = sum_primes(2, 2000000, 0);
    printf("%lld\n", result);
    return 0;
}
