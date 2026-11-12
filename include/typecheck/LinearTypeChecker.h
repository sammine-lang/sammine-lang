#pragma once
#include "ast/ASTProperties.h"
#include "ast/Ast.h"
#include "util/Utilities.h"
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

//! \file LinearTypeChecker.h
//! \brief Linear type checker — tracks heap-allocated pointers and enforces
//! they are consumed (freed or moved) exactly once before scope exit.
//!
//! Uses direct recursive dispatch via dyn_cast (no visitor pattern).
//! Runs as a separate pass after BiTypeChecker.

namespace sammine_lang::AST {

enum class VarState { Unconsumed, Consumed };

struct VarInfo {
  VarState state;
  sammine_util::Location def_location;
  sammine_util::Location consume_location;
  std::string name;
  std::string consume_reason;
  // Recursive: per-field/element tracking for wrapper types with linear innards
  std::unordered_map<std::string, VarInfo> children;
  // An owned pointer must itself be consumed, whatever its parts hold. Any
  // other entry is an aggregate, done once all of its parts are.
  bool is_owner = false;
};

using LinearVarMap = std::unordered_map<std::string, VarInfo>;

// Child keys that are not field names: what an owned pointer points at, and
// every element of an array.
inline constexpr const char *kPointeeKey = "*";
inline constexpr const char *kElementKey = "[*]";

class LinearTypeChecker : public sammine_util::Reportee {
  std::vector<LinearVarMap> scope_stack;
  int loop_depth = 0;

  // Scope management
  void push_scope();
  void pop_scope_and_check(sammine_util::Location loc);

  // Variable tracking
  void consume(VarInfo *info, sammine_util::Location loc,
               const std::string &reason = "consumed");
  VarInfo *find_linear(const std::string &name);

  // Report a use of `info` after it was consumed, or before it held a value.
  void report_consumed(const VarInfo &info, sammine_util::Location use_loc,
                       const std::string &what);
  // Moving a whole value moves every part of it.
  void move_whole(VarInfo *info, sammine_util::Location loc,
                  const std::string &reason);
  // A part of `info` already consumed, if any. What an owned pointer points
  // at moves with it whatever its state, so it is not a part here.
  const VarInfo *first_consumed_part(const VarInfo &info) const;
  static void mark_parts_consumed(VarInfo &info, sammine_util::Location loc,
                                  const std::string &reason);
  // A pointee nothing has been stored in yet, and so none of its parts.
  static void mark_unset(VarInfo &info);
  // `target` now holds a value just stored in it. Its states are `source`'s
  // when the value came from a tracked place; otherwise the value is live and
  // what it points at is unset.
  static void fill(VarInfo &target, const VarInfo *source,
                   sammine_util::Location loc);
  // Every owner in `info`'s tree still holding memory.
  static void collect_live_owners(const VarInfo &info,
                                  std::vector<const VarInfo *> &out);
  void report_leaks(const VarInfo &info);
  // The tracked state of `expr` as it is before `expr` is checked, when
  // `expr` names a tracked place.
  std::optional<VarInfo> tracked_state(ExprAST *expr);

  // Get merged view of all scopes for snapshotting
  LinearVarMap snapshot() const;
  void restore(const LinearVarMap &snap);

  // Branch analysis
  enum class BranchAgreement { AllUnconsumed, AllConsumed, Mismatch };

  // Each branch's copy of one variable or part; null where it is absent.
  using BranchCopies = std::vector<const VarInfo *>;

  static BranchAgreement check_agreement(const BranchCopies &branches);
  void apply_consistency(BranchAgreement result, VarInfo *info,
                         const BranchCopies &branches,
                         const std::string &display_name,
                         sammine_util::Location loc);

  void check_branch_consistency(const LinearVarMap &before,
                                const std::vector<LinearVarMap> &branches,
                                sammine_util::Location loc);
  // Agree one variable and, where it stays live, each of its parts.
  void reconcile(VarInfo *current, const VarInfo &before,
                 const BranchCopies &branches, sammine_util::Location loc);

  // Recursive dispatch
  void check_program(ProgramAST *ast);
  void check_func(FuncDefAST *ast);
  void check_block(BlockAST *ast, const Type &result_type);

