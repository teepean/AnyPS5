#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/ShapeCensus.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {
namespace {

enum Outcome : std::uint8_t { First, Strict, BaseOnly, TemplateRule, Structure, Descriptor, OutcomeCount };
constexpr const char* OutcomeNames[OutcomeCount] = {"first", "strict", "base-only", "template-rule", "structure", "descriptor"};

enum WordClass : std::uint8_t { V0, V1, V1High, V2, V3, VWritten, Image, Sampler, Data, User, Nested, Other, Variant, Bindings, RunCount, RunOrigin, RunOffset, RunLength, ClassCount };
constexpr const char* ClassNames[ClassCount] = {"V0", "V1lo", "V1hi", "V2", "V3", "Vw", "T", "S", "F", "U", "P", "O", "variant", "bindings", "run-count", "run-origin", "run-offset", "run-length"};

using ClassMask = std::uint32_t;
constexpr ClassMask Bit(WordClass value) { return ClassMask{1} << value; }
constexpr ClassMask TemplateClasses = Bit(V0) | Bit(V1) | Bit(V2) | Bit(Data) | Bit(User) | Bit(Nested);
constexpr ClassMask StructureClasses = Bit(RunCount) | Bit(RunOrigin) | Bit(RunOffset) | Bit(RunLength);

struct Run {
    std::int32_t origin;
    std::uint64_t at;
    std::uint32_t words;
};

struct Entry {
    std::vector<Run> runs;
    std::vector<std::uint32_t> userData;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
};

struct ProgramMisses {
    std::uint64_t misses = 0;
    std::array<std::uint64_t, ClassCount> first{};
};

struct Census {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, Entry> entries;
    std::array<std::uint64_t, OutcomeCount> stages{};
    std::array<double, OutcomeCount> stageUs{};
    std::array<std::uint64_t, ClassCount> firstMiss{};
    std::array<std::uint64_t, ClassCount> templatePresent{};
    std::unordered_map<std::uint64_t, ProgramMisses> programs;
    std::uint64_t draws = 0;
    std::uint64_t allHitDraws = 0;
    double allHitUs = 0.0;
    double otherUs = 0.0;
    std::uint64_t replays = 0;
    double replayUs = 0.0;
    std::uint64_t replayWords = 0;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

Census& census() {
    static Census* state = new Census();
    return *state;
}

struct OpenDraw {
    std::uint64_t shapeKey = 0;
    bool open = false;
    bool allHit = true;
    std::uint32_t stages = 0;
    double us = 0.0;
};

thread_local OpenDraw openDraw;

std::uint64_t entryKey(std::uint64_t codeAddress, std::size_t codeOffset, std::uint32_t pushOffset, std::uint64_t shapeKey) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto value : {codeAddress, static_cast<std::uint64_t>(codeOffset), static_cast<std::uint64_t>(pushOffset), shapeKey}) {
        hash ^= value;
        hash *= 0x100000001b3ull;
        hash ^= hash >> 29u;
    }
    return hash;
}

std::vector<Run> relativeRuns(std::span<const std::uint32_t> userData, const ShaderRecompiler::SrtUserDataUse& use, std::span<const ShaderRecompiler::MemoryRegion> regions) {
    auto reads = use.reads;
    std::sort(reads.begin(), reads.end());
    const auto origin = [&](std::uint64_t address) -> std::int32_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0 || static_cast<std::size_t>(found->second) >= userData.size()) return -1;
        return found->second;
    };
    std::vector<Run> runs;
    for (const auto& region : regions) {
        const auto words = region.bytes.size() / sizeof(std::uint32_t);
        if (words == 0) continue;
        std::size_t start = 0;
        auto current = origin(region.guestAddress);
        for (std::size_t word = 1; word <= words; ++word) {
            const auto next = word < words ? origin(region.guestAddress + word * sizeof(std::uint32_t)) : std::int32_t{-2};
            if (next == current) continue;
            const auto address = region.guestAddress + start * sizeof(std::uint32_t);
            const auto base = current >= 0 ? static_cast<std::uint64_t>(userData[static_cast<std::size_t>(current)] & ~std::uint32_t{3}) : 0u;
            runs.push_back({current, current >= 0 ? address - base : address, static_cast<std::uint32_t>(word - start)});
            start = word;
            current = next;
        }
    }
    return runs;
}

