#include "wii_remote.h"
#include "wii_remote_passthrough_api.h"
#include <cstdio>
#include <new>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <libusb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace input::wii {
std::unique_ptr<Backend> CreateWindowsBackend();

namespace {
using Clock = std::chrono::steady_clock;

constexpr uint8_t kUsbHciClass = 0xE0;
constexpr uint8_t kUsbHciSubclass = 0x01;
constexpr uint8_t kUsbHciProtocol = 0x01;

constexpr uint8_t kEventInquiryComplete = 0x01;
constexpr uint8_t kEventInquiryResult = 0x02;
constexpr uint8_t kEventConnectionComplete = 0x03;
constexpr uint8_t kEventConnectionRequest = 0x04;
constexpr uint8_t kEventDisconnectionComplete = 0x05;
constexpr uint8_t kEventAuthenticationComplete = 0x06;
constexpr uint8_t kEventRemoteNameComplete = 0x07;
constexpr uint8_t kEventEncryptionChange = 0x08;
constexpr uint8_t kEventCommandComplete = 0x0E;
constexpr uint8_t kEventCommandStatus = 0x0F;
constexpr uint8_t kEventNumberCompletedPackets = 0x13;
constexpr uint8_t kEventPinCodeRequest = 0x16;
constexpr uint8_t kEventLinkKeyRequest = 0x17;
constexpr uint8_t kEventLinkKeyNotification = 0x18;

constexpr uint16_t kHciReset = 0x0C03;
constexpr uint16_t kHciReadBdAddr = 0x1009;
constexpr uint16_t kHciInquiry = 0x0401;
constexpr uint16_t kHciReadRemoteName = 0x0419;
constexpr uint16_t kHciCreateConnection = 0x0405;
constexpr uint16_t kHciAcceptConnectionRequest = 0x0409;
constexpr uint16_t kHciDisconnect = 0x0406;
constexpr uint16_t kHciAuthenticationRequested = 0x0411;
constexpr uint16_t kHciSetConnectionEncryption = 0x0413;
constexpr uint16_t kHciLinkKeyRequestReply = 0x040B;
constexpr uint16_t kHciLinkKeyRequestNegativeReply = 0x040C;
constexpr uint16_t kHciWriteStoredLinkKey = 0x0C11;
constexpr uint16_t kHciDeleteStoredLinkKey = 0x0C12;
constexpr uint16_t kHciPinCodeRequestReply = 0x040D;

constexpr uint16_t kL2capSignalingCid = 0x0001;
constexpr uint16_t kL2capHidControlPsm = 0x0011;
constexpr uint16_t kL2capHidInterruptPsm = 0x0013;
constexpr uint16_t kLocalControlCid = 0x0040;
constexpr uint16_t kLocalInterruptCid = 0x0041;

struct HciInterface {
    int number = -1;
    int alternate = 0;
    uint8_t eventIn = 0;
    uint8_t aclIn = 0;
    uint8_t aclOut = 0;
};

struct Candidate {
    std::array<uint8_t, 6> address{};
    uint32_t classOfDevice = 0;
    std::string name;
};

struct Channel {
    uint16_t psm = 0;
    uint16_t localCid = 0;
    uint16_t remoteCid = 0;
    uint8_t configIdentifier = 0;
    bool localConfigured = false;
    bool peerConfigured = false;
};

struct Session {
    bool inUse = false;
    bool ready = false;
    int player = 0;
    std::array<uint8_t, 6> address{};
    uint16_t handle = 0;
    Channel control{0x0011, kLocalControlCid, 0, 3};
    Channel interrupt{0x0013, kLocalInterruptCid, 0, 4};
    std::deque<std::vector<uint8_t>> pendingSignals;
    Remote state{};
    Clock::time_point lastReport{};
    Clock::time_point lastStatus{};
    std::vector<uint8_t> partialAcl;
    uint16_t partialAclLength = 0;
    int led = -1;
};

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

void AppendLe16(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back(static_cast<uint8_t>(value));
    data.push_back(static_cast<uint8_t>(value >> 8));
}

bool SameAddress(const std::array<uint8_t, 6>& a, const uint8_t* b) {
    return std::equal(a.begin(), a.end(), b);
}

bool IsWiiRemoteName(const std::string& name) {
    return name.rfind("Nintendo RVL-CNT-01", 0) == 0;
}

bool IsKnownWiiRemoteClass(uint32_t classOfDevice) {
    // Nintendo RVL-CNT-01 and RVL-CNT-01-TR Class of Device values.
    return classOfDevice == 0x002504 || classOfDevice == 0x000508;
}

bool FindHciInterface(libusb_device* device, HciInterface& result) {
    libusb_config_descriptor* config = nullptr;
    const int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != LIBUSB_SUCCESS || !config)
        return false;

    bool found = false;
    for (uint8_t i = 0; i < config->bNumInterfaces && !found; ++i) {
        const auto& group = config->interface[i];
        for (int a = 0; a < group.num_altsetting && !found; ++a) {
            const auto& alt = group.altsetting[a];
            if (alt.bInterfaceClass != kUsbHciClass ||
                alt.bInterfaceSubClass != kUsbHciSubclass ||
                alt.bInterfaceProtocol != kUsbHciProtocol)
                continue;

            uint8_t eventIn = 0, aclIn = 0, aclOut = 0;
            for (uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                const auto& ep = alt.endpoint[e];
                const auto type = ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                const bool input =
                    (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
                if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && input)
                    eventIn = ep.bEndpointAddress;
                else if (type == LIBUSB_TRANSFER_TYPE_BULK && input)
                    aclIn = ep.bEndpointAddress;
                else if (type == LIBUSB_TRANSFER_TYPE_BULK && !input)
                    aclOut = ep.bEndpointAddress;
            }

            if (eventIn && aclIn && aclOut) {
                result = {alt.bInterfaceNumber, alt.bAlternateSetting, eventIn, aclIn, aclOut};
                found = true;
            }
        }
    }

    libusb_free_config_descriptor(config);
    return found;
}

bool HasUsbHciAdapter(int selectedVid, int selectedPid) {
    const bool automatic = selectedVid == -1 && selectedPid == -1;
    if (!automatic && (selectedVid <= 0 || selectedVid > 0xffff ||
                       selectedPid <= 0 || selectedPid > 0xffff))
        return false;
    libusb_context* context = nullptr;
    if (libusb_init(&context) != LIBUSB_SUCCESS)
        return false;

    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    bool found = false;
    if (count >= 0) {
        for (ssize_t i = 0; i < count && !found; ++i) {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS)
                continue;
            if (!automatic && (descriptor.idVendor != selectedVid ||
                               descriptor.idProduct != selectedPid))
                continue;
            HciInterface hci;
            if (!FindHciInterface(devices[i], hci))
                continue;
            libusb_device_handle* testHandle = nullptr;
            if (libusb_open(devices[i], &testHandle) != LIBUSB_SUCCESS)
                continue;
            const int detach = libusb_set_auto_detach_kernel_driver(testHandle, 1);
            const bool canDetach = detach == LIBUSB_SUCCESS || detach == LIBUSB_ERROR_NOT_SUPPORTED;
            const bool canClaim = canDetach &&
                libusb_claim_interface(testHandle, hci.number) == LIBUSB_SUCCESS;
            if (canClaim)
                libusb_release_interface(testHandle, hci.number);
            libusb_close(testHandle);
            found = canClaim;
        }
        libusb_free_device_list(devices, 1);
    }
    libusb_exit(context);
    return found;
}

class LibusbBackend final : public Backend {
public:
    LibusbBackend(int selectedVid, int selectedPid)
        : selectedVid_(selectedVid), selectedPid_(selectedPid), worker_([this] { Run(); }) {}

