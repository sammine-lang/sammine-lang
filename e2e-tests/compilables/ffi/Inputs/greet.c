// A C library for c_library*.mn: built as libgreet.so, which the module
// that binds it, greeter.mn, isn't named after.
#include <stdio.h>

int greet_twice(int x) {
  printf("greet %d\n", x);
  return 2 * x;
}
