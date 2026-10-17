// PSVXXII P0-SPV6: Developer-only native Vulkan layout receipt shared by
// the guest runtime and the independent real-device Vulkan smoke test.
// The identical implementation is exercised against actual Vk calls;
// it NEVER reads GPU memory or grants patch privileges.
#pragma once
#if defined(XXII_DEV_GPU_LAYOUT_TRACE)

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>

namespace Libs::Graphics::Developer {

inline bool NativeLayoutTraceEnabled() {
    // One stable evaluation per process avoids inspecting environment
    // in the hot path. The parent must explicitly arm the next session.
    static const bool enabled=[] {
        const char* probe=std::getenv("XXII_COMPAT_PROBE");
        const char* trace=std::getenv("XXII_GPU_LAYOUT_TRACE");
        return probe && probe[0]=='1' && probe[1]=='\0' &&
               trace && trace[0]=='1' && trace[1]=='\0';
    }();
    return enabled;
}

// Binding, Push and Module are small standard Vk/vk-hpp-compatible
// structures. Module must have (stage, program), program must contain the
// final SPIR-V XXH3_128 pair, word count and process-local program id.
// No independent trace formatter or pipeline descriptor builder exists.
template <typename Binding,typename Push,typename Module>
inline void TraceNativePipelineLayout(
    const char* pipeline_class,
    std::span<const Binding> bindings,
    const Push& push, bool created,
    std::span<const Module> modules, bool complete,
    FILE* sink=stderr) {
    if(!NativeLayoutTraceEnabled() || sink==nullptr)return;
    static std::atomic<uint32_t> next{0};
    const uint32_t seq=next.fetch_add(1,std::memory_order_relaxed)+1;
    if(seq>64)return; // a fixed, per-process receipt budget

    constexpr size_t MaxBindings=16;
    constexpr uint64_t MaxDescriptors=4096;
    uint64_t total=0;
    for(const auto& binding:bindings)total+=binding.descriptorCount;
    const bool capped=bindings.size()>MaxBindings || total>MaxDescriptors;
    std::string tuples=capped ? "CAP" : "-";
    if(!capped && !bindings.empty()) {
        tuples.clear();
        for(const auto& binding:bindings) {
            if(!tuples.empty())tuples.push_back(';');
            tuples+=std::to_string(binding.binding);
            tuples.push_back(':');
            tuples+=std::to_string(static_cast<uint32_t>(binding.descriptorType));
            tuples.push_back(':');
            tuples+=std::to_string(binding.descriptorCount);
            tuples.push_back(':');
            tuples+=std::to_string(static_cast<uint32_t>(binding.stageFlags));
        }
    }

    // Program IDs are meaningful only within one cache/session. XXH3_128 is
    // a non-cryptographic fingerprint of the final shader, never an
    // authenticated attestation of where a child stderr line came from.
    const bool moduleCap=modules.empty() || modules.size()>4;
    bool fullIdentity=complete && !moduleCap;
    std::string ids;
    for(const auto& module:modules) {
        if(!module.program || module.program->id==0 ||
           module.program->evidence_words<5 ||
           module.program->evidence_words>65536u ||
           (module.program->evidence_low==0 && module.program->evidence_high==0)) {
            fullIdentity=false;
            break;
        }
        char hex[33]{};
        std::snprintf(hex,sizeof(hex),"%016" PRIx64 "%016" PRIx64,
                      module.program->evidence_high,module.program->evidence_low);
        if(!ids.empty())ids.push_back(';');
        ids+=std::to_string(module.stage);
        ids.push_back(':');
        ids+=std::to_string(module.program->id);
        ids.push_back(':');
        ids+=std::to_string(module.program->evidence_words);
        ids.push_back(':');
        ids+=hex;
    }
    if(moduleCap || ids.size()>320)fullIdentity=false;
    if(!fullIdentity)ids="CAP"; // all-or-nothing stage provenance

    // No shader bytes, driver messages, resource names, files, GPU handles,
    // guest paths or process addresses are ever written to the receipt.
    std::fprintf(sink,
        "XXII_GPU_LAYOUT_V2 seq=%u class=%s created=%d bindings=%u total=%u "
        "push_offset=%u push_size=%u push_stages=%u tuples=%s "
        "modules=%u complete=%u ids=%s\n",
        seq,pipeline_class,created?1:0,
        static_cast<unsigned>(std::min(bindings.size(),size_t(999))),
        static_cast<unsigned>(std::min(total,uint64_t(65535))),
        push.offset,push.size,static_cast<uint32_t>(push.stageFlags),
        tuples.c_str(),static_cast<unsigned>(std::min(modules.size(),size_t(4))),
        fullIdentity?1u:0u,ids.c_str());
    std::fflush(sink);
}

} // namespace Libs::Graphics::Developer
#endif // XXII_DEV_GPU_LAYOUT_TRACE
