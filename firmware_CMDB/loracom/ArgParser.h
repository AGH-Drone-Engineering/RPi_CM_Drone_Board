#include <vector>
#include <string>
#include <string_view>
#include <concepts>

class ArgParser 
{
    concept UIntegerT = std::same_as<uint64_t> || std::same_as<uint32_t> || 
                        std::same_as<uint16_t> || std::same_as<uint8_t>;
    concept SIntegerT = std::same_as<int64_t> || std::same_as<int32_t> || 
                        std::same_as<int16_t> || std::same_as<int8_t>;
    concept IntegerT = UIntegerT || SIntegerT;
    using ArgumentVector = std::vector<std::string_view>;
    struct ParserException : public std::exception {
        std::string message_;
    };
public:
    ArgParser(int argc, char *argv[]);

    bool hasOption(const std::string_view& longOpt, const std::string_view& shortOpt);
    std::optional<std::string_view> getArgValueStr(const std::string_view& longOpt, const std::string_view& shortOpt);
    std::optional<int64_t> getArgValueInt64(const std::string_view& longOpt, const std::string_view& shortOpt);

private:
    ArgumentVector args_;
};