    ~LibusbBackend() override {
        // Do not stop the worker before it has processed disconnectAll_.
        // The worker owns every libusb/HCI operation and must disconnect the
        // remotes before releasing the USB interface.
        enabled_.store(false);
        scanRequested_.store(false);
        disconnectAll_.store(true);
        shutdownRequested_.store(true);
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    void Enable(bool enabled, bool continuous) override {
        enabled_.store(enabled);
        continuous_.store(continuous);
        if (!enabled) {
            scanRequested_.store(false);
            disconnectAll_.store(true);
        }
        cv_.notify_all();
    }

    void Scan() override {
        {
            std::lock_guard lock(commandMutex_);
            scanDeadline_ = Clock::now() + std::chrono::seconds(20);
        }
        scanRequested_.store(true);
        cv_.notify_all();
    }

    void Disconnect(int player) override {
        if (player < 0 || player >= 2)
            return;
        {
            std::lock_guard lock(commandMutex_);
            disconnectRequested_[player] = true;
        }
        cv_.notify_all();
    }

    void SwapPlayers() override {
        swapRequested_.store(true);
        cv_.notify_all();
    }

    Snapshot Poll() override {
        std::lock_guard lock(stateMutex_);
        Snapshot snapshot;
        snapshot.player = visibleState_;
        for (auto& remote : visibleState_)
            remote.pressed = 0;
        snapshot.scanning = scanning_;
        snapshot.message = message_;
        return snapshot;
    }

private:
    std::atomic<bool> stop_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> continuous_{false};
    std::atomic<bool> scanRequested_{false};
    std::atomic<bool> swapRequested_{false};
    std::atomic<bool> disconnectAll_{false};
    std::atomic<bool> shutdownRequested_{false};

    std::mutex commandMutex_;
    std::array<bool, 2> disconnectRequested_{};
    std::condition_variable cv_;
    int selectedVid_ = -1;
    int selectedPid_ = -1;
    std::thread worker_;

    std::mutex stateMutex_;
    std::array<Remote, 2> visibleState_{};
    bool scanning_ = false;
    std::string message_ = "Select Connect and press the red SYNC button";

    libusb_context* context_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    HciInterface hci_{};
    std::array<uint8_t, 6> localAddress_{};
    std::array<Session, 2> sessions_{};
    std::array<std::array<uint8_t, 6>, 2> slotAddresses_{};
    std::array<bool, 2> slotReserved_{};
    std::deque<std::vector<uint8_t>> pendingEvents_;
    std::map<std::array<uint8_t, 6>, std::array<uint8_t, 16>> linkKeys_;
    Clock::time_point scanDeadline_{};
    bool initialized_ = false;

    void SetMessage(const std::string& message, bool scanning = false) {
        std::lock_guard lock(stateMutex_);
        message_ = message;
        scanning_ = scanning;
    }

    void Publish(int player, const Remote& state) {
        if (player < 0 || player >= 2)
            return;
        std::lock_guard lock(stateMutex_);
        const uint16_t pendingPresses =
            static_cast<uint16_t>(visibleState_[player].pressed | state.pressed);
        visibleState_[player] = state;
        visibleState_[player].pressed = pendingPresses;
        visibleState_[player].reserved = slotReserved_[player];
    }

    void ClearVisible(int player) {
        if (player < 0 || player >= 2)
            return;
        std::lock_guard lock(stateMutex_);
        visibleState_[player] = {};
        visibleState_[player].reserved = slotReserved_[player];
    }

    void ReleaseSlot(int player) {
        if (player < 0 || player >= 2)
            return;
        slotReserved_[player] = false;
        slotAddresses_[player] = {};
        ClearVisible(player);
    }

    void ClearAllVisible() {
        std::lock_guard lock(stateMutex_);
        visibleState_ = {};
        for (int i = 0; i < 2; ++i)
            visibleState_[i].reserved = slotReserved_[i];
        scanning_ = false;
    }

    bool OpenAdapter() {
        if (libusb_init(&context_) != LIBUSB_SUCCESS) {
            SetMessage("libusb initialization failed");
            return false;
        }
        libusb_set_option(context_, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

        libusb_device** devices = nullptr;
        const ssize_t count = libusb_get_device_list(context_, &devices);
        if (count < 0) {
            SetMessage("USB enumeration failed");
            libusb_exit(context_);
            context_ = nullptr;
            return false;
        }

        bool opened = false;
        // Automatic mode accepts the first claimable standard USB HCI adapter.
        // An explicit VID/PID selection never silently falls back to another device.
        for (ssize_t i = 0; i < count && !opened; ++i) {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS)
                continue;
            if ((selectedVid_ != -1 || selectedPid_ != -1) &&
                (descriptor.idVendor != selectedVid_ || descriptor.idProduct != selectedPid_))
                continue;

            HciInterface candidate{};
            if (!FindHciInterface(devices[i], candidate))
                continue;

            libusb_device_handle* candidateHandle = nullptr;
            int rc = libusb_open(devices[i], &candidateHandle);
            if (rc != LIBUSB_SUCCESS)
                continue;

            const int detach = libusb_set_auto_detach_kernel_driver(candidateHandle, 1);
            if (detach != LIBUSB_SUCCESS && detach != LIBUSB_ERROR_NOT_SUPPORTED) {
                libusb_close(candidateHandle);
                continue;
            }
            rc = libusb_claim_interface(candidateHandle, candidate.number);
            if (rc != LIBUSB_SUCCESS) {
                libusb_close(candidateHandle);
                continue;
            }
            if (candidate.alternate != 0 &&
                libusb_set_interface_alt_setting(candidateHandle, candidate.number,
                                                 candidate.alternate) != LIBUSB_SUCCESS) {
                libusb_release_interface(candidateHandle, candidate.number);
                libusb_close(candidateHandle);
                continue;
            }

            handle_ = candidateHandle;
            hci_ = candidate;
            opened = true;
        }

        libusb_free_device_list(devices, 1);
        if (!opened) {
            libusb_exit(context_);
            context_ = nullptr;
            SetMessage("No usable USB Bluetooth HCI adapter; check its WinUSB/libusbK driver");
            return false;
        }

        SetMessage("Initializing USB Bluetooth adapter");
        return true;
    }

    void CloseAdapter() {
        for (int i = 0; i < 2; ++i)
            sessions_[i] = {};
        pendingEvents_.clear();
        initialized_ = false;
        if (handle_) {
            libusb_release_interface(handle_, hci_.number);
            libusb_close(handle_);
            handle_ = nullptr;
        }
        if (context_) {
            libusb_exit(context_);
            context_ = nullptr;
        }
        localAddress_ = {};
        ClearAllVisible();
    }

    bool ReadEvent(std::vector<uint8_t>& event, unsigned timeoutMs) {
        if (!handle_)
            return false;
        std::array<uint8_t, 260> buffer{};
        int transferred = 0;
        const int rc = libusb_interrupt_transfer(handle_, hci_.eventIn, buffer.data(),
                                                 static_cast<int>(buffer.size()), &transferred,
                                                 timeoutMs);
        if (rc != LIBUSB_SUCCESS || transferred < 2)
            return false;
        const unsigned declared = buffer[1];
        if (static_cast<unsigned>(transferred) < declared + 2)
            return false;
        event.assign(buffer.begin(), buffer.begin() + transferred);
        return true;
    }

    bool SendCommand(uint16_t opcode, const uint8_t* parameters, uint8_t parameterLength,
                     std::vector<uint8_t>* response = nullptr, unsigned timeoutMs = 10000) {
        if (!handle_)
            return false;
        std::array<uint8_t, 258> packet{};
        packet[0] = static_cast<uint8_t>(opcode & 0xFF);
        packet[1] = static_cast<uint8_t>(opcode >> 8);
        packet[2] = parameterLength;
        if (parameterLength && parameters)
            std::memcpy(packet.data() + 3, parameters, parameterLength);

        constexpr uint8_t requestType = LIBUSB_ENDPOINT_OUT |
                                        LIBUSB_REQUEST_TYPE_CLASS |
                                        LIBUSB_RECIPIENT_DEVICE;
        const int written = libusb_control_transfer(
            handle_, requestType, 0, 0, 0, packet.data(),
            static_cast<uint16_t>(parameterLength + 3), timeoutMs);
        if (written != parameterLength + 3)
            return false;

        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!stop_ && Clock::now() < deadline) {
            std::vector<uint8_t> event;
            if (!ReadEvent(event, 5)) {
                PumpAcl(1);
                continue;
            }

            bool matches = false;
            if (event[0] == kEventCommandComplete && event.size() >= 6)
                matches = ReadLe16(event.data() + 3) == opcode;
            else if (event[0] == kEventCommandStatus && event.size() >= 6)
                matches = ReadLe16(event.data() + 4) == opcode;

            if (matches) {
                if (response)
                    *response = event;
                if (event[0] == kEventCommandComplete)
                    return event.size() >= 6 && event[5] == 0;
                return event.size() >= 6 && event[2] == 0;
            }

            if (!IsAsynchronousEvent(event))
                pendingEvents_.push_back(std::move(event));
            PumpAcl(1);
        }
        return false;
    }

