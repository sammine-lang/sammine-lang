// C side of c_callback.mn: each calls back the function it is given, as C
// calls any function. clang trusts a char result to come back extended to
// 32 bits, signed, and a bool unsigned, so call_char and call_bool return
// what the callback gives without widening it.
#include <stdbool.h>

int apply(int (*f)(int), int x) { return f(x); }
int apply_twice(int (*f)(int), int x) { return f(f(x)); }
int call_char(signed char (*f)(signed char), signed char c) { return f(c); }
int call_bool(bool (*f)(int), int x) { return f(x); }
double call_mixed(double (*f)(double, float, long), double a, float b,
                  long c) {
  return f(a, b, c);
}
static int numbers[] = {4, 8, 15};
void each(void (*f)(const int *), int n) {
  for (int i = 0; i < n; i++)
    f(&numbers[i]);
}
