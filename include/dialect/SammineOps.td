include "dialect/SammineDialect.td"
include "dialect/SammineTypes.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

class Sammine_Op<string mnemonic, list<Trait> traits = []>
    : Op<Sammine_Dialect, mnemonic, traits>;

def ToDeviceOp : Sammine_Op<"to_device", [Pure]> {
  let summary = "Transfer a value from host to device";
  let arguments = (ins AnyType:$input);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$input attr-dict `:` type($input) `->` type($result)";
}

def ToHostOp : Sammine_Op<"to_host", [Pure]> {
  let summary = "Transfer a value from device to host";
  let arguments = (ins AnyType:$input);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$input attr-dict `:` type($input) `->` type($result)";
}

//===----------------------------------------------------------------------===//
// MIR: memory and ownership. See knowledge_base/plans/mir.md.
//===----------------------------------------------------------------------===//

// The place a pointer of type $_self points at.
defvar PlaceOfPointee = [{::sammine_lang::smn::PlaceType::get($_self.getContext(),
    ::llvm::cast<::sammine_lang::smn::PtrType>($_self).getPointee())}];
// The type of the value a place of type $_self holds.
defvar ValueOfPlace =
    [{::llvm::cast<::sammine_lang::smn::PlaceType>($_self).getValueType()}];
// A view of what a place of type $_self holds.
defvar ViewOfPlace = [{::sammine_lang::smn::PtrType::get($_self.getContext(),
    ::llvm::cast<::sammine_lang::smn::PlaceType>($_self).getValueType(),
    ::sammine_lang::smn::Ownership::View)}];

def AllocOp : Sammine_Op<"alloc", [MemoryEffects<[MemAlloc]>]> {
  let summary = "room on the heap for `count` values, owned by the result";
  let description = [{
    `alloc<T>(n)`. Nothing is stored there yet: what the pointer points at
    holds no value until something is stored in it.
  }];
  let arguments = (ins AnySignlessInteger:$count);
  let results = (outs Sammine_OwnedPtr:$result);
  let assemblyFormat = "$count attr-dict `:` type($count) `->` type($result)";
}

def FreeOp : Sammine_Op<"free", [MemoryEffects<[MemFree]>]> {
  let summary = "give back the memory an owned pointer holds";
  let arguments = (ins Sammine_OwnedPtr:$ptr);
  let assemblyFormat = "$ptr attr-dict `:` type($ptr)";
}

def DerefOp : Sammine_Op<"deref", [Pure,
    TypesMatchWith<"the place holds what the pointer points at",
                   "ptr", "place", PlaceOfPointee>]> {
  let summary = "the place a pointer points at, `*p`";
  let arguments = (ins Sammine_PtrType:$ptr);
  let results = (outs Sammine_PlaceType:$place);
  let assemblyFormat = "$ptr attr-dict `:` type($ptr)";
}

def LocalOp : Sammine_Op<"local", [MemoryEffects<[MemAlloc]>]> {
  let summary = "a binding's slot, `let x = v` when the code takes x's address";
  let description = [{
    Holds no value until one is stored. Only a binding whose address is taken
    (`&x`, or `x[i]` through an Indexer, which lends x) lives in a slot: any
    other is the value it names.
  }];
  let arguments = (ins StrAttr:$name);
  let results = (outs Sammine_PlaceType:$place);
  let assemblyFormat = "$name attr-dict `:` type($place)";
}

def FieldOp : Sammine_Op<"field", [Pure]> {
  let summary = "the place of field `index` of the struct or tuple in a place, `x.f`";
  let arguments = (ins Sammine_PlaceType:$base, I64Attr:$index);
  let results = (outs Sammine_PlaceType:$place);
  let assemblyFormat = "$base `[` $index `]` attr-dict `:` type($base) `->` type($place)";
  let hasVerifier = 1;
}

def ElemOp : Sammine_Op<"elem", [Pure]> {
  let summary = "the place of element `index` of the array in a place, `a[i]`";
  let description = [{
    Nothing checks the index here: MIR checks it before, as for
    sammine.element.
  }];
  let arguments = (ins Sammine_PlaceType:$base, AnySignlessInteger:$index);
  let results = (outs Sammine_PlaceType:$place);
  let assemblyFormat = "$base `[` $index `]` attr-dict `:` type($base) `,` type($index) `->` type($place)";
  let hasVerifier = 1;
}