    bool IsAsynchronousEvent(const std::vector<uint8_t>& event) {
        if (event.size() < 2)
            return true;
        if (event[0] == kEventLinkKeyNotification && event.size() >= 25) {
            RememberLinkKey(event);
            return true;
        }
        if (event[0] == kEventDisconnectionComplete && event.size() >= 6) {
            const uint16_t h = ReadLe16(event.data() + 3) & 0x0FFF;
            for (int i = 0; i < 2; ++i) {
                if (sessions_[i].inUse && sessions_[i].handle == h) {
                    sessions_[i] = {};
                    ClearVisible(i);
                }
            }
            return true;
        }
        if (event[0] == 0x12 || event[0] == 0x14 ||
            event[0] == kEventNumberCompletedPackets)
            return true;
        return false;
    }

    bool NextEvent(std::vector<uint8_t>& event, unsigned timeoutMs) {
        if (!pendingEvents_.empty()) {
            event = std::move(pendingEvents_.front());
            pendingEvents_.pop_front();
            return true;
        }
        return ReadEvent(event, timeoutMs);
    }

    template <typename Predicate>
    bool WaitForEvent(Predicate predicate, std::vector<uint8_t>& matched,
                      unsigned timeoutMs) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        std::deque<std::vector<uint8_t>> deferred;
        bool found = false;
        while (!stop_ && enabled_ && Clock::now() < deadline) {
            std::vector<uint8_t> event;
            if (!NextEvent(event, 5)) {
                PumpAcl(1);
                continue;
            }
            if (predicate(event)) {
                matched = std::move(event);
                found = true;
                break;
            }
            if (!IsAsynchronousEvent(event))
                deferred.push_back(std::move(event));
            PumpAcl(1);
        }
        // Keep unrelated future-phase events available, but never put them back
        // at the front while this wait loop is still running.
        while (!deferred.empty()) {
            pendingEvents_.push_back(std::move(deferred.front()));
            deferred.pop_front();
        }
        return found;
    }

    static bool ParseHexByte(char high, char low, uint8_t& value) {
        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int h = digit(high);
        const int l = digit(low);
        if (h < 0 || l < 0)
            return false;
        value = static_cast<uint8_t>((h << 4) | l);
        return true;
    }

    static std::filesystem::path LinkKeyPath() {
        const char* localAppData = std::getenv("LOCALAPPDATA");
        if (!localAppData || !*localAppData)
            return {};
        return std::filesystem::path(localAppData) / "HOTD2RE" / "wii_link_keys.txt";
    }

    void LoadLinkKeys() {
        const auto path = LinkKeyPath();
        if (path.empty())
            return;
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            const auto separator = line.find('=');
            if (separator != 12 || line.size() != 45)
                continue;
            std::array<uint8_t, 6> address{};
            std::array<uint8_t, 16> key{};
            bool valid = true;
            for (size_t i = 0; i < address.size(); ++i)
                valid = ParseHexByte(line[i * 2], line[i * 2 + 1], address[i]) && valid;
            for (size_t i = 0; i < key.size(); ++i)
                valid = ParseHexByte(line[separator + 1 + i * 2],
                                     line[separator + 2 + i * 2], key[i]) && valid;
            if (valid)
                linkKeys_[address] = key;
        }
    }

    void SaveLinkKeys() {
        const auto path = LinkKeyPath();
        if (path.empty())
            return;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return;
        static constexpr char digits[] = "0123456789abcdef";
        std::ofstream file(path, std::ios::trunc);
        if (!file)
            return;
        for (const auto& [address, key] : linkKeys_) {
            for (const uint8_t byte : address) {
                file.put(digits[byte >> 4]);
                file.put(digits[byte & 0x0F]);
            }
            file.put('=');
            for (const uint8_t byte : key) {
                file.put(digits[byte >> 4]);
                file.put(digits[byte & 0x0F]);
            }
            file.put('\n');
        }
    }

    void RememberLinkKey(const std::vector<uint8_t>& event) {
        if (event.size() < 25 || event[0] != kEventLinkKeyNotification)
            return;
        std::array<uint8_t, 6> address{};
        std::array<uint8_t, 16> key{};
        std::copy_n(event.begin() + 2, address.size(), address.begin());
        std::copy_n(event.begin() + 8, key.size(), key.begin());
        linkKeys_[address] = key;
        SaveLinkKeys();
    }

    void RestoreLinkKeys() {
        // Do not issue an extra controller command on first-time pairing. The
        // known-good discovery tool goes directly from Reset/BD_ADDR to Inquiry
        // and legacy PIN authentication when no application keys exist.
        if (linkKeys_.empty())
            return;
        // Match Dolphin: clear the adapter's possibly stale key table, then
        // restore only keys remembered by this application.
        const std::array<uint8_t, 7> deleteAll{0, 0, 0, 0, 0, 0, 1};
        (void)SendCommand(kHciDeleteStoredLinkKey, deleteAll.data(),
                          static_cast<uint8_t>(deleteAll.size()));

        size_t written = 0;
        for (const auto& [address, key] : linkKeys_) {
            if (written >= 11) // HCI Write Stored Link Key allows at most 11 entries.
                break;
            std::vector<uint8_t> parameters;
            parameters.reserve(23);
            parameters.push_back(1);
            parameters.insert(parameters.end(), address.begin(), address.end());
            parameters.insert(parameters.end(), key.begin(), key.end());
            if (SendCommand(kHciWriteStoredLinkKey, parameters.data(),
                            static_cast<uint8_t>(parameters.size())))
                ++written;
        }
    }

    bool InitializeAdapter() {
        std::vector<uint8_t> response;
        if (!SendCommand(kHciReset, nullptr, 0, &response)) {
            SetMessage("HCI Reset failed");
            return false;
        }
        if (!SendCommand(kHciReadBdAddr, nullptr, 0, &response) ||
            response.size() < 12 || response[5] != 0) {
            SetMessage("Could not read Bluetooth adapter address");
            return false;
        }
        std::copy_n(response.begin() + 6, localAddress_.size(), localAddress_.begin());
        RestoreLinkKeys();
        initialized_ = true;
        SetMessage("USB Bluetooth ready; press red SYNC on the Wii Remote");
        return true;
    }

    std::vector<Candidate> Inquiry() {
        std::vector<Candidate> candidates;
        const std::array<uint8_t, 5> parameters{0x33, 0x8B, 0x9E, 0x08, 0x00};
        if (!SendCommand(kHciInquiry, parameters.data(),
                         static_cast<uint8_t>(parameters.size()))) {
            SetMessage("Bluetooth inquiry command failed");
            return candidates;
        }

        SetMessage("Searching for Wii Remotes; press red SYNC", true);
        const auto deadline = Clock::now() + std::chrono::seconds(12);
        bool complete = false;
        while (!stop_ && enabled_ && Clock::now() < deadline && !complete) {
            std::vector<uint8_t> event;
            if (!NextEvent(event, 10)) {
                PumpAcl(1);
                continue;
            }

            if (event[0] == kEventInquiryResult && event.size() >= 3) {
                const uint8_t count = event[2];
                constexpr size_t kEntrySize = 13;
                if (event.size() >= 3 + static_cast<size_t>(count) * kEntrySize) {
                    for (uint8_t i = 0; i < count; ++i) {
                        const uint8_t* entry = event.data() + 3 + i * kEntrySize;
                        Candidate candidate;
                        std::copy_n(entry, candidate.address.size(), candidate.address.begin());
                        candidate.classOfDevice = static_cast<uint32_t>(entry[8]) |
                            (static_cast<uint32_t>(entry[9]) << 8) |
                            (static_cast<uint32_t>(entry[10]) << 16);
                        if (IsKnownWiiRemoteClass(candidate.classOfDevice))
                            candidate.name = "Nintendo Wii Remote";
                        const bool duplicate = std::any_of(candidates.begin(), candidates.end(),
                            [&](const Candidate& c) { return c.address == candidate.address; });
                        if (!duplicate)
                            candidates.push_back(candidate);
                    }
                }
            } else if (event[0] == kEventInquiryComplete) {
                if (event.size() < 3 || event[2] != 0) {
                    SetMessage("Bluetooth inquiry completed with HCI error");
                    complete = true;
                    candidates.clear();
                } else {
                    complete = true;
                }
            } else {
                // Inquiry only consumes Inquiry Result and Inquiry Complete.
                // Other asynchronous/controller events are handled by the HCI pump.
                (void)IsAsynchronousEvent(event);
            }
            PumpAcl(1);
        }
        return candidates;
    }

    bool ReadRemoteName(Candidate& candidate) {
        std::array<uint8_t, 10> parameters{};
        std::copy(candidate.address.begin(), candidate.address.end(), parameters.begin());
        // Use the same conservative page-scan parameters as the known-good
        // direct-connect probe; do not let Inquiry Result metadata alter this
        // preliminary name request either.
        parameters[6] = 0x01;
        parameters[7] = 0;
        parameters[8] = 0;
        parameters[9] = 0;
        if (!SendCommand(kHciReadRemoteName, parameters.data(),
                         static_cast<uint8_t>(parameters.size()))) {
            SetMessage("Bluetooth device found, but Read Remote Name was rejected");
            return false;
        }

        std::vector<uint8_t> event;
        if (!WaitForEvent([&](const std::vector<uint8_t>& e) {
                return e.size() >= 9 && e[0] == kEventRemoteNameComplete &&
                       SameAddress(candidate.address, e.data() + 3);
            }, event, 5000)) {
            SetMessage("Bluetooth device found, but its name request timed out");
            return false;
        }

        if (event[2] != 0 || event.size() < 10) {
            SetMessage("Bluetooth device found, but reading its name failed");
            return false;
        }
        const char* name = reinterpret_cast<const char*>(event.data() + 9);
        const size_t available = event.size() - 9;
        const auto end = std::find(name, name + available, '\0');
        candidate.name.assign(name, end);
        return IsWiiRemoteName(candidate.name);
    }

    int FindFreeSlot() const {
        for (int i = 0; i < 2; ++i)
            if (!sessions_[i].inUse)
                return i;
        return -1;
    }

    int FindSlotForAddress(const std::array<uint8_t, 6>& address) const {
        // Reconnect a known remote into its previous player slot first.
        for (int i = 0; i < 2; ++i) {
            if (!sessions_[i].inUse && slotReserved_[i] && slotAddresses_[i] == address)
                return i;
        }
        // A new remote may only occupy a slot which has not been reserved by
        // a previously connected remote.
        for (int i = 0; i < 2; ++i) {
            if (!sessions_[i].inUse && !slotReserved_[i])
                return i;
        }
        return -1;
    }

    Session* FindSession(uint16_t connectionHandle) {
        for (auto& session : sessions_)
            if (session.inUse && session.handle == connectionHandle)
                return &session;
        return nullptr;
    }

    bool ConnectBaseband(const Candidate& candidate, Session& session) {
        // Match the independently working wii_discovery --connect path exactly:
        // all BR/EDR packet types, page-scan repetition R1, unknown clock offset,
        // and permission for the controller to switch us to master.
        std::array<uint8_t, 13> parameters{};
        std::copy(candidate.address.begin(), candidate.address.end(), parameters.begin());
        parameters[6] = 0x18;
        parameters[7] = 0xCC;
        parameters[8] = 0x01; // Page scan repetition mode R1 (fixed by working probe).
        parameters[9] = 0x00; // Reserved.
        parameters[10] = 0x00; // Clock offset intentionally unknown, as in working probe.
        parameters[11] = 0x00;
        parameters[12] = 0x01; // Allow role switch.

        SetMessage("Sending HCI Create Connection to " + candidate.name, true);
        if (!SendCommand(kHciCreateConnection, parameters.data(),
                         static_cast<uint8_t>(parameters.size()))) {
            SetMessage("HCI Create Connection was rejected for " + candidate.name);
            return false;
        }

        const auto deadline = Clock::now() + std::chrono::seconds(20);
        std::deque<std::vector<uint8_t>> deferred;
        std::vector<uint8_t> event;
        bool connected = false;
        while (!stop_ && enabled_ && Clock::now() < deadline && !connected) {
            if (!NextEvent(event, 10)) {
                PumpAcl(1);
                continue;
            }

            if (event[0] == kEventConnectionRequest && event.size() >= 12 &&
                SameAddress(candidate.address, event.data() + 2)) {
                // SYNC can race with Create Connection: the remote may initiate
                // its side of the ACL link first. The proven probe accepts that
                // request as master instead of waiting only for Connection Complete.
                std::array<uint8_t, 7> accept{};
                std::copy(candidate.address.begin(), candidate.address.end(), accept.begin());
                accept[6] = 0x00; // Become master.
                if (!SendCommand(kHciAcceptConnectionRequest, accept.data(),
                                 static_cast<uint8_t>(accept.size()))) {
                    SetMessage("HCI Accept Connection Request failed for " + candidate.name);
                    return false;
                }
                continue;
            }

            if (event[0] == kEventConnectionComplete && event.size() >= 13 &&
                SameAddress(candidate.address, event.data() + 5)) {
                if (event[2] != 0) {
                    char status[3]{};
                    std::snprintf(status, sizeof(status), "%02X", event[2]);
                    SetMessage("HCI Connection Complete failed for " + candidate.name +
                               " (status 0x" + status + ")");
                    return false;
                }
                connected = true;
                break;
            }

            if (!IsAsynchronousEvent(event))
                deferred.push_back(std::move(event));
        }
        while (!deferred.empty()) {
            pendingEvents_.push_back(std::move(deferred.front()));
            deferred.pop_front();
        }
        if (!connected) {
            SetMessage("Timed out waiting for HCI Connection Complete from " + candidate.name);
            return false;
        }
        SetMessage("Bluetooth baseband link established with " + candidate.name, true);

        session.inUse = true;
        session.ready = false;
        session.address = candidate.address;
        slotAddresses_[session.player] = candidate.address;
        slotReserved_[session.player] = true;
        session.handle = static_cast<uint16_t>(ReadLe16(event.data() + 3) & 0x0FFF);
        session.state = {};
        session.state.reserved = true;
        session.lastReport = Clock::now();
        session.lastStatus = session.lastReport;
        return true;
    }

    bool Authenticate(Session& session) {
        std::array<uint8_t, 2> parameters{
            static_cast<uint8_t>(session.handle),
            static_cast<uint8_t>(session.handle >> 8)
        };
        if (!SendCommand(kHciAuthenticationRequested, parameters.data(),
                         static_cast<uint8_t>(parameters.size())))
            return false;

        bool authenticated = false;
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        while (!stop_ && enabled_ && Clock::now() < deadline && !authenticated) {
            std::vector<uint8_t> event;
            if (!NextEvent(event, 10)) {
                PumpAcl(1);
                continue;
            }

            if (event[0] == kEventPinCodeRequest && event.size() >= 8) {
                std::array<uint8_t, 23> reply{};
                std::copy_n(event.begin() + 2, 6, reply.begin());
                reply[6] = 6;
                // SYNC-button bonding uses the host adapter address as the legacy PIN.
                std::copy(localAddress_.begin(), localAddress_.end(), reply.begin() + 7);
                if (!SendCommand(kHciPinCodeRequestReply, reply.data(),
                                 static_cast<uint8_t>(reply.size())))
                    return false;
            } else if (event[0] == kEventLinkKeyRequest && event.size() >= 8) {
                std::array<uint8_t, 6> address{};
                std::copy_n(event.begin() + 2, address.size(), address.begin());
                const auto key = linkKeys_.find(address);
                if (key != linkKeys_.end()) {
                    std::vector<uint8_t> reply;
                    reply.insert(reply.end(), address.begin(), address.end());
                    reply.insert(reply.end(), key->second.begin(), key->second.end());
                    if (!SendCommand(kHciLinkKeyRequestReply, reply.data(),
                                     static_cast<uint8_t>(reply.size())))
                        return false;
                } else if (!SendCommand(kHciLinkKeyRequestNegativeReply,
                                        address.data(), static_cast<uint8_t>(address.size()))) {
                    return false;
                }
            } else if (event[0] == kEventLinkKeyNotification && event.size() >= 25) {
                RememberLinkKey(event);
            } else if (event[0] == kEventAuthenticationComplete && event.size() >= 5) {
                const uint16_t eventHandle =
                    static_cast<uint16_t>(ReadLe16(event.data() + 3) & 0x0FFF);
                if (eventHandle != session.handle)
                    continue;
                if (event[2] != 0)
                    return false;
                authenticated = true;
            } else {
                // Unrelated HCI events are not authentication input; do not requeue
                // them where they could block the next phase's event wait.
                (void)IsAsynchronousEvent(event);
            }
            PumpAcl(1);
        }
        return authenticated;
    }

    bool EnableEncryption(Session& session) {
        const std::array<uint8_t, 3> parameters{
            static_cast<uint8_t>(session.handle),
            static_cast<uint8_t>(session.handle >> 8),
            0x01
        };
        if (!SendCommand(kHciSetConnectionEncryption, parameters.data(),
                         static_cast<uint8_t>(parameters.size())))
            return false;

        std::vector<uint8_t> event;
        if (!WaitForEvent([&](const std::vector<uint8_t>& e) {
                if (e.size() < 6 || e[0] != kEventEncryptionChange)
                    return false;
                return (ReadLe16(e.data() + 3) & 0x0FFF) == session.handle;
            }, event, 15000))
            return false;

        return event[2] == 0 && event[5] == 1;
    }

    bool SendAcl(uint16_t connectionHandle, uint16_t cid,
                 const uint8_t* payload, uint16_t payloadSize) {
        if (!handle_ || !payload || payloadSize > 1024)
            return false;
        std::vector<uint8_t> packet(8 + payloadSize);
        const uint16_t handleFlags = static_cast<uint16_t>((connectionHandle & 0x0FFF) | 0x2000);
        const uint16_t aclLength = static_cast<uint16_t>(4 + payloadSize);
        packet[0] = static_cast<uint8_t>(handleFlags);
        packet[1] = static_cast<uint8_t>(handleFlags >> 8);
        packet[2] = static_cast<uint8_t>(aclLength);
        packet[3] = static_cast<uint8_t>(aclLength >> 8);
        packet[4] = static_cast<uint8_t>(payloadSize);
        packet[5] = static_cast<uint8_t>(payloadSize >> 8);
        packet[6] = static_cast<uint8_t>(cid);
        packet[7] = static_cast<uint8_t>(cid >> 8);
        std::copy_n(payload, payloadSize, packet.begin() + 8);
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle_, hci_.aclOut, packet.data(),
                                            static_cast<int>(packet.size()),
                                            &transferred, 1000);
        return rc == LIBUSB_SUCCESS && transferred == static_cast<int>(packet.size());
    }

    bool SendSignal(Session& session, uint8_t code, uint8_t identifier,
                    const std::vector<uint8_t>& body) {
        std::vector<uint8_t> signal{code, identifier};
        AppendLe16(signal, static_cast<uint16_t>(body.size()));
        signal.insert(signal.end(), body.begin(), body.end());
        return SendAcl(session.handle, kL2capSignalingCid,
                       signal.data(), static_cast<uint16_t>(signal.size()));
    }

    bool SendConnectionRequest(Session& session, uint16_t psm, uint16_t localCid,
                               uint8_t identifier) {
        std::vector<uint8_t> body;
        AppendLe16(body, psm);
        AppendLe16(body, localCid);
        return SendSignal(session, 0x02, identifier, body);
    }

    bool SendConfigurationRequest(Session& session, Channel& channel) {
        std::vector<uint8_t> body;
        AppendLe16(body, channel.remoteCid);
        AppendLe16(body, 0);
        return SendSignal(session, 0x04, channel.configIdentifier, body);
    }

    void ProcessHidInput(Session& session, const std::vector<uint8_t>& pdu) {
        if (pdu.size() < 6 || pdu[4] != 0xA1)
            return;
        const bool valid = DecodeReport(pdu.data() + 5, pdu.size() - 5, session.state);
        if (valid) {
            session.state.connected = true;
            session.state.reserved = true;
            session.lastReport = Clock::now();
            Publish(session.player, session.state);
            session.state.pressed = 0;
        }
    }

    void ProcessSignaling(Session& session, const std::vector<uint8_t>& pdu) {
        if (pdu.size() < 8 || ReadLe16(pdu.data() + 2) != kL2capSignalingCid)
            return;
        const uint8_t code = pdu[4];
        const uint8_t identifier = pdu[5];
        const uint16_t commandLength = ReadLe16(pdu.data() + 6);
        if (pdu.size() < 8u + commandLength)
            return;

        if (code == 0x06 && commandLength >= 4) {
            const uint16_t destination = ReadLe16(pdu.data() + 8);
            const uint16_t source = ReadLe16(pdu.data() + 10);
            const Channel* channel = destination == session.control.localCid ? &session.control :
                                     destination == session.interrupt.localCid ? &session.interrupt : nullptr;
            if (channel && source == channel->remoteCid) {
                std::vector<uint8_t> body;
                AppendLe16(body, channel->localCid);
                AppendLe16(body, channel->remoteCid);
                (void)SendSignal(session, 0x07, identifier, body);
                session.ready = false;
                session.state.connected = false;
                session.state.tracking = false;
                Publish(session.player, session.state);
            }
            return;
        }

        // Keep signaling packets for the synchronous L2CAP connection/configuration
        // state machine. This also preserves peer configuration requests that arrive
        // while the second PSM is still being opened.
        session.pendingSignals.push_back(pdu);
    }

    bool ReadAclPdu(uint16_t& connectionHandle, std::vector<uint8_t>& pdu,
                    unsigned timeoutMs) {
        if (!handle_)
            return false;
        std::array<uint8_t, 4096> packet{};
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle_, hci_.aclIn, packet.data(),
                                            static_cast<int>(packet.size()),
                                            &transferred, timeoutMs);
        if (rc != LIBUSB_SUCCESS || transferred < 8)
            return false;

        const uint16_t handleFlags = ReadLe16(packet.data());
        const uint16_t aclLength = ReadLe16(packet.data() + 2);
        const uint16_t l2capLength = ReadLe16(packet.data() + 4);
        const uint16_t packetHandle = static_cast<uint16_t>(handleFlags & 0x0FFF);
        const uint8_t packetBoundary = static_cast<uint8_t>((handleFlags >> 12) & 0x03);
        if (packetBoundary == 0x01 || aclLength < 4 ||
            aclLength + 4u > static_cast<unsigned>(transferred) ||
            l2capLength + 4u > aclLength ||
            l2capLength + 8u > static_cast<unsigned>(transferred))
            return false;

        connectionHandle = packetHandle;
        pdu.assign(packet.begin() + 4, packet.begin() + 8 + l2capLength);
        return true;
    }

    void PumpAcl(unsigned timeoutMs) {
        uint16_t connectionHandle = 0;
        std::vector<uint8_t> pdu;
        if (!ReadAclPdu(connectionHandle, pdu, timeoutMs))
            return;
        Session* session = FindSession(connectionHandle);
        if (!session || pdu.size() < 4)
            return;
        const uint16_t cid = ReadLe16(pdu.data() + 2);
        if (cid == kL2capSignalingCid)
            ProcessSignaling(*session, pdu);
        else if (cid == session->interrupt.localCid)
            ProcessHidInput(*session, pdu);
        else if (cid == session->control.localCid) {
            // HIDP control handshakes are not required for normal Wii Remote DATA
            // output on the interrupt channel, but keep the channel alive.
        }
    }

    bool FindPendingSignal(Session& session, uint8_t code, uint8_t identifier,
                           std::vector<uint8_t>& pdu) {
        auto it = std::find_if(session.pendingSignals.begin(), session.pendingSignals.end(),
            [&](const std::vector<uint8_t>& packet) {
                return packet.size() >= 8 && packet[4] == code && packet[5] == identifier;
            });
        if (it == session.pendingSignals.end())
            return false;
        pdu = std::move(*it);
        session.pendingSignals.erase(it);
        return true;
    }

    bool ConnectL2cap(Session& session, Channel& channel) {
        const uint8_t identifier = channel.configIdentifier == 3 ? 1 : 2;
        if (!SendConnectionRequest(session, channel.psm, channel.localCid, identifier))
            return false;

        const auto deadline = Clock::now() + std::chrono::seconds(12);
        while (!stop_ && enabled_ && Clock::now() < deadline) {
            std::vector<uint8_t> pdu;
            if (FindPendingSignal(session, 0x03, identifier, pdu)) {
                if (pdu.size() < 16 || ReadLe16(pdu.data() + 6) < 8)
                    continue;
                const uint16_t remoteCid = ReadLe16(pdu.data() + 8);
                const uint16_t echoedCid = ReadLe16(pdu.data() + 10);
                const uint16_t result = ReadLe16(pdu.data() + 12);
                if (echoedCid != channel.localCid)
                    continue;
                if (result == 0x0001) // L2CAP Pending.
                    continue;
                if (result != 0)
                    return false;
                channel.remoteCid = remoteCid;
                return true;
            }
            PumpAcl(5);
        }
        return false;
    }

    bool HandleConfigurationRequest(Session& session, const std::vector<uint8_t>& pdu) {
        if (pdu.size() < 12 || pdu[4] != 0x04)
            return false;
        const uint8_t identifier = pdu[5];
        const uint16_t commandLength = ReadLe16(pdu.data() + 6);
        if (commandLength < 4 || pdu.size() < 8u + commandLength)
            return false;

        const uint16_t destinationCid = ReadLe16(pdu.data() + 8);
        const uint16_t flags = ReadLe16(pdu.data() + 10);
        Channel* channel = destinationCid == session.control.localCid ? &session.control :
                           destinationCid == session.interrupt.localCid ? &session.interrupt : nullptr;
        if (!channel || flags != 0)
            return false;

        std::vector<uint8_t> unknownOptions;
        size_t offset = 12;
        const size_t end = 8u + commandLength;
        while (offset < end) {
            if (end - offset < 2)
                return false;
            const uint8_t type = pdu[offset];
            const uint8_t length = pdu[offset + 1];
            if (offset + 2u + length > end)
                return false;
            const uint8_t option = type & 0x7F;
            if (option == 0x01) {
                if (length != 2 || ReadLe16(pdu.data() + offset + 2) < 48)
                    return false;
            } else if ((type & 0x80) == 0) {
                unknownOptions.insert(unknownOptions.end(),
                    pdu.begin() + static_cast<std::ptrdiff_t>(offset),
                    pdu.begin() + static_cast<std::ptrdiff_t>(offset + 2u + length));
            }
            offset += 2u + length;
        }

        std::vector<uint8_t> response;
        // SCID names the peer endpoint which receives this Configuration Response.
        // This is the CID assigned by the Wii Remote, not our local CID.
        AppendLe16(response, channel->remoteCid);
        AppendLe16(response, 0);
        AppendLe16(response, unknownOptions.empty() ? 0 : 3);
        response.insert(response.end(), unknownOptions.begin(), unknownOptions.end());
        if (!SendSignal(session, 0x05, identifier, response))
            return false;
        if (unknownOptions.empty())
            channel->peerConfigured = true;
        return true;
    }

    bool ConfigureChannels(Session& session) {
        if (!SendConfigurationRequest(session, session.control) ||
            !SendConfigurationRequest(session, session.interrupt))
            return false;

        const auto deadline = Clock::now() + std::chrono::seconds(15);
        while (!stop_ && enabled_ && Clock::now() < deadline) {
            if (session.control.localConfigured && session.control.peerConfigured &&
                session.interrupt.localConfigured && session.interrupt.peerConfigured)
                return true;

            if (session.pendingSignals.empty()) {
                PumpAcl(5);
                continue;
            }

            std::vector<uint8_t> pdu = std::move(session.pendingSignals.front());
            session.pendingSignals.pop_front();
            if (pdu.size() < 8 || pdu[4] == 0x00)
                continue;

            const uint8_t code = pdu[4];
            const uint8_t identifier = pdu[5];
            const uint16_t commandLength = ReadLe16(pdu.data() + 6);
            if (pdu.size() < 8u + commandLength)
                continue;

            if (code == 0x04) {
                (void)HandleConfigurationRequest(session, pdu);
            } else if (code == 0x05 && commandLength >= 6) {
                Channel* channel = identifier == session.control.configIdentifier ? &session.control :
                                   identifier == session.interrupt.configIdentifier ? &session.interrupt : nullptr;
                if (!channel)
                    continue;
                const uint16_t sourceCid = ReadLe16(pdu.data() + 8);
                const uint16_t flags = ReadLe16(pdu.data() + 10);
                const uint16_t result = ReadLe16(pdu.data() + 12);
                if (sourceCid == channel->localCid && flags == 0 && result == 0)
                    channel->localConfigured = true;
                else if (result != 0)
                    return false;
            }
        }
        return false;
    }

    bool SendHidpOutput(Session& session, const uint8_t* report, size_t size) {
        if (!session.ready || !report || size == 0 || size > 64)
            return false;
        std::vector<uint8_t> packet;
        packet.reserve(size + 1);
        packet.push_back(0xA2); // HIDP DATA, Output.
        packet.insert(packet.end(), report, report + size);
        return SendAcl(session.handle, session.interrupt.remoteCid,
                       packet.data(), static_cast<uint16_t>(packet.size()));
    }

    bool SendHidpOutput(Session& session, std::initializer_list<uint8_t> report) {
        return SendHidpOutput(session, report.begin(), report.size());
    }

    bool InitializeIr(Session& session) {
        const uint8_t start = 0x08;
        const uint8_t mode = 0x03;
        const uint8_t sensitivity1[] = {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64};
        const uint8_t sensitivity2[] = {0x63, 0x03};
        if (!SendHidpOutput(session, {0x13, 0x04}))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (!SendHidpOutput(session, {0x1A, 0x04}))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        const auto writeStart = WriteRegister(0xB00030, &start, 1);
        const auto writeSensitivity1 = WriteRegister(0xB00000, sensitivity1, sizeof(sensitivity1));
        const auto writeSensitivity2 = WriteRegister(0xB0001A, sensitivity2, sizeof(sensitivity2));
        const auto writeMode = WriteRegister(0xB00033, &mode, 1);
        for (const auto* write : {&writeStart, &writeSensitivity1, &writeSensitivity2, &writeMode}) {
            if (!SendHidpOutput(session, write->data(), write->size()))
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        if (!SendHidpOutput(session, writeStart.data(), writeStart.size()))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        // Continuous report 0x33 carries core buttons, accelerometer and extended IR.
        if (!SendHidpOutput(session, {0x12, 0x04, 0x33}))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (!SendHidpOutput(session, {0x15, 0x00}))
            return false;

        session.lastReport = Clock::now();
        session.lastStatus = session.lastReport;
        session.state.connected = true;
        session.state.reserved = true;
        session.ready = true;
        Publish(session.player, session.state);
        return true;
    }

    bool ConnectCandidate(const Candidate& candidate, int player) {
        Session& session = sessions_[player];
        session = {};
        session.player = player;
        session.control = {0x0011, kLocalControlCid, 0, 3};
        session.interrupt = {0x0013, kLocalInterruptCid, 0, 4};

        SetMessage("Connecting to " + candidate.name, true);
        if (!ConnectBaseband(candidate, session)) {
            session = {};
            ClearVisible(player);
            return false;
        }

        // Keep initial pairing identical to the known-good standalone sequence.
        // QoS/Sniff tuning is deliberately deferred until a basic HID link works.
        SetMessage("Bluetooth connected; authenticating Wii Remote", true);
        if (!Authenticate(session)) {
            // A Link Key Notification can arrive before Authentication Complete.
            // Do not keep a key from a failed exchange: that would make every
            // later SYNC attempt reuse the same stale key instead of falling back
            // to the working legacy host-address PIN exchange.
            if (linkKeys_.erase(session.address) != 0)
                SaveLinkKeys();
            SetMessage("Bluetooth link established, but Wii Remote authentication failed; "
                       "discarded any unverified link key for the next SYNC attempt");
            (void)DisconnectSession(session);
            session = {};
            ClearVisible(player);
            return false;
        }
        SetMessage("Wii Remote authenticated; enabling link encryption", true);
        if (!EnableEncryption(session)) {
            SetMessage("Wii Remote authenticated, but Bluetooth encryption failed");
            (void)DisconnectSession(session);
            session = {};
            ClearVisible(player);
            return false;
        }

        SetMessage("Opening Wii Remote HID control channel", true);
        if (!ConnectL2cap(session, session.control)) {
            SetMessage("Bluetooth secured, but HID control channel (PSM 0x11) failed");
            (void)DisconnectSession(session);
            session = {};
            ClearVisible(player);
            return false;
        }
        SetMessage("Opening Wii Remote HID interrupt channel", true);
        if (!ConnectL2cap(session, session.interrupt)) {
            SetMessage("HID control opened, but HID interrupt channel (PSM 0x13) failed");
            (void)DisconnectSession(session);
            session = {};
            ClearVisible(player);
            return false;
        }
        if (!ConfigureChannels(session)) {
            SetMessage("Wii Remote HID channels opened, but L2CAP configuration failed");
            (void)DisconnectSession(session);
            session = {};
            ClearVisible(player);
            return false;
        }

        session.ready = true;
        if (!SendHidpOutput(session, {0x11, static_cast<uint8_t>(0x10u << player)})) {
            SetMessage("Wii Remote HID connected, but the player LED command failed");
            (void)DisconnectSession(session);
            session = {};
            ClearVisible(player);
            return false;
        }
        if (!InitializeIr(session)) {
            SetMessage("Wii Remote HID connected, but report/IR initialization failed");
            (void)DisconnectSession(session);
            session = {};
            ClearVisible(player);
            return false;
        }
        session.led = player;
        SetMessage("Wii Remote connected through USB Bluetooth passthrough");
        return true;
    }

    bool DisconnectSession(Session& session) {
        if (!handle_ || !session.inUse)
            return true;
        if (session.ready)
            (void)SendHidpOutput(session, {0x11, 0x00}); // Turn off this player's LED.
        const std::array<uint8_t, 3> parameters{
            static_cast<uint8_t>(session.handle),
            static_cast<uint8_t>(session.handle >> 8),
            0x13
        };
        return SendCommand(kHciDisconnect, parameters.data(),
                           static_cast<uint8_t>(parameters.size()), nullptr, 1000);
    }

    void ProcessRuntimeEvent(const std::vector<uint8_t>& event) {
        if (event.empty())
            return;
        if (event[0] == kEventLinkKeyNotification && event.size() >= 25) {
            RememberLinkKey(event);
            return;
        }
        if (event[0] == kEventDisconnectionComplete && event.size() >= 6) {
            const uint16_t eventHandle =
                static_cast<uint16_t>(ReadLe16(event.data() + 3) & 0x0FFF);
            for (int i = 0; i < 2; ++i) {
                if (sessions_[i].inUse && sessions_[i].handle == eventHandle) {
                    sessions_[i] = {};
                    ClearVisible(i); // Keep the address reserved for the same remote.
                    SetMessage("Wii Remote disconnected; player slot reserved");
                }
            }
        }
    }

    void PumpEvents() {
        std::vector<uint8_t> event;
        if (ReadEvent(event, 1))
            ProcessRuntimeEvent(event);
        while (!pendingEvents_.empty()) {
            event = std::move(pendingEvents_.front());
            pendingEvents_.pop_front();
            ProcessRuntimeEvent(event);
        }
    }

    void PumpSessions() {
        for (int i = 0; i < 2; ++i) {
            Session& session = sessions_[i];
            if (!session.inUse || !session.ready)
                continue;

            const auto now = Clock::now();
            if (now - session.lastReport > std::chrono::milliseconds(300)) {
                session.state.tracking = false;
                session.state.dots = 0;
            }
            if (now - session.lastReport > std::chrono::seconds(5)) {
                (void)DisconnectSession(session);
                session = {};
                ClearVisible(i); // Keep this player's slot for the same Bluetooth address.
                SetMessage("Wii Remote timed out; player slot reserved");
                continue;
            }
            if (now - session.lastStatus > std::chrono::seconds(10)) {
                (void)SendHidpOutput(session, {0x15, 0x00});
                session.lastStatus = now;
            }
            if (session.led != i) {
                if (SendHidpOutput(session, {0x11, static_cast<uint8_t>(0x10u << i)}))
                    session.led = i;
            }
            Publish(i, session.state);
        }
    }

    void Pump() {
        PumpEvents();
        PumpAcl(1);
        PumpSessions();
    }

    bool ConnectDiscoveredRemotes() {
        const int freeSlot = FindFreeSlot();
        if (freeSlot < 0)
            return false;

        auto candidates = Inquiry();
        if (candidates.empty()) {
            SetMessage("No Bluetooth devices discovered; press red SYNC and retry");
            return false;
        }
        SetMessage("Discovered " + std::to_string(candidates.size()) +
                   " Bluetooth device(s); trying direct Wii Remote connections", true);

        bool connectedAny = false;
        for (auto& candidate : candidates) {
            if (stop_ || !enabled_)
                break;
            if (FindFreeSlot() < 0)
                break;

            bool known = false;
            for (const auto& session : sessions_)
                known = known || (session.inUse && session.address == candidate.address);
            if (known)
                continue;

            // Match the proven standalone workflow: connect directly to the
            // Bluetooth address from Inquiry Result. Remote Name Request is not
            // reliable for Wii Remotes on this adapter and must never block pairing.
            // Unknown Class-of-Device values are still attempted; authentication
            // and HIDP are the definitive checks that this is a Wii Remote.
            if (candidate.name.empty()) {
                char address[18]{};
                std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
                              candidate.address[5], candidate.address[4],
                              candidate.address[3], candidate.address[2],
                              candidate.address[1], candidate.address[0]);
                candidate.name = std::string("Bluetooth device ") + address;
            }

            const int slot = FindSlotForAddress(candidate.address);
            if (slot < 0)
                continue;
            if (ConnectCandidate(candidate, slot))
                connectedAny = true;
        }

        // Keep the last stage-specific status visible when a candidate fails.
        return connectedAny;
    }

    void HandleCommands() {
        if (swapRequested_.exchange(false)) {
            std::swap(sessions_[0], sessions_[1]);
            std::swap(slotAddresses_[0], slotAddresses_[1]);
            std::swap(slotReserved_[0], slotReserved_[1]);
            sessions_[0].player = 0;
            sessions_[1].player = 1;
            for (int i = 0; i < 2; ++i) {
                if (sessions_[i].inUse && sessions_[i].ready) {
                    (void)SendHidpOutput(sessions_[i],
                        {0x11, static_cast<uint8_t>(0x10u << i)});
                    sessions_[i].led = i;
                    Publish(i, sessions_[i].state);
                } else {
                    ClearVisible(i);
                }
            }
        }

        std::array<bool, 2> disconnect{};
        {
            std::lock_guard lock(commandMutex_);
            disconnect = disconnectRequested_;
            disconnectRequested_ = {};
        }
        for (int i = 0; i < 2; ++i) {
            if (!disconnect[i])
                continue;
            if (sessions_[i].inUse)
                (void)DisconnectSession(sessions_[i]);
            sessions_[i] = {};
            ReleaseSlot(i);
        }

        if (disconnectAll_.exchange(false)) {
            for (int i = 0; i < 2; ++i) {
                if (sessions_[i].inUse)
                    (void)DisconnectSession(sessions_[i]);
                sessions_[i] = {};
                ReleaseSlot(i);
            }
        }
    }

    void Run() {
        // The runtime polls HCI events and ACL input with 1 ms synchronous
        // libusb timeouts. On Windows, the default timer quantum can stretch
        // those waits (and sleep_for below) to a much larger interval. Request
        // 1 ms resolution only while USB passthrough is actively enabled.
        bool timerResolutionActive = false;
        LoadLinkKeys();
        while (!stop_) {
            if (enabled_ && !timerResolutionActive)
                timerResolutionActive = timeBeginPeriod(1) == TIMERR_NOERROR;
            else if (!enabled_ && timerResolutionActive) {
                timeEndPeriod(1);
                timerResolutionActive = false;
            }
            if (!enabled_) {
                HandleCommands(); // Release reserved slots and disconnect live remotes.
                if (handle_ || context_)
                    CloseAdapter();
                if (shutdownRequested_.load())
                    break;
                std::unique_lock lock(commandMutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(150),
                             [&] { return stop_ || enabled_.load(); });
                continue;
            }

            if (!handle_) {
                if (!OpenAdapter()) {
                    std::unique_lock lock(commandMutex_);
                    cv_.wait_for(lock, std::chrono::milliseconds(1000),
                                 [&] { return stop_ || !enabled_.load(); });
                    continue;
                }
            }

            if (!initialized_ && !InitializeAdapter()) {
                CloseAdapter();
                std::unique_lock lock(commandMutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(1000));
                continue;
            }

            HandleCommands();

            const bool requested = scanRequested_.exchange(false);
            Clock::time_point scanDeadline;
            {
                std::lock_guard lock(commandMutex_);
                scanDeadline = scanDeadline_;
            }
            const bool scanActive = requested || continuous_ ||
                                    Clock::now() < scanDeadline;
            if (scanActive && FindFreeSlot() >= 0) {
                (void)ConnectDiscoveredRemotes();
                if (!continuous_ && Clock::now() >= scanDeadline) {
                    std::lock_guard lock(commandMutex_);
                    scanDeadline_ = {};
                }
            }

            Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        CloseAdapter();
        if (timerResolutionActive)
            timeEndPeriod(1);
    }
};

} // namespace

