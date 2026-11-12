include "dialect/SammineDialect.td"
include "dialect/SammineTypes.td"
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

