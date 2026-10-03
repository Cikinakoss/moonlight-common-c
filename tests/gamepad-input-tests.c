// Tests the real InputStream producer, latch, queue, and worker. Only encryption
// and the host transport are stubbed; no parallel implementation of coalescing.
#include "../src/InputStream.c"
#include <stdarg.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

int AppVersionQuad[4] = {7, 1, 500, -1};
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
struct sockaddr_storage RemoteAddr;
SOCKADDR_LEN AddrLen;
uint32_t SunshineFeatureFlags;

static struct {
    NV_MULTI_CONTROLLER_PACKET packet;
    bool moreData;
    uint8_t channel;
    uint32_t flags;
} sent[8192];
static unsigned sentCount;
static bool failTransport;
static unsigned terminations;
static unsigned diagnosticLines;

static void terminated(int error) { terminations++; }
static void logged(const char* format, ...) { diagnosticLines++; }

int sendInputPacketOnControlStream(unsigned char* data, int length, uint8_t channel, uint32_t flags, bool moreData) {
    if (failTransport) return -1;
    if (((PNV_INPUT_HEADER)data)->magic == LE32(MULTI_CONTROLLER_MAGIC_GEN5)) {
        CHECK(length == sizeof(NV_MULTI_CONTROLLER_PACKET));
        CHECK(sentCount < sizeof(sent) / sizeof(sent[0]));
        memcpy(&sent[sentCount].packet, data, length);
        sent[sentCount].moreData = moreData;
        sent[sentCount].channel = channel;
        sent[sentCount++].flags = flags;
    }
    return 0;
}
void flushInputOnControlStream(void) {}
bool isControlDataInTransit(void) { return false; }
PPLT_CRYPTO_CONTEXT PltCreateCryptoContext(void) { return calloc(1, sizeof(PLT_CRYPTO_CONTEXT)); }
void PltDestroyCryptoContext(PPLT_CRYPTO_CONTEXT ctx) { free(ctx); }
bool PltEncryptMessage(PPLT_CRYPTO_CONTEXT ctx, int algorithm, int flags,
    unsigned char* key, int keyLength, unsigned char* iv, int ivLength,
    unsigned char* tag, int tagLength, unsigned char* input, int inputLength,
    unsigned char* output, int* outputLength) { abort(); }
SOCKET connectTcpSocket(struct sockaddr_storage* addr, SOCKADDR_LEN len, unsigned short port, int timeout) { return INVALID_SOCKET; }
int enableNoDelay(SOCKET s) { return 0; }
void closeSocket(SOCKET s) {}
void shutdownTcpSocket(SOCKET s) {}
int initializePlatformSockets(void) { return 0; }
void cleanupPlatformSockets(void) {}
void enterLowLatencyMode(void) {}
void exitLowLatencyMode(void) {}
int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}

static void begin(bool snappy) {
    LiSetSnappyGamepadInput(snappy);
    CHECK(initializeInputStream() == 0);
    PltLockMutex(&gamepadLifecycleMutex);
    acceptingPhysicalGamepadEvents = initialized = true;
    PltUnlockMutex(&gamepadLifecycleMutex);
    sentCount = terminations = diagnosticLines = 0;
    failTransport = false;
    ListenerCallbacks.connectionTerminated = terminated;
    ListenerCallbacks.logMessage = logged;
}

static void finish(void) {
    PltLockMutex(&gamepadLifecycleMutex);
    acceptingPhysicalGamepadEvents = initialized = false;
    PltUnlockMutex(&gamepadLifecycleMutex);
    LbqSignalQueueShutdown(&packetQueue);
    LbqSignalQueueShutdown(&packetHolderFreeList);
    destroyInputStream();
    for (unsigned i = 0; i < MAX_GAMEPADS; i++) CHECK(currentQueuedControllerPacket[i] == NULL);
}

static int state(short slot, short mask, int buttons, unsigned char trigger, short x) {
    uint64_t event = LiRecordGamepadCallback();
    return LiSendPhysicalGamepadEvent(slot, mask, buttons, trigger, trigger, x, -x, x, -x, event);
}

