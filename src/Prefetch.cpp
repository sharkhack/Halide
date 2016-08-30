#include <algorithm>
#include <map>
#include <string>
#include <limits>

#include "Prefetch.h"
#include "CodeGen_Internal.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Bounds.h"
#include "Scope.h"
#include "Simplify.h"
#include "Substitute.h"
#include "Util.h"

namespace Halide {
namespace Internal {

using std::map;
using std::string;
using std::vector;
using std::stack;

// Prefetch debug levels
int dbg_prefetch0 = 1;
int dbg_prefetch1 = 2;
int dbg_prefetch2 = 3;
int dbg_prefetch3 = 10;

#define MIN(x,y)        (((x)<(y)) ? (x) : (y))
#define MAX(x,y)        (((x)>(y)) ? (x) : (y))

class InjectPrefetch : public IRMutator {
public:
    InjectPrefetch(const map<string, Function> &e)
        : env(e) { }
private:
    const map<string, Function> &env;
    Scope<Interval> scope;
    unsigned long ptmp = 0;   // ID for all tmp vars in a prefetch op

private:
    using IRMutator::visit;

    // Strip down the tuple name, e.g. f.*.var into f
    string tuple_func(const string &name) {
        vector<string> v = split_string(name, ".");
        internal_assert(v.size() > 0);
        return v[0];
    }

    // Strip down the tuple name, e.g. f.*.var into var
    string tuple_var(const string &name) {
        vector<string> v = split_string(name, ".");
        internal_assert(v.size() > 0);
        return v[v.size()-1];
    }

    // Lookup a function in the environment
    Function get_func(const string &name) {
        map<string, Function>::const_iterator iter = env.find(name);
        internal_assert(iter != env.end()) << "function not in environment.\n";
        return iter->second;
    }

    // Determine the static type of a named buffer (if available)
    // Note: the type of input is only known at runtime (use input.elem_size)
    Type get_type(string varname, bool &has_static_type) {
        debug(dbg_prefetch2) << "    getType(" << varname << ")\n";
        Type t = UInt(8);
        map<string, Function>::const_iterator varit = env.find(varname);
        if (varit != env.end()) {
            Function varf = varit->second;
            debug(dbg_prefetch2) << "      found: " << varit->first << "\n";
            if (varf.outputs()) {
                vector<Type> varts = varf.output_types();
                t = varts[0];
                debug(dbg_prefetch2) << "      type: " << t << "\n";
            }
            has_static_type = true;
        } else {
            debug(dbg_prefetch2) << "      could not statically determine type of " << varname
                                 << ", temporarily using: " << t << "\n";
            has_static_type = false;
        }
        return t;
    }

    void visit(const Let *op) {
        Interval in = bounds_of_expr_in_scope(op->value, scope);
        scope.push(op->name, in);
        IRMutator::visit(op);
        scope.pop(op->name);
    }
    void visit(const LetStmt *op) {
        Interval in = bounds_of_expr_in_scope(op->value, scope);
        scope.push(op->name, in);
        IRMutator::visit(op);
        scope.pop(op->name);
    }