  // What a parent expression does with a child expression.
  struct Use {
    enum class Kind { Own, Read, Write, TakeAddress };
    Kind kind;
    sammine_util::Location loc;
    const char *action; // consume reason for Own, verb otherwise

    static Use owns(sammine_util::Location loc, const char *reason) {
      return {Kind::Own, loc, reason};
    }
    static Use reads(sammine_util::Location loc, const char *verb) {
      return {Kind::Read, loc, verb};
    }
    static Use writes(sammine_util::Location loc) {
      return {Kind::Write, loc, "write"};
    }
    static Use takes_address(sammine_util::Location loc) {
      return {Kind::TakeAddress, loc, "take the address of"};
    }

    // What this use means for the place an access is made through: writing or
    // taking an address reaches the place; anything else just reads it.
    Use toward_place(sammine_util::Location access_loc, const char *verb) const {
      if (kind == Kind::Write || kind == Kind::TakeAddress)
        return *this;
      return reads(access_loc, verb);
    }
  };

  void check_expr(ExprAST *expr, Use use);
  void apply_use(VarInfo *info, Use use);
  void check_variable(VariableExprAST *ast, Use use);
  void check_field_access(FieldAccessExprAST *ast, Use use);
  VarInfo *find_field(FieldAccessExprAST *ast);
  // The tracked entry a place names, if any: a variable, a field of a place
  // (e.f), or what a place points at (*e).
  VarInfo *find_tracked(ExprAST *expr);
  bool pointer_consumed(DerefExprAST *deref);
  void check_store(BinaryExprAST *ast);
  bool contains_tracked(const VarInfo &owner, const VarInfo *part);
  void check_through_pointer(ExprAST *pointer, Use use,
                             sammine_util::Location loc, const char *verb);
  void check_var_def(VarDefAST *ast);
  void check_binary(BinaryExprAST *ast);
  void check_call(CallExprAST *ast);
  void check_free(FreeExprAST *ast);
  void check_return(ReturnStmtAST *ast);
  void check_if(IfExprAST *ast);
  void check_for(ForExprAST *ast);
  void check_loop_body(std::function<void()> check_body);
  void check_case(CaseExprAST *ast);
  void check_addr_of(AddrOfExprAST *ast);

  // Use-after-consume check: emits error if info is already consumed.
  // Returns true if consumed (caller should bail), false if ok.
  bool check_use_after_consume(VarInfo *info, sammine_util::Location use_loc,
                               const std::string &verb);

  // Report `type` if it holds a non-owning ptr<T> where one is not allowed.
  // `ptr_allowed_here` says whether `type` itself may be a ptr<T>: true for
  // parameters and locals, false for returns and anything stored.
  void check_placement(const Type &type, bool ptr_allowed_here,
                       sammine_util::Location loc, const std::string &where);
  void check_prototype_placement(PrototypeAST *proto);

  // Register a variable as linear if its type is or contains linear.
  void register_if_linear(const std::string &name, const Type &type,
                          sammine_util::Location loc);

  // Register a case arm's payload bindings as linear where their types are.
  void register_case_bindings(const CasePattern &pattern,
                              const Type &scrutinee_type);

  // Reject a wildcard arm that could match a variant with a linear payload.
  void check_wildcard_arm(const CaseExprAST &ast, const CaseArm &wildcard);

  // Each element of a literal moves into the literal.
  void move_elements(const std::vector<std::unique_ptr<ExprAST>> &elements,
                     sammine_util::Location loc, const char *reason);

  // Recursive inner-linear tracking for wrapper types
  void register_inner_linear(VarInfo &parent, const Type &t,
                             sammine_util::Location loc);
  static VarInfo make_entry(std::string name, const Type &type,
                            sammine_util::Location loc);
  static VarInfo *child_of(VarInfo *owner, const std::string &key);

  const ASTProperties *props_ = nullptr;

  // Maps closure variable names to the types of their captured (bound) args.
  // Used at return time: if any captured type containsNonLinearPtr(), the
  // closure cannot escape because it holds a reference to stack-local data.
  std::unordered_map<std::string, std::vector<Type>> closure_captures_;

  // Record the bound arg types of a partial application into closure_captures_.
  void record_closure_captures(CallExprAST *ast, const std::string &dest_var);

public:
  void check(ProgramAST *program, const ASTProperties &props);
};

} // namespace sammine_lang::AST
