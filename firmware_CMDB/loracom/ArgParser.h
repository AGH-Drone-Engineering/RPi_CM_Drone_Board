#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

template<typename T>
concept UIntegerT = std::same_as<T, uint64_t> || std::same_as<T, uint32_t> ||
                     std::same_as<T, uint16_t> || std::same_as<T, uint8_t>;
template<typename T>
concept SIntegerT = std::same_as<T, int64_t> || std::same_as<T, int32_t> ||
                     std::same_as<T, int16_t> || std::same_as<T, int8_t>;
template<typename T>
concept IntegerT = UIntegerT<T> || SIntegerT<T>;

class ArgParser
{
    using ArgumentVector = std::vector<std::string_view>;

public:
    struct ParserException : public std::exception
    {
        explicit ParserException(std::string message) : message_(std::move(message)) {}
        const char* what() const noexcept override { return message_.c_str(); }

        std::string message_;
    };

    ArgParser(int argc, char *argv[]);

    bool hasOption(const std::string_view& longOpt, const std::string_view& shortOpt);
    std::optional<std::string_view> getArgValueStr(const std::string_view& longOpt, const std::string_view& shortOpt);

    template<IntegerT T>
    std::optional<T> getArgValueInt(const std::string_view& longOpt, const std::string_view& shortOpt);

private:
    ArgumentVector args_;
};