    void visit(const For *op) {
        Stmt body = op->body;

        string func_name = tuple_func(op->name);
        string ivar_name = tuple_var(op->name);
        vector<Prefetch> &prefetches = get_func(func_name).schedule().prefetches();

        if (prefetches.empty()) {
            debug(dbg_prefetch2) << "InjectPrefetch: " << op->name << " " << func_name << " " << ivar_name;
            debug(dbg_prefetch2) << " No prefetch\n";
        } else {
            debug(dbg_prefetch1) << "InjectPrefetch: " << op->name << " " << func_name << " " << ivar_name;
            debug(dbg_prefetch1) << " Found prefetch directive(s)\n";
        }

        for (const Prefetch &p : prefetches) {
            debug(dbg_prefetch1) << "InjectPrefetch: check ivar:" << p.var << "\n";
            if (p.var == ivar_name) {
                debug(dbg_prefetch0) << " " << func_name
                                     << " prefetch(" << ivar_name << ", " << p.offset << ")\n";

                // Interval prein(op->name, op->name);
                // Add in prefetch offset
                Expr var = Variable::make(Int(32), op->name);
                Interval prein(var + p.offset, var + p.offset);
                scope.push(op->name, prein);

                map<string, Box> boxes;
                boxes = boxes_required(body, scope);

                int cnt = 0;            // Number of prefetch ops generated
                Stmt pstmts;            // Generated prefetch statements
                debug(dbg_prefetch1) << "  boxes required:\n";
                for (auto &b : boxes) {
                    bool do_prefetch = true;
                    const string &varname = b.first;
                    Box &box = b.second;
                    int dims = box.size();
                    bool has_static_type = true;
                    Type t = get_type(varname, has_static_type);
                    debug(dbg_prefetch0) << "  prefetch" << ptmp << ": "
                                         << varname << " (" << t
                                         << (has_static_type ? "" : ":dynamic_type")
                                         << ", dims:" << dims << ")\n";
                    for (int i = 0; i < dims; i++) {
                        debug(dbg_prefetch1) << "    ---\n";
                        debug(dbg_prefetch1) << "    box[" << i << "].min: " << box[i].min << "\n";
                        debug(dbg_prefetch1) << "    box[" << i << "].max: " << box[i].max << "\n";
                    }
                    debug(dbg_prefetch1) << "    ---------\n";

                    // TODO: Opt: check box if it should be prefetched? 
                    // TODO       - Only prefetch if varying by ivar_name? 
                    // TODO       - Don't prefetch if "small" all constant dimensions?
                    // TODO         e.g. see: camera_pipe.cpp corrected matrix(4,3)

                    string pstr = std::to_string(ptmp++);
                    string varname_prefetch_buf = varname + ".prefetch_" + pstr + "_buf";
                    Expr var_prefetch_buf = Variable::make(Int(32), varname_prefetch_buf);

                    // Make the names for accessing the buffer strides
                    vector<Expr> stride_var(dims);
                    for (int i = 0; i < dims; i++) {
                        string istr = std::to_string(i);
                        string stride_name = varname + ".stride." + istr;
#if 0                   // TODO: Determine if the stride varname is defined - check not yet working
                        // if (!scope.contains(stride_name)) [
                        if (env.find(stride_name) == env.end()) {
                            do_prefetch = false;
                            debug(dbg_prefetch0) << "  " << stride_name << " undefined\n";
                            break;
                        }
#endif
                        stride_var[i] = Variable::make(Int(32), stride_name);
                    }

                    // This box should not be prefetched
                    if (!do_prefetch) {
                        debug(dbg_prefetch0) << "  not prefetching " << varname << "\n";
                        continue;
                    }

                    // Make the names for the prefetch box mins & maxes
                    vector<string> min_name(dims), max_name(dims);
                    for (int i = 0; i < dims; i++) {
                        string istr = std::to_string(i);
                        min_name[i] = varname + ".prefetch_" + pstr + "_min_" + istr;
                        max_name[i] = varname + ".prefetch_" + pstr + "_max_" + istr;
                    }
                    vector<Expr> min_var(dims), max_var(dims);
                    for (int i = 0; i < dims; i++) {
                        min_var[i] = Variable::make(Int(32), min_name[i]);
                        max_var[i] = Variable::make(Int(32), max_name[i]);
                    }

                    // Create a buffer_t object for this prefetch.
                    vector<Expr> args(dims*3 + 2);
                    Expr first_elem = Load::make(t, varname, 0, Buffer(), Parameter());
                    args[0] = Call::make(Handle(), Call::address_of, {first_elem}, Call::PureIntrinsic);
                    args[1] = make_zero(t);
                    for (int i = 0; i < dims; i++) {
                        args[3*i+2] = min_var[i];
                        args[3*i+3] = max_var[i] - min_var[i] + 1;
                        args[3*i+4] = stride_var[i];
                    }

                    // Inject the prefetch call
                    vector<Expr> args_prefetch(3);
                    args_prefetch[0] = dims;
                    if (has_static_type) {
                        args_prefetch[1] = t.bytes();
                    } else {
                        // Element size for inputs that don't have static types
                        string elem_size_name = varname + ".elem_size";
                        Expr elem_size_var = Variable::make(Int(32), elem_size_name);
                        args_prefetch[1] = elem_size_var;
                    }
                    args_prefetch[2] = var_prefetch_buf;
                    // TODO: Opt: Keep running sum of bytes prefetched on this sequence
                    // TODO: Opt: Keep running sum of number of prefetch instructions issued
                    // TODO       on this sequence? (to not exceed MAX_PREFETCH)
                    // TODO: Opt: Generate control code for prefetch_buffer_t in Prefetch.cpp
                    // TODO       Passing box info through a buffer_t results in ~30 additional stores/loads
                    Stmt stmt_prefetch = Evaluate::make(Call::make(Int(32), Call::prefetch_buffer_t,
                                          args_prefetch, Call::Intrinsic));

                    if (cnt == 0) {     // First prefetch in sequence
                        pstmts = stmt_prefetch;
                    } else {            // Add to existing sequence
                        pstmts = Block::make({stmt_prefetch, pstmts});
                    }
                    cnt++;

                    // Inject the create_buffer_t call
                    Expr prefetch_buf = Call::make(type_of<struct buffer_t *>(), Call::create_buffer_t,
                                          args, Call::Intrinsic);
                    pstmts = LetStmt::make(varname_prefetch_buf, prefetch_buf, pstmts);

                    // Inject bounds variable assignments
                    for (int i = dims-1; i >= 0; i--) {
                        pstmts = LetStmt::make(max_name[i], box[i].max, pstmts);
                        pstmts = LetStmt::make(min_name[i], box[i].min, pstmts);
                        // stride already defined by input buffer
                    }
                }

                if (cnt) {
#if 1
                    // Guard to not prefetch past the end of the iteration space
                    // TODO: Opt: Use original extent of loop for guard (avoid conservative guard when stripmined)
                    Expr pcond = likely((Variable::make(Int(32), op->name) + p.offset)
                                                        < (op->min + op->extent - 1));
                    Stmt pguard = IfThenElse::make(pcond, pstmts);
                    body = Block::make({pguard, body});

                    debug(dbg_prefetch3) << "    prefetch: (cnt:" << cnt << ")\n";
                    debug(dbg_prefetch3) << pguard << "\n";
#else
                    body = Block::make({pstmts, body});
#endif
                }

                scope.pop(op->name);
            }
        }

        body = mutate(body);
        stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
    }

};

Stmt inject_prefetch(Stmt s, const std::map<std::string, Function> &env)
{
    size_t read;
    std::string lvl = get_env_variable("HL_DEBUG_PREFETCH", read);
    if (read) {
        int dbg_level = atoi(lvl.c_str());
        dbg_prefetch0 = MAX(dbg_prefetch0 - dbg_level, 0);
        dbg_prefetch1 = MAX(dbg_prefetch1 - dbg_level, 0);
        dbg_prefetch2 = MAX(dbg_prefetch2 - dbg_level, 0);
        dbg_prefetch3 = MAX(dbg_prefetch3 - dbg_level, 0);
    }
    return InjectPrefetch(env).mutate(s);
}

}
}
