#pragma once
#include <variant>
#include <string>
#include <cstdint>
#include <optional>

namespace db {

    using Value = std::variant<std::monostate, int64_t, std::string>;

    inline bool is_null(const Value& v) { return std::holds_alternative<std::monostate>(v); }

    inline std::string value_to_string(const Value& v) {
        if (std::holds_alternative<std::monostate>(v)) return "null";
        if (auto p = std::get_if<int64_t>(&v)) return std::to_string(*p);
        return std::get<std::string>(v);
    }

    inline std::string json_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;
            }
        }
        return out;
    }

    inline std::string value_to_json(const Value& v) {
        if (std::holds_alternative<std::monostate>(v)) return "null";
        if (auto p = std::get_if<int64_t>(&v)) return std::to_string(*p);
        return "\"" + json_escape(std::get<std::string>(v)) + "\"";
    }

    struct ValueLess {
        bool operator()(const Value& a, const Value& b) const {
            if (a.index() != b.index()) return a.index() < b.index();
            if (auto pa = std::get_if<int64_t>(&a)) return *pa < std::get<int64_t>(b);
            return std::get<std::string>(a) < std::get<std::string>(b);
        }
    };

}