#ifndef HOTD2_WII_USB_DLL_BUILD
std::unique_ptr<Backend> CreateBackend() { return CreateWindowsBackend(); }
#else
extern "C" WII_BT_API uint32_t WII_BT_CALL WiiBt_GetApiVersion(void) { return WII_BT_API_VERSION; }
extern "C" WII_BT_API int WII_BT_CALL WiiBt_HasCompatibleAdapter(int32_t vid, int32_t pid) {
    return HasUsbHciAdapter(vid, pid) ? 1 : 0;
}
extern "C" WII_BT_API void* WII_BT_CALL WiiBt_Create(int32_t vid, int32_t pid) {
    if (!((vid == -1 && pid == -1) ||
          (vid > 0 && vid <= 0xffff && pid > 0 && pid <= 0xffff)))
        return nullptr;
    try { return new LibusbBackend(vid, pid); } catch (...) { return nullptr; }
}
extern "C" WII_BT_API void WII_BT_CALL WiiBt_Destroy(void* instance) {
    delete static_cast<LibusbBackend*>(instance);
}
extern "C" WII_BT_API int WII_BT_CALL WiiBt_Enable(void* instance, int enabled, int continuous) {
    if (!instance) return 0;
    static_cast<LibusbBackend*>(instance)->Enable(enabled != 0, continuous != 0);
    return 1;
}
extern "C" WII_BT_API int WII_BT_CALL WiiBt_Scan(void* instance) {
    if (!instance) return 0;
    static_cast<LibusbBackend*>(instance)->Scan();
    return 1;
}
extern "C" WII_BT_API int WII_BT_CALL WiiBt_Disconnect(void* instance, int player) {
    if (!instance || player < 0 || player > 1) return 0;
    static_cast<LibusbBackend*>(instance)->Disconnect(player);
    return 1;
}
extern "C" WII_BT_API int WII_BT_CALL WiiBt_SwapPlayers(void* instance) {
    if (!instance) return 0;
    static_cast<LibusbBackend*>(instance)->SwapPlayers();
    return 1;
}
extern "C" WII_BT_API int WII_BT_CALL WiiBt_Poll(void* instance, WiiBtSnapshot* output) {
    if (!instance || !output || output->struct_size < sizeof(WiiBtSnapshot)) return 0;
    try {
        const Snapshot source = static_cast<LibusbBackend*>(instance)->Poll();
        WiiBtSnapshot result{};
        result.struct_size = sizeof(result);
        result.scanning = source.scanning ? 1u : 0u;
        for (size_t i = 0; i < source.player.size(); ++i) {
            const Remote& in = source.player[i];
            WiiBtPlayerState& out = result.player[i];
            out.struct_size = sizeof(out);
            out.battery = in.battery;
            out.buttons = in.buttons;
            out.pressed = in.pressed;
            out.connected = in.connected ? 1u : 0u;
            out.reserved = in.reserved ? 1u : 0u;
            out.tracking = in.tracking ? 1u : 0u;
            out.dots = static_cast<uint8_t>(in.dots);
            out.raw_x = in.raw.x;
            out.raw_y = in.raw.y;
        }
        std::snprintf(result.message, sizeof(result.message), "%s", source.message.c_str());
        *output = result;
        return 1;
    } catch (...) { return 0; }
}
#endif
} // namespace input::wii
#endif
