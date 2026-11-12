#include "codegen/RangeIteration.h"

namespace sammine_lang {

RangeIteration rangeIteration(const Type &range, const Type &element) {
  auto step = Type::Tuple({range, element});
  auto option = Type::Enum(
      sammine_util::QualifiedName::from_parts(
          {"Option<(" + range.to_string() + ", " + element.to_string() + ")>"}),
      {{/*name=*/"Some", /*payload=*/{step}, /*discriminant=*/std::nullopt},
       {/*name=*/"None", /*payload=*/{}, /*discriminant=*/std::nullopt}});
  return {step, option,
          "Iterator<" + range.to_string() + ", " + element.to_string() +
              ">::next"};
}

} // namespace sammine_lang
