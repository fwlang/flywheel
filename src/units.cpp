// Units: `unitns time`, `unit time.min = 60`, and
// `unitconv 1metric_length.m / 1time.s = 1metric_speed.mps`.
//
// Each namespace becomes hidden classes, `__fwu_time__int` and `__fwu_time__double`, whose
// instances hold a number and the index of its unit; `int[time]` and `float[time]` name them.
// Values keep the unit they were written in (`15s` stays 15 seconds). Unit tables are generated
// as top-level constants, and the methods below do the arithmetic; the compiler maps operators,
// literals, `in` conversions, and unit()/unitns()/type() onto them (see Compiler::unitBinary).
//
// Conversions use exact ratios of the factors as written (`0.01` is 1/100), so converting
// between units whose ratio is whole stays exact for int values. Adding int values converts
// both to the finer unit; a ratio that is not whole raises ValueError (use float[...]).
#include "flywheel/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fw {
namespace {
using i128 = __int128;
// A positive rational number, exact when both parts fit; `value` always holds the double.
struct Ratio {
    i128 num = 1, den = 1;
    bool exact = true;
    double value = 1;
};
i128 gcd(i128 a, i128 b) {
    while (b) {
        auto t = a % b;
        a = b;
        b = t;
    }
    return a < 0 ? -a : a;
}
const i128 limit = static_cast<i128>(1) << 120;
bool multiply(i128 a, i128 b, i128 &out) {
    return !__builtin_mul_overflow(a, b, &out) && out < limit && out > -limit;
}
// A decimal literal (60, 0.01, 2.5e3) as an exact ratio when it fits.
std::optional<Ratio> decimal(const std::string &text) {
    Ratio r;
    try {
        r.value = std::stod(text);
    } catch (...) {
        return std::nullopt;
    }
    i128 mantissa = 0;
    int exponent = 0;
    size_t i = 0;
    bool point = false;
    for (; i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.');
         ++i) {
        if (text[i] == '.') {
            point = true;
            continue;
        }
        if (!multiply(mantissa, 10, mantissa)) {
            r.exact = false;
            return r;
        }
        mantissa += text[i] - '0';
        if (point)
            --exponent;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
        exponent += std::stoi(text.substr(i + 1));
    i128 scale = 1;
    for (int k = 0; k < std::abs(exponent); ++k)
        if (!multiply(scale, 10, scale)) {
            r.exact = false;
            return r;
        }
    if (exponent >= 0) {
        if (!multiply(mantissa, scale, r.num)) {
            r.exact = false;
            return r;
        }
        r.den = 1;
    } else {
        r.num = mantissa;
        r.den = scale;
    }
    auto g = gcd(r.num, r.den);
    if (g > 1) {
        r.num /= g;
        r.den /= g;
    }
    return r;
}
// a / b of two ratios.
Ratio divide(const Ratio &a, const Ratio &b) {
    Ratio r;
    r.value = a.value / b.value;
    if (!a.exact || !b.exact) {
        r.exact = false;
        return r;
    }
    auto g1 = gcd(a.num, b.num), g2 = gcd(a.den, b.den);
    if (!multiply(a.num / g1, b.den / g2, r.num) || !multiply(a.den / g2, b.num / g1, r.den)) {
        r.exact = false;
        return r;
    }
    return r;
}
Ratio product(const Ratio &a, const Ratio &b) {
    Ratio inverse{b.den, b.num, b.exact, 1 / b.value};
    return divide(a, inverse);
}
Ratio factor(const std::string &text, Location loc) {
    auto slash = text.find('/');
    auto top = decimal(text.substr(0, slash));
    auto bottom = slash == std::string::npos ? decimal("1") : decimal(text.substr(slash + 1));
    if (!top || !bottom || !(top->value > 0) || !(bottom->value > 0) || !std::isfinite(top->value) ||
        !std::isfinite(bottom->value))
        throw Error(loc, "a unit's factor must be a positive number");
    return divide(*top, *bottom);
}
double toDouble(i128 v) {
    return static_cast<double>(v);
}
// A double as a Flywheel floating-point literal.
std::string real(double v) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.17g", v);
    std::string s = buffer;
    if (s.find_first_of(".en") == std::string::npos)
        s += ".0";
    return s;
}
std::string quote(const std::string &s) {
    return "\"" + s + "\"";
}
struct Relation {
    std::string left, op, right, result; // namespace names
    size_t resultUnit = 0;
    double k = 1; // result base = left base (op) right base * k
    bool declared = false;
    Location loc;
};

std::string generate(Program &p, const std::vector<UnitNamespace> &imported, bool strict) {
    std::ostringstream out;
    std::map<std::string, UnitNamespace *> byName;
    std::map<std::string, std::vector<Ratio>> factors;
    std::vector<UnitNamespace> foreign = imported;
    std::set<std::string> own;
    for (auto &ns : p.unitNamespaces)
        own.insert(ns.name);
    for (auto &ns : foreign) {
        // A namespace of this module hides an imported one of the same name.
        if (own.count(ns.name))
            continue;
        byName[ns.name] = &ns;
        for (size_t i = 0; i < ns.units.size(); ++i)
            factors[ns.name].push_back(factor(ns.factors[i], ns.unitLocations[i]));
    }
    for (auto &ns : p.unitNamespaces) {
        byName[ns.name] = &ns;
        if (ns.units.empty())
            throw Error(ns.loc, "unit namespace '" + ns.name + "' has no units; add one, such "
                                "as unit " + ns.name + ".base = 1");
        for (size_t i = 0; i < ns.units.size(); ++i)
            factors[ns.name].push_back(factor(ns.factors[i], ns.unitLocations[i]));
        ns.intClass = "__fwu_" + ns.name + "__int";
        ns.realClass = "__fwu_" + ns.name + "__double";
    }
    // Relations between namespaces, with the ones that follow from each declared one.
    std::vector<Relation> relations;
    auto add = [&](Relation r) {
        for (auto &other : relations)
            if (other.left == r.left && other.op == r.op && other.right == r.right) {
                if (other.declared && r.declared)
                    throw Error(r.loc, "unitconv already relates " + r.left + " " + r.op + " " +
                                           r.right);
                if (r.declared)
                    other = r;
                return;
            }
        relations.push_back(r);
    };
    for (auto &c : p.unitConversions) {
        std::vector<double> base;
        bool known = true;
        for (int k = 0; k < 3; ++k)
            known = known && byName.count(c.namespaces[k]);
        if (!known && !strict)
            continue;
        if (!own.count(c.namespaces[0]) && !own.count(c.namespaces[1]) &&
            !own.count(c.namespaces[2]) && known)
            throw Error(c.loc, "unitconv relates namespaces of other modules; relate them in a "
                               "module that declares one of them");
        if (c.op == "/" && known && !own.count(c.namespaces[0]))
            throw Error(c.loc, "unitconv divides " + c.namespaces[0] + ", which another module "
                               "declares; write the relation as a product, such as unitconv 1" +
                               c.namespaces[2] + "." + c.units[2] + " * 1" + c.namespaces[1] +
                               "." + c.units[1] + " = 1" + c.namespaces[0] + "." + c.units[0]);
        for (int k = 0; k < 3; ++k) {
            auto it = byName.find(c.namespaces[k]);
            if (it == byName.end())
                throw Error(c.loc, "unknown unit namespace '" + c.namespaces[k] +
                                       "'; declare it with unitns or import a module that "
                                       "declares it");
            auto &ns = *it->second;
            auto u = std::find(ns.units.begin(), ns.units.end(), c.units[k]);
            if (u == ns.units.end())
                throw Error(c.loc, "unit namespace '" + ns.name + "' has no unit '" + c.units[k] +
                                       "'");
            double amount = std::stod(c.amounts[k]);
            if (!(amount > 0))
                throw Error(c.loc, "unitconv amounts must be positive");
            base.push_back(amount * factors[ns.name][static_cast<size_t>(u - ns.units.begin())].value);
        }
        auto &resultNs = *byName.at(c.namespaces[2]);
        size_t z = static_cast<size_t>(
            std::find(resultNs.units.begin(), resultNs.units.end(), c.units[2]) -
            resultNs.units.begin());
        // c = a op b * k, so k = c_base / (a_base op b_base).
        double k = c.op == "/" ? base[2] / (base[0] / base[1]) : base[2] / (base[0] * base[1]);
        auto &a = c.namespaces[0], &b = c.namespaces[1], &r = c.namespaces[2];
        auto unitOf = [&](const std::string &name) -> size_t {
            // An inferred result is expressed in the unit the declaration used for it.
            for (int j = 0; j < 3; ++j)
                if (c.namespaces[j] == name)
                    return static_cast<size_t>(
                        std::find(byName.at(name)->units.begin(), byName.at(name)->units.end(),
                                  c.units[j]) -
                        byName.at(name)->units.begin());
            return 0;
        };
        add({a, c.op, b, r, z, k, true, c.loc});
        if (c.op == "/") {
            // c = a / b * k  =>  a = c * b / k = b * c / k, and b = a / c * k.
            add({r, "*", b, a, unitOf(a), 1 / k, false, c.loc});
            add({b, "*", r, a, unitOf(a), 1 / k, false, c.loc});
            add({a, "/", r, b, unitOf(b), k, false, c.loc});
        } else {
            // c = a * b * k  =>  c = b * a * k, a = c / b / k, b = c / a / k.
            add({b, "*", a, r, z, k, false, c.loc});
            add({r, "/", b, a, unitOf(a), 1 / k, false, c.loc});
            add({r, "/", a, b, unitOf(b), 1 / k, false, c.loc});
        }
    }
    for (auto &ns : p.unitNamespaces) {
        auto &f = factors[ns.name];
        size_t n = f.size();
        auto table = [&](const std::string &suffix) { return "__fwu_" + ns.name + "__" + suffix; };
        std::string names, base, finer, num, den, mul;
        for (size_t i = 0; i < n; ++i) {
            names += (i ? ", " : "") + quote(ns.units[i]);
            base += (i ? ", " : "") + real(f[i].value);
            for (size_t j = 0; j < n; ++j) {
                auto r = divide(f[i], f[j]);
                auto sep = i || j ? ", " : "";
                finer += sep + std::to_string(f[i].value <= f[j].value ? i : j);
                if (r.exact) {
                    num += sep + real(toDouble(r.num));
                    den += sep + real(toDouble(r.den));
                } else {
                    num += sep + real(r.value);
                    den += std::string(sep) + "1.0";
                }
                bool whole = r.exact && r.den == 1 && r.num <= static_cast<i128>(INT64_MAX);
                mul += sep + (whole ? std::to_string(static_cast<long long>(r.num)) : "0");
            }
        }
        out << "const " << table("names") << " = [" << names << "]\n";
        out << "const " << table("factor") << " = [" << base << "]\n";
        out << "const " << table("finer") << " = [" << finer << "]\n";
        out << "const " << table("num") << " = [" << num << "]\n";
        out << "const " << table("den") << " = [" << den << "]\n";
        out << "const " << table("mul") << " = [" << mul << "]\n";
        auto K = std::to_string(n);
        auto at = [&](const std::string &from, const std::string &to) {
            return "[" + from + " * " + K + " + " + to + "]";
        };
        for (int kind = 0; kind < 2; ++kind) {
            bool isInt = kind == 0;
            auto C = isInt ? ns.intClass : ns.realClass;
            auto T = isInt ? "int" : "double";
            auto self = "inst __fwu_unit";
            out << "class " << C << "\n"
                << "    readonly " << T << " value\n"
                << "    readonly int __fwu_unit\n"
                << "    function to_string returns string\n"
                << "        return \"{inst value}\" + " << table("names") << "[" << self << "]\n"
                << "    end\n"
                << "    function operator -() returns " << C << "\n"
                << "        return " << C << "(-inst value, " << self << ")\n"
                << "    end\n"
                << "    function __fwu_unit_name() returns string\n"
                << "        return " << table("names") << "[" << self << "]\n"
                << "    end\n"
                // The value in unit j, in floating point.
                << "    function __fwu_real(int j) returns double\n"
                << "        return " << (isInt ? "double(inst value)" : "inst value") << " * "
                << table("num") << at(self, "j") << " / " << table("den") << at(self, "j") << "\n"
                << "    end\n"
                << "    function __fwu_base() returns double\n"
                << "        return " << (isInt ? "double(inst value)" : "inst value") << " * "
                << table("factor") << "[" << self << "]\n"
                << "    end\n";
            if (isInt) {
                out << "    function __fwu_exact(int j) returns int\n"
                    << "        if " << self << " == j then\n"
                    << "            return inst value\n"
                    << "        end\n"
                    << "        var m = " << table("mul") << at(self, "j") << "\n"
                    << "        if m == 0 then\n"
                    << "            throw ValueError(\"{inst value}\" + " << table("names") << "["
                    << self << "] + \" is not a whole number of \" + " << table("names")
                    << "[j] + \"; use float[" << ns.name << "]\")\n"
                    << "        end\n"
                    << "        return inst value * m\n"
                    << "    end\n";
                for (auto [name, op] : {std::pair{"add", "+"}, std::pair{"sub", "-"}})
                    out << "    function __fwu_" << name << "(" << C << " o) returns " << C << "\n"
                        << "        var j = " << table("finer") << at(self, "o.__fwu_unit") << "\n"
                        << "        return " << C << "(inst __fwu_exact(j) " << op
                        << " o.__fwu_exact(j), j)\n"
                        << "    end\n";
                out << "    function __fwu_cmp(" << C << " o) returns int\n"
                    << "        var j = " << table("finer") << at(self, "o.__fwu_unit") << "\n"
                    << "        if " << table("mul") << at(self, "j") << " != 0 and "
                    << table("mul") << at("o.__fwu_unit", "j") << " != 0 then\n"
                    << "            var a = inst __fwu_exact(j)\n"
                    << "            var b = o.__fwu_exact(j)\n"
                    << "            if a < b then\n                return -1\n            end\n"
                    << "            if a > b then\n                return 1\n            end\n"
                    << "            return 0\n"
                    << "        end\n"
                    << "        var x = inst __fwu_real(j)\n"
                    << "        var y = o.__fwu_real(j)\n"
                    << "        if x < y then\n            return -1\n        end\n"
                    << "        if x > y then\n            return 1\n        end\n"
                    << "        return 0\n"
                    << "    end\n"
                    << "    function __fwu_scale(int k) returns " << C << "\n"
                    << "        return " << C << "(inst value * k, " << self << ")\n"
                    << "    end\n"
                    << "    function __fwu_floordiv(int k) returns " << C << "\n"
                    << "        return " << C << "(inst value // k, " << self << ")\n"
                    << "    end\n";
                out << "    function __fwu_float() returns " << ns.realClass << "\n"
                    << "        return " << ns.realClass << "(double(inst value), " << self << ")\n"
                    << "    end\n"
                    << "    function __fwu_to(int j) returns " << ns.realClass << "\n"
                    << "        return " << ns.realClass << "(inst __fwu_real(j), j)\n"
                    << "    end\n";
            } else {
                for (auto [name, op] : {std::pair{"add", "+"}, std::pair{"sub", "-"}})
                    out << "    function __fwu_" << name << "(" << C << " o) returns " << C << "\n"
                        << "        var j = " << table("finer") << at(self, "o.__fwu_unit") << "\n"
                        << "        return " << C << "(inst __fwu_real(j) " << op
                        << " o.__fwu_real(j), j)\n"
                        << "    end\n";
                out << "    function __fwu_cmp(" << C << " o) returns int\n"
                    << "        var j = " << table("finer") << at(self, "o.__fwu_unit") << "\n"
                    << "        var x = inst __fwu_real(j)\n"
                    << "        var y = o.__fwu_real(j)\n"
                    << "        if x < y then\n            return -1\n        end\n"
                    << "        if x > y then\n            return 1\n        end\n"
                    << "        return 0\n"
                    << "    end\n"
                    << "    function __fwu_scale(double k) returns " << C << "\n"
                    << "        return " << C << "(inst value * k, " << self << ")\n"
                    << "    end\n"
                    << "    function __fwu_divide(double k) returns " << C << "\n"
                    << "        return " << C << "(inst value / k, " << self << ")\n"
                    << "    end\n"
                    << "    function __fwu_to(int j) returns " << C << "\n"
                    << "        return " << C << "(inst __fwu_real(j), j)\n"
                    << "    end\n";
                for (auto &r : relations) {
                    if (r.left != ns.name)
                        continue;
                    auto &other = *byName.at(r.right);
                    auto &result = *byName.at(r.result);
                    out << "    function __fwu_" << (r.op == "*" ? "mul" : "div") << "__"
                        << other.name << "(" << other.realClass << " o) returns "
                        << result.realClass << "\n"
                        << "        return " << result.realClass << "(inst __fwu_base() "
                        << r.op << " o.__fwu_base() * " << real(r.k) << " / __fwu_" << result.name
                        << "__factor[" << r.resultUnit << "], " << r.resultUnit << ")\n"
                        << "    end\n";
                }
            }
            out << "    function __fwu_ratio(" << C << " o) returns double\n"
                << "        var j = " << table("finer") << at(self, "o.__fwu_unit") << "\n"
                << "        return inst __fwu_real(j) / o.__fwu_real(j)\n"
                << "    end\n"
                << "    function equals(" << C << " o) returns bool\n"
                << "        return inst __fwu_cmp(o) == 0\n"
                << "    end\n"
                // Equal quantities hash alike: the base value, normalized to 12 digits.
                << "    function hash() returns int\n"
                << "        var h = inst __fwu_base()\n"
                << "        if h < 0.0 then\n            h = -h\n        end\n"
                << "        if h == 0.0 or h != h or h > 1e300 then\n            return 0\n"
                << "        end\n"
                << "        while h >= 1000000000000.0 do\n            h = h / 10.0\n        end\n"
                << "        while h < 100000000000.0 do\n            h = h * 10.0\n        end\n"
                << "        return int(h + 0.5)\n"
                << "    end\n"
                << "end\n";
        }
    }
    return out.str();
}
} // namespace
void lowerUnits(Program &p, const std::vector<UnitNamespace> &imported, bool strict) {
    if (p.unitNamespaces.empty()) {
        if (!p.unitConversions.empty() && (strict || imported.empty()))
            throw Error(p.unitConversions[0].loc,
                        imported.empty() ? "unitconv needs unit namespaces; declare them with "
                                           "unitns"
                                         : "unitconv relates namespaces of other modules; relate "
                                           "them in a module that declares one of them");
        p.unitConversions.clear();
        return;
    }
    auto text = generate(p, imported, strict);
    p.unitConversions.clear();
    Program generated;
    try {
        generated = Parser(lex(text)).parse();
    } catch (Error &e) {
        throw Error(p.unitNamespaces[0].loc,
                    std::string("internal error in generated unit code: ") + e.what() + " at line " +
                        std::to_string(e.loc.line));
    }
    auto loc = p.unitNamespaces[0].loc;
    for (auto &c : generated.classes) {
        c.loc = loc;
        for (auto &f : c.fields)
            f.loc = loc;
        for (auto &m : c.methods)
            m.loc = loc;
        p.classes.push_back(std::move(c));
    }
    for (auto &s : p.body)
        generated.body.push_back(std::move(s));
    p.body = std::move(generated.body);
}
} // namespace fw