static PPACKET_HOLDER claim(short slot) {
    PPACKET_HOLDER holder;
    CHECK(LbqPollQueueElement(&packetQueue, (void**)&holder) == LBQ_SUCCESS);
    latchControllerPacket(holder, slot);
    return holder;
}

static void runWorker(void) {
    LbqSignalQueueDrain(&packetQueue);
    CHECK(PltCreateThread("InputSend", inputSendThreadProc, NULL, &inputSendThread) == 0);
    PltJoinThread(&inputSendThread);
}

static void analogAndClaim(bool snappy) {
    begin(snappy);
    CHECK(state(0, 1, 0, 1, 100) == 0);
    PPACKET_HOLDER editable = currentQueuedControllerPacket[0];
    for (short x = 101; x <= 140; x++) CHECK(state(0, 1, 0, 255, x) == 0);
    CHECK(LbqGetItemCount(&packetQueue) == 1);
    CHECK(editable == currentQueuedControllerPacket[0]);
    CHECK(LE16(editable->packet.multiController.leftStickX) == 140);
    CHECK(editable->packet.multiController.leftTrigger == 255);
    CHECK(gamepadDiagnostics.created == 1 && gamepadDiagnostics.coalesced == 40);
    CHECK(gamepadDiagnostics.callbacks == 41 && gamepadDiagnostics.highWater == 1);
    uint64_t latest = editable->gamepadLatestEventUs;
    PPACKET_HOLDER inflight = claim(0);
    CHECK(inflight == editable && currentQueuedControllerPacket[0] == NULL);
    CHECK(state(0, 1, 0, 0, -140) == 0);
    CHECK(LbqGetItemCount(&packetQueue) == 1);
    CHECK(currentQueuedControllerPacket[0] != inflight);
    CHECK(inflight->gamepadLatestEventUs == latest);
    CHECK(LE16(inflight->packet.multiController.leftStickX) == 140);
    CHECK(LE16(currentQueuedControllerPacket[0]->packet.multiController.leftStickX) == -140);
    CHECK(gamepadDiagnostics.pending == 1);
    freePacketHolder(inflight);
    runWorker();
    CHECK(sentCount == 1 && LE16(sent[0].packet.leftStickX) == -140);
    CHECK(gamepadDiagnostics.pending == 0);
    finish();
}

static void edges(bool snappy) {
    const int sequence[] = {A_FLAG, 0, B_FLAG, A_FLAG | B_FLAG, 0, LEFT_FLAG, 0, RB_FLAG, 0, PADDLE1_FLAG, 0};
    begin(snappy);
    for (unsigned i = 0; i < sizeof(sequence)/sizeof(sequence[0]); i++) {
        CHECK(state(0, 1, sequence[i], i, i * 10) == 0);
        // Analog after an edge may update only that edge's latest packet.
        CHECK(state(0, 1, sequence[i], i, i * 10 + 1) == 0);
    }
    CHECK(LbqGetItemCount(&packetQueue) == sizeof(sequence)/sizeof(sequence[0]));
    CHECK(gamepadDiagnostics.digitalEdges == 10);
    runWorker();
    CHECK(sentCount == sizeof(sequence)/sizeof(sequence[0]));
    for (unsigned i = 0; i < sentCount; i++) {
        int buttons = (uint16_t)LE16(sent[i].packet.buttonFlags) | ((uint32_t)(uint16_t)LE16(sent[i].packet.buttonFlags2) << 16);
        CHECK(buttons == sequence[i]);
        CHECK(LE16(sent[i].packet.leftStickX) == (short)(i * 10 + 1));
        CHECK(sent[i].flags & ENET_PACKET_FLAG_RELIABLE);
        CHECK(sent[i].moreData == (!snappy && i + 1 < sentCount));
    }
    finish();
}

