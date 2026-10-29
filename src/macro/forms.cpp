#include "check/names.hpp"
#include "macro/eval_internal.hpp"

#include <cstdint>

namespace tisple {

namespace {

// Tisple code that means nothing at compile time; a template builds it instead
const char* const kCodeOnlyForms[] = {
    "const", "struct", "enum",    "macro",   "import",   "make", "as",          "at",
    "load",  "store",  "addr_of", "io_read", "io_write", "jsr",  "wait_cycles", ".",
};

const char* const kTypeWords[] = {"u8", "i8", "u16", "i16", "bool"};

bool InList(const char* const* list, std::size_t count, const std::string& text) {
    for (std::size_t i = 0; i < count; i++) {
        if (text == list[i]) {
            return true;
        }
    }
    return false;
}

bool IsTypeWord(const Node& node) {
    return node.kind == NodeKind::Symbol &&
           InList(kTypeWords, sizeof(kTypeWords) / sizeof(kTypeWords[0]), node.text);
}

bool IsMark(const Node& node, const char* name) {
    return node.kind == NodeKind::List && node.items.size() == 2 &&
           node.items[0].kind == NodeKind::Symbol && node.items[0].text == name;
}

bool NeedBool(MacroRun& run, Frame& frame, const Node& at, const Value& value, const char* what) {
    if (Failed(run)) {
        return false;
    }
    if (value.kind == ValueKind::Bool) {
        return true;
    }
    RunFail(run, frame, at,
            std::string(what) + " needs true or false, got " + DescribeValue(value) +
                "; compare it: (= x 0)");
    return false;
}

bool Arity(MacroRun& run, Frame& frame, const Node& node, std::size_t min, std::size_t max,
           const char* shape) {
    std::size_t given = node.items.size() - 1;
    if (given >= min && given <= max) {
        return true;
    }
    RunFail(run, frame, node, std::string("write ") + shape);
    return false;
}

// ---- templates: ` builds a form, , drops a value in, ,@ splices a list

Value Template(MacroRun& run, Frame& frame, const Node& node);

std::string FreshName(MacroRun& run, Frame& frame, const std::string& text) {
    auto it = frame.fresh.find(text);
    if (it == frame.fresh.end()) {
        run.world->fresh_counter++;
        std::string base = text.substr(0, text.size() - 1);
        std::string name = base + "__" + std::to_string(run.world->fresh_counter);
        it = frame.fresh.emplace(text, name).first;
    }
    return it->second;
}

bool Splice(MacroRun& run, Frame& frame, const Node& item, Value& list) {
    const Node& expr = item.items[1];
    Value value = Eval(run, frame, expr);
    if (Failed(run)) {
        return false;
    }
    if (value.kind == ValueKind::List && value.mark == QuoteMark::None) {
        for (Value& spliced : value.items) {
            list.items.push_back(std::move(spliced));
        }
        return true;
    }
    if (value.written && expr.kind == NodeKind::Symbol) {
        // an argument that is not a list of forms: point at the argument
        Loc loc;
        loc.file = value.file;
        loc.span = value.span;
        loc.expansion = value.expansion;
        AddError(*run.world->errors, loc,
                 ",@" + expr.text + " needs a list of forms; pass ((f a) (g b))");
        run.flow = RunFlow::Failed;
        return false;
    }
    std::string shown;
    PrintFlatNode(expr, shown);
    RunFail(run, frame, item, ",@" + shown + " needs a list, got " + DescribeValue(value));
    return false;
}

Value TemplateList(MacroRun& run, Frame& frame, const Node& node) {
    Value list = ListValue({}, frame.module);
    list.mark = node.mark;
    for (const Node& item : node.items) {
        if (IsMark(item, "unquote_splicing")) {
            if (!Splice(run, frame, item, list)) {
                return Value();
            }
            continue;
        }
        list.items.push_back(Template(run, frame, item));
        if (Failed(run)) {
            return Value();
        }
    }
    return list;
}

Value Template(MacroRun& run, Frame& frame, const Node& node) {
    if (IsMark(node, "unquote")) {
        return Eval(run, frame, node.items[1]);
    }
    if (IsMark(node, "unquote_splicing")) {
        RunFail(run, frame, node, ",@ only works inside a list; write (do ,@body)");
        return Value();
    }
    if (IsMark(node, "quasiquote")) {
        RunFail(run, frame, node, "a ` inside a template is not supported; use one ` per macro");
        return Value();
    }
    if (node.kind == NodeKind::List) {
        return TemplateList(run, frame, node);
    }
    Value value = NodeToValue(node, false, frame.file, frame.module);
    if (node.kind == NodeKind::Symbol && node.text.size() > 1 && node.text.back() == '#') {
        value.kind = ValueKind::Symbol;
        value.text = FreshName(run, frame, node.text);
    }
    return value;
}

// ---- declarations and assignment

Value EvalLet(MacroRun& run, Frame& frame, const Node& node) {
    if (node.items.size() == 4 && IsTypeWord(node.items[2])) {
        RunFail(run, frame, node,
                "macro code has no types; write (let " + node.items[1].text + " value)");
        return Value();
    }
    if (!Arity(run, frame, node, 2, 2, "(let name value)") ||
        !CheckBindingName(run, frame, node.items[1], "let")) {
        return Value();
    }
    const std::string& name = node.items[1].text;
    for (const Binding& binding : frame.scopes.back()) {
        if (binding.name == name) {
            RunFail(run, frame, node.items[1],
                    name + " is already declared in this body; change it with (set " + name +
                        " value)");
            return Value();
        }
    }
    Value value = Eval(run, frame, node.items[2]);
    if (!Failed(run)) {
        frame.scopes.back().push_back(Binding{name, std::move(value)});
    }
    return Value();
}

Value EvalSet(MacroRun& run, Frame& frame, const Node& node) {
    if (!Arity(run, frame, node, 2, 2, "(set name value)")) {
        return Value();
    }
    const Node& name = node.items[1];
    if (name.kind != NodeKind::Symbol) {
        RunFail(run, frame, name, "macro code sets names only: (set x value); lists are values");
        return Value();
    }
    Binding* binding = FindBinding(frame, name.text);
    if (!binding) {
        RunFail(run, frame, name,
                name.text + " is not a let or parameter here; declare it: (let " + name.text +
                    " value)");
        return Value();
    }
    Value value = Eval(run, frame, node.items[2]);
    if (!Failed(run)) {
        binding->value = std::move(value);
    }
    return Value();
}

// ---- control

Value EvalIf(MacroRun& run, Frame& frame, const Node& node) {
    if (!Arity(run, frame, node, 2, 3, "(if c then else)")) {
        return Value();
    }
    Value cond = Eval(run, frame, node.items[1]);
    if (!NeedBool(run, frame, node.items[1], cond, "if")) {
        return Value();
    }
    if (cond.integer != 0) {
        return Eval(run, frame, node.items[2]);
    }
    return node.items.size() == 4 ? Eval(run, frame, node.items[3]) : Value();
}

// one round of a loop body; false when the loop ends here
bool LoopRound(MacroRun& run, Frame& frame, const Node& node, std::size_t first) {
    EvalBody(run, frame, node, first);
    if (run.flow == RunFlow::Continue) {
        run.flow = RunFlow::Normal;
    }
    if (run.flow == RunFlow::Break) {
        run.flow = RunFlow::Normal;
        return false;
    }
    return run.flow == RunFlow::Normal;
}

Value EvalWhile(MacroRun& run, Frame& frame, const Node& node) {
    if (!Arity(run, frame, node, 1, SIZE_MAX, "(while c body ...)")) {
        return Value();
    }
    while (true) {
        Value cond = Eval(run, frame, node.items[1]);
        if (!NeedBool(run, frame, node.items[1], cond, "while") || cond.integer == 0) {
            return Value();
        }
        if (!LoopRound(run, frame, node, 2)) {
            return Value();
        }
    }
}

Value EvalFor(MacroRun& run, Frame& frame, const Node& node) {
    if (node.items.size() >= 5 && IsTypeWord(node.items[2])) {
        RunFail(run, frame, node,
                "macro code has no types; write (for " + node.items[1].text + " start end ...)");
        return Value();
    }
    if (!Arity(run, frame, node, 3, SIZE_MAX, "(for i start end body ...)") ||
        !CheckBindingName(run, frame, node.items[1], "for")) {
        return Value();
    }
    Value start = Eval(run, frame, node.items[2]);
    Value end = Eval(run, frame, node.items[3]);
    if (Failed(run)) {
        return Value();
    }
    if (start.kind != ValueKind::Int || end.kind != ValueKind::Int) {
        RunFail(run, frame, node,
                "for counts over integers, got " + DescribeValue(start) + " to " +
                    DescribeValue(end));
        return Value();
    }
    for (std::int64_t i = start.integer; i < end.integer; i++) {
        frame.scopes.emplace_back();
        frame.scopes.back().push_back(Binding{node.items[1].text, IntValue(i, frame.module)});
        bool more = LoopRound(run, frame, node, 4);
        frame.scopes.pop_back();
        if (!more) {
            break;
        }
    }
    return Value();
}

// a pattern: a literal, 'symbol, true, false or _
bool PatternValue(MacroRun& run, Frame& frame, const Node& pattern, Value& out) {
    if (pattern.kind == NodeKind::Integer || pattern.kind == NodeKind::String ||
        pattern.kind == NodeKind::Keyword || IsMark(pattern, "quote") ||
        (pattern.kind == NodeKind::Symbol && (pattern.text == "true" || pattern.text == "false"))) {
        out = Eval(run, frame, pattern);
        return !Failed(run);
    }
    RunFail(run, frame, pattern,
            "a macro code pattern is a literal, 'symbol, true, false or _; write ('a body ...)");
    return false;
}

Value EvalMatch(MacroRun& run, Frame& frame, const Node& node) {
    if (!Arity(run, frame, node, 1, SIZE_MAX, "(match x (pattern body ...) ...)")) {
        return Value();
    }
    Value value = Eval(run, frame, node.items[1]);
    if (Failed(run)) {
        return Value();
    }
    for (std::size_t i = 2; i < node.items.size(); i++) {
        const Node& clause = node.items[i];
        if (clause.kind != NodeKind::List || clause.mark != QuoteMark::None ||
            clause.items.empty()) {
            RunFail(run, frame, clause, "a match clause is (pattern body ...)");
            return Value();
        }
        const Node& pattern = clause.items[0];
        bool wildcard = pattern.kind == NodeKind::Symbol && pattern.text == "_";
        Value want;
        if (!wildcard && !PatternValue(run, frame, pattern, want)) {
            return Value();
        }
        if (wildcard || ValuesEqual(value, want)) {
            return EvalBody(run, frame, clause, 1);
        }
    }
    RunFail(run, frame, node,
            "no clause matches " + DescribeValue(value) + "; add a last clause (_ body ...)");
    return Value();
}

Value EvalAndOr(MacroRun& run, Frame& frame, const Node& node, bool is_and) {
    if (!Arity(run, frame, node, 2, 2, is_and ? "(and a b)" : "(or a b)")) {
        return Value();
    }
    const char* what = is_and ? "and" : "or";
    Value a = Eval(run, frame, node.items[1]);
    if (!NeedBool(run, frame, node.items[1], a, what)) {
        return Value();
    }
    if ((a.integer != 0) != is_and) {
        return a;
    }
    Value b = Eval(run, frame, node.items[2]);
    if (!NeedBool(run, frame, node.items[2], b, what)) {
        return Value();
    }
    return b;
}

Value EvalNot(MacroRun& run, Frame& frame, const Node& node) {
    if (!Arity(run, frame, node, 1, 1, "(not a)")) {
        return Value();
    }
    Value a = Eval(run, frame, node.items[1]);
    if (!NeedBool(run, frame, node.items[1], a, "not")) {
        return Value();
    }
    return BoolValue(a.integer == 0, frame.module);
}

Value EvalReturn(MacroRun& run, Frame& frame, const Node& node) {
    if (!Arity(run, frame, node, 0, 1, "(return value) or (return)")) {
        return Value();
    }
    Value value = node.items.size() == 2 ? Eval(run, frame, node.items[1]) : Value();
    if (!Failed(run)) {
        run.returned = std::move(value);
        run.flow = RunFlow::Return;
    }
    return Value();
}

// (fn (a b) body ...): a function value that keeps the values it can see now
Value EvalFn(MacroRun& run, Frame& frame, const Node& node) {
    if (node.items.size() >= 2 && node.items[1].kind == NodeKind::Symbol) {
        RunFail(run, frame, node,
                "a fn in macro code has no name or types; write (fn (a b) body ...), or a "
                "macro for a named one");
        return Value();
    }
    if (!Arity(run, frame, node, 2, SIZE_MAX, "(fn (a b) body ...)")) {
        return Value();
    }
    const Node& params = node.items[1];
    if (params.kind != NodeKind::List || params.mark != QuoteMark::None) {
        RunFail(run, frame, params, "write (fn (a b) body ...)");
        return Value();
    }
    auto fn = std::make_shared<FnValue>();
    for (const Node& param : params.items) {
        if (!CheckBindingName(run, frame, param, "fn parameter")) {
            return Value();
        }
        fn->params.push_back(param.text);
    }
    fn->body.assign(node.items.begin() + 2, node.items.end());
    for (const std::vector<Binding>& scope : frame.scopes) {
        fn->captured.insert(fn->captured.end(), scope.begin(), scope.end());
    }
    fn->module = frame.module;
    fn->file = frame.file;
    Value value;
    value.kind = ValueKind::Fn;
    value.fn = std::move(fn);
    value.module = frame.module;
    return value;
}

} // namespace

bool CheckBindingName(MacroRun& run, const Frame& frame, const Node& name, const char* what) {
    if (name.kind != NodeKind::Symbol || name.text.find('.') != std::string::npos ||
        name.text.back() == '#') {
        RunFail(run, frame, name, std::string("a ") + what + " needs a plain name here");
        return false;
    }
    if (IsBuiltinName(name.text) || IsMacroLibraryName(name.text)) {
        RunFail(run, frame, name,
                name.text + " already means something in macro code; rename this " + what);
        return false;
    }
    return true;
}

Value EvalBody(MacroRun& run, Frame& frame, const Node& node, std::size_t first) {
    frame.scopes.emplace_back();
    Value value;
    for (std::size_t i = first; i < node.items.size(); i++) {
        value = Eval(run, frame, node.items[i]);
        if (run.flow != RunFlow::Normal) {
            value = Value();
            break;
        }
    }
    frame.scopes.pop_back();
    return value;
}

bool EvalSpecialForm(MacroRun& run, Frame& frame, const Node& node, const std::string& head,
                     Value& out) {
    if (IsMark(node, "quote")) {
        out = NodeToValue(node.items[1], false, frame.file, frame.module);
    } else if (IsMark(node, "quasiquote")) {
        out = Template(run, frame, node.items[1]);
    } else if (head == "unquote" || head == "unquote_splicing") {
        RunFail(run, frame, node, ", and ,@ only work inside a ` template");
    } else if (head == "let") {
        out = EvalLet(run, frame, node);
    } else if (head == "set") {
        out = EvalSet(run, frame, node);
    } else if (head == "if") {
        out = EvalIf(run, frame, node);
    } else if (head == "do") {
        out = EvalBody(run, frame, node, 1);
    } else if (head == "while") {
        out = EvalWhile(run, frame, node);
    } else if (head == "for") {
        out = EvalFor(run, frame, node);
    } else if (head == "match") {
        out = EvalMatch(run, frame, node);
    } else if (head == "and" || head == "or") {
        out = EvalAndOr(run, frame, node, head == "and");
    } else if (head == "not") {
        out = EvalNot(run, frame, node);
    } else if (head == "return") {
        out = EvalReturn(run, frame, node);
    } else if (head == "break" || head == "continue") {
        if (Arity(run, frame, node, 0, 0, head == "break" ? "(break)" : "(continue)")) {
            run.flow = head == "break" ? RunFlow::Break : RunFlow::Continue;
        }
    } else if (head == "fn") {
        out = EvalFn(run, frame, node);
    } else if (InList(kCodeOnlyForms, sizeof(kCodeOnlyForms) / sizeof(kCodeOnlyForms[0]), head)) {
        RunFail(run, frame, node,
                head + " is Tisple code, not macro code; build it in a template: `(" + head +
                    " ...)");
    } else {
        return false;
    }
    return true;
}

} // namespace tisple
