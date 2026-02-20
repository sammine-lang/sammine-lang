#include "util/Utilities.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include "util/FileRAII.h"
#include <cassert>
#include <cpptrace/cpptrace.hpp>
#include <cpptrace/from_current.hpp>
#include <cstdlib>
#include <filesystem>
#include <string_view>
namespace sammine_util {
auto get_string_from_file(const std::string &file_name) -> std::string {
  auto file = FileRAII(file_name);

  std::string error_msg =
      fmt::format("Cannot find or open file : {}", file_name);
  abort_on(!file.isOpen(), error_msg);
  auto input = file.getInternalStr();

  return input;
}
[[noreturn]] auto abort(const std::string &message) -> void {
  auto style = stderr_is_tty() ? fg(fmt::terminal_color::bright_red)
                                : fmt::text_style{};
  fmt::print(stderr, style, "[Internal Compiler Error] : {}\n", message);
  fmt::print(stderr, style, "[Generating stack traces]...\n");
  fmt::print(stderr, style, "[Please wait]...\n");
  auto trace = cpptrace::generate_trace();
  trace.print_with_snippets();
  std::abort();
}

namespace {
using IndexPair = Reporter::IndexPair;
using DiagnosticData = Reporter::DiagnosticData;
class Locator {
  IndexPair source_start_end;
  int64_t context_radius;
  const Reporter::DiagnosticData &data;

  int64_t source_index_to_line(int64_t source_index) const {
    auto cmp = [](const auto &a, const auto &b) { return a.first < b.first; };
    auto idx =
        std::ranges::lower_bound(
            data, std::make_pair(source_index, std::string_view("")), cmp) -
        data.begin();
    return idx + (idx == 0);
  }

public:
  Locator(IndexPair source_start_end, int64_t context_radius,
          const Reporter::DiagnosticData &data)
      : source_start_end(source_start_end), context_radius(context_radius),
        data(data) {}

  IndexPair get_lines_indices() const {
    auto [start, end] = source_start_end;
    return {source_index_to_line(start) - 1, source_index_to_line(end) - 1};
  }

  IndexPair get_lines_indices_with_radius() const {
    auto [line_start, line_end] = get_lines_indices();
    line_start = line_start > context_radius ? line_start - context_radius : 0;
    line_end = line_end + context_radius <= int64_t(data.size()) - 1
                   ? line_end + context_radius
                   : data.size() - 1;

    return {line_start, line_end};
  }

  std::tuple<int64_t, int64_t, int64_t>
  get_start_end_of_singular_line_token() const {
    auto [start, end] = source_start_end;
    auto num_row = source_index_to_line(start) - 1;
    auto num_col = start - data[num_row].first;
    return {num_row, num_col, num_col + end - start};
  }

  IndexPair get_row_col() const {
    auto [start, end] = source_start_end;
    auto num_row = source_index_to_line(start) - 1;
    auto num_col = start - data[num_row].first;
    return {num_row, num_col};
  }