def AddrOp : Sammine_Op<"addr", [Pure,
    TypesMatchWith<"the result is a view of what the place holds",
                   "place", "result", ViewOfPlace>]> {
  let summary = "a view of a place, `&x`, for as long as the place lives";
  let description = [{
    Unlike sammine.lend, the view may outlive a call: `let p = &x` keeps it.
    Nothing writes through a view, so the place keeps its value.
  }];
  let arguments = (ins Sammine_PlaceType:$place);
  let results = (outs Sammine_PtrType:$result);
  let assemblyFormat = "$place attr-dict `:` type($place)";
}

def PtrElemOp : Sammine_Op<"ptr_elem", [Pure,
    TypesMatchWith<"the place holds what the pointer points at",
                   "ptr", "place", PlaceOfPointee>]> {
  let summary = "the place `index` elements past where a pointer points, `p[i]`";
  let description = [{
    Only in an unsafe function: nothing checks the index, and the ownership
    check doesn't follow what is stored there.
  }];
  let arguments = (ins Sammine_PtrType:$ptr, AnySignlessInteger:$index);
  let results = (outs Sammine_PlaceType:$place);
  let assemblyFormat = "$ptr `[` $index `]` attr-dict `:` type($ptr) `,` type($index)";
}

def LendOp : Sammine_Op<"lend", [Pure,
    TypesMatchWith<"the result is a view of what the place holds",
                   "place", "result", ViewOfPlace>]> {
  let summary = "a view of a place, for the length of a call";
  let description = [{
    What passing an owned `'ptr<T>` to a `ptr<T>` parameter does to `*p`:
    the callee looks at the place through the view, and the place keeps its
    value and its owner. The view lasts as long as the call, so it is only
    passed to `sammine.call`.
  }];
  let arguments = (ins Sammine_PlaceType:$place);
  let results = (outs Sammine_PtrType:$result);
  let assemblyFormat = "$place attr-dict `:` type($place)";
  let hasVerifier = 1;
}

def StringOp : Sammine_Op<"string", [Pure]> {
  let summary = "a string literal, `\"hi\"`";
  let description = [{
    A view of the string's characters, followed by a NUL as C reads them, in
    memory that lives as long as the program. Nothing owns it.
  }];
  let arguments = (ins StrAttr:$value);
  let results = (outs Sammine_CharView:$result);
  let assemblyFormat = "$value attr-dict `:` type($result)";
}

def AggregateOp : Sammine_Op<"aggregate", [Pure]> {
  let summary = "a struct, tuple or array of the values of its fields, in order";
  let description = [{
    `Point { x: a, y: b }`, `(a, b)` or `[a, b]`. A field that owns memory is
    moved in: the aggregate owns it now.
  }];
  let arguments = (ins Variadic<AnyType>:$fields);
  let results = (outs AnyType:$result);
  let assemblyFormat = "`(` $fields `)` attr-dict `:` functional-type($fields, $result)";
  let hasVerifier = 1;
}

def ExtractOp : Sammine_Op<"extract", [Pure]> {
  let summary = "field `index` of a struct or tuple value, `p.x` or `t.0`";
  let description = [{
    A field that owns memory is moved out of the aggregate.
  }];
  let arguments = (ins AnyType:$aggregate, I64Attr:$index);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$aggregate `[` $index `]` attr-dict `:` type($aggregate) `->` type($result)";
  let hasVerifier = 1;
}

def VariantOp : Sammine_Op<"variant", [Pure]> {
  let summary = "an enum value of variant `index`, with its payload";
  let description = [{
    `Option<i64>::Some(x)`. `offsets` are the payload fields' byte offsets.
    A payload field that owns memory is moved in.
  }];
  let arguments = (ins I64Attr:$index, DenseI64ArrayAttr:$offsets,
                       Variadic<AnyType>:$payload);
  let results = (outs Sammine_EnumType:$result);
  let assemblyFormat = [{
    $index `(` $payload `)` `at` $offsets attr-dict `:` functional-type($payload, $result)
  }];
  let hasVerifier = 1;
}

