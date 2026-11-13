#pragma once
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//! \file FormatString.h
//! \brief The format `print` and `panic` take: text with a `{}` where each
//! argument goes.

namespace sammine_util {

/// A format, read: its text and placeholders, in order. `{}` is where an
/// argument is printed, as its type prints; `{.2}` prints a float with two
/// digits after the point; `{{` and `}}` are a brace each.
struct FormatString {
  struct Piece {
    /// The text as it is printed, for a piece that is no placeholder.
    std::string text;
    bool placeholder = false;
    /// A placeholder's digits after the point, if it says.
    std::optional<unsigned> precision;
  };
  std::vector<Piece> pieces;

  /// The placeholders, in order: one for each argument.
  std::vector<const Piece *> placeholders() const {
    std::vector<const Piece *> result;
    for (auto &piece : pieces)
      if (piece.placeholder)
        result.push_back(&piece);
    return result;
  }

  /// `format` read, or what is wrong with it.
  static std::expected<FormatString, std::string>
  parse(std::string_view format) {
    FormatString result;
    std::string text;
    auto flush = [&] {
      if (!text.empty())
        result.pieces.push_back({std::move(text), false, std::nullopt});
      text.clear();
    };
    for (size_t i = 0; i < format.size(); ++i) {
      char c = format[i];
      bool doubled = i + 1 < format.size() && format[i + 1] == c;
      if ((c == '{' || c == '}') && doubled) {
        text += c;
        ++i;
        continue;
      }
      if (c == '}')
        return std::unexpected("a '}' with no '{' before it; write '}}' "
                               "for a brace");
      if (c != '{') {
        text += c;
        continue;
      }
      auto close = format.find('}', i);
      if (close == std::string_view::npos)
        return std::unexpected("a '{' with no '}' after it; write '{{' for "
                               "a brace");
      auto spec = format.substr(i + 1, close - i - 1);
      Piece piece{{}, true, std::nullopt};
      if (!spec.empty()) {
        auto digits = spec.substr(1);
        bool numbered = spec.front() == '.' && !digits.empty() &&
                        digits.size() <= 2 &&
                        digits.find_first_not_of("0123456789") ==
                            std::string_view::npos;
        if (!numbered)
          return std::unexpected(
              "'{" + std::string(spec) +
              "}' is no placeholder: write '{}', or '{.2}' for a float's "
              "digits after the point");
        piece.precision = static_cast<unsigned>(std::stoul(std::string(digits)));
      }
      flush();
      result.pieces.push_back(std::move(piece));
      i = close;
    }
    flush();
    return result;
  }
};

} // namespace sammine_util
