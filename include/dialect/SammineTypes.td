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
      I32EnumAttrCase<"View", 1, "view">]> {
  let cppNamespace = "::sammine_lang::smn";
  let genSpecializedAttr = 0;
}

def Sammine_PtrType : Sammine_Type<"Ptr", "ptr"> {
  let summary = "a pointer that owns what it points at, or a view of it";
  let description = [{
    `!sammine.ptr<i32, owned>` is sammine's `'ptr<i32>`: whoever holds it
    must free it or hand it on. `!sammine.ptr<i32, view>` is `ptr<i32>`: it
    only looks at memory someone else owns.
  }];
  let parameters = (ins "::mlir::Type":$pointee,
                        EnumParameter<Sammine_Ownership>:$ownership);
  let assemblyFormat = "`<` $pointee `,` $ownership `>`";
  let extraClassDeclaration = [{
    bool isOwned() const { return getOwnership() == Ownership::Owned; }
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
  let summary = "a struct: its name and the types of its fields, in order";
  let description = [{
    `!sammine.struct<"Point", (i32, i32)>`. The name is the struct's, as
    NameResolver gives it (`shapes::Point`, `Range<i32>`): lowered to LLVM
    it is the identified struct `sammine.struct.<name>`, the very type the
    AST codegen makes for it.
  }];
  let parameters = (ins StringRefParameter<"the struct's name">:$name,
                        ArrayRefParameter<"::mlir::Type", "field types">:$fields);
  let assemblyFormat = "`<` $name `,` `(` custom<TypeList>($fields) `)` `>`";
}

def Sammine_TupleType : Sammine_Type<"Tuple", "tuple"> {
  let summary = "a tuple of the types of its elements, in order";
  let parameters = (ins ArrayRefParameter<"::mlir::Type", "element types">:$elements);
  let assemblyFormat = "`<` custom<TypeList>($elements) `>`";
}

def Sammine_ArrayType : Sammine_Type<"Array", "array"> {
  let summary = "a fixed-size array, `[N]T`, a value";
  let description = [{
    `!sammine.array<4, i32>`. Lowered to LLVM it is `!llvm.array<4 x i32>`,
    as the AST codegen has it.
  }];
  let parameters = (ins "int64_t":$size, "::mlir::Type":$element);
  let assemblyFormat = "`<` $size `,` $element `>`";
}

def Sammine_EnumType : Sammine_Type<"Enum", "enum"> {
  let summary = "an enum: its name, its payload's size, and each variant's payload";
  let description = [{
    `!sammine.enum<"Option<i64>", 8, (!sammine.tuple<i64>, !sammine.tuple<>)>`:
    variant k's payload is the k-th tuple's elements. Lowered to LLVM it is
    the identified struct `sammine.enum.<name>`, an i32 tag then the payload
    bytes, the AST codegen's very type for it: payload fields sit at the
    byte offsets layout:: gives, which the ops that read and write them
    carry.
  }];
  let parameters = (ins StringRefParameter<"the enum's name">:$name,
                        "int64_t":$payloadSize,
                        ArrayRefParameter<"::mlir::Type", "variant payloads">:$variants);
  let assemblyFormat = "`<` $name `,` $payloadSize `,` `(` custom<TypeList>($variants) `)` `>`";
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
