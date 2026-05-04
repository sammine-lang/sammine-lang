#pragma once

#include <memory>

namespace mlir {
class Pass;
}

namespace sammine_lang {
std::unique_ptr<mlir::Pass> createLowerSammineOpsPass();
}
