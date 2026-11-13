// The prelude's C runtime: what print, eprint, panic and format write to.
// Every program links the prelude, so every program has these.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// C's streams, which print and eprint write to with fprintf: stdout keeps
// its buffer, shared with any C code printing beside, and stderr has none.
FILE *sammine_stdout(void) { return stdout; }
FILE *sammine_stderr(void) { return stderr; }

// A stream that keeps what is written to it, for format(): its chars, in
// memory that grows as they come, with a terminator after the last.
typedef struct {
  FILE *stream;
  char *data;
  size_t size;
} Text;

void *sammine_text_open(void) {
  Text *text = malloc(sizeof *text);
  text->stream = open_memstream(&text->data, &text->size);
  return text;
}

FILE *sammine_text_stream(void *text) { return ((Text *)text)->stream; }

// Ends the writing: how many chars were written.
int64_t sammine_text_finish(void *text) {
  Text *t = text;
  fclose(t->stream);
  return (int64_t)t->size;
}

// The chars written, the caller's to free now, after sammine_text_finish.
char *sammine_text_take(void *text) {
  Text *t = text;
  char *data = t->data;
  free(t);
  return data;
}
