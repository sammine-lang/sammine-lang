// C side of c_struct_stack_args.mn: struct arguments that no longer fit in
// the remaining registers, so C passes them on the stack.
#include <stdint.h>
#include <stdio.h>

typedef struct { int64_t a, b; } Two;
typedef struct { double x, y; } TwoF64;
typedef struct { int64_t a, b, c; } Three;

// Six integer registers are used up before `t`.
void after_six_ints(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                    int64_t f, Two t) {
  printf("six %ld %ld\n", a + b + c + d + e + f, t.a * 10 + t.b);
}

// One integer register is left, and `t` needs two.
void after_five_ints(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                     Two t) {
  printf("five %ld %ld\n", a + b + c + d + e, t.a * 10 + t.b);
}

// Eight floating-point registers are used up before `t`.
void after_eight_doubles(double a, double b, double c, double d, double e,
                         double f, double g, double h, TwoF64 t) {
  printf("eight %.1f %.1f\n", a + b + c + d + e + f + g + h, t.x * 10 + t.y);
}

// One floating-point register is left, and `t` needs two.
void after_seven_doubles(double a, double b, double c, double d, double e,
                         double f, double g, TwoF64 t) {
  printf("seven %.1f %.1f\n", a + b + c + d + e + f + g, t.x * 10 + t.y);
}

// The sret pointer takes an integer register too.
Three sret_then_five(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                     Two t) {
  Three r = {a + b + c + d + e, t.a, t.b};
  return r;
}
