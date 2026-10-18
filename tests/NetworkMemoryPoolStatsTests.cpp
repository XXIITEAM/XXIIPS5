// PSVXXII P0: hardware-observed network pool-stat contracts tested via real NID registration.
// Synthetic contexts only; never loads or exports a proprietary game binary.
#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Libs::LibNet {
void InitNet_1_Net(Loader::SymbolDatabase*);
}
namespace Libs::LibSsl {
void InitNet_1_Ssl(Loader::SymbolDatabase*);
}
namespace Libs::LibHttp2 {
void InitNet_1_Http2(Loader::SymbolDatabase*);
}

namespace {

struct PoolStats {
    uint64_t pool_size;
    uint64_t max_inuse_size;
    uint64_t current_inuse_size;
    int32_t reserved;
};
struct NetPoolStats {
    uint64_t pool_size;
    uint64_t max_inuse_size;
    uint64_t current_inuse_size;
};
static_assert(sizeof(PoolStats) == 32 && offsetof(PoolStats, reserved) == 24);
static_assert(sizeof(NetPoolStats) == 24);

using Stats = int(KYTY_SYSV_ABI*)(int, void*);
using Http2Init = int(KYTY_SYSV_ABI*)(int, int, size_t, int);
using Http2Term = int(KYTY_SYSV_ABI*)(int);

int failures = 0;
void Check(bool pass, const char* what) {
    if (!pass) {
        std::fprintf(stderr, "NetworkMemoryPoolStatsTests: %s\n", what);
        ++failures;
    }
}

template<class F>
F Resolve(const Loader::SymbolDatabase& db, const char* nid, const char* library) {
    const Loader::SymbolResolve id{nid, library, 1, library, 1, 1, Loader::SymbolType::Func};
    const auto* found = db.Find(id);
    Check(found != nullptr, "missing expected per-library NID");
    return found == nullptr ? nullptr : reinterpret_cast<F>(found->vaddr);
}

template<size_t N>
bool Filled(const std::array<uint8_t, N>& data, size_t start, size_t stop, uint8_t val) {
    for (size_t n = start; n < stop; n++) if (data[n] != val) return false;
    return true;
}

void CheckValidStats(Stats call, int id, uint64_t expected, size_t bytes, const char* title) {
    alignas(16) std::array<uint8_t, 64> buffer{};
    buffer.fill(0xA5);
    Check(call(id, buffer.data()) == 0, title);
    uint64_t fields[3]{};
    std::memcpy(fields, buffer.data(), sizeof(fields));
    Check(fields[0] == expected, "actual pool size is the init/create size");
    Check(fields[1] == 0 && fields[2] == 0, "unused pool reports zero measured use");
    if (bytes == sizeof(PoolStats)) {
        int32_t reserved = -1;
        std::memcpy(&reserved, buffer.data() + 24, sizeof(reserved));
        Check(reserved == 0, "reserved memory-stat word is zero");
    }
    Check(Filled(buffer, bytes, buffer.size(), 0xA5), "stat write did not exceed ABI struct");
}

void CheckInvalidCleared(Stats call, int id, int expected) {
    alignas(16) std::array<uint8_t, 64> buffer{};
    buffer.fill(0xA5);
    Check(call(id, buffer.data()) == expected, "invalid ID error code");
    Check(Filled(buffer, 0, sizeof(PoolStats), 0), "invalid HTTP/SSL context zeros stat result");
    Check(Filled(buffer, sizeof(PoolStats), buffer.size(), 0xA5),
          "invalid HTTP/SSL context does not write past 32 bytes");
}

} // namespace

