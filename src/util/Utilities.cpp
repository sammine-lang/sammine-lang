#include "util/Utilities.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include "util/FileRAII.h"
#include <algorithm>
#include <cassert>
#include <cpptrace/cpptrace.hpp>
#include <cstdlib>
#include <filesystem>
#include <map>
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
  auto style =
      stderr_is_tty() ? fg(fmt::terminal_color::bright_red) : fmt::text_style{};
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
        std::ranges::upper_bound(
            data, std::make_pair(source_index, std::string_view("")), cmp) -
        data.begin();
    return std::max(int64_t(idx), int64_t(1));
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
    line_end = line_end + context_radius <= static_cast<int64_t>(data.size()) - 1
                   ? line_end + context_radius
                   : static_cast<int64_t>(data.size()) - 1;

    return {line_start, line_end};
  }

  std::tuple<int64_t, int64_t, int64_t>
  get_start_end_of_singular_line_token() const {
    auto [start, end] = source_start_end;
    auto num_row = source_index_to_line(start) - 1;
    auto num_col = start - data[static_cast<size_t>(num_row)].first;
    return {num_row, num_col, num_col + end - start};
  }

  IndexPair get_row_col() const {
    auto [start, end] = source_start_end;
    auto num_row = source_index_to_line(start) - 1;
    auto num_col = start - data[static_cast<size_t>(num_row)].first;
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

// A report after merging: multiple add_error() calls at the same location
// become one GroupedReport with all their messages combined.
struct GroupedReport {
  Location loc;
  std::vector<std::string> msgs;
  Reportee::ReportKind kind;
  std::vector<std::source_location> srcs; // C++ source locs for --dev mode
};

// A set of GroupedReports whose displayed line ranges overlap, so they can
// share a single source code snippet instead of repeating the same lines.
struct Cluster {
  std::vector<size_t> group_indices; // indices into the GroupedReport vector
  int64_t line_start, line_end;      // overall display range (with context)
};

// Step 1 of error rendering: merge reports that share the same (Location,
// ReportKind) into one GroupedReport, deduplicating identical message strings.
// e.g. two add_error(loc_4_6, "firstly defined x") → one group, one message.
std::vector<GroupedReport> group_reports(const Reportee &reports) {
  std::vector<GroupedReport> groups;
  for (const auto &[loc, report_msg, report_kind, src] : reports) {
    auto it =
        std::find_if(groups.begin(), groups.end(), [&](const GroupedReport &g) {
          return g.loc == loc && g.kind == report_kind;
        });
    if (it != groups.end()) {
      // Merge into existing group, skipping duplicate messages.
      for (const auto &msg : report_msg) {
        if (std::find(it->msgs.begin(), it->msgs.end(), msg) == it->msgs.end())
          it->msgs.push_back(msg);
      }
      // Deduplicate source locations (same file + line) so --dev mode
      // doesn't print the same origin twice.
      bool dup = std::any_of(
          it->srcs.begin(), it->srcs.end(), [&](const std::source_location &s) {
            return s.line() == src.line() &&
                   std::string_view(s.file_name()) == src.file_name();
          });
      if (!dup)
        it->srcs.push_back(src);
    } else {
      groups.push_back({loc, report_msg, report_kind, {src}});
    }
  }
  return groups;
}

// Step 2 of error rendering: cluster groups whose displayed line ranges
// (error line ± context_radius) overlap. Groups are sorted by source line,
// then greedily merged — if a group's range overlaps the previous cluster's,
// it joins that cluster and extends its range.
// e.g. errors on lines 4, 5, 6 with radius=2 all overlap → one cluster.
std::vector<Cluster>
cluster_groups(const std::vector<GroupedReport> &groups, int64_t context_radius,
               const Reporter::DiagnosticData &diagnostic_data) {
  std::vector<size_t> sorted(groups.size());
  for (size_t i = 0; i < sorted.size(); i++)
    sorted[i] = i;
  std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
    Locator la(groups[a].loc.as_pair(), context_radius, diagnostic_data);
    Locator lb(groups[b].loc.as_pair(), context_radius, diagnostic_data);
    return la.get_lines_indices().first < lb.get_lines_indices().first;
  });

  std::vector<Cluster> clusters;
  for (size_t si : sorted) {
    Locator locator(groups[si].loc.as_pair(), context_radius, diagnostic_data);
    auto [ls, le] = locator.get_lines_indices_with_radius();
    if (!clusters.empty() && ls <= clusters.back().line_end) {
      // Overlaps with previous cluster — merge.
      clusters.back().group_indices.push_back(si);
      clusters.back().line_end = std::max(clusters.back().line_end, le);
    } else {
      clusters.push_back({{si}, ls, le});
    }
  }
  return clusters;
}

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
  auto effective_start = col_start;
  auto effective_end = col_end;
  if (effective_start == effective_end) {
    effective_start = std::max(int64_t(0), effective_start - 1);
    effective_end = effective_start + 1;
  }
  int64_t j = 0;
  for (; j < effective_start; j++)
    print_fmt(report_kind, " ");

  for (; j < effective_end; j++)
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
                                        int64_t col_end) {

  for (size_t j = 0; j < static_cast<size_t>(col_start); j++)
    fmt::print(stderr, "{}", msg[j]);
  for (size_t j = static_cast<size_t>(col_start); j < static_cast<size_t>(col_end); j++) {
    if (sammine_util::stderr_is_tty())
      fmt::print(stderr, fmt::emphasis::bold, "{}", msg[j]);
    else
      fmt::print(stderr, "{}", msg[j]);
  }
  for (size_t j = static_cast<size_t>(col_end); j < msg.size(); j++)
    fmt::print(stderr, "{}", msg[j]);

  fmt::print(stderr, "\n");
}
void Reporter::report_single_msg(const Location &loc,
                                 const std::vector<std::string> &format_strs,
                                 const ReportKind report_kind,
                                 std::source_location src) const {
  // Use source_info from the location if available (imported file), otherwise
  // fall back to the Reporter's own data (main file).
  const std::string &diag_file =
      loc.source_info ? loc.source_info->file_name : file_name;
  const DiagnosticData diag_data =
      loc.source_info ? get_diagnostic_data(loc.source_info->source_text)
                      : diagnostic_data;

  Locator locator(loc.as_pair(), context_radius, diag_data);
  auto [new_start, new_end] = locator.get_lines_indices_with_radius();
  auto [row_num, col_start, col_end] =
      locator.get_start_end_of_singular_line_token();

  print_fmt(LINE_COLOR, "    |");
  print_fmt(fmt::terminal_color::blue, "{}:{}:{}\n", diag_file, row_num + 1,
            col_start);
  if (!locator.is_on_singular_line()) {
    for (const auto &s : format_strs) {
      print_fmt(LINE_COLOR, "    |");
      fmt::print(stderr, "{}\n", s);
    }
  }
  for (auto i = new_start; i <= new_end; i++) {
    print_fmt(LINE_COLOR, "{:>4}|", i + 1);
    std::string_view str = diag_data[static_cast<size_t>(i)].second;

    if (locator.is_on_singular_line(i)) {
      print_data_singular_line(str, col_start, col_end);
      indicate_singular_line(report_kind, col_start, col_end);
      report_singular_line(report_kind, format_strs, col_start, col_end);
    } else {
      fmt::print(stderr, "{}\n", str);
    }
  }
  if (dev_mode) {
    auto sf = std::filesystem::path(src.file_name()).filename().string();
    print_fmt(LINE_COLOR, "    |");
    for (int64_t j = 0; j < col_start; j++)
      print_fmt(report_kind, " ");
    print_fmt(report_kind, "[Error-borne `dev` location: {}:{}]\n", sf, src.line());
  }
}

