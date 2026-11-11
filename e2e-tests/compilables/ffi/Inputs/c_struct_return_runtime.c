// C side of c_struct_return.mn.
#include <stdint.h>

typedef struct { int64_t a, b; } Two;
typedef struct { int64_t a, b, c; } Three;
typedef struct { int32_t a, b, c, d; } FourI32;
typedef struct { float x, y, z; } ThreeF32;
typedef struct { int32_t a; float x; } I32F32;
typedef struct { char a, b, c; } Chars;
typedef struct { double d; int64_t n; } F64I64;

Two make_two(int64_t a) { Two t = { a, a + 1 }; return t; }
Three make_three(int64_t a, int64_t b) { Three t = { a, b, a + b }; return t; }
FourI32 make_four_i32(void) { FourI32 r = { 1, 2, 3, 4 }; return r; }
ThreeF32 make_three_f32(void) { ThreeF32 r = { 0.5f, 1.5f, 2.5f }; return r; }
I32F32 make_i32_f32(void) { I32F32 r = { 7, 3.5f }; return r; }
Chars make_chars(void) { Chars r = { 'a', 'b', 'c' }; return r; }
F64I64 make_f64_i64(void) { F64I64 r = { 9.25, 42 }; return r; }
