#pragma once
#include "fmt/base.h"
#include "fmt/color.h"
#include "fmt/core.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

//! \file Utilities.h
//! \brief Holds classes and functionalities for dealing with Error handling,
//! source locations caching & indexing

namespace sammine_util {
auto get_string_from_file(const std::string &file_name) -> std::string;
inline int64_t unique_ast_id = 0;

template <typename T>
concept explicitly_bool_like = requires(T t) {
  { static_cast<bool>(t) } -> std::same_as<bool>;
};
[[noreturn]] auto abort(const std::string &message = "<NO MESSAGE>") -> void;

template <explicitly_bool_like T>
void abort_on(const T &condition, const std::string &message = "<NO MESSAGE>") {
  if (static_cast<bool>(condition)) {
    abort(message);
  }
}

template <explicitly_bool_like T>
void abort_if_not(const T &condition,
                  const std::string &message = "<NO MESSAGE>") {
  if (!static_cast<bool>(condition)) {
    abort(message);
  }
}
inline void log_diagnostics(const std::string &diagnostics) {
  fmt::print(stderr, fg(fmt::terminal_color::bright_green), "{}\n",
             diagnostics);
}

//! A class representing a location for sammine-lang, this is helpful in
//! debugging

//! The default constructor gives 0,0.
//! When we don't want the Reporter to print out anything, construct it with
//! NonPrintableLocation
//!
class Location {
public:
  // True location in original source code string
  int64_t source_start, source_end;

  // Default constructor
  Location() : source_start(0), source_end(0) {}
  static Location NonPrintable() { return Location(-1, -1); }
  Location(int64_t source_start, int64_t source_end)
      : source_start(source_start), source_end(source_end) {}

  // Advance column position
  inline void advance() { source_end++; }

  // Move column position backwards
  inline void devance() {
    source_end--;
    assert(source_end >= source_start);
  }

  // Handle newline
  inline void newLine() { advance(); }

  // Combine two locations
  Location operator|(const Location &other) const {
    Location result;
    result.source_start = std::min(source_start, other.source_start);
    result.source_end = std::max(source_end, other.source_end);
    return result;
  }

  void operator|=(const Location &other) {
    source_start = std::min(source_start, other.source_start);
    source_end = std::max(source_end, other.source_end);
  }
  operator std::pair<int64_t, int64_t>() const {
    return std::make_pair(source_start, source_end);
  }

  // Stream output operator
  friend std::ostream &operator<<(std::ostream &out, const Location &loc) {
    out << loc.source_start << ":" << loc.source_end;

    return out;
  }

  // Equality operator
  bool operator==(const Location &other) const {
    return source_start == other.source_start && source_end == other.source_end;
  }
};
class Reportee {
public:
  enum ReportKind {
    error,
    warn,
    diag,
  };
  using Report = std::tuple<Location, std::string, ReportKind>;

  using iterator = std::vector<Report>::iterator;
  using const_iterator = std::vector<Report>::const_iterator;

  // Iterator methods
  iterator begin() { return reports.begin(); }
  iterator end() { return reports.end(); }
  const_iterator begin() const { return reports.begin(); }
  const_iterator end() const { return reports.end(); }
  const_iterator cbegin() const { return reports.cbegin(); }
  const_iterator cend() const { return reports.cend(); }
  [[noreturn]] virtual void abort(const std::string &msg = "<NO MESSAGE>") {
    sammine_util::abort(msg);
  }
  template <explicitly_bool_like T>
  void abort_on(const T &condition,
                const std::string &message = "<NO MESSAGE>") {
    if (static_cast<bool>(condition)) {
      this->abort(message);
    }
  }

  template <explicitly_bool_like T>
  void abort_if_not(const T &condition,
                    const std::string &message = "<NO MESSAGE>") {
    if (!static_cast<bool>(condition)) {
      this->abort(message);
    }
  }
  void add_error(Location loc, std::string msg) {
    reports.push_back({loc, msg, ReportKind::error});
    error_count++;
  }
  void add_warn(Location loc, std::string msg) {
    reports.push_back({loc, msg, ReportKind::warn});
    warn_count++;
  }
  void add_diagnostics(Location loc, std::string msg) {
    reports.push_back({loc, msg, ReportKind::diag});
    diag_count++;
  }

