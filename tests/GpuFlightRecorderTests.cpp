#include "graphics/host_gpu/renderer/gpuFlightRecorder.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>

using namespace Libs::Graphics::GpuTrace;

template <typename T>
concept HasSubmissionHistory = requires { &T::RecordSubmission; };
template <typename T>
concept HasCommandTrace = requires { &T::RecordGpuTrace; };
template <typename T>
concept HasDiagnosticImageId = requires { &T::SetDiagnosticId; };
template <typename T>
concept HasGpuCheckpoints = requires(T& value) { value.device_checkpoints_enabled; };
template <typename T>
concept HasCheckpointDump = requires { &T::DumpCheckpoints; };
static_assert(HasSubmissionHistory<Libs::Graphics::MasterSemaphore>);
static_assert(HasCommandTrace<Libs::Graphics::CommandBuffer>);
static_assert(HasDiagnosticImageId<Libs::Graphics::Image>);
static_assert(HasGpuCheckpoints<Libs::Graphics::GraphicContext>);
static_assert(HasCheckpointDump<Libs::Graphics::MasterSemaphore>);

void Require(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "GpuFlightRecorderTests: FAILED %s\n", label);
        std::exit(1);
    }
}

int main() {
    Require(!ShouldSampleTick(0), "never sample unsubmitted tick zero");
    Require(ShouldSampleTick(1) && ShouldSampleTick(2) &&
            ShouldSampleTick(8) && ShouldSampleTick(512) &&
            ShouldSampleTick(2048), "startup samples");
    Require(!ShouldSampleTick(3) && !ShouldSampleTick(511) &&
            !ShouldSampleTick(2049), "bounded startup samples");
    Require(ShouldSampleTick(4096) && ShouldSampleTick(8192) &&
            !ShouldSampleTick(4097), "periodic steady samples");
    CommandTrace trace;
    Require(trace.Count() == 0 && trace.Dropped() == 0, "empty");
    for (uint32_t i = 0; i < CommandTrace::Capacity + 7; ++i)
        trace.Push(Kind::DrawIndexed, i, 3, 1, uint64_t {i} + 100, 99);
    Require(trace.Count() == CommandTrace::Capacity, "bounded capacity");
    Require(trace.Dropped() == 7, "truncation count");
    Require(trace.At(0).a == 0, "first command retained");
    Require(trace.OriginalIndex(0) == 0, "prefix retains original CPU sequence index");
    Require(trace.OriginalIndex(CommandTrace::PrefixCapacity) ==
            CommandTrace::PrefixCapacity + 7, "tail reports noncontiguous original sequence");
    Require(trace.OriginalIndex(trace.Count()-1) == CommandTrace::Capacity + 6,
            "last recorded CPU sequence index");
    Require(trace.At(CommandTrace::PrefixCapacity - 1).a ==
            CommandTrace::PrefixCapacity - 1, "first segment boundary");
    Require(trace.At(CommandTrace::PrefixCapacity).a ==
            CommandTrace::PrefixCapacity + 7, "tail oldest after wrap");
    Require(trace.At(trace.Count()-1).a == CommandTrace::Capacity + 6, "latest preserved");
    trace.Reset();
    Require(trace.Total() == 0, "begin new buffer");
    trace.Push(Kind::GraphicsPipeline, 0, 0, 0, 123);
    trace.Push(Kind::DescriptorWrites, 4, 2, 1);
    trace.Push(Kind::DrawIndexed, 2550, 1, 0);
    const auto signature = trace.Signature();
    CommandTrace different_submit;
    different_submit.Push(Kind::GuestBegin, 2, 3, 4, 0xdeadbeef);
    // Submit IDs are intentionally excluded from the grouping signature.
    different_submit.Push(Kind::GraphicsPipeline, 0, 0, 0, 123);
    different_submit.Push(Kind::DescriptorWrites, 4, 2, 1);
    different_submit.Push(Kind::DrawIndexed, 2550, 1, 0);
    Require(signature == different_submit.Signature(), "signature ignores guest submit");
    different_submit.Push(Kind::DrawIndexed, 2551);
    Require(signature != different_submit.Signature(), "signature detects changed workload");
    CommandTrace access_a, access_b;
    access_a.Push(Kind::ImageBarrierAccess, 44, 3, 0, 0x20, 0x100);
    access_b.Push(Kind::ImageBarrierAccess, 44, 3, 0, 0x20, 0x200);
    Require(access_a.Signature() != access_b.Signature(),
            "barrier destination access must contribute to the GPU command fingerprint");
    CommandTrace stage_a, stage_b;
    stage_a.Push(Kind::ImageBarrierStages, 44, 3, 0, 1, 2);
    stage_b.Push(Kind::ImageBarrierStages, 44, 3, 0, 1, 8);
    Require(stage_a.Signature() != stage_b.Signature(),
            "pipeline stage change must distinguish CPU encoded work");
    CommandTrace range_a, range_b;
    range_a.Push(Kind::ImageBarrierSubresource, 44, 3, 0, (uint64_t {1} << 32), 1);
    range_b.Push(Kind::ImageBarrierSubresource, 44, 3, 1, (uint64_t {1} << 32), 1);
    Require(range_a.Signature() != range_b.Signature(),
            "mip subresource changes must distinguish CPU encoded work");
    Require(std::strcmp(Name(Kind::ImageBarrierStages), "image_barrier_stages") == 0,
            "source and destination stage trace kind");
    Require(std::strcmp(Name(Kind::ImageBarrierAccess), "image_barrier_access") == 0,
            "source and destination access trace kind");
    Require(std::strcmp(Name(Kind::ImageBarrierSubresource), "image_barrier_subresource") == 0,
            "barrier subresource trace kind");
    Require(std::strcmp(Name(Kind::ShaderResourceShape), "shader_resource_shape") == 0,
            "shader resource shape trace kind");
    auto recorder = std::make_unique<FlightRecorder>();
    for (uint64_t tick = 1; tick <= FlightRecorder::Capacity + 8; ++tick)
        recorder->Add(tick, trace);
    Require(recorder->Count() == FlightRecorder::Capacity, "flight buffer bound");
    Require(recorder->At(0).tick == 9, "oldest buffer");
    Require(recorder->At(recorder->Count()-1).tick == FlightRecorder::Capacity + 8,
            "last buffer");
    Require(recorder->At(0).commands.At(0).identity == 123, "snapshot copied");
    Require(std::strcmp(Name(Kind::DescriptorImage), "descriptor_image") == 0, "known op");
    Require(std::strcmp(Name(Kind::DescriptorImageIdentity), "descriptor_image_id") == 0,
            "resource identity event");
    Require(std::strcmp(Name(static_cast<Kind>(0)), "unknown") == 0, "unknown op");
    constexpr const char* report_name = "gpu-flight-test-report.txt";
    FILE* report = std::fopen(report_name, "w+bx");
    Require(report != nullptr, "create exclusive project-local report");
    recorder->Dump(report, 20);
    std::rewind(report);
    char output[12000] {};
    const size_t bytes = std::fread(output, 1, sizeof(output)-1, report);
    std::fclose(report);
    Require(std::remove(report_name) == 0, "remove project-local report");
    Require(bytes > 0, "report nonempty");
    Require(std::strstr(output, "first_unconfirmed=21") != nullptr, "fault boundary");
    Require(std::strstr(output, "kind=draw_indexed a=2550") != nullptr, "real command");
    Require(std::strstr(output, "tick=21 completed=0") != nullptr, "in flight bound");
    Require(std::strstr(output, "XXII_GPU_FLIGHT_PRIMARY tick=21") != nullptr, "primary fault marker");
    FILE* success_report = std::fopen(report_name, "w+bx");
    Require(success_report != nullptr, "healthy project-local report");
    recorder->Dump(success_report, 20, false);
    std::rewind(success_report);
    char healthy_output[12000] {};
    (void)std::fread(healthy_output, 1, sizeof(healthy_output)-1, success_report);
    std::fclose(success_report);
    Require(std::remove(report_name) == 0, "remove project-local healthy report");
    Require(std::strstr(healthy_output, "XXII_GPU_FLIGHT_PRIMARY") == nullptr,
            "healthy GPU in-flight is not a fault");
    std::printf("GPU flight recorder tests passed master_bytes=%zu command_bytes=%zu image_bytes=%zu\n",
                sizeof(Libs::Graphics::MasterSemaphore),
                sizeof(Libs::Graphics::CommandBuffer), sizeof(Libs::Graphics::Image));
}