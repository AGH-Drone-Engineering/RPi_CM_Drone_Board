#include <iostream>
#include <vector>
#include <string>
#include <string_view>

#include "ArgParser.h"
#include "LoRaCom.h"

constexpr std::string_view HELP_MSG = 
R"(Usage: loracom [options])
Send and receive messages over LoRa communication on HAT.
Options:
  -h,     --help        Show this help message and exit
  
  -s=ID,  --send=ID     Send a message over LoRa to the specified ID. Requires 
                        -m/--message option to specify the message content.
  
  -m=MSG, --message=MSG Specify the message payload to be sent. Must be used in 
                        conjunction with -s/--send option
  
  -g,     --get         Get a message from HAT buffer. If the buffer is not
                        empty, the message will be outputted to stdout and the 
                        return code will be the sender ID.
                        If the buffer is empty, the program will return -1.
  
  -c,     --config      Request config info from HAT. The config string is a 
                        series of whitespace-separated VARIABLE=VALUE pairs, 
                        that are meant to be set as environment variables for 
                        the program. 
                        The config string will be outputted to stdout. 
                        The return code will be 0 on success, or -1 on failure.
)";

constexpr std::string_view DEFAULT_DEVICE = "/dev/serial0";
constexpr uint32_t DEFAULT_BAUDRATE = 115200;

int send(LoRaCom& loraCom, uint8_t destId, const std::string_view& message);
int get(LoRaCom& loraCom);
int requestConfig(LoRaCom& loraCom);

int main(int argc, char *argv[])
{
    ArgParser parser(argc, argv);

    if (parser.hasOption("--help", "-h")) {
        std::cout << HELP_MSG << std::endl;
        return 0;
    }

    if (parser.hasOption("--send", "-s") || parser.hasOption("--message", "-m")) {
        auto destId = parser.getArgValueInt<uint8_t>("--send", "-s");
        auto messageOpt = parser.getArgValueStr("--message", "-m");
        if (!destId || !messageOpt) {
            std::cerr << "Error: --send and --message options must be used together and have valid values." << std::endl;
            return -1;
        }

        LoRaCom loraCom(std::string(DEFAULT_DEVICE), DEFAULT_BAUDRATE);
        return send(loraCom, *destId, *messageOpt);
    }
    else if (parser.hasOption("--get", "-g")) {
        LoRaCom loraCom(std::string(DEFAULT_DEVICE), DEFAULT_BAUDRATE);
        return get(loraCom);
    }
    else if (parser.hasOption("--config", "-c")) {
        LoRaCom loraCom(std::string(DEFAULT_DEVICE), DEFAULT_BAUDRATE);
        return requestConfig(loraCom);
    }

    return 0;
}

int send(LoRaCom& loraCom, uint8_t destId, const std::string_view& message)
{
    if (!loraCom.sendTransmission(TransmissionType::SENDMSG, destId, std::string(message))) {
        std::cerr << "Error: Failed to send message." << std::endl;
        return -1;
    }
    return 0;
}

int get(LoRaCom& loraCom)
{
    auto received = loraCom.getTransmission(TransmissionType::GETMSG);
    if (!received) {
        return -1;
    }
    std::cout << received->payload << std::endl;
    return static_cast<int>(received->senderId);
}

int requestConfig(LoRaCom& loraCom)
{
    auto received = loraCom.getTransmission(TransmissionType::CONFREQ);
    if (!received) {
        return -1;
    }
    std::cout << received->payload << std::endl;
    return 0;
}