static void slotsAndDisconnect(bool snappy) {
    begin(snappy);
    CHECK(state(0, 3, A_FLAG, 30, 100) == 0);
    CHECK(state(1, 3, B_FLAG, 60, -200) == 0);
    CHECK(state(0, 3, A_FLAG, 40, 150) == 0);
    CHECK(state(1, 3, B_FLAG, 70, -250) == 0);
    CHECK(LbqGetItemCount(&packetQueue) == 2);
    CHECK(currentQueuedControllerPacket[0] != currentQueuedControllerPacket[1]);
    CHECK(state(1, 1, 0, 0, 0) == 0); // removal preserves queued press then release
    runWorker();
    CHECK(sentCount == 3);
    CHECK(LE16(sent[0].packet.controllerNumber) == 0 && LE16(sent[0].packet.leftStickX) == 150);
    CHECK(LE16(sent[1].packet.controllerNumber) == 1 && LE16(sent[1].packet.leftStickX) == -250);
    CHECK(LE16(sent[2].packet.activeGamepadMask) == 1 && sent[2].packet.buttonFlags == 0);
    CHECK(sent[0].channel != sent[1].channel);
    finish();
    // Empty-state connect -> disconnect masks must not coalesce in Snappy mode.
    begin(snappy);
    CHECK(state(1, 3, 0, 0, 0) == 0);
    CHECK(state(1, 1, 0, 0, 0) == 0);
    CHECK(LbqGetItemCount(&packetQueue) == (snappy ? 2 : 1));
    finish();
}

static void enqueueFailure(bool snappy) {
    begin(snappy);
    packetQueue.sizeBound = 1;
    CHECK(state(0, 1, A_FLAG, 0, 10) == 0);
    CHECK(state(0, 1, 0, 0, 20) == LBQ_BOUND_EXCEEDED);
    CHECK(currentQueuedControllerPacket[0] == NULL);
    PPACKET_HOLDER old = claim(0);
    freePacketHolder(old);
    CHECK(state(0, 1, 0, 0, 30) == 0);
    CHECK(LE16(currentQueuedControllerPacket[0]->packet.multiController.leftStickX) == 30);
    finish();
    begin(snappy);
    LbqSignalQueueDrain(&packetQueue);
    CHECK(state(0, 1, 0, 0, 30) == LBQ_INTERRUPTED);
    CHECK(currentQueuedControllerPacket[0] == NULL && gamepadDiagnostics.pending == 0);
    finish();
}

static void shutdownAndReconnect(bool snappy) {
    begin(snappy);
    CHECK(state(0, 1, A_FLAG, 255, 123) == 0);
    // Close the same lifetime gate used at stop before the worker can claim input.
    PltLockMutex(&gamepadLifecycleMutex);
    acceptingPhysicalGamepadEvents = initialized = false;
    PltUnlockMutex(&gamepadLifecycleMutex);
    CHECK(LiRecordGamepadCallback() == 0);
    CHECK(state(0, 1, 0, 0, 0) == -2);
    CHECK(PltCreateThread("InputSend", inputSendThreadProc, NULL, &inputSendThread) == 0);
    CHECK(stopInputStream() == 0); // actual drain/join
    CHECK(sentCount == (snappy ? 0 : 1));
    destroyInputStream();
    CHECK(LiSendPhysicalGamepadEvent(0, 1, 0, 0, 0, 0, 0, 0, 0, 0) == -2);
    begin(snappy);
    CHECK(currentQueuedControllerPacket[0] == NULL);
    CHECK(state(0, 1, 0, 0, -123) == 0);
    runWorker();
    CHECK(sentCount == 1 && LE16(sent[0].packet.leftStickX) == -123);
    finish();
}

static void unrelatedInputAndMetrics(bool snappy) {
    begin(snappy);
    CHECK(LiSendMultiControllerEvent(0, 3, 0, 0, 0, 10, 0, 0, 0) == 0);
    CHECK(state(1, 3, 0, 0, 20) == 0);
    CHECK(state(1, 3, A_FLAG, 0, 30) == 0);
    LiSetSnappyGamepadInput(!snappy);
    CHECK(snappyGamepadInput == snappy); // reconnect required
    gamepadDiagnostics.windowStartUs -= 1000000;
    runWorker();
    CHECK(sentCount == 3);
    CHECK(sent[0].moreData); // virtual input always retains stock batching hint
    CHECK(sent[1].moreData == !snappy && !sent[2].moreData);
    CHECK(diagnosticLines > 0);
    finish();
    begin(snappy);
    failTransport = true;
    CHECK(state(0, 1, A_FLAG, 0, 0) == 0);
    runWorker();
    CHECK(terminations == 1 && currentQueuedControllerPacket[0] == NULL);
    finish();
}

