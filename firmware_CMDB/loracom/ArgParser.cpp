#include "ArgParser.h"

#include <limits>

ArgParser::ArgParser(int argc, char *argv[])
{
    for (int i = 0; i < argc; ++i) {
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
T1 safeCastInteger(T2 value)
{
    if (value < static_cast<T2>(std::numeric_limits<T1>::min()) ||
        value > static_cast<T2>(std::numeric_limits<T1>::max())) {
        throw ArgParser::ParserException{"Integer value out of range for target type"};
    }
    return static_cast<T1>(value);
}

template<IntegerT T>
std::optional<T> ArgParser::getArgValueInt(const std::string_view& longOpt,
                                           const std::string_view& shortOpt)
{
    auto strOpt = getArgValueStr(longOpt, shortOpt);
    if (!strOpt) return std::nullopt;
    try {
        if constexpr (UIntegerT<T>) {
            uint64_t value = std::stoull(std::string(*strOpt));
            return safeCastInteger<T>(value);
        }
        else {
            int64_t value = std::stoll(std::string(*strOpt));
            return safeCastInteger<T>(value);
        }
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
