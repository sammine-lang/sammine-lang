// C side of c_small_args.mn. These return their argument as an int without
// widening it: clang trusts the caller to have extended a char (signed) and
// a bool (unsigned) to 32 bits.
#include <stdbool.h>
#include <stdio.h>

int char_to_int(signed char c) { return c; }
int bool_to_int(bool b) { return b; }
signed char negate_char(signed char c) { return (signed char)-c; }
// A char passed through ... arrives promoted to int.
void print_promoted(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  int c = __builtin_va_arg(ap, int);
  int b = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  printf("promoted %d %d\n", c, b);
}
