#include <iostream>
#include <vector>
#include <string>
#include <string_view>

#include "ArgParser.h"
#include "UartHandler.h"

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

int send(uint8_t destId, const std::string_view& message);
int get();
int requestConfig();

int main(int argc, char *argv[])
{
    ArgParser parser(argc, argv);

    if (parser.hasOption("--help", "-h")) {
        std::cout << HELP_MSG << std::endl;
    }
    else if (parser.hasOption("--send", "-s") || parser.hasOption("--message", "-m")) {
        auto destId = parser.getArgValueInt<uint8_t>("--send", "-s");
        auto messageOpt = parser.getArgValueStr("--message", "-m");
        if (!destId || !messageOpt) {
            std::cerr << "Error: --send and --message options must be used together and have valid values." << std::endl;
            return -1;
        }

        return send(*destId, *messageOpt);
    }
    else if (parser.hasOption("--get", "-g")) {
        return get();
    }
    else if (parser.hasOption("--config", "-c")) {
        return requestConfig();
    }

    return 0;
}

int send(uint8_t destId, const std::string_view& message)
{
    if (!sendTransmission(TransmissionType::SENDMSG, destId, std::string(message))) {
        std::cerr << "Error: Failed to send message." << std::endl;
        return -1;
    }
    return 0;
}

int get()
{
    auto received = getTransmission(TransmissionType::GETMSG);
    if (!received) {
        return -1;
    }
    std::cout << received->payload << std::endl;
    return static_cast<int>(received->senderId);
}

int requestConfig()
{
    auto received = getTransmission(TransmissionType::CONFREQ);
    if (!received) {
        return -1;
    }
    std::cout << received->payload << std::endl;
    return 0;
}