static void sharedSlotSources(bool snappy) {
    begin(snappy);
    CHECK(state(0, 1, A_FLAG, 10, 100) == 0);
    PPACKET_HOLDER physical = currentQueuedControllerPacket[0];
    uint64_t timestamp = physical->gamepadLatestEventUs;
    CHECK(LiSendMultiControllerEvent(0, 1, A_FLAG, 20, 20, 200, 0, 0, 0) == 0);
    PPACKET_HOLDER virtual = currentQueuedControllerPacket[0];
    CHECK(physical != virtual && LbqGetItemCount(&packetQueue) == 2);
    CHECK(physical->gamepadLatestEventUs == timestamp && physical->snappyGamepad == snappy);
    CHECK(!virtual->snappyGamepad && !virtual->gamepadFirstEventUs && !virtual->gamepadLatestEventUs);
    CHECK(LiSendMultiControllerEvent(0, 1, A_FLAG, 30, 30, 300, 0, 0, 0) == 0);
    CHECK(currentQueuedControllerPacket[0] == virtual); // Same-source analog still batches.
    CHECK(state(0, 1, A_FLAG, 40, 400) == 0); // Reverse source transition also splits.
    CHECK(currentQueuedControllerPacket[0] != virtual && LbqGetItemCount(&packetQueue) == 3);
    CHECK(gamepadDiagnostics.created == 2 && gamepadDiagnostics.coalesced == 0);
    runWorker();
    CHECK(sentCount == 3 && gamepadDiagnostics.sent == 2);
    CHECK(LE16(sent[0].packet.leftStickX) == 100 && LE16(sent[1].packet.leftStickX) == 300);
    CHECK(sent[0].moreData == !snappy && sent[1].moreData && !sent[2].moreData);
    finish();

    begin(snappy);
    CHECK(state(0, 1, 0, 0, 100) == 0);
    CHECK(LiSendMultiControllerEvent(0, 1, 0, 0, 0, 200, 0, 0, 0) == 0);
    PltLockMutex(&gamepadLifecycleMutex);
    acceptingPhysicalGamepadEvents = initialized = false;
    PltUnlockMutex(&gamepadLifecycleMutex);
    CHECK(PltCreateThread("InputSend", inputSendThreadProc, NULL, &inputSendThread) == 0);
    CHECK(stopInputStream() == 0);
    CHECK(sentCount == (snappy ? 1 : 2));
    CHECK(LE16(sent[sentCount - 1].packet.leftStickX) == 200); // Virtual input survives stock drain.
    destroyInputStream();
}

static void liveWorker(bool snappy) {
    begin(snappy);
    CHECK(PltCreateThread("InputSend", inputSendThreadProc, NULL, &inputSendThread) == 0);
    for (unsigned i = 0; i < 4000; i++) CHECK(state(0, 1, (i / 100) % 2 ? A_FLAG : 0, i % 256, i) == 0);
    LbqSignalQueueDrain(&packetQueue);
    PltJoinThread(&inputSendThread);
    unsigned transitions = 0;
    int previous = -1;
    for (unsigned i = 0; i < sentCount; i++) {
        int buttons = LE16(sent[i].packet.buttonFlags);
        if (buttons != previous) { transitions++; previous = buttons; }
    }
    CHECK(transitions == 40);
    CHECK(LE16(sent[sentCount - 1].packet.leftStickX) == 3999);
    CHECK(gamepadDiagnostics.pending == 0);
    finish();
}

int main(void) {
    PltTicksInit();
    CHECK(!snappyGamepadInput);
    CHECK(LiRecordGamepadCallback() == 0);
    for (unsigned mode = 0; mode < 2; mode++) {
        analogAndClaim(mode); edges(mode); slotsAndDisconnect(mode);
        enqueueFailure(mode); shutdownAndReconnect(mode);
        unrelatedInputAndMetrics(mode); sharedSlotSources(mode); liveWorker(mode);
    }
    puts("PASS: gamepad coalescing, claim races, digital edges, slots, queue failures, teardown, OFF hints, metrics, live worker");
    return 0;
}
