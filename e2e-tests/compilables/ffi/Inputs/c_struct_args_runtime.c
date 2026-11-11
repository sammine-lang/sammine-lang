// C side of c_struct_args.mn.
#include <stdint.h>
#include <stdio.h>

typedef struct { int64_t a, b, c; } Three;
typedef struct { int32_t a, b, c, d; } FourI32;
typedef struct { float x, y, z; } ThreeF32;
typedef struct { int32_t a; float x; } I32F32;
typedef struct { char a, b, c; } Chars;

void take_three(int32_t k, Three t) { printf("three %d %ld %ld %ld\n", k, t.a, t.b, t.c); }
void take_four_i32(FourI32 f) { printf("four_i32 %d %d %d %d\n", f.a, f.b, f.c, f.d); }
void take_three_f32(ThreeF32 f) { printf("three_f32 %.1f %.1f %.1f\n", f.x, f.y, f.z); }
void take_i32_f32(I32F32 m) { printf("i32_f32 %d %.1f\n", m.a, m.x); }
void take_chars(Chars c) { printf("chars %c%c%c\n", c.a, c.b, c.c); }