// Render a set of clusters using the given diagnostic_data and file name.
// Extracted to avoid duplicating the rendering code for each source-file
// partition.
static void render_clusters(
    const std::vector<Cluster> &clusters,
    const std::vector<GroupedReport> &groups,
    const Reporter::DiagnosticData &diag_data,
    const std::string &diag_file,
    int64_t context_radius, bool dev_mode, bool &first) {

  struct LineAnnotation {
    int64_t col_start, col_end;
    std::vector<std::string> msgs;
    Reportee::ReportKind kind;
  };

  for (const auto &cluster : clusters) {
    if (!first)
      Reporter::print_fmt(
          Reporter::LINE_COLOR,
          "----------------------------------------------------------------"
          "----------\n");
    first = false;

    std::map<int64_t, std::vector<LineAnnotation>> line_anns;

    for (size_t gi : cluster.group_indices) {
      const auto &g = groups[gi];
      Locator locator(g.loc.as_pair(), context_radius, diag_data);
      int64_t row, cs, ce;
      if (locator.is_on_singular_line()) {
        std::tie(row, cs, ce) = locator.get_start_end_of_singular_line_token();
      } else {
        auto [r, c] = locator.get_row_col();
        row = r;
        cs = c;
        ce = static_cast<int64_t>(diag_data[static_cast<size_t>(r)].second.size());
      }
      auto msgs = g.msgs;
      if (dev_mode) {
        for (const auto &src : g.srcs) {
          auto sf = std::filesystem::path(src.file_name()).filename().string();
          msgs.push_back(fmt::format("[Error-borne `dev` location: {}:{}]", sf,
                                     src.line()));
        }
      }
      line_anns[row].push_back({cs, ce, std::move(msgs), g.kind});
    }

    Reporter::print_fmt(Reporter::LINE_COLOR, "    |");
    if (cluster.group_indices.size() == 1) {
      Locator locator(groups[cluster.group_indices[0]].loc.as_pair(),
                      context_radius, diag_data);
      auto [row, col] = locator.get_row_col();
      Reporter::print_fmt(fmt::terminal_color::blue, "{}:{}:{}\n", diag_file,
                          row + 1, col);
    } else {
      Reporter::print_fmt(fmt::terminal_color::blue, "{}\n", diag_file);
    }

    for (int64_t i = cluster.line_start; i <= cluster.line_end; i++) {
      Reporter::print_fmt(Reporter::LINE_COLOR, "{:>4}|", i + 1);
      std::string_view str = diag_data[static_cast<size_t>(i)].second;
      auto it = line_anns.find(i);
      if (it == line_anns.end() || it->second.empty()) {
        fmt::print(stderr, "{}\n", str);
        continue;
      }
      Reporter::print_data_singular_line(str, it->second[0].col_start,
                                         it->second[0].col_end);
      auto &anns = it->second;
      size_t ai = 0;
      while (ai < anns.size()) {
        size_t run_end = ai + 1;
        while (run_end < anns.size() && anns[run_end].msgs == anns[ai].msgs &&
               anns[run_end].kind == anns[ai].kind)
          run_end++;

        {
          size_t n = run_end - ai;
          auto kind = anns[ai].kind;

          auto eff_span = [&](size_t idx, int64_t &es, int64_t &ee) {
            es = anns[idx].col_start;
            ee = anns[idx].col_end;
            if (es == ee) {
              es = std::max(int64_t(0), es - 1);
              ee = es + 1;
            }
          };
          auto pipe_col = [&](size_t idx) -> int64_t {
            int64_t es, ee;
            eff_span(idx, es, ee);
            return ee - 1;
          };

          auto msg_start = pipe_col(run_end - 1) + 3;

          Reporter::print_fmt(Reporter::LINE_COLOR, "    |");
          int64_t pos = 0;
          for (size_t k = ai; k < run_end; k++) {
            int64_t es, ee;
            eff_span(k, es, ee);
            auto pc = ee - 1;
            for (; pos < es; pos++)
              Reporter::print_fmt(kind, " ");
            for (; pos < pc; pos++)
              Reporter::print_fmt(kind, "\u2500");
            Reporter::print_fmt(kind, "\u252C");
            pos++;
          }
          Reporter::print_fmt(kind, "\n");

          for (size_t mi = n; mi > 0; mi--) {
            Reporter::print_fmt(Reporter::LINE_COLOR, "    |");
            pos = 0;
            for (size_t k = ai; k < ai + mi - 1; k++) {
              auto pc = pipe_col(k);
              for (; pos < pc; pos++)
                Reporter::print_fmt(kind, " ");
              Reporter::print_fmt(kind, "\u2502");
              pos++;
            }
            auto cur_pc = pipe_col(ai + mi - 1);
            for (; pos < cur_pc; pos++)
              Reporter::print_fmt(kind, " ");
            Reporter::print_fmt(kind, "\u2570");
            pos++;
            for (; pos < msg_start - 1; pos++)
              Reporter::print_fmt(kind, "\u2500");
            Reporter::print_fmt(kind, " ");
            pos++;
            auto &cur_msgs = anns[ai + mi - 1].msgs;
            Reporter::print_fmt(kind, "{}\n", cur_msgs[0]);
            for (size_t msi = 1; msi < cur_msgs.size(); msi++) {
              Reporter::print_fmt(Reporter::LINE_COLOR, "    |");
              pos = 0;
              for (size_t k = ai; k < ai + mi - 1; k++) {
                auto pc = pipe_col(k);
                for (; pos < pc; pos++)
                  Reporter::print_fmt(kind, " ");
                Reporter::print_fmt(kind, "\u2502");
                pos++;
              }
              for (; pos < msg_start; pos++)
                Reporter::print_fmt(kind, " ");
              Reporter::print_fmt(kind, "{}\n", cur_msgs[msi]);
            }
          }
        }
        ai = run_end;
      }
    }
  }
}

void Reporter::report(const Reportee &reports) const {

  auto groups = group_reports(reports);

  // Partition groups by source_info pointer so that errors from different
  // files are clustered independently (they have different diagnostic_data).
  std::map<SourceInfo *, std::vector<size_t>> partitions;
  for (size_t i = 0; i < groups.size(); i++)
    partitions[groups[i].loc.source_info.get()].push_back(i);

  bool first = true;

  for (auto &[si_ptr, indices] : partitions) {
    // Build a sub-vector of groups for this partition
    std::vector<GroupedReport> sub_groups;
    sub_groups.reserve(indices.size());
    for (size_t idx : indices)
      sub_groups.push_back(groups[idx]);

    const DiagnosticData &diag_data =
        si_ptr ? get_diagnostic_data(si_ptr->source_text) : diagnostic_data;
    const std::string &diag_file = si_ptr ? si_ptr->file_name : file_name;

    auto clusters = cluster_groups(sub_groups, context_radius, diag_data);
    render_clusters(clusters, sub_groups, diag_data, diag_file, context_radius,
                    dev_mode, first);
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
