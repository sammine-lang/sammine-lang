#include "codegen/RangeIteration.h"

namespace sammine_lang {

RangeIteration rangeIteration(const Type &range, const Type &element) {
  auto step = Type::Tuple({range, element});
  auto option = Type::Enum(
      sammine_util::QualifiedName::from_parts({"Option"}).with_type_args(
          {step}),
      {{/*name=*/"Some", /*payload=*/{step}, /*discriminant=*/std::nullopt},
       {/*name=*/"None", /*payload=*/{}, /*discriminant=*/std::nullopt}});
  return {step, option,
          MonomorphizedKey::instance("Iterator", {range, element})
              .to_typeclass_name("next")};
}

} // namespace sammine_lang
