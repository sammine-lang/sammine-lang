include "mlir/IR/OpBase.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

def Sammine_Dialect : Dialect {
  let name = "sammine";
  let cppNamespace = "::sammine_lang::smn";
}

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