  bool is_on_singular_line(int64_t i) {
    auto [og_start, og_end] = this->get_lines_indices();
    return og_start == og_end && i == og_start;
  }
  bool is_on_singular_line() {
    auto [og_start, og_end] = this->get_lines_indices();
    return og_start == og_end;
  }
};
} // namespace

DiagnosticData Reporter::get_diagnostic_data(std::string_view str) {
  DiagnosticData result;
  size_t start = 0;

  while (start < str.size()) {
    size_t end = str.find('\n', start);

    if (end == std::string_view::npos) {
      // Add the last substring if no more newlines are found
      result.push_back({start, str.substr(start)});
      break;
    }

    // Add the substring excluding the newline character
    result.push_back({start, str.substr(start, end - start)});
    start = end + 1;
  }

  return result;
}
void Reporter::indicate_singular_line(ReportKind report_kind, int64_t col_start,
                                      int64_t col_end) const {

  print_fmt(LINE_COLOR, "    |");
  int64_t j = 0;
  for (; j < col_start; j++)
    print_fmt(report_kind, " ");

  for (; j < col_end; j++)
    print_fmt(report_kind, "^");
  print_fmt(report_kind, "\n");
}

void Reporter::report_singular_line(ReportKind report_kind,
                                    const std::vector<std::string> &msgs,
                                    int64_t col_start, int64_t col_end) {
  for (const auto &msg : msgs) {
    print_fmt(LINE_COLOR, "    |");
    for (int64_t j = 0; j < col_start; j++)
      print_fmt(report_kind, " ");
    print_fmt(report_kind, "{}\n", msg);
  }
}
void Reporter::print_data_singular_line(std::string_view msg, int64_t col_start,
                                        int64_t col_end) const {

  for (int64_t j = 0; j < col_start; j++)
    fmt::print(stderr, "{}", msg[j]);
  for (int64_t j = col_start; j < col_end; j++) {
    if (sammine_util::stderr_is_tty())
      fmt::print(stderr, fmt::emphasis::bold, "{}", msg[j]);
    else
      fmt::print(stderr, "{}", msg[j]);
  }
  for (size_t j = col_end; j < msg.size(); j++)
    fmt::print(stderr, "{}", msg[j]);

  fmt::print(stderr, "\n");
}
void Reporter::report_single_msg(std::pair<int64_t, int64_t> index_pair,
                                 const std::vector<std::string> &format_strs,
                                 const ReportKind report_kind) const {
  Locator locator(index_pair, context_radius, diagnostic_data);
  auto [new_start, new_end] = locator.get_lines_indices_with_radius();
  auto [row_num, col_start, col_end] =
      locator.get_start_end_of_singular_line_token();

  print_fmt(LINE_COLOR, "    |");
  print_fmt(fmt::terminal_color::blue, "{}:{}:{}\n", file_name,
            row_num + 1, col_start);
  if (!locator.is_on_singular_line()) {
    for (const auto &s : format_strs) {
      print_fmt(LINE_COLOR, "    |");
      fmt::print(stderr, "{}\n", s);
    }
  }
  for (auto i = new_start; i <= new_end; i++) {
    print_fmt(LINE_COLOR, "{:>4}|", i + 1);
    std::string_view str = diagnostic_data[i].second;

    if (locator.is_on_singular_line(i)) {
      print_data_singular_line(str, col_start, col_end);
      indicate_singular_line(report_kind, col_start, col_end);
      report_singular_line(report_kind, format_strs, col_start, col_end);
    } else {
      fmt::print(stderr, "{}\n", str);
    }
  }
}

void Reporter::report(const Reportee &reports) const {

  bool first = true;
  for (const auto &[loc, report_msg, report_kind, src] : reports) {

    if (!first)
      print_fmt(LINE_COLOR, "----------------------------------------------------------------"
                                "----------\n");

    first = false;
    if (dev_mode) {
      auto msgs = report_msg;
      auto src_file = std::filesystem::path(src.file_name()).filename().string();
      msgs.push_back(fmt::format("[Error-borne --dev location is {}:{}]", src_file, src.line()));
      report_single_msg(loc, msgs, report_kind);
    } else {
      report_single_msg(loc, report_msg, report_kind);
    }
  }

  if (reports.has_message()) {
    print_fmt(fmt::terminal_color::green,
              "\n# Did something seems wrong? Report it via "
              "[https://codeberg.org/badumbatish/sammine-lang/issues]\n");
    print_fmt(fmt::terminal_color::green,
              "# Give us a screenshot of the error as well as your contextual "
              "source code\n");
    print_fmt(fmt::terminal_color::green,
              "----------------------------------------------------------------"
              "----------\n");
  }
}
fmt::terminal_color Reporter::get_color_from(ReportKind report_kind) {
  switch (report_kind) {
  case Reportee::error:
    return fmt::terminal_color::bright_red;
  case Reportee::warn:
    return fmt::terminal_color::bright_yellow;
  case Reportee::diag:
    return fmt::terminal_color::green;
  }
  abort("fail to get color");
}

} // namespace sammine_util
