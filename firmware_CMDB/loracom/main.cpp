#include <sys/stat.h>

#include <cerrno>
#include <iostream>
#include <limits>
#include <system_error>
#include <vector>
#include <string>
#include <string_view>

#include "ArgParser.h"
#include "DeviceLock.h"
#include "LoRaCom.h"

// Generated into the object dir by the makefile (see VERSION_H there). Guarded
// so an ad-hoc `g++ *.cpp` outside the makefile still compiles.
#if __has_include("version.h")
#include "version.h"
#endif
#ifndef LORACOM_VERSION
#define LORACOM_VERSION "unknown"
#endif

constexpr std::string_view HELP_MSG = 
R"(Usage: loracom [options]
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
-EREMOTEIO, -EINVAL, -ENODEV, -EBUSY) - see errno(3).

If the UART device doesn't exist, loracom exits with -ENODEV - UART3 most
likely isn't enabled yet, see firmware_CMDB/system-setup.sh.

Only one loracom transaction runs on the UART at a time. An instance that finds
it busy waits TIM ms and retries, up to 3*(RET+1) times, then exits with -EBUSY
- long enough to cover the worst case the instance holding the port is allowed
to take before it gives up itself.

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

constexpr std::string_view DEFAULT_DEVICE = "/dev/ttyAMA3";
constexpr uint32_t DEFAULT_BAUDRATE = 115200;

int send(LoRaCom& loraCom, uint8_t destId, const std::string_view& message, bool force);
int get(LoRaCom& loraCom, bool force);
int requestConfig(LoRaCom& loraCom);

namespace {

// The build is identified by the commit it came from rather than a version
// number - there are no releases, and "which commit is on this drone" is the
// question that actually gets asked.
void printHelp(std::ostream& out)
{
    out << "loracom (commit " << LORACOM_VERSION << ")\n" << HELP_MSG << std::endl;
}

// The UART node only exists once UART3 is enabled in config.txt, so a missing
// device means the board isn't set up - a distinct situation from the link
// being broken, and worth its own exit code rather than the ENOENT that
// open() would otherwise surface.
bool uartExists(const std::string& device)
{
    struct stat st{};
    return ::stat(device.c_str(), &st) == 0 && S_ISCHR(st.st_mode);
}

} // namespace

int main(int argc, char *argv[])
{
    try {
        ArgParser parser(argc, argv);

        if (parser.hasOption("--help", "-h")) {
            printHelp(std::cout);
            return 0;
        }

        uint32_t timeoutMs = parser.getArgValueInt<uint32_t>("--timeout", "-t").value_or(DEFAULT_ACK_TIMEOUT_MS);
        uint32_t maxRetries = parser.getArgValueInt<uint32_t>("--retries", "-r").value_or(DEFAULT_MAX_RETRIES);
        bool force = parser.hasOption("--force", "-f");

        auto destId = parser.getArgValueInt<uint8_t>("--send", "-s");
        auto messageOpt = parser.getArgValueStr("--message", "-m");

        const bool wantsSend = destId.has_value() || messageOpt.has_value();
        const bool wantsGet = parser.hasOption("--get", "-g");
        const bool wantsConfig = parser.hasOption("--config", "-c");

        if (!wantsSend && !wantsGet && !wantsConfig) {
            // Nothing was asked for. Unlike an explicit -h/--help, that's a
            // usage error, so the help goes to stderr and the exit code says so.
            printHelp(std::cerr);
            return -EINVAL;
        }

        if (wantsSend && (!destId || !messageOpt)) {
            std::cerr << "Error: --send and --message options must be used together and have valid values." << std::endl;
            return -EINVAL;
        }

        const std::string device(DEFAULT_DEVICE);

        if (!uartExists(device)) {
            std::cerr << "Error: " << device << " does not exist - UART3 is probably not enabled;"
                         " run system-setup.sh and reboot." << std::endl;
            return -ENODEV;
        }

        // How long to keep trying for the lock. The instance holding it may
        // legitimately need (maxRetries + 1) attempts, each lasting up to
        // 3*timeoutMs - timeoutMs for the reply, plus the 2*timeoutMs
        // LoRaCom::awaitReply() spends waiting for a retransmission after a
        // bad checksum. Waiting for any less than that hands out a spurious
        // -EBUSY exactly when the link is noisy and the holder needs its full
        // retry budget. maxRetries comes from --retries, so both steps are
        // guarded against wrapping round to a *smaller* wait.
        constexpr uint32_t MAX_UINT32 = std::numeric_limits<uint32_t>::max();
        const uint32_t lockAttempts = maxRetries == MAX_UINT32 ? MAX_UINT32 : maxRetries + 1;
        const uint32_t lockRetries = lockAttempts > MAX_UINT32 / 3 ? MAX_UINT32 : lockAttempts * 3;

        // Taken before the port is opened and held for the whole transaction:
        // LoRaCom's reply matching is positional, so a second instance reading
        // the same UART would happily consume this one's reply.
        DeviceLock lock(device, timeoutMs, lockRetries);

        LoRaCom loraCom(device, DEFAULT_BAUDRATE, timeoutMs, maxRetries);

        if (wantsSend) {
            return send(loraCom, *destId, *messageOpt, force);
        }
        if (wantsGet) {
            return get(loraCom, force);
        }
        return requestConfig(loraCom);
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