def TagOp : Sammine_Op<"tag", [Pure]> {
  let summary = "which variant an enum value is, as its index";
  let arguments = (ins Sammine_EnumType:$value);
  let results = (outs I32:$result);
  let assemblyFormat = "$value attr-dict `:` type($value)";
}

def PayloadOp : Sammine_Op<"payload", [Pure]> {
  let summary = "field `field` of the payload of an enum value of variant `variant`";
  let description = [{
    What a case arm binds. The value must be of that variant: the arm's
    block is only reached when its tag says so. `offset` is the field's byte
    offset. A field that owns memory is moved out.
  }];
  let arguments = (ins Sammine_EnumType:$value, I64Attr:$variant,
                       I64Attr:$field, I64Attr:$offset);
  let results = (outs AnyType:$result);
  let assemblyFormat = [{
    $value `[` $variant `,` $field `]` `at` $offset attr-dict `:` type($value) `->` type($result)
  }];
  let hasVerifier = 1;
}

def ElementOp : Sammine_Op<"element", [Pure]> {
  let summary = "element `index` of an array value, `a[i]`";
  let description = [{
    Nothing checks the index here: MIR checks it before, as code of its own,
    and reports one out of bounds.
  }];
  let arguments = (ins Sammine_ArrayType:$array, AnySignlessInteger:$index);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$array `[` $index `]` attr-dict `:` type($array) `,` type($index) `->` type($result)";
  let hasVerifier = 1;
}

def UnreachableOp : Sammine_Op<"unreachable", [Terminator]> {
  let summary = "a point the program never reaches, as after exit()";
  let assemblyFormat = "attr-dict";
}

def CopyOp : Sammine_Op<"copy", [MemoryEffects<[MemRead]>,
    TypesMatchWith<"the result is what the place holds",
                   "place", "result", ValueOfPlace>]> {
  let summary = "read a value that owns nothing out of a place";
  let description = [{
    The place keeps its value. A value that owns memory can't be copied, or
    two would own it: take it with `sammine.move`.
  }];
  let arguments = (ins Sammine_PlaceType:$place);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$place attr-dict `:` type($place)";
  let hasVerifier = 1;
}

def MoveOp : Sammine_Op<"move", [MemoryEffects<[MemRead]>,
    TypesMatchWith<"the result is what the place holds",
                   "place", "result", ValueOfPlace>]> {
  let summary = "take a value that owns memory out of a place";
  let description = [{
    The place holds no value afterwards: the result owns what it did.
  }];
  let arguments = (ins Sammine_PlaceType:$place);
  let results = (outs AnyType:$result);
  let assemblyFormat = "$place attr-dict `:` type($place)";
  let hasVerifier = 1;
}

def StoreOp : Sammine_Op<"store", [MemoryEffects<[MemWrite]>,
    TypesMatchWith<"the value is what the place holds",
                   "place", "value", ValueOfPlace>]> {
  let summary = "put a value in a place, `*p = v`";
  let arguments = (ins AnyType:$value, Sammine_PlaceType:$place);
  let assemblyFormat = "$value `,` $place attr-dict `:` type($place)";
}

//===----------------------------------------------------------------------===//
// MIR: calls.
//===----------------------------------------------------------------------===//

def CallOp : Sammine_Op<"call",
    [DeclareOpInterfaceMethods<SymbolUserOpInterface>]> {
  let summary = "call a function by name, `f(a, b)`";
  let description = [{
    The operands and results have the types MIR gives the call's sammine
    types. While MIR and the AST codegen both make functions, the callee may
    have been emitted from the AST, with LLVM types where MIR has its own
    (`!llvm.ptr` for `!sammine.ptr`): then only the counts must match. A
    callee that went through MIR (marked `sammine.mir`) must take and give
    exactly these types. Lowering to LLVM makes both the same.

    The callee may be a C function, an `llvm.func`: a variadic one takes any
    number of arguments after those it declares, which lowering promotes as
    C does.
  }];
  let arguments = (ins FlatSymbolRefAttr:$callee, Variadic<AnyType>:$args);
  let results = (outs Variadic<AnyType>:$results);
  let assemblyFormat = [{
    $callee `(` $args `)` attr-dict `:` functional-type($args, results)
  }];
}