ClassMask compareRuns(const std::vector<Run>& before, const std::vector<Run>& now) {
    if (before.size() != now.size()) return Bit(RunCount);
    ClassMask mask = 0;
    for (std::size_t i = 0; i < now.size(); ++i) {
        if (before[i].origin != now[i].origin) mask |= Bit(RunOrigin);
        else if (before[i].words != now[i].words) mask |= Bit(RunLength);
        else if (before[i].at != now[i].at) mask |= now[i].origin >= 0 ? Bit(RunOffset) : Bit(Nested);
    }
    return mask;
}

bool imageRole(ShaderRecompiler::DescriptorRole role) {
    using Role = ShaderRecompiler::DescriptorRole;
    return role == Role::GuestImages || role == Role::ImageTable || role == Role::ImageTableMap || role == Role::ImageStorageTable;
}

ClassMask compareResults(const ShaderRecompiler::RecompileResult& before, const ShaderRecompiler::RecompileResult& now) {
    using Role = ShaderRecompiler::DescriptorRole;
    if (before.PipelineVariantId() != now.PipelineVariantId()) return Bit(Variant);
    if (before.bindings.size() != now.bindings.size()) return Bit(Bindings);
    ClassMask mask = 0;
    for (std::size_t b = 0; b < now.bindings.size(); ++b) {
        const auto& was = before.bindings[b];
        const auto& is = now.bindings[b];
        if (was.kind != is.kind || was.role != is.role || was.count != is.count || was.guestDescriptor.size() != is.guestDescriptor.size()) {
            mask |= Bit(Bindings);
            continue;
        }
        if (was.guestDescriptor == is.guestDescriptor) continue;
        if (is.role == Role::GuestBuffers && is.guestDescriptor.size() == static_cast<std::size_t>(is.count) * 4u) {
            for (std::uint32_t element = 0; element < is.count; ++element) {
                const auto* a = was.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
                const auto* c = is.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
                if (std::equal(a, a + 4, c)) continue;
                const bool written = element >= is.bufferWritten.size() || is.bufferWritten[element];
                const auto empty = [](const std::uint32_t* words) { return words[2] == 0 || (words[0] == 0 && (words[1] & 0xffffu) == 0); };
                if (written || empty(a) || empty(c)) {
                    mask |= Bit(VWritten);
                    continue;
                }
                if (a[0] != c[0]) mask |= Bit(V0);
                if ((a[1] & 0xffffu) != (c[1] & 0xffffu)) mask |= Bit(V1);
                if ((a[1] >> 16u) != (c[1] >> 16u)) mask |= Bit(V1High);
                if (a[2] != c[2]) mask |= Bit(V2);
                if (a[3] != c[3]) mask |= Bit(V3);
            }
            continue;
        }
        if (imageRole(is.role)) mask |= Bit(Image);
        else if (is.role == Role::GuestSamplers || is.role == Role::SamplerTable) mask |= Bit(Sampler);
        else if (is.role == Role::FlattenedSrt || is.role == Role::ShaderData) mask |= Bit(Data);
        else mask |= Bit(Other);
    }
    if (before.pushConstants != now.pushConstants) mask |= Bit(Data);
    return mask;
}

ClassMask compareSnapshots(const ShaderRecompiler::ResourceSnapshot& before, const ShaderRecompiler::ResourceSnapshot& now) {
    ClassMask mask = 0;
    if (!(before.tables == now.tables) || before.nullRootReads != now.nullRootReads || before.srtPoison.size() != now.srtPoison.size()) mask |= Bit(Other);
    if (before.images != now.images) mask |= Bit(Image);
    if (before.samplers != now.samplers) mask |= Bit(Sampler);
    if (before.flattenedSrt != now.flattenedSrt) mask |= Bit(Data);
    return mask;
}

