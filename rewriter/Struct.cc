#include "rewriter/Struct.h"
#include "absl/strings/match.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "core/Context.h"
#include "core/Names.h"
#include "core/core.h"
#include "rewriter/Util.h"
#include "rewriter/rewriter.h"

using namespace std;

namespace sorbet::rewriter {

namespace {

bool isKeywordInitKey(const core::GlobalState &gs, const ast::ExpressionPtr &node) {
    if (auto *lit = ast::cast_tree<ast::Literal>(node)) {
        return lit->isSymbol(gs) && lit->asSymbol(gs) == core::Names::keywordInit();
    }
    return false;
}

} // namespace

vector<ast::ExpressionPtr> Struct::run(core::MutableContext ctx, ast::Assign *asgn) {
    vector<ast::ExpressionPtr> empty;

    if (ctx.state.runningUnderAutogen) {
        return empty;
    }

    auto lhs = ast::cast_tree<ast::UnresolvedConstantLit>(asgn->lhs);
    if (lhs == nullptr) {
        return empty;
    }

    auto send = ast::cast_tree<ast::Send>(asgn->rhs);
    if (send == nullptr) {
        return empty;
    }

    auto recv = ast::cast_tree<ast::UnresolvedConstantLit>(send->recv);
    if (recv == nullptr) {
        return empty;
    }

    if (!ast::isa_tree<ast::EmptyTree>(recv->scope) || recv->cnst != core::Symbols::Struct().data(ctx)->name ||
        send->fun != core::Names::new_() || send->args.empty()) {
        return empty;
    }

    auto loc = asgn->loc;

    ast::MethodDef::ARGS_store newArgs;
    ast::Send::ARGS_store sigArgs;
    ast::ClassDef::RHS_store body;

    bool keywordInit = false;
    if (send->hasKwArgs()) {
        if (send->numPosArgs == 0) {
            // leave bad usages like `Struct.new(keyword_init: true)` untouched so we error later
            return empty;
        }
        if (send->hasKwSplat()) {
            return empty;
        }
        auto [posEnd, kwEnd] = send->kwArgsRange();
        if (kwEnd - posEnd != 2) {
            return empty;
        }

        auto &key = send->args[posEnd];
        if (!isKeywordInitKey(ctx, key)) {
            return empty;
        }

        auto &value = send->args[posEnd + 1];
        if (auto *lit = ast::cast_tree<ast::Literal>(value)) {
            if (lit->isTrue(ctx)) {
                keywordInit = true;
            } else if (!lit->isFalse(ctx)) {
                return empty;
            }
        } else {
            return empty;
        }
    }

    for (int i = 0; i < send->numPosArgs; i++) {
        auto *sym = ast::cast_tree<ast::Literal>(send->args[i]);
        if (!sym || !sym->isSymbol(ctx)) {
            return empty;
        }
        core::NameRef name = sym->asSymbol(ctx);
        auto symLoc = sym->loc;
        if (symLoc.exists() && absl::StartsWith(core::Loc(ctx.file, symLoc).source(ctx), ":")) {
            symLoc = core::LocOffsets{symLoc.beginPos() + 1, symLoc.endPos()};
        }

        sigArgs.emplace_back(ast::MK::Symbol(symLoc, name));
        sigArgs.emplace_back(ast::MK::Constant(symLoc, core::Symbols::BasicObject()));
        auto argName = ast::MK::Local(symLoc, name);
        if (keywordInit) {
            argName = ast::MK::KeywordArg(symLoc, move(argName));
        }
        newArgs.emplace_back(ast::MK::OptionalArg(symLoc, move(argName), ast::MK::Nil(symLoc)));

        body.emplace_back(ast::MK::SyntheticMethod0(symLoc, symLoc, name, ast::MK::RaiseUnimplemented(loc)));
        body.emplace_back(ast::MK::SyntheticMethod1(symLoc, symLoc, name.addEq(ctx), ast::MK::Local(symLoc, name),
                                                    ast::MK::RaiseUnimplemented(loc)));
    }

    // Elem = type_member(fixed: T.untyped)
    {
        auto typeMember =
            ast::MK::Send(loc, ast::MK::Self(loc), core::Names::typeMember(), 0,
                          ast::MK::SendArgs(ast::MK::Symbol(loc, core::Names::fixed()), ast::MK::Untyped(loc)));
        body.emplace_back(
            ast::MK::Assign(loc, ast::MK::UnresolvedConstant(loc, ast::MK::EmptyTree(), core::Names::Constants::Elem()),
                            std::move(typeMember)));
    }

    if (send->block != nullptr) {
        auto &block = ast::cast_tree_nonnull<ast::Block>(send->block);

        // Steal the trees, because the run is going to remove the original send node from the tree anyway.
        if (auto *insSeq = ast::cast_tree<ast::InsSeq>(block.body)) {
            for (auto &&stat : insSeq->stats) {
                body.emplace_back(move(stat));
            }
            body.emplace_back(move(insSeq->expr));
        } else {
            body.emplace_back(move(block.body));
        }

        // NOTE: the code in this block _STEALS_ trees. No _return empty_'s should go after it
    }

    body.emplace_back(ast::MK::SigVoid(loc, std::move(sigArgs)));
    body.emplace_back(ast::MK::SyntheticMethod(loc, loc, core::Names::initialize(), std::move(newArgs),
                                               ast::MK::RaiseUnimplemented(loc)));

    ast::ClassDef::ANCESTORS_store ancestors;
    ancestors.emplace_back(ast::MK::UnresolvedConstant(loc, ast::MK::Constant(loc, core::Symbols::root()),
                                                       core::Names::Constants::Struct()));

    vector<ast::ExpressionPtr> stats;
    stats.emplace_back(ast::MK::Class(loc, loc, std::move(asgn->lhs), std::move(ancestors), std::move(body)));
    return stats;
}

}; // namespace sorbet::rewriter