int main() {
    Common::VirtualMemory::Init();
    Common::InitializeThreads();
    static Common::Subsystems systems;
    systems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    Config::Load(options);
    systems.Initialize<Log::Lifecycle>();
    systems.Initialize<Libs::Network::Lifecycle>();

    Loader::SymbolDatabase db;
    Libs::LibNet::InitNet_1_Net(&db);
    Libs::LibSsl::InitNet_1_Ssl(&db);
    Libs::LibHttp2::InitNet_1_Http2(&db);

    const auto net = Resolve<Stats>(db, "rMyh97BU5pY", "Net");
    const auto ssl = Resolve<Stats>(db, "-PoIzr3PEk0", "Ssl");
    const auto http2 = Resolve<Stats>(db, "otUQuZa-mv0", "Http2");
    const auto init = Resolve<Http2Init>(db, "3JCe3lCbQ8A", "Http2");
    const auto term = Resolve<Http2Term>(db, "YiBUtz-pGkc", "Http2");
    if (!net || !ssl || !http2 || !init || !term) return 1;

    // Public experimental ABI evidence (AnyPS5 PS5-hardware measurements):
    // HTTP2 / SSL: 32 bytes, Net: 24 bytes; specific return codes per library.
    constexpr int HTTP_BAD_ID = static_cast<int>(0x817B1100u);
    constexpr int HTTP_NULL = static_cast<int>(0x817B1225u);
    constexpr int SSL_BAD_ID = static_cast<int>(0x8095F006u);
    constexpr int SSL_NULL = static_cast<int>(0x8095F007u);
    constexpr int NET_BAD_ID = static_cast<int>(0x80410109u);
    constexpr int NET_NULL = static_cast<int>(0x80410116u);

    const int netid = Libs::Network::Net::NetPoolCreate("test", 0x28000, 0);
    const int sslid = Libs::Network::Ssl::SslInit(0x34000);
    const int httpid = init(netid, sslid, 0x48000, 1);
    Check(netid > 0 && sslid > 0 && httpid > 0, "created three real HLE pool contexts");

    CheckValidStats(net, netid, 0x28000, sizeof(NetPoolStats), "Net live pool");
    CheckValidStats(ssl, sslid, 0x34000, sizeof(PoolStats), "Ssl live pool");
    CheckValidStats(http2, httpid, 0x48000, sizeof(PoolStats), "Http2 live pool");

    Check(http2(httpid, nullptr) == HTTP_NULL &&
          http2(12345, nullptr) == HTTP_NULL, "HTTP2 null pointer precedes ID check");
    Check(ssl(sslid, nullptr) == SSL_NULL &&
          ssl(12345, nullptr) == SSL_NULL, "Ssl null pointer precedes ID check");
    Check(net(netid, nullptr) == NET_NULL &&
          net(12345, nullptr) == NET_BAD_ID, "Net ID check precedes null pointer");

    CheckInvalidCleared(http2, 12345, HTTP_BAD_ID);
    CheckInvalidCleared(http2, -1, HTTP_BAD_ID);
    CheckInvalidCleared(ssl, 12345, SSL_BAD_ID);
    CheckInvalidCleared(ssl, -1, SSL_BAD_ID);
    CheckInvalidCleared(ssl, netid, SSL_BAD_ID); // typed handles, no cross-pool confusion

    alignas(16) std::array<uint8_t, 64> untouched{};
    untouched.fill(0xA5);
    const auto original = untouched;
    Check(net(12345, untouched.data()) == NET_BAD_ID && untouched == original,
          "invalid Net descriptor must not change caller buffer");

    Check(term(httpid) == 0, "HTTP2 context termination");
    CheckInvalidCleared(http2, httpid, HTTP_BAD_ID);
    Check(Libs::Network::Ssl::SslTerm(sslid) == 0, "Ssl context termination");
    CheckInvalidCleared(ssl, sslid, SSL_BAD_ID);
    Check(Libs::Network::Net::NetPoolDestroy(netid) == 0, "Net pool destruction");
    Check(net(netid, untouched.data()) == NET_BAD_ID && untouched == original,
          "destroyed Net pool has no stale successful stat");
    Check(http2(12345, nullptr) == HTTP_NULL, "HTTP2 error order remains stable");

    if (failures == 0) std::puts("NetworkMemoryPoolStatsTests: all tests passed");
    return failures == 0 ? 0 : 1;
}
