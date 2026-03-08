// A minimal FileCheck-like utility for testing compiler output
// Supports: CHECK:, CHECK-NEXT:, CHECK-NOT:, CHECK-SAME:, CHECK-LABEL:

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

struct Check {
    enum Type { PLAIN, NEXT, NOT, SAME, LABEL };
    Type type;
    std::string pattern;
    int line_number;
    std::string prefix;
};

class FileChecker {
public:
    FileChecker(const std::string &check_prefix = "CHECK")
        : check_prefix_(check_prefix), verbose_(false) {}

    void set_verbose(bool v) { verbose_ = v; }

    bool parse_check_file(const std::string &filename) {
        std::ifstream file(filename);
        if (!file) {
            std::cerr << "error: Could not open check file: " << filename << "\n";
            return false;
        }

        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            line_num++;
            parse_check_line(line, line_num);
        }

        if (unknown_directive_count_ > 0)
            return false;
        return !checks_.empty();
    }

    bool parse_check_file_from_string(const std::string &content) {
        std::istringstream stream(content);
        std::string line;
        int line_num = 0;
        while (std::getline(stream, line)) {
            line_num++;
            parse_check_line(line, line_num);
        }
        if (unknown_directive_count_ > 0)
            return false;
        return !checks_.empty();
    }

    bool check(std::istream &input) {
        std::vector<std::string> input_lines;
        std::string line;
        while (std::getline(input, line)) {
            input_lines.push_back(line);
        }

        if (checks_.empty()) {
            std::cerr << "error: No check patterns found\n";
            return false;
        }

        size_t input_idx = 0;
        int last_match_line = -1;
        std::vector<std::pair<size_t, size_t>> not_ranges; // Ranges where CHECK-NOT must not match

        for (size_t check_idx = 0; check_idx < checks_.size(); check_idx++) {
            const Check &chk = checks_[check_idx];

            if (chk.type == Check::NOT) {
                // CHECK-NOT: collect and verify after finding next positive match
                continue;
            }

            // Collect any pending CHECK-NOT patterns before this check
            std::vector<const Check *> pending_nots;
            for (size_t i = 0; i < check_idx; i++) {
                if (checks_[i].type == Check::NOT) {
                    // Check if this NOT is between previous positive match and now
                    bool in_range = true;
                    for (size_t j = i + 1; j < check_idx; j++) {
                        if (checks_[j].type != Check::NOT) {
                            in_range = false;
                            break;
                        }
                    }
                    if (in_range) {
                        pending_nots.push_back(&checks_[i]);
                    }
                }
            }

            size_t search_start = input_idx;
            bool found = false;

            if (chk.type == Check::NEXT) {
                // Must match the very next line
                if (last_match_line + 1 < static_cast<int>(input_lines.size())) {
                    size_t next_line = last_match_line + 1;
                    if (match_pattern(input_lines[next_line], chk.pattern)) {
                        // Check pending NOT patterns
                        bool not_failed = false;
                        for (const auto *not_chk : pending_nots) {
                            for (size_t k = search_start; k <= next_line; k++) {
                                if (match_pattern(input_lines[k], not_chk->pattern)) {
                                    std::cerr << chk.prefix << "-NOT: error: found forbidden pattern\n";
                                    std::cerr << "  Pattern: " << not_chk->pattern << "\n";
                                    std::cerr << "  Found at line " << (k + 1) << ": " << input_lines[k] << "\n";
                                    not_failed = true;
                                    break;
                                }
                            }
                        }
                        if (not_failed) return false;

                        found = true;
                        input_idx = next_line + 1;
                        last_match_line = next_line;
                        if (verbose_) {
                            std::cerr << chk.prefix << "-NEXT: matched at line " << (next_line + 1) << "\n";
                        }
                    }
                }
                if (!found) {
                    std::cerr << chk.prefix << "-NEXT: error: expected pattern not found on next line\n";
                    std::cerr << "  Expected pattern: " << chk.pattern << "\n";
                    std::cerr << "  From check file line " << chk.line_number << "\n";
                    if (last_match_line + 1 < static_cast<int>(input_lines.size())) {
                        std::cerr << "  Actual next line (line " << (last_match_line + 2) << "): "
                                  << input_lines[last_match_line + 1] << "\n";
                    } else {
                        std::cerr << "  No more input lines (expected more output)\n";
                    }
                    return false;
                }
            } else if (chk.type == Check::SAME) {
                // Must match on the same line as previous match
                if (last_match_line >= 0 && last_match_line < static_cast<int>(input_lines.size())) {
                    if (match_pattern(input_lines[last_match_line], chk.pattern)) {
                        found = true;
                        if (verbose_) {
                            std::cerr << chk.prefix << "-SAME: matched at line " << (last_match_line + 1) << "\n";
                        }
                    }
                }
                if (!found) {
                    std::cerr << chk.prefix << "-SAME: error: expected pattern not found on same line\n";
                    std::cerr << "  Expected pattern: " << chk.pattern << "\n";
                    std::cerr << "  From check file line " << chk.line_number << "\n";
                    if (last_match_line >= 0) {
                        std::cerr << "  Actual line (line " << (last_match_line + 1) << "): "
                                  << input_lines[last_match_line] << "\n";
                    }
                    return false;
                }
            } else {
                // CHECK: or CHECK-LABEL: - search forward
                for (size_t i = search_start; i < input_lines.size(); i++) {
                    if (match_pattern(input_lines[i], chk.pattern)) {
                        // Check pending NOT patterns in the search range
                        bool not_failed = false;
                        for (const auto *not_chk : pending_nots) {
                            for (size_t k = search_start; k < i; k++) {
                                if (match_pattern(input_lines[k], not_chk->pattern)) {
                                    std::cerr << chk.prefix << "-NOT: error: found forbidden pattern\n";
                                    std::cerr << "  Pattern: " << not_chk->pattern << "\n";
                                    std::cerr << "  Found at line " << (k + 1) << ": " << input_lines[k] << "\n";
                                    not_failed = true;
                                    break;
                                }
                            }
                            if (not_failed) break;
                        }
                        if (not_failed) return false;

                        found = true;
                        input_idx = i + 1;
                        last_match_line = i;
                        if (verbose_) {
                            std::cerr << chk.prefix << ": matched at line " << (i + 1) << "\n";
                        }
                        break;
                    }
                }

                if (!found) {
                    std::cerr << chk.prefix << ": error: expected pattern not found\n";
                    std::cerr << "  Expected pattern: " << chk.pattern << "\n";
                    std::cerr << "  From check file line " << chk.line_number << "\n";
                    std::cerr << "  Searched from line " << (search_start + 1) << " to end\n";

                    // Show actual input lines to help identify the mismatch
                    if (search_start < input_lines.size()) {
                        std::cerr << "\n  Actual input (lines " << (search_start + 1) << "-"
                                  << std::min(search_start + 10, input_lines.size()) << "):\n";
                        for (size_t i = search_start; i < std::min(search_start + 10, input_lines.size()); i++) {
                            std::cerr << "    " << (i + 1) << ": " << input_lines[i] << "\n";
                        }
                        if (input_lines.size() > search_start + 10) {
                            std::cerr << "    ... (" << (input_lines.size() - search_start - 10) << " more lines)\n";
                        }
                    } else {
                        std::cerr << "  No input lines in search range\n";
                    }
                    return false;
                }
            }
        }

        // Check any trailing CHECK-NOT patterns
        std::vector<const Check *> trailing_nots;
        for (size_t i = checks_.size(); i > 0; i--) {
            if (checks_[i - 1].type == Check::NOT) {
                trailing_nots.push_back(&checks_[i - 1]);
            } else {
                break;
            }
        }

        for (const auto *not_chk : trailing_nots) {
            for (size_t k = input_idx; k < input_lines.size(); k++) {
                if (match_pattern(input_lines[k], not_chk->pattern)) {
                    std::cerr << not_chk->prefix << "-NOT: error: found forbidden pattern\n";
                    std::cerr << "  Pattern: " << not_chk->pattern << "\n";
                    std::cerr << "  Found at line " << (k + 1) << ": " << input_lines[k] << "\n";
                    return false;
                }
            }
        }

        return true;
    }

    bool update(const std::string &check_filename, std::istream &input) {
        // Read all actual output lines from stdin
        std::vector<std::string> output_lines;
        std::string line;
        while (std::getline(input, line)) {
            output_lines.push_back(line);
        }

        // Read all lines from the check file
        std::ifstream file(check_filename);
        if (!file) {
            std::cerr << "error: Could not open check file: " << check_filename << "\n";
            return false;
        }

        std::vector<std::string> file_lines;
        while (std::getline(file, line)) {
            file_lines.push_back(line);
        }
        file.close();

        // Find the last RUN line index and remove existing CHECK directive lines
        int last_run_idx = -1;
        std::vector<std::string> filtered_lines;
        for (size_t i = 0; i < file_lines.size(); i++) {
            if (is_check_directive(file_lines[i])) {
                continue; // Strip existing CHECK lines
            }
            if (file_lines[i].find("// RUN:") != std::string::npos) {
                last_run_idx = static_cast<int>(filtered_lines.size());
            }
            filtered_lines.push_back(file_lines[i]);
        }

        // Build new CHECK lines from actual output, skipping blank lines.
        // After a blank gap, use CHECK: (search forward) instead of CHECK-NEXT:.
        // Pad CHECK: with extra spaces so pattern text aligns with CHECK-NEXT:.
        std::string next_directive = check_prefix_ + "-NEXT:";
        std::string plain_directive = check_prefix_ + ":";
        std::string padding(next_directive.size() - plain_directive.size(), ' ');

        std::vector<std::string> check_lines;
        bool first = true;
        bool after_blank = false;
        for (size_t i = 0; i < output_lines.size(); i++) {
            if (output_lines[i].empty() || is_lit_directive(output_lines[i])) {
                after_blank = true;
                continue;
            }
            // Sanitize: strip directory prefixes from paths
            std::string escaped = sanitize_output_line(output_lines[i]);
            if (first || after_blank) {
                check_lines.push_back("// " + plain_directive + padding + " " + escaped);
            } else {
                check_lines.push_back("// " + next_directive + " " + escaped);
            }
            first = false;
            after_blank = false;
        }

        if (output_lines.empty()) {
            std::cerr << "warning: No output lines; existing CHECK lines removed\n";
        }

        // Insert CHECK lines after the last RUN line (or at the top if no RUN line)
        std::vector<std::string> result;
        if (last_run_idx >= 0) {
            // Insert after the last RUN line
            for (int i = 0; i <= last_run_idx; i++) {
                result.push_back(filtered_lines[i]);
            }
            for (const auto &cl : check_lines) {
                result.push_back(cl);
            }
            for (size_t i = last_run_idx + 1; i < filtered_lines.size(); i++) {
                result.push_back(filtered_lines[i]);
            }
        } else {
            // No RUN line found — insert CHECK lines at the top
            for (const auto &cl : check_lines) {
                result.push_back(cl);
            }
            for (const auto &fl : filtered_lines) {
                result.push_back(fl);
            }
        }

        // Write the file back
        std::ofstream out(check_filename);
        if (!out) {
            std::cerr << "error: Could not write to check file: " << check_filename << "\n";
            return false;
        }
        for (size_t i = 0; i < result.size(); i++) {
            out << result[i];
            if (i + 1 < result.size()) {
                out << "\n";
            }
        }
        // Preserve trailing newline
        out << "\n";
        out.close();

        std::cerr << "Updated " << check_filename << " with " << check_lines.size()
                  << " CHECK lines from " << output_lines.size() << " output lines\n";
        return true;
    }

