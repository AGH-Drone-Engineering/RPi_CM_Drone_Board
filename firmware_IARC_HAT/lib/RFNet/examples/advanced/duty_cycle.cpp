// Duty cycle — full picture: persistence across reboots + correct single-frame
// duty handling (the "no budget" outcome arrives asynchronously via onSendFail,
// with an advisory getDutyCycleWaitMs() pre-flight to avoid needless attempts).
//
// RFNet enforces regional airtime limits (e.g. ETSI EN 300 220: 1% for EU868)
// with the LoRaWAN minimum-off-time (Toff) rule: after a frame of air-time Ton
// the next send is blocked for Ton×(denom-1), so any sliding window stays within
// the limit. Sends that would breach it are refused, not silently sent.
//
// Persistence matters because the off-time deadline is stored in wall-clock
// epoch-ms: a node that reboots mid-Toff resumes the remaining wait. Without a
// clock the deadline is tick-based and lost on reboot (see custom_clock.cpp for
// how to add one). This file uses EspClock (SNTP) + PreferencesBackend (NVS).
//
// Target: ESP32 + SX1262 (EU868, 1% duty cycle)

#include <RFNode.h>
#include <esp_sntp.h>

#define MY_ADDR  0x01
#define DST_ADDR 0x02

#define LORA_CS   41
#define LORA_IRQ  39
#define LORA_RST  42
#define LORA_BUSY 40

static SX1262LoRaRadio   radio(SX1262LoRaRadio::Channel::EU868_CH0,
                               LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY);
static EspClock           espClock;
static PreferencesBackend nvs;

static RFNodeConfig cfg = []() {
    RFNodeConfig c;
    c.addr       = MY_ADDR;
    c.mode       = PacketMode::P2P;
    c.clock      = &espClock;    // lets the Toff deadline survive reboots
    c.nv.backend = &nvs;         // where the duty deadline (and seq) is stored

    c.dutyCycle.enabled = true;  // on by default now; shown explicitly for clarity
    // 0 = trust the radio's regulatory value (EU868_CH0 reports 100 = 1%).
    // Set a stricter (higher) value only to be MORE conservative than the
    // legal floor — you can never loosen below what the HAL reports.
    c.dutyCycle.denominatorOverride = 0;

    return c;
}();

static RFNode node(radio, cfg);

static const char* kMessage = "duty-gated telemetry payload";

// Backoff deadline (millis) requested by the last duty-blocked send. loop()
// honours it before the next attempt. volatile: written from the worker task
// (onSendFail) and read from loop().
static volatile uint32_t g_dutyBackoffUntilMs = 0;

static void onSendFail(const SentInfo& info, TxFailReason reason, void*) {
    // For a SINGLE-FRAME send this is the PRIMARY duty signal, not a rare race:
    // send()/sendAck() return OK (the frame is queued) and the duty gate runs
    // later on the worker, so an exhausted bucket surfaces here as
    // TxFailReason::DUTY_CYCLE with the exact remaining wait in info.dutyWaitMs.
    // Back off for that wait instead of busy-retrying.
    if (reason == TxFailReason::DUTY_CYCLE) {
        g_dutyBackoffUntilMs = millis() + info.dutyWaitMs;
        Serial.printf("[DUTY] budget exhausted, backing off %lu ms\n",
                      (unsigned long)info.dutyWaitMs);
    } else {
        Serial.printf("[WARN] send failed, reason=%u\n", (unsigned)reason);
    }
}

void setup() {
    Serial.begin(115200);

    // SNTP — required for EspClock::isValid() to become true, which is what
    // lets the persisted bucket credit elapsed time correctly on the next
    // boot instead of falling back to the pessimistic no-clock assumption.
    esp_sntp_setoperatingmode((esp_sntp_operatingmode_t)SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    node.onSendFail(onSendFail);

    if (node.begin() != BeginStatus::OK) {
        Serial.println("[ERR] begin() failed");
        while (true) delay(1000);
    }
    node.startWorkerTask(4096, 5);
}

void loop() {
    // 1) Honour any backoff the last duty-blocked send asked for (set in
    //    onSendFail). Signed comparison so millis() rollover is handled.
    const uint32_t now = millis();
    if ((int32_t)(g_dutyBackoffUntilMs - now) > 0) {
        delay(g_dutyBackoffUntilMs - now);
        return;
    }

    // 2) Advisory pre-flight. getDutyCycleWaitMs() is ADVISORY ONLY (the value
    //    can drift before you act on it — the real gate is inside send()), but
    //    it lets us avoid queueing a send that would just fail:
    //      0          → budget available now
    //      N ms       → wait ~N ms before it fits
    //      UINT32_MAX → never fits this denominator (shorten / speed up PHY)
    const uint32_t wait = node.getDutyCycleWaitMs(strlen(kMessage));
    if (wait == UINT32_MAX) {
        Serial.println("[DUTY] message can never fit this duty limit — "
                       "shorten it, speed up the PHY, or loosen the override");
        delay(60000);
        return;
    }
    if (wait > 0) {
        delay(wait);
        return;
    }

    // 3) Fire. A SINGLE-FRAME send returns only resource statuses here
    //    (OK / POOL_EXHAUSTED / QUEUE_FULL / BAD_LENGTH / NOT_INITIALIZED);
    //    the DUTY_* SendStatus values are returned only by FRAGMENTED sends.
    //    The duty outcome of this frame arrives asynchronously in onSendFail
    //    (TxFailReason::DUTY_CYCLE) if the bucket drained after admission.
    const SendStatus st = node.sendAck(DST_ADDR, kMessage);
    if (st != SendStatus::OK) {
        Serial.printf("[WARN] sendAck() status=%u\n", (unsigned)st);
        delay(1000);
        return;
    }

    delay(5000);
}