WordClass firstClass(ClassMask mask, ClassMask among) {
    for (std::uint8_t value = 0; value < ClassCount; ++value) {
        if ((mask & among & (ClassMask{1} << value)) != 0) return static_cast<WordClass>(value);
    }
    return Other;
}

double replayRuns(const std::vector<Run>& runs, std::span<const std::uint32_t> userData, std::uint64_t& words) {
    thread_local std::vector<std::uint32_t> scratch;
    const auto started = std::chrono::steady_clock::now();
    for (const auto& run : runs) {
        if (run.origin < 0 || static_cast<std::size_t>(run.origin) >= userData.size()) continue;
        const auto address = static_cast<std::uint64_t>(userData[static_cast<std::size_t>(run.origin)] & ~std::uint32_t{3}) + run.at;
        scratch.resize(run.words);
        static_cast<void>(GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(scratch))));
        words += run.words;
    }
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
}

void closeDraw(Census& state) {
    auto& draw = openDraw;
    if (!draw.open) return;
    draw.open = false;
    if (draw.stages == 0) return;
    ++state.draws;
    if (draw.allHit) {
        ++state.allHitDraws;
        state.allHitUs += draw.us;
    } else {
        state.otherUs += draw.us;
    }
}

void report(Census& state) {
    const auto now = std::chrono::steady_clock::now();
    if (now - state.reported < std::chrono::seconds(10)) return;
    state.reported = now;
    std::uint64_t total = 0;
    for (const auto count : state.stages) total += count;
    std::string line = "[shape-census] (10 s): " + std::to_string(total) + " stages:";
    char piece[160];
    for (std::size_t outcome = 0; outcome < OutcomeCount; ++outcome) {
        std::snprintf(piece, sizeof(piece), " %s %llu (%.1f%%, %.1f us each)", OutcomeNames[outcome], static_cast<unsigned long long>(state.stages[outcome]), total != 0 ? 100.0 * static_cast<double>(state.stages[outcome]) / static_cast<double>(total) : 0.0, state.stages[outcome] != 0 ? state.stageUs[outcome] / static_cast<double>(state.stages[outcome]) : 0.0);
        line += piece;
    }
    std::snprintf(piece, sizeof(piece), "; draws %llu, every stage strict/base-only/template-rule %llu (%.1f%%), capture+materialize ms on those %.1f, on the others %.1f", static_cast<unsigned long long>(state.draws), static_cast<unsigned long long>(state.allHitDraws), state.draws != 0 ? 100.0 * static_cast<double>(state.allHitDraws) / static_cast<double>(state.draws) : 0.0, state.allHitUs / 1000.0, state.otherUs / 1000.0);
    line += piece;
    std::snprintf(piece, sizeof(piece), "; replay %.2f us per template-rule stage (%.1f words)", state.replays != 0 ? state.replayUs / static_cast<double>(state.replays) : 0.0, state.replays != 0 ? static_cast<double>(state.replayWords) / static_cast<double>(state.replays) : 0.0);
    line += piece;
    line += "; misses by first class:";
    for (std::size_t value = 0; value < ClassCount; ++value) {
        if (state.firstMiss[value] == 0) continue;
        std::snprintf(piece, sizeof(piece), " %s %llu", ClassNames[value], static_cast<unsigned long long>(state.firstMiss[value]));
        line += piece;
    }
    line += "; template-rule stages with:";
    for (std::size_t value = 0; value < ClassCount; ++value) {
        if (state.templatePresent[value] == 0) continue;
        std::snprintf(piece, sizeof(piece), " %s %llu", ClassNames[value], static_cast<unsigned long long>(state.templatePresent[value]));
        line += piece;
    }
    std::fprintf(stderr, "%s; %zu shapes held\n", line.c_str(), state.entries.size());
    std::vector<std::pair<std::uint64_t, ProgramMisses>> programs(state.programs.begin(), state.programs.end());
    std::sort(programs.begin(), programs.end(), [](const auto& left, const auto& right) { return left.second.misses > right.second.misses; });
    std::string top = "[shape-census] programs with the most misses:";
    for (std::size_t index = 0; index < programs.size() && index < 8; ++index) {
        const auto& [address, misses] = programs[index];
        const auto dominant = static_cast<std::size_t>(std::max_element(misses.first.begin(), misses.first.end()) - misses.first.begin());
        std::snprintf(piece, sizeof(piece), " 0x%llx %llu (%s %llu)", static_cast<unsigned long long>(address), static_cast<unsigned long long>(misses.misses), ClassNames[dominant], static_cast<unsigned long long>(misses.first[dominant]));
        top += piece;
    }
    std::fprintf(stderr, "%s\n", top.c_str());
    state.stages = {};
    state.stageUs = {};
    state.firstMiss = {};
    state.templatePresent = {};
    state.programs.clear();
    state.draws = state.allHitDraws = 0;
    state.allHitUs = state.otherUs = 0.0;
    state.replays = state.replayWords = 0;
    state.replayUs = 0.0;
}

}

