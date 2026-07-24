#include <cerrno>
#include <iostream>
#include <system_error>
#include <vector>
#include <string>
#include <string_view>

#include "ArgParser.h"
#include "LoRaCom.h"

constexpr std::string_view HELP_MSG = 
R"(Usage: loracom [options])
Send and receive messages over LoRa communication on HAT.
Basic options:
  -h,     --help        Show this help message and exit
  
  -s=ID,  --send=ID     Send a message over LoRa to the specified ID. Requires 
                        -m/--message option to specify the message content.
  
  -m=MSG, --message=MSG Specify the message payload to be sent. Must be used in
                        conjunction with -s/--send option. Limited to 248 bytes
                        (the ESP's default UART RX buffer size) - longer
                        messages are rejected with -EMSGSIZE.

  -g,     --get         Get a message from HAT buffer. If the buffer is not
                        empty, the message will be outputted to stdout and the
                        return code will be the sender ID.
                        If the buffer is empty, the program will return -ENODATA.

  -c,     --config      Request config info from HAT. The config string is a
                        series of whitespace-separated VARIABLE=VALUE pairs,
                        that are meant to be set as environment variables for
                        the program.
                        The config string will be outputted to stdout.
                        The return code will be 0 on success, or a negative
                        errno code on failure.

All error codes are returned as negative errno values (e.g. -ENODATA, -EMSGSIZE,
-EREMOTEIO, -EINVAL) - see errno(3).

Additional flags (do not touch under normal circumstances):
 -t=TIM,  --timeout=TIM A timeout (in ms) after which an UART transmission is 
                        retried when HAT is not responding. If HAT has 
                        responded with erroneous checksum, loracom will drop 
                        the package, and wait for 2*TIM for retransmission.

 -r=RET,  --retries=RET Max number of retires. 0 for no reties. If a maximum
                        number of retries has been exceeded, loracom will
                        exit with EREMOTEIO message (-121 by default).

 -f,      --force       Skip the message size check on --send. On --get, also
                        accept a reply even if its checksum doesn't match,
                        instead of treating it as corrupted.

Default values: TIM=250, RET=3.
Keep in mind that send/get are blocking, but bound by timeout/retry count.
Under normal circumstances, send/get should not take longer than a few 
milliseconds to complete, but if UART is unstable and retransmission occurs,
send/get call may take over a second (depending on selected timeout).
)";

constexpr std::string_view DEFAULT_DEVICE = "/dev/serial0";
constexpr uint32_t DEFAULT_BAUDRATE = 115200;

int send(LoRaCom& loraCom, uint8_t destId, const std::string_view& message, bool force);
int get(LoRaCom& loraCom, bool force);
int requestConfig(LoRaCom& loraCom);

int main(int argc, char *argv[])
{
    try {
        ArgParser parser(argc, argv);

        if (parser.hasOption("--help", "-h")) {
            std::cout << HELP_MSG << std::endl;
            return 0;
        }

        uint32_t timeoutMs = parser.getArgValueInt<uint32_t>("--timeout", "-t").value_or(DEFAULT_ACK_TIMEOUT_MS);
        uint32_t maxRetries = parser.getArgValueInt<uint32_t>("--retries", "-r").value_or(DEFAULT_MAX_RETRIES);
        bool force = parser.hasOption("--force", "-f");

        auto destId = parser.getArgValueInt<uint8_t>("--send", "-s");
        auto messageOpt = parser.getArgValueStr("--message", "-m");

        if (destId || messageOpt) {
            if (!destId || !messageOpt) {
                std::cerr << "Error: --send and --message options must be used together and have valid values." << std::endl;
                return -EINVAL;
            }

            LoRaCom loraCom(std::string(DEFAULT_DEVICE), DEFAULT_BAUDRATE, timeoutMs, maxRetries);
            return send(loraCom, *destId, *messageOpt, force);
        }
        else if (parser.hasOption("--get", "-g")) {
            LoRaCom loraCom(std::string(DEFAULT_DEVICE), DEFAULT_BAUDRATE, timeoutMs, maxRetries);
            return get(loraCom, force);
        }
        else if (parser.hasOption("--config", "-c")) {
            LoRaCom loraCom(std::string(DEFAULT_DEVICE), DEFAULT_BAUDRATE, timeoutMs, maxRetries);
            return requestConfig(loraCom);
        }

        return 0;
    } catch (const ArgParser::ParserException& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -EINVAL;
    } catch (const std::system_error& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -e.code().value();
    }
}

int send(LoRaCom& loraCom, uint8_t destId, const std::string_view& message, bool force)
{
    if (!force && message.size() > MAX_MESSAGE_SIZE) {
        std::cerr << "Error: message is " << message.size() << " bytes, over the " << MAX_MESSAGE_SIZE
                  << "-byte limit; use --force to override." << std::endl;
        return -EMSGSIZE;
    }

    loraCom.sendTransmission(TransmissionType::SENDMSG, destId, std::string(message));
    return 0;
}

int get(LoRaCom& loraCom, bool force)
{
    auto received = loraCom.getTransmission(TransmissionType::GETMSG, force);
    if (!received) {
        return -ENODATA;
    }
    std::cout << received->payload << std::endl;
    return static_cast<int>(received->senderId);
}

int requestConfig(LoRaCom& loraCom)
{
    auto received = loraCom.getTransmission(TransmissionType::CONFREQ);
    if (!received) {
        return -ENODATA;
    }
    std::cout << received->payload << std::endl;
    return 0;
}