#include "ArgParser.h"

#include <limits>

ArgParser::ArgParser(int argc, char *argv[])
{
    // From 1: argv[0] is the program path, not an argument. Included, it would
    // be matched against the options like any other word.
    for (int i = 1; i < argc; ++i) {
        args_.push_back(std::string_view(argv[i]));
    }
}

bool ArgParser::hasOption(const std::string_view& longOpt, const std::string_view& shortOpt)
{
    for (const auto& arg : args_) {
        if (arg == longOpt || arg == shortOpt) return true;
    }
    return false;
}

std::optional<std::string_view> ArgParser::getArgValueStr(const std::string_view& longOpt,
                                                          const std::string_view& shortOpt)
{
    const std::string longPrefix = std::string(longOpt) + "=";
    const std::string shortPrefix = std::string(shortOpt) + "=";
    for (const auto& arg : args_) {
        if (arg.starts_with(longPrefix)) {
            return arg.substr(longPrefix.size());
        }
        if (arg.starts_with(shortPrefix)) {
            return arg.substr(shortPrefix.size());
        }
    }
    return std::nullopt;
}

template<IntegerT T1, IntegerT T2>
T1 safeCastInteger(T2 value, const std::string_view& opt)
{
    if (value < static_cast<T2>(std::numeric_limits<T1>::min()) ||
        value > static_cast<T2>(std::numeric_limits<T1>::max())) {
        throw ArgParser::ParserException{"Value out of range for option " + std::string(opt) +
                                         ": " + std::to_string(value)};
    }
    return static_cast<T1>(value);
}

template<IntegerT T>
std::optional<T> ArgParser::getArgValueInt(const std::string_view& longOpt,
                                           const std::string_view& shortOpt)
{
    auto strOpt = getArgValueStr(longOpt, shortOpt);
    if (!strOpt) return std::nullopt;

    const std::string text(*strOpt);
    size_t parsed = 0;
    try {
        T result;
        if constexpr (UIntegerT<T>) {
            // stoull wraps a negative literal round to a huge positive value,
            // which for uint64_t would then pass the range check below.
            if (text.starts_with('-')) {
                throw ParserException{"Value out of range for option " + std::string(longOpt) +
                                      ": " + text};
            }
            // Narrowed to the fixed-width type first: stoull returns unsigned
            // long long, which is a distinct type from uint64_t here and
            // wouldn't satisfy IntegerT.
            uint64_t value = std::stoull(text, &parsed);
            result = safeCastInteger<T>(value, longOpt);
        }
        else {
            int64_t value = std::stoll(text, &parsed);
            result = safeCastInteger<T>(value, longOpt);
        }

        // stoull/stoll stop at the first character that isn't part of the
        // number and report success, so "5abc" would silently parse as 5.
        if (parsed != text.size()) {
            throw ParserException{"Trailing characters in value for option " + std::string(longOpt) +
                                  ": '" + text + "'"};
        }
        return result;
    } catch (const ParserException&) {
        throw; // already says what is wrong with the value - don't flatten it
    } catch (const std::exception&) {
        throw ArgParser::ParserException{"Invalid integer value for option " + std::string(longOpt)};
    }
}

// Explicit template instantiations for supported integer types
template std::optional<uint64_t> ArgParser::getArgValueInt<uint64_t>(const std::string_view&, const std::string_view&);
template std::optional<uint32_t> ArgParser::getArgValueInt<uint32_t>(const std::string_view&, const std::string_view&);
template std::optional<uint16_t> ArgParser::getArgValueInt<uint16_t>(const std::string_view&, const std::string_view&);
template std::optional<uint8_t> ArgParser::getArgValueInt<uint8_t>(const std::string_view&, const std::string_view&);
template std::optional<int64_t> ArgParser::getArgValueInt<int64_t>( const std::string_view&, const std::string_view&);
template std::optional<int32_t> ArgParser::getArgValueInt<int32_t>( const std::string_view&, const std::string_view&);
template std::optional<int16_t> ArgParser::getArgValueInt<int16_t>( const std::string_view&, const std::string_view&);
template std::optional<int8_t> ArgParser::getArgValueInt<int8_t>( const std::string_view&, const std::string_view&);
