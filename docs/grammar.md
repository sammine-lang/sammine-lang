# Grammar

Formal grammar for the sammine language, derived from `src/Parser.cpp`.

## Conventions

- **bold** = non-terminal
- `MONO` = terminal token
- `?` = optional, `*` = zero or more, `+` = one or more
- `|` = alternation
- `(...)` = grouping

## Program Structure

**program** ::= **import*** **definition***

**import** ::= `import` ID (`;` | `as` ID `;`)

## Definitions

**definition** ::= **func_def**
  | **extern_def**
  | **struct_def**
  | **enum_def**
  | **type_alias_def**
  | **typeclass_decl**
  | **typeclass_instance**

**func_def** ::= `export`? `let` ID **type_params**? `(` **param_list**? `)` (`->` **type**)? **block**

**extern_def** ::= `export`? `reuse` ID `(` **param_list_with_vararg**? `)` `->` **type** `;`

**struct_def** ::= `export`? `struct` ID **type_params**? `{` **typed_var** (`,` **typed_var**)* `,` `}` `;`

**enum_def** ::= `export`? `type` ID **type_params**? (`:` **backing_type**)? `=` **variant** (`|` **variant**)* `;`

**type_alias_def** ::= `export`? `type` ID `=` **type** `;`

**typeclass_decl** ::= `typeclass` ID `<` ID (`,` ID)* `>` `{` **method_proto*** `}`

**typeclass_instance** ::= `instance` ID `<` **type** (`,` **type**)* `>` `{` **func_def*** `}`

## Supporting Rules

**type_params** ::= `<` ID (`,` ID)* `>`

**param_list** ::= **typed_var** (`,` **typed_var**)*

**param_list_with_vararg** ::= **param_list** (`,` `...`)? | `...`

**typed_var** ::= `mut`? ID (`:` **type**)?

**variant** ::= ID                             *unit variant*
  | ID `(` **type** (`,` **type**)* `)`        *payload variant*
  | ID `(` INTEGER `)`                         *integer-backed variant*

**backing_type** ::= `i32` | `i64` | `u32` | `u64`

**method_proto** ::= ID `(` **param_list**? `)` (`->` **type**)? `;`?

**block** ::= `{` **statement*** **expr**? `}`

**statement** ::= **expr** `;`
  | **let_expr** `;`
  | **return_expr** `;`

## Types

**type** ::= **simple_type**
  | `ptr` `<` **type** `>`                     *pointer*
  | `'ptr` `<` **type** `>`                    *linear pointer*
  | `[` **type** `;` INTEGER `]`               *fixed-size array*
  | `(` **type** (`,` **type**)+ `)`           *tuple type*
  | `(` (**type** (`,` **type**)*)? `)` `->` **type**  *function type*
  | **simple_type** `<` **type** (`,` **type**)* `>`  *generic type*

**simple_type** ::= ID | **qualified_name**

**qualified_name** ::= ID `::` ID

## Expressions

**expr** ::= **binary_expr**
  | **let_expr**
  | **return_expr**
  | **if_expr**
  | **while_expr**
  | **case_expr**

**let_expr** ::= `let` `mut`? **typed_var** `=` **expr**
  | `let` `mut`? `(` ID (`,` ID)* `)` `=` **expr**    *tuple destructuring*

**return_expr** ::= `return` **expr**?

**if_expr** ::= `if` **expr** **block** (`else` (`if` **expr**)? **block**)?

**while_expr** ::= `while` **expr** **block**

**case_expr** ::= `case` **expr** `{` **case_arm** (`,` **case_arm**)* `,`? `}`

**case_arm** ::= **pattern** `=>` **expr**

**pattern** ::= **qualified_name** (`(` ID (`,` ID)* `)`)?    *variant pattern*
  | ID (`(` ID (`,` ID)* `)`)?                              *unqualified variant*
  | `_`                                                      *wildcard*

## Binary Expressions (Precedence Climbing)

**binary_expr** ::= **unary_expr** (**binop** **unary_expr**)*

Operator precedence (low to high):

| Prec | Operators | Description |
|------|-----------|-------------|
| 1    | `\|>`     | pipe |
| 2    | `=`       | assignment |
| 3    | `\|`      | bitwise OR |
| 4    | `\|\|`    | logical OR |
| 5    | `&`       | bitwise AND |
| 6    | `^`       | bitwise XOR |
| 7    | `&&`      | logical AND |
| 10   | `==` `!=` `<` `>` `<=` `>=` | comparison |
| 15   | `<<` `>>` | shift |
| 20   | `+` `-`   | additive |
| 40   | `*` `/` `%` | multiplicative |

## Unary and Primary Expressions

**unary_expr** ::= `-` **unary_expr** | `*` **unary_expr** | `&` **unary_expr** | **postfix_expr**

**postfix_expr** ::= **primary** (`[` **expr** `]` | `.` ID)*

**primary** ::= INTEGER | FLOAT | STRING | `true` | `false` | CHAR
  | `(` `)`                                   *unit literal*
  | `(` **expr** (`,` **expr**)+ `)`          *tuple literal*
  | `(` **expr** `)`                          *parenthesized*
  | ID                                        *variable reference*
  | **if_expr** | **while_expr** | **case_expr**
  | ID (`::` ID)? (`<` **type** (`,` **type**)* `>`)? `(` **arg_list**? `)`   *function call*
  | ID (`<` **type** (`,` **type**)* `>`)? `{` **field_init** (`,` **field_init**)* `}`  *struct literal*
  | `[` **expr** (`,` **expr**)* `]`          *array literal*
  | `alloc` `<` **type** `>` `(` **expr** `)` *heap allocation*
  | `free` `(` **expr** `)`                   *deallocation*
  | `len` `(` **expr** `)`                    *array length*

**field_init** ::= ID `:` **expr**

**arg_list** ::= **expr** (`,` **expr**)*

## Literals

INTEGER ::= DIGITS (`i32` | `i64` | `u32` | `u64`)?
FLOAT ::= DIGITS `.` DIGITS (`f32`)?
STRING ::= `"` ... `"`
CHAR ::= `'` ... `'`

## Comments

Comments start with `#` and extend to end of line.
