#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jmini {

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct Value
{
    std::variant<std::monostate, std::string, double, bool, std::unique_ptr<Array>, std::unique_ptr<Object>> v;

    Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    Value(Value&&) = default;
    Value& operator=(Value&&) = default;

    [[nodiscard]] bool is_null() const { return std::holds_alternative<std::monostate>(v); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(v); }
    [[nodiscard]] bool is_number() const { return std::holds_alternative<double>(v); }
    [[nodiscard]] bool is_bool() const { return std::holds_alternative<bool>(v); }
    [[nodiscard]] bool is_array() const { return std::holds_alternative<std::unique_ptr<Array>>(v); }
    [[nodiscard]] bool is_object() const { return std::holds_alternative<std::unique_ptr<Object>>(v); }

    [[nodiscard]] const std::string& str() const { return std::get<std::string>(v); }
    [[nodiscard]] double num() const { return std::get<double>(v); }
    [[nodiscard]] bool boolean() const { return std::get<bool>(v); }
    [[nodiscard]] const Array& arr() const { return *std::get<std::unique_ptr<Array>>(v); }
    [[nodiscard]] const Object& obj() const { return *std::get<std::unique_ptr<Object>>(v); }

    [[nodiscard]] const Value* find(std::string_view key) const
    {
        if (!is_object())
            return nullptr;
        for (const auto& [k, val] : obj())
        {
            if (k == key)
                return &val;
        }
        return nullptr;
    }

    static Value make_array(Array a)
    {
        Value v;
        v.v = std::make_unique<Array>(std::move(a));
        return v;
    }
    static Value make_object(Object o)
    {
        Value v;
        v.v = std::make_unique<Object>(std::move(o));
        return v;
    }
};

class Parser
{
public:
    explicit Parser(std::string_view s) : mS(s) {}

    Value parse()
    {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (mPos != mS.size())
            throw std::runtime_error("trailing chars in JSON");
        return v;
    }

private:
    std::string_view mS;
    std::size_t mPos = 0;

    void skip_ws()
    {
        while (mPos < mS.size())
        {
            const char c = mS[mPos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++mPos;
            else
                break;
        }
    }

    char peek() const
    {
        if (mPos >= mS.size())
            throw std::runtime_error("unexpected EOF");
        return mS[mPos];
    }

    char next() { return mS[mPos++]; }

    void expect(char c)
    {
        if (mPos >= mS.size() || mS[mPos] != c)
            throw std::runtime_error(std::string("expected '") + c + "'");
        ++mPos;
    }

    Value parse_value()
    {
        skip_ws();
        const char c = peek();
        if (c == '"')
            return parse_string();
        if (c == '{')
            return parse_object();
        if (c == '[')
            return parse_array();
        if (c == 't' || c == 'f')
            return parse_bool();
        if (c == 'n')
            return parse_null();
        return parse_number();
    }

    Value parse_string()
    {
        expect('"');
        std::string out;
        while (mPos < mS.size())
        {
            char c = next();
            if (c == '"')
            {
                Value v; v.v = std::move(out);
                return v;
            }
            if (c == '\\')
            {
                if (mPos >= mS.size()) throw std::runtime_error("bad escape");
                char e = next();
                switch (e)
                {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                default:
                    out += e;
                    break;
                }
            }
            else out += c;
        }
        throw std::runtime_error("unterminated string");
    }

    Value parse_object()
    {
        expect('{');
        Object o;
        skip_ws();
        if (mPos < mS.size() && mS[mPos] == '}')
        {
            ++mPos;
            return Value::make_object(std::move(o));
        }
        while (true)
        {
            skip_ws();
            Value key = parse_string();
            skip_ws();
            expect(':');
            Value val = parse_value();
            o.emplace_back(std::get<std::string>(std::move(key.v)), std::move(val));
            skip_ws();
            if (mPos < mS.size() && mS[mPos] == ',')
            {
                ++mPos;
                continue;
            }
            if (mPos < mS.size() && mS[mPos] == '}')
            {
                ++mPos;
                break;
            }
            throw std::runtime_error("expected , or }");
        }
        return Value::make_object(std::move(o));
    }

    Value parse_array()
    {
        expect('[');
        Array a;
        skip_ws();
        if (mPos < mS.size() && mS[mPos] == ']')
        {
            ++mPos;
            return Value::make_array(std::move(a));
        }
        while (true)
        {
            a.push_back(parse_value());
            skip_ws();
            if (mPos < mS.size() && mS[mPos] == ',')
            {
                ++mPos;
                continue;
            }
            if (mPos < mS.size() && mS[mPos] == ']')
            {
                ++mPos;
                break;
            }
            throw std::runtime_error("expected , or ]");
        }
        return Value::make_array(std::move(a));
    }

    Value parse_bool()
    {
        if (mPos + 4 <= mS.size() && mS.substr(mPos, 4) == "true")
        {
            mPos += 4;
            Value v; v.v = true;
            return v;
        }
        if (mPos + 5 <= mS.size() && mS.substr(mPos, 5) == "false")
        {
            mPos += 5;
            Value v; v.v = false;
            return v;
        }
        throw std::runtime_error("bad bool");
    }

    Value parse_null()
    {
        if (mPos + 4 <= mS.size() && mS.substr(mPos, 4) == "null")
        {
            mPos += 4;
            return Value{};
        }
        throw std::runtime_error("bad null");
    }

    Value parse_number()
    {
        const std::size_t start = mPos;
        if (mPos < mS.size() && mS[mPos] == '-')
            ++mPos;
        while (mPos < mS.size())
        {
            char c = mS[mPos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
                ++mPos;
            else
                break;
        }
        const std::string s{mS.substr(start, mPos - start)};
        Value v;
        v.v = std::stod(s);
        return v;
    }
};

inline void append_escaped_string(std::string& out, std::string_view s)
{
    out += '"';
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\b':
            out += "\\b";
            break;
        default:
            out += c;
            break;
        }
    }
    out += '"';
}

class Writer
{
public:
    Writer& begin_obj()
    {
        value_prelude();
        mOut += '{';
        mFirst.push_back(1);
        return *this;
    }

    Writer& end_obj()
    {
        mOut += '}';
        mFirst.pop_back();
        return *this;
    }

    Writer& begin_arr()
    {
        value_prelude();
        mOut += '[';
        mFirst.push_back(1);
        return *this;
    }

    Writer& end_arr()
    {
        mOut += ']';
        mFirst.pop_back();
        return *this;
    }

    Writer& key(std::string_view k)
    {
        comma();
        append_escaped_string(mOut, k);
        mOut += ':';
        mPendingValue = true;
        return *this;
    }

    Writer& str(std::string_view s)
    {
        value_prelude();
        append_escaped_string(mOut, s);
        return *this;
    }

    Writer& num(double d)
    {
        value_prelude();
        if (d == static_cast<long long>(d))
            mOut += std::to_string(static_cast<long long>(d));
        else
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6g", d);
            mOut += buf;
        }
        return *this;
    }

    Writer& boolean(bool b)
    {
        value_prelude();
        mOut += b ? "true" : "false";
        return *this;
    }

    const std::string& str() const& { return mOut; }
    std::string take() { return std::move(mOut); }

private:
    std::string mOut;
    std::vector<char> mFirst;
    bool mPendingValue = false;

    void value_prelude()
    {
        if (mPendingValue)
        {
            mPendingValue = false;
            return;
        }
        comma();
    }
    void comma()
    {
        if (mFirst.empty())
            return;
        if (mFirst.back())
            mFirst.back() = 0;
        else
            mOut += ',';
    }
};

} // namespace jmini