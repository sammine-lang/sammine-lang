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

#endif // SAMMINE_TYPES_TD