private:
    static std::string sanitize_output_line(const std::string &line) {
        // Strip directory prefixes from .mn paths to just the filename.
        // Matches "some/path/to/foo.mn" or "/abs/path/foo.mn" -> "foo.mn"
        std::regex path_re("[^ |]*/([^/ ]+\\.mn)");
        return std::regex_replace(line, path_re, "$1");
    }

    bool is_lit_directive(const std::string &line) const {
        // Skip output lines containing lit/check directives (RUN:, CHECK:, etc.)
        // These are source context lines echoed by the compiler.
        if (line.find("RUN:") != std::string::npos) return true;
        if (is_check_directive(line)) return true;
        return false;
    }

    bool is_check_directive(const std::string &line) const {
        // Check if line contains any CHECK directive with the configured prefix
        if (line.find(check_prefix_ + ":") != std::string::npos) return true;
        if (line.find(check_prefix_ + "-NEXT:") != std::string::npos) return true;
        if (line.find(check_prefix_ + "-NOT:") != std::string::npos) return true;
        if (line.find(check_prefix_ + "-SAME:") != std::string::npos) return true;
        if (line.find(check_prefix_ + "-LABEL:") != std::string::npos) return true;
        return false;
    }

    void parse_check_line(const std::string &line, int line_num) {
        // Look for CHECK patterns - they can appear anywhere in the line (usually in comments)
        bool found_directive = false;
        auto parse_with_prefix = [&](const std::string &prefix) {
            std::string check_str = prefix + ":";
            std::string check_next = prefix + "-NEXT:";
            std::string check_not = prefix + "-NOT:";
            std::string check_same = prefix + "-SAME:";
            std::string check_label = prefix + "-LABEL:";

            size_t pos;

            if ((pos = line.find(check_label)) != std::string::npos) {
                std::string pattern = line.substr(pos + check_label.length());
                pattern = trim(pattern);
                if (!pattern.empty()) {
                    checks_.push_back({Check::LABEL, pattern, line_num, prefix});
                    found_directive = true;
                }
            } else if ((pos = line.find(check_next)) != std::string::npos) {
                std::string pattern = line.substr(pos + check_next.length());
                pattern = trim(pattern);
                if (!pattern.empty()) {
                    checks_.push_back({Check::NEXT, pattern, line_num, prefix});
                    found_directive = true;
                }
            } else if ((pos = line.find(check_not)) != std::string::npos) {
                std::string pattern = line.substr(pos + check_not.length());
                pattern = trim(pattern);
                if (!pattern.empty()) {
                    checks_.push_back({Check::NOT, pattern, line_num, prefix});
                    found_directive = true;
                }
            } else if ((pos = line.find(check_same)) != std::string::npos) {
                std::string pattern = line.substr(pos + check_same.length());
                pattern = trim(pattern);
                if (!pattern.empty()) {
                    checks_.push_back({Check::SAME, pattern, line_num, prefix});
                    found_directive = true;
                }
            } else if ((pos = line.find(check_str)) != std::string::npos) {
                std::string pattern = line.substr(pos + check_str.length());
                pattern = trim(pattern);
                if (!pattern.empty()) {
                    checks_.push_back({Check::PLAIN, pattern, line_num, prefix});
                    found_directive = true;
                }
            }
        };

        parse_with_prefix(check_prefix_);

        // Warn about lines that look like CHECK directives but aren't recognized.
        // Match PREFIX-<WORD>: where <WORD> is not a known suffix.
        if (!found_directive) {
            std::string prefix_dash = check_prefix_ + "-";
            size_t pos = line.find(prefix_dash);
            if (pos != std::string::npos) {
                // Extract the suffix up to the next ':'
                size_t suffix_start = pos + prefix_dash.length();
                size_t colon = line.find(':', suffix_start);
                if (colon != std::string::npos && colon > suffix_start) {
                    std::string suffix = line.substr(suffix_start, colon - suffix_start);
                    // Only flag if suffix looks like an identifier (all alpha/hyphen)
                    bool looks_like_directive = !suffix.empty();
                    for (char c : suffix) {
                        if (!std::isalpha(c) && c != '-') {
                            looks_like_directive = false;
                            break;
                        }
                    }
                    if (looks_like_directive) {
                        std::cerr << "error: unknown check directive '"
                                  << check_prefix_ << "-" << suffix
                                  << ":' on line " << line_num << "\n";
                        std::cerr << "  Known directives: "
                                  << check_prefix_ << ":, "
                                  << check_prefix_ << "-NEXT:, "
                                  << check_prefix_ << "-NOT:, "
                                  << check_prefix_ << "-SAME:, "
                                  << check_prefix_ << "-LABEL:\n";
                        unknown_directive_count_++;
                    }
                }
            }
        }
    }

    bool match_pattern(const std::string &line, const std::string &pattern) {
        // Simple substring match for now
        // Pattern can contain {{regex}} for regex matching

        std::string regex_pattern;
        size_t i = 0;
        while (i < pattern.length()) {
            if (i + 1 < pattern.length() && pattern[i] == '{' && pattern[i + 1] == '{') {
                // Find closing }}
                size_t end = pattern.find("}}", i + 2);
                if (end != std::string::npos) {
                    regex_pattern += pattern.substr(i + 2, end - i - 2);
                    i = end + 2;
                    continue;
                }
            }
            // Escape regex special chars for literal matching
            char c = pattern[i];
            if (c == '.' || c == '*' || c == '+' || c == '?' || c == '[' || c == ']' ||
                c == '(' || c == ')' || c == '{' || c == '}' || c == '|' || c == '^' ||
                c == '$' || c == '\\') {
                regex_pattern += '\\';
            }
            regex_pattern += c;
            i++;
        }

        try {
            std::regex re(regex_pattern);
            return std::regex_search(line, re);
        } catch (const std::regex_error &) {
            // Fall back to simple substring match
            return line.find(pattern) != std::string::npos;
        }
    }

    std::string trim(const std::string &s) {
        auto start = s.find_first_not_of(" \t");
        if (start == std::string::npos)
            return "";
        auto end = s.find_last_not_of(" \t");
        return s.substr(start, end - start + 1);
    }

    std::string check_prefix_;
    std::vector<Check> checks_;
    bool verbose_;
    int unknown_directive_count_ = 0;
};

