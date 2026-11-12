#ifndef SAMMINE_DIALECT_TD
#define SAMMINE_DIALECT_TD

include "mlir/IR/OpBase.td"

def Sammine_Dialect : Dialect {
  let name = "sammine";
  let summary = "sammine's GPU transfer ops and its mid-level IR (MIR)";
  let description = [{
    MIR is what a type-checked sammine function is lowered to before LLVM:
    see knowledge_base/plans/mir.md.
  }];
  let cppNamespace = "::sammine_lang::smn";
  let useDefaultTypePrinterParser = 1;
}

#endif // SAMMINE_DIALECT_TD
