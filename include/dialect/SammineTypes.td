#ifndef SAMMINE_TYPES_TD
#define SAMMINE_TYPES_TD

include "dialect/SammineDialect.td"
include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/EnumAttr.td"

class Sammine_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<Sammine_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

def Sammine_Ownership : I32EnumAttr<"Ownership",
    "whether a pointer owns what it points at", [
      I32EnumAttrCase<"Owned", 0, "owned">,
      I32EnumAttrCase<"View", 1, "view">,
      I32EnumAttrCase<"Buffer", 2, "buffer">]> {
  let cppNamespace = "::sammine_lang::smn";
  let genSpecializedAttr = 0;
}

def Sammine_PtrType : Sammine_Type<"Ptr", "ptr"> {
  let summary = "a pointer that owns what it points at, or a view of it";
  let description = [{
    `!sammine.ptr<i32, owned>` is sammine's `'ptr<i32>`: whoever holds it
    must free it or hand it on, and it holds one i32, which the ownership
    check follows. `!sammine.ptr<i32, buffer>` is `'buf<i32>`: owned as
    well, but holding as many i32 as its owner says, which the check
    doesn't follow. `!sammine.ptr<i32, view>` is `ptr<i32>`: it only looks
    at memory someone else owns.
  }];
  let parameters = (ins "::mlir::Type":$pointee,
                        EnumParameter<Sammine_Ownership>:$ownership);
  let assemblyFormat = "`<` $pointee `,` $ownership `>`";
  let extraClassDeclaration = [{
    /// Whether whoever holds it must free it or hand it on: a box or a
    /// buffer.
    bool isOwned() const { return getOwnership() != Ownership::View; }
    /// Whether it owns exactly one value, what it points at.
    bool isBox() const { return getOwnership() == Ownership::Owned; }
  }];
}

def Sammine_PlaceType : Sammine_Type<"Place", "place"> {
  let summary = "where a value lives";
  let description = [{
    `!sammine.place<i32>` is memory holding an i32, such as what a pointer
    points at. A place is never a value of its own: each use of it says what
    it does to ownership, `sammine.copy`, `sammine.move` or `sammine.store`.
  }];
  let parameters = (ins "::mlir::Type":$valueType);
  let assemblyFormat = "`<` $valueType `>`";
}

def Sammine_StructType : Sammine_Type<"Struct", "struct"> {
  let summary = "a struct: its name and its fields' names and types, in order";
  let description = [{
    `!sammine.struct<"Point", (x: i32, y: i32)>`. The name is the struct's, as
    NameResolver gives it (`shapes::Point`, `Range<i32>`): lowered to LLVM
    it is the identified struct `sammine.struct.<name>`, the very type the
    module's other code, and its declarations, use for it.
  }];
  let parameters = (ins StringRefParameter<"the struct's name">:$name,
                        ArrayRefParameter<"::mlir::StringAttr", "field names">:$fieldNames,
                        ArrayRefParameter<"::mlir::Type", "field types">:$fields);
  let assemblyFormat =
      "`<` $name `,` `(` custom<FieldList>($fieldNames, $fields) `)` `>`";
  let genVerifyDecl = 1;
}

def Sammine_TupleType : Sammine_Type<"Tuple", "tuple"> {
  let summary = "a tuple of the types of its elements, in order";
  let parameters = (ins ArrayRefParameter<"::mlir::Type", "element types">:$elements);
  let assemblyFormat = "`<` custom<TypeList>($elements) `>`";
}

def Sammine_ArrayType : Sammine_Type<"Array", "array"> {
  let summary = "a fixed-size array, `[N]T`, a value";
  let description = [{
    `!sammine.array<4, i32>`. Lowered to LLVM it is `!llvm.array<4 x i32>`.
  }];
  let parameters = (ins "int64_t":$size, "::mlir::Type":$element);
  let assemblyFormat = "`<` $size `,` $element `>`";
}

def Sammine_FnType : Sammine_Type<"Fn", "fn"> {
  let summary = "a function value, `(A) -> R`: a function and what it binds";
  let description = [{
    `!sammine.fn<(i32) -> i64>`. Lowered to LLVM it is a closure, the
    identified struct `sammine.closure` of the code to call and an
    environment the code is passed first.
  }];
  let parameters = (ins "::mlir::FunctionType":$signature);
  let assemblyFormat = "`<` $signature `>`";
}

def Sammine_EnumType : Sammine_Type<"Enum", "enum"> {
  let summary = "an enum: its name, its payload's size, and each variant's payload";
  let description = [{
    `!sammine.enum<"Option<i64>", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>`:
    variant k's payload is the k-th tuple's elements. Lowered to LLVM it is
    the identified struct `sammine.enum.<name>`, an i32 tag then the payload
    bytes, which declarations of functions taking or giving one use too:
    payload fields sit at the byte offsets layout:: gives, which the ops
    that read and write them carry.
  }];
  let parameters = (ins StringRefParameter<"the enum's name">:$name,
                        "int64_t":$payloadSize,
                        ArrayRefParameter<"::mlir::StringAttr", "variant names">:$variantNames,
                        ArrayRefParameter<"::mlir::Type", "variant payloads">:$variants);
  let assemblyFormat = "`<` $name `,` $payloadSize `,` `(` custom<FieldList>($variantNames, $variants) `)` `>`";
  let genVerifyDecl = 1;
  let extraClassDeclaration = [{
    /// The types of variant `index`'s payload.
    ::llvm::ArrayRef<::mlir::Type> getPayload(uint64_t index) const {
      return ::llvm::cast<TupleType>(getVariants()[index]).getElements();
    }
  }];
}

def Sammine_OwnedPtr : Type<
    And<[Sammine_PtrType.predicate,
         CPred<"::llvm::cast<::sammine_lang::smn::PtrType>($_self).isOwned()">]>,
    "owned pointer">;

def Sammine_CharView : Type<
    And<[Sammine_PtrType.predicate,
         CPred<"!::llvm::cast<::sammine_lang::smn::PtrType>($_self).isOwned()">,
         CPred<"::llvm::cast<::sammine_lang::smn::PtrType>($_self).getPointee()"
               ".isInteger(8)">]>,
    "view of char">;

#endif // SAMMINE_TYPES_TD
