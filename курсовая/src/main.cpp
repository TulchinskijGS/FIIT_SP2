#include <cctype>
#include "catalog.h"
#include "executor.h"
#include "lexer.h"
#include "parser.h"
#include <iostream>
#include <fstream>
#include <sstream>

using namespace db;

static std::string to_json(const QueryResult& r) {
    if (!r.ok) return "{\"error\":\"" + json_escape(r.message) + "\"}";
    std::string out = "[";
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) out += ",";
        out += "{";
        for (size_t j = 0; j < r.column_names.size(); ++j) {
            if (j) out += ",";
            out += "\"" + r.column_names[j] + "\":";
            out += value_to_json(r.rows[i][j]);
        }
        out += "}";
    }
    out += "]";
    return out;
}

static void run_one(const std::string& sql, Catalog& cat, Executor& ex) {
    bool has_content = false;
    for (char c : sql) {
        if (!std::isspace(static_cast<unsigned char>(c)) && c != ';') {
            has_content = true;
            break;
        }
    }
    if (!has_content) return;

    try {
        Lexer lex(sql);
        auto toks = lex.tokenize();
        Parser p(std::move(toks));
        auto stmt = p.parse_statement();
        auto res = ex.execute(stmt.get());

        if (!res.ok) {
            std::cout << "ERROR: " << res.message << std::endl;
        } else if (!res.rows.empty()) {
            std::cout << to_json(res) << std::endl;
        } else if (!res.message.empty()) {
            std::cout << res.message << std::endl;
        } else {
            std::cout << "OK" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "ERROR: " << e.what() << std::endl;
    }
}

int main(int argc, char** argv) {
    Catalog cat("data");
    Executor ex(cat);

    if (argc >= 2) {
        std::ifstream f(argv[1]);
        if (!f) { std::cerr << "Cannot open " << argv[1] << "\n"; return 1; }
        std::stringstream ss; ss << f.rdbuf();
        std::string content = ss.str();

        std::string cur;
        bool in_str = false;
        for (char c : content) {
            if (c == '"') in_str = !in_str;
            cur += c;
            if (c == ';' && !in_str) {
                if (!cur.empty()) run_one(cur, cat, ex);
                cur.clear();
            }
        }
        if (!cur.empty()) run_one(cur, cat, ex);
        return 0;
    }

    std::cout << "MiniDB (B-tree). Type SQL ending with ';'. Ctrl+D to exit.\n";
    std::string line, buf;
    while (true) {
        std::cout << (buf.empty() ? "db> " : "... ");
        if (!std::getline(std::cin, line)) break;
        buf += line;
        buf += '\n';
        if (buf.find(';') != std::string::npos) {
            run_one(buf, cat, ex);
            buf.clear();
        }
    }
    return 0;
}