  [[nodiscard]] virtual bool has_errors() const { return error_count > 0; }
  [[nodiscard]] bool has_warn() const { return warn_count > 0; }
  [[nodiscard]] bool has_message() const { return !reports.empty(); }

  [[nodiscard]] bool has_diagnostics() const { return diag_count > 0; }

  [[nodiscard]] int64_t get_error_count() const { return error_count; }

  [[nodiscard]] int64_t get_warn_count() const { return warn_count; }
  [[nodiscard]] int64_t get_diagnostic_count() const { return diag_count; }

protected:
  std::vector<Report> reports;
  int64_t error_count = 0;
  int64_t warn_count = 0;
  int64_t diag_count = 0;
};

class Reporter {
public:
  using ReportKind = Reportee::ReportKind;
  using IndexPair = std::pair<int64_t, int64_t>;
  using DiagnosticData = std::vector<std::pair<std::int64_t, std::string_view>>;

private:
  static DiagnosticData get_diagnostic_data(std::string_view str);
  inline static fmt::terminal_color LINE_COLOR =
      fmt::terminal_color::bright_magenta;

  inline static fmt::terminal_color MSG_COLOR =
      fmt::terminal_color::bright_blue;
  std::string file_name;
  std::string input;
  std::vector<std::pair<std::int64_t, std::string_view>> diagnostic_data;
  int64_t context_radius;
  static fmt::terminal_color get_color_from(ReportKind report_kind);

  void report_single_msg(std::pair<int64_t, int64_t> index_pair,
                         const std::string &format_str,
                         const ReportKind report_kind) const;

  template <typename... T>
  static void print_fmt(fmt::terminal_color ts,
                        fmt::format_string<T...> format_str, T &&...args) {
    fmt::print(stderr, fg(ts), format_str, std::forward<T>(args)...);
  }
  template <typename... T>
  static void print_fmt(fmt::color ts, fmt::format_string<T...> format_str,
                        T &&...args) {
    fmt::print(stderr, fg(ts), format_str, std::forward<T>(args)...);
  }
  template <typename... T>
  static void print_fmt(const ReportKind report_kind,
                        fmt::format_string<T...> format_str, T &&...args) {
    fmt::print(stderr, fg(get_color_from(report_kind)), format_str,
               std::forward<T>(args)...);
  }

  void indicate_singular_line(ReportKind report_kind, int64_t col_start,
                              int64_t col_end) const;

  static void report_singular_line(ReportKind report_kind,
                                   const std::string &msg, int64_t col_start,
                                   int64_t col_end);

  void print_data_singular_line(std::string_view msg, int64_t col_start,
                                int64_t col_end) const;

public:
  void report(const Reportee &reports) const;
  void immediate_error(const std::string &str, Location l = Location(-1, -1)) {
    if (l.source_start <= 0 && l.source_end <= 0) {
      print_fmt(LINE_COLOR, "    |");
      print_fmt(fmt::terminal_color::bright_blue, "In {}\n", file_name);
      report_singular_line(ReportKind::error, str, 0, 0);

    } else {
      report_single_msg(l, str, ReportKind::error);
    }
  }
  void immediate_diag(const std::string &str, Location l = Location(-1, -1)) {
    if (l.source_start <= 0 && l.source_end <= 0) {
      print_fmt(LINE_COLOR, "    |");
      print_fmt(fmt::terminal_color::bright_blue, "In {}\n", file_name);
      report_singular_line(ReportKind::diag, str, 0, 0);
    } else {
      report_single_msg(l, str, ReportKind::diag);
    }
  }
  Reporter() {}
  Reporter(std::string file_name, std::string input, int64_t context_radius)
      : file_name(file_name), input(input),
        diagnostic_data(get_diagnostic_data(this->input)),
        context_radius(context_radius) {}
};
} // namespace sammine_util