bool ShapeCensusEnabled() {
    static const bool enabled = std::getenv("APS5_SHAPE_CENSUS") != nullptr;
    return enabled;
}

void ShapeCensusBeginDraw(std::uint64_t shapeKey) {
    auto& state = census();
    std::lock_guard lock(state.mutex);
    closeDraw(state);
    report(state);
    openDraw = OpenDraw{shapeKey, true, true, 0, 0.0};
}

void ShapeCensusStage(std::uint64_t codeAddress, std::size_t codeOffset, std::uint32_t pushOffset, std::span<const std::uint32_t> userData, const std::shared_ptr<const ShaderRecompiler::ResourceCapture>& capture, std::span<const ShaderRecompiler::MemoryRegion> regions, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiled, double stageMicroseconds) {
    if (capture == nullptr || compiled == nullptr) return;
    auto runs = relativeRuns(userData, capture->userDataUse, regions);
    auto& state = census();
    std::lock_guard lock(state.mutex);
    auto& draw = openDraw;
    const auto key = entryKey(codeAddress, codeOffset, pushOffset, draw.shapeKey);
    auto& entry = state.entries[key];
    Outcome outcome = First;
    ClassMask mask = 0;
    if (entry.compiled != nullptr) {
        mask = compareRuns(entry.runs, runs);
        if ((mask & StructureClasses) == 0) {
            mask |= compareResults(*entry.compiled, *compiled) | compareSnapshots(entry.capture->snapshot, capture->snapshot);
            if (!std::equal(entry.userData.begin(), entry.userData.end(), userData.begin(), userData.end())) mask |= Bit(User);
        }
        if ((mask & StructureClasses) != 0) outcome = Structure;
        else if ((mask & ~TemplateClasses) != 0) outcome = Descriptor;
        else if (mask == 0) outcome = Strict;
        else if (mask == Bit(V0)) outcome = BaseOnly;
        else outcome = TemplateRule;
    }
    ++state.stages[outcome];
    state.stageUs[outcome] += stageMicroseconds;
    if (outcome == Structure || outcome == Descriptor) {
        const auto first = firstClass(mask, outcome == Structure ? StructureClasses : ~TemplateClasses);
        ++state.firstMiss[first];
        auto& program = state.programs[codeAddress];
        ++program.misses;
        ++program.first[first];
    }
    if (outcome == TemplateRule || outcome == BaseOnly) {
        for (std::size_t value = 0; value < ClassCount; ++value) {
            if ((mask & (ClassMask{1} << value)) != 0) ++state.templatePresent[value];
        }
        ++state.replays;
        state.replayUs += replayRuns(runs, userData, state.replayWords);
    }
    if (draw.open) {
        ++draw.stages;
        draw.us += stageMicroseconds;
        if (outcome == First || outcome == Structure || outcome == Descriptor) draw.allHit = false;
    }
    entry.runs = std::move(runs);
    entry.userData.assign(userData.begin(), userData.end());
    entry.capture = capture;
    entry.compiled = compiled;
}

}