void print_usage(const char *prog) {
    std::cerr << "Usage: " << prog << " [options] <check-file>\n";
    std::cerr << "\n";
    std::cerr << "Options:\n";
    std::cerr << "  --input-file <file>   Read input from file instead of stdin\n";
    std::cerr << "  --check-prefix <pfx>  Use <pfx> instead of CHECK (default: CHECK)\n";
    std::cerr << "  --update              Update CHECK lines in check-file to match actual output\n";
    std::cerr << "  -v, --verbose         Verbose mode\n";
    std::cerr << "  -h, --help            Show this help\n";
    std::cerr << "\n";
    std::cerr << "Check directives:\n";
    std::cerr << "  CHECK: <pattern>       Match pattern somewhere in input\n";
    std::cerr << "  CHECK-NEXT: <pattern>  Match pattern on the next line\n";
    std::cerr << "  CHECK-SAME: <pattern>  Match pattern on the same line\n";
    std::cerr << "  CHECK-NOT: <pattern>   Pattern must NOT appear before next match\n";
    std::cerr << "  CHECK-LABEL: <pattern> Like CHECK, but resets search position\n";
    std::cerr << "\n";
    std::cerr << "Patterns can contain {{regex}} for regex matching.\n";
}

int main(int argc, char *argv[]) {
    std::string check_file;
    std::string input_file;
    std::string check_prefix = "CHECK";
    bool verbose = false;
    bool update_mode = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--update") {
            update_mode = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--input-file") {
            if (i + 1 >= argc) {
                std::cerr << "error: --input-file requires an argument\n";
                return 1;
            }
            input_file = argv[++i];
        } else if (arg == "--check-prefix") {
            if (i + 1 >= argc) {
                std::cerr << "error: --check-prefix requires an argument\n";
                return 1;
            }
            check_prefix = argv[++i];
        } else if (arg[0] == '-') {
            std::cerr << "error: Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        } else {
            check_file = arg;
        }
    }

    if (check_file.empty()) {
        std::cerr << "error: No check file specified\n";
        print_usage(argv[0]);
        return 1;
    }

    FileChecker checker(check_prefix);
    checker.set_verbose(verbose);

    if (update_mode) {
        // In update mode, read actual output and rewrite CHECK lines in the file
        bool result;
        if (!input_file.empty()) {
            std::ifstream input(input_file);
            if (!input) {
                std::cerr << "error: Could not open input file: " << input_file << "\n";
                return 1;
            }
            result = checker.update(check_file, input);
        } else {
            result = checker.update(check_file, std::cin);
        }
        return result ? 0 : 1;
    }

    if (!checker.parse_check_file(check_file)) {
        std::cerr << "error: No check patterns found in " << check_file << "\n";
        return 1;
    }

    bool result;
    if (!input_file.empty()) {
        std::ifstream input(input_file);
        if (!input) {
            std::cerr << "error: Could not open input file: " << input_file << "\n";
            return 1;
        }
        result = checker.check(input);
    } else {
        result = checker.check(std::cin);
    }

    return result ? 0 : 1;
}
