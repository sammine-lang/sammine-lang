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
