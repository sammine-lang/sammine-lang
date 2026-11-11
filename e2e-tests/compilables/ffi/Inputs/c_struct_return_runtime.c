// C side of c_struct_return.mn.
#include <stdint.h>

typedef struct { int64_t a, b; } Two;
typedef struct { int64_t a, b, c; } Three;

Two make_two(int64_t a) { Two t = { a, a + 1 }; return t; }
Three make_three(int64_t a, int64_t b) { Three t = { a, b, a + b }; return t; }
