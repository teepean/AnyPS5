#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/StageInstances.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace AgcDriver::DriverDetail {
namespace {


struct Run {
    std::int32_t origin;
    std::uint64_t at;
    std::uint32_t words;
    std::uint32_t first;
};

enum class SourceKind : std::uint8_t { User, Word, Vertex, Draw };

struct Source {
    SourceKind kind;
    std::uint32_t index;
    std::uint32_t delta;
    std::uint64_t wide = 0;
};

enum class FieldKind : std::uint8_t { Binding, Push, Address };

constexpr std::uint64_t AddressMask = (std::uint64_t{1} << 48u) - 1u;

struct Patch {
    FieldKind kind;
    std::uint32_t binding;
    std::uint32_t word;
    Source source;
    std::uint32_t confirmations = 0;
    bool overflow = false;
};

constexpr std::size_t DrawWords = 5;
using Draw = std::array<std::uint32_t, DrawWords>;
constexpr std::size_t VertexWords = ShaderRecompiler::ShaderVertexStageInfo::MaxResources * 4u;

struct Entry {
    std::shared_ptr<const void> shader;
    std::vector<Run> runs;
    std::vector<std::uint32_t> words;
    std::vector<std::uint32_t> userData;
    std::optional<ShaderRecompiler::ShaderVertexStageInfo> vertex;
    Draw draw{};
    ShaderRecompiler::SrtUserDataUse use;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> result;
    std::vector<Patch> patches;
    std::vector<bool> userObserved;
    std::vector<bool> wordObserved;
    std::vector<bool> vertexObserved;
    std::array<bool, DrawWords> drawObserved{};
    std::vector<bool> userPinned;
    bool learned = false;
    bool blocked = false;
    bool ready = false;
    std::uint32_t stable = 0;
};

enum Miss : std::uint8_t { MissNoEntry, MissNotLearned, MissBlocked, MissNotReady, MissUser, MissPointerFlags, MissVertex, MissDraw, MissRead, MissWord, MissCarry, MissCount };
constexpr const char* MissNames[MissCount] = {"no entry", "not learned", "tables or poison", "not confirmed", "user word", "pointer flags", "vertex", "draw", "read", "run word", "carry"};

enum LearnFail : std::uint8_t { FailStructure, FailVariant, FailBindings, FailPushSize, FailVertex, FailForwarded, FailDescriptor, FailNoSource, FailCount };
constexpr const char* LearnFailNames[FailCount] = {"structure", "variant", "bindings", "push size", "vertex", "forwarded words", "texture or sampler", "no source"};

struct Counters {
    std::uint64_t lookups = 0;
    std::uint64_t hits = 0;
    std::uint64_t shared = 0;
    std::uint64_t patchedWords = 0;
    std::array<std::uint64_t, MissCount> misses{};
    std::uint64_t learned = 0;
    std::array<std::uint64_t, FailCount> failed{};
    std::uint64_t compared = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t mismatchedPatched = 0;
    std::uint64_t mismatchedUnpatched = 0;
    std::uint64_t mismatchedShape = 0;
    std::uint64_t regionMismatches = 0;
    std::uint64_t fastTimed = 0;
    double fastUs = 0.0;
    std::uint64_t fullTimed = 0;
    double fullUs = 0.0;
};

struct State {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<const Entry>> entries;
    Counters counters;
    std::chrono::steady_clock::time_point reported = std::chrono::steady_clock::now();
};

State& state() {
    static State* value = new State();
    return *value;
}

thread_local std::uint64_t currentShape = 0;

void mix(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value;
    hash *= 0x100000001b3ull;
    hash ^= hash >> 31u;
}

std::uint32_t aligned(std::uint32_t value) {
    return value & ~std::uint32_t{3};
}

std::vector<Run> relativeRuns(std::span<const std::uint32_t> userData, const ShaderRecompiler::SrtUserDataUse& use, std::span<const ShaderRecompiler::MemoryRegion> regions, std::vector<std::uint32_t>& words) {
    auto reads = use.reads;
    std::sort(reads.begin(), reads.end());
    const auto origin = [&](std::uint64_t address) -> std::int32_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0 || static_cast<std::size_t>(found->second) >= userData.size()) return -1;
        return found->second;
    };
    std::vector<Run> runs;
    words.clear();
    for (const auto& region : regions) {
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        if (count == 0) continue;
        const auto first = words.size();
        words.resize(first + count);
        std::memcpy(words.data() + first, region.bytes.data(), count * sizeof(std::uint32_t));
        std::size_t start = 0;
        auto current = origin(region.guestAddress);
        for (std::size_t word = 1; word <= count; ++word) {
            const auto next = word < count ? origin(region.guestAddress + word * sizeof(std::uint32_t)) : std::int32_t{-2};
            if (next == current) continue;
            const auto address = region.guestAddress + start * sizeof(std::uint32_t);
            const auto base = current >= 0 ? static_cast<std::uint64_t>(aligned(userData[static_cast<std::size_t>(current)])) : 0u;
            runs.push_back({current, current >= 0 ? address - base : address, static_cast<std::uint32_t>(word - start), static_cast<std::uint32_t>(first + start)});
            start = word;
            current = next;
        }
    }
    return runs;
}

bool sameRuns(const std::vector<Run>& left, const std::vector<Run>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].origin != right[i].origin || left[i].at != right[i].at || left[i].words != right[i].words || left[i].first != right[i].first) return false;
    }
    return true;
}

bool sameBindingShape(const ShaderRecompiler::DescriptorBinding& a, const ShaderRecompiler::DescriptorBinding& b) {
    return a.kind == b.kind && a.role == b.role && a.descriptorSet == b.descriptorSet && a.binding == b.binding && a.count == b.count && a.readOnly == b.readOnly && a.imageShape == b.imageShape && a.guestDescriptor.size() == b.guestDescriptor.size() && a.samplerDepthCompare == b.samplerDepthCompare && a.imageWritten == b.imageWritten && a.imageDepthCompare == b.imageDepthCompare && a.imageAtomic == b.imageAtomic && a.imageAtomic64 == b.imageAtomic64 && a.imageShapes == b.imageShapes && a.bufferAtomic == b.bufferAtomic && a.bufferWritten == b.bufferWritten && a.samplerUnnormalized == b.samplerUnnormalized && a.samplerIntegerBorder == b.samplerIntegerBorder && a.samplerCoordinatesIgnored == b.samplerCoordinatesIgnored && a.imageUnnormalized == b.imageUnnormalized && a.imageUnnormalizedBaseLevel == b.imageUnnormalizedBaseLevel && a.imageSamplers == b.imageSamplers && a.bufferRead == b.bufferRead && a.bufferExtent == b.bufferExtent;
}

std::uint32_t pushWord(const ShaderRecompiler::RecompileResult& result, std::size_t index) {
    std::uint32_t value = 0;
    std::memcpy(&value, result.pushConstants.data() + index * sizeof(std::uint32_t), sizeof(value));
    return value;
}

std::uint32_t vertexWord(const ShaderRecompiler::ShaderVertexStageInfo& vertex, std::uint32_t index) {
    return vertex.resources[index / 4u].fields[index % 4u];
}

bool imageRole(ShaderRecompiler::DescriptorRole role) {
    using Role = ShaderRecompiler::DescriptorRole;
    return role == Role::GuestImages || role == Role::GuestSamplers || role == Role::ImageTable || role == Role::SamplerTable || role == Role::ImageTableMap || role == Role::ImageStorageTable;
}

std::size_t vertexWords(const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex) {
    return vertex.has_value() ? static_cast<std::size_t>(std::min(vertex->resourcesNum, ShaderRecompiler::ShaderVertexStageInfo::MaxResources)) * 4u : 0u;
}

std::uint32_t sourceValue(const Source& source, std::span<const std::uint32_t> userData, std::span<const std::uint32_t> words, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const Draw& draw) {
    switch (source.kind) {
        case SourceKind::User: return userData[source.index] + source.delta;
        case SourceKind::Word: return words[source.index] + source.delta;
        case SourceKind::Vertex: return vertexWord(*vertex, source.index) + source.delta;
        case SourceKind::Draw: return draw[source.index] + source.delta;
    }
    return 0;
}

std::uint32_t sourceBase(const Source& source, std::span<const std::uint32_t> userData, std::span<const std::uint32_t> words, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const Draw& draw) {
    auto plain = source;
    plain.delta = 0;
    return sourceValue(plain, userData, words, vertex, draw);
}

bool overflows(std::uint32_t base, std::uint32_t delta) {
    return static_cast<std::uint32_t>(base + delta) < base;
}

bool sourceExists(const Source& source, std::size_t userWords, std::size_t readWords, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex) {
    switch (source.kind) {
        case SourceKind::User: return source.index < userWords;
        case SourceKind::Word: return source.index < readWords;
        case SourceKind::Vertex: return source.index < vertexWords(vertex);
        case SourceKind::Draw: return source.index < DrawWords;
    }
    return false;
}

bool fieldExists(const ShaderRecompiler::RecompileResult& result, const Patch& patch) {
    if (patch.kind == FieldKind::Binding) return patch.binding < result.bindings.size() && patch.word < result.bindings[patch.binding].guestDescriptor.size();
    return (static_cast<std::size_t>(patch.word) + 1u) * sizeof(std::uint32_t) <= result.pushConstants.size();
}

std::uint32_t fieldValue(const ShaderRecompiler::RecompileResult& result, const Patch& patch) {
    return patch.kind == FieldKind::Binding ? result.bindings[patch.binding].guestDescriptor[patch.word] : pushWord(result, patch.word);
}

std::uint64_t addressOf(const ShaderRecompiler::DescriptorBinding& binding, std::uint32_t element) {
    const auto* words = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
    return (static_cast<std::uint64_t>(words[1] & 0xffffu) << 32u) | words[0];
}

bool bufferQuads(const ShaderRecompiler::DescriptorBinding& binding) {
    return binding.role == ShaderRecompiler::DescriptorRole::GuestBuffers && binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 4u;
}

bool pairExists(const Source& source, std::size_t userWords, std::size_t readWords, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex) {
    switch (source.kind) {
        case SourceKind::User: return static_cast<std::size_t>(source.index) + 1u < userWords;
        case SourceKind::Word: return static_cast<std::size_t>(source.index) + 1u < readWords;
        case SourceKind::Vertex: return source.index % 4u == 0u && source.index < vertexWords(vertex);
        case SourceKind::Draw: return source.index == 0u;
    }
    return false;
}

std::uint64_t pairValue(const Source& source, std::span<const std::uint32_t> userData, std::span<const std::uint32_t> words, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const Draw& draw) {
    switch (source.kind) {
        case SourceKind::User: return (static_cast<std::uint64_t>(userData[source.index + 1u]) << 32u) | userData[source.index];
        case SourceKind::Word: return (static_cast<std::uint64_t>(words[source.index + 1u]) << 32u) | words[source.index];
        case SourceKind::Vertex: return (static_cast<std::uint64_t>(vertexWord(*vertex, source.index + 1u) & 0xffffu) << 32u) | vertexWord(*vertex, source.index);
        case SourceKind::Draw: return (static_cast<std::uint64_t>(draw[1]) << 32u) | draw[0];
    }
    return 0;
}

std::uint64_t addressValue(const Source& source, std::span<const std::uint32_t> userData, std::span<const std::uint32_t> words, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const Draw& draw) {
    return (pairValue(source, userData, words, vertex, draw) + source.wide) & AddressMask;
}

struct Inputs {
    std::span<const std::uint32_t> userData;
    std::span<const std::uint32_t> words;
    const std::optional<ShaderRecompiler::ShaderVertexStageInfo>* vertex;
    const Draw* draw;
};

std::optional<Source> findSource(const Inputs& before, const Inputs& now, std::uint32_t previous, std::uint32_t current, bool exactOnly) {
    const auto vertexCount = std::min(vertexWords(*before.vertex), vertexWords(*now.vertex));
    for (int pass = 0; pass < (exactOnly ? 1 : 2); ++pass) {
        const auto matches = [&](std::uint32_t was, std::uint32_t is) {
            if (pass == 0) return was == previous && is == current;
            return was != is && current - is == previous - was;
        };
        for (std::uint32_t u = 0; u < now.userData.size() && u < before.userData.size(); ++u) {
            if (matches(before.userData[u], now.userData[u])) return Source{SourceKind::User, u, current - now.userData[u]};
        }
        for (std::uint32_t v = 0; v < vertexCount; ++v) {
            const auto was = vertexWord(**before.vertex, v);
            const auto is = vertexWord(**now.vertex, v);
            if (matches(was, is)) return Source{SourceKind::Vertex, v, current - is};
        }
        for (std::uint32_t d = 0; d < DrawWords; ++d) {
            if (matches((*before.draw)[d], (*now.draw)[d])) return Source{SourceKind::Draw, d, current - (*now.draw)[d]};
        }
        for (std::uint32_t w = 0; w < now.words.size() && w < before.words.size(); ++w) {
            if (matches(before.words[w], now.words[w])) return Source{SourceKind::Word, w, current - now.words[w]};
        }
    }
    return std::nullopt;
}

std::optional<Source> findAddressSource(const Inputs& before, const Inputs& now, std::uint64_t previous, std::uint64_t current) {
    const auto vertexCount = std::min(vertexWords(*before.vertex), vertexWords(*now.vertex));
    std::vector<Source> candidates;
    for (std::uint32_t u = 0; u + 1u < now.userData.size() && u + 1u < before.userData.size(); ++u) candidates.push_back({SourceKind::User, u, 0});
    for (std::uint32_t v = 0; v < vertexCount; v += 4u) candidates.push_back({SourceKind::Vertex, v, 0});
    candidates.push_back({SourceKind::Draw, 0, 0});
    for (std::uint32_t w = 0; w + 1u < now.words.size() && w + 1u < before.words.size(); ++w) candidates.push_back({SourceKind::Word, w, 0});
    for (int pass = 0; pass < 2; ++pass) {
        for (auto candidate : candidates) {
            const auto was = pairValue(candidate, before.userData, before.words, *before.vertex, *before.draw) & AddressMask;
            const auto is = pairValue(candidate, now.userData, now.words, *now.vertex, *now.draw) & AddressMask;
            if (pass == 0 && was == previous && is == current) return candidate;
            if (pass == 1 && was != is && ((current - is) & AddressMask) == ((previous - was) & AddressMask)) {
                candidate.wide = (current - is) & AddressMask;
                return candidate;
            }
        }
    }
    return std::nullopt;
}

std::optional<LearnFail> learn(Entry& entry, const std::vector<Run>& runs, std::span<const std::uint32_t> words, std::span<const std::uint32_t> userData, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const Draw& draw, const ShaderRecompiler::RecompileResult& current) {
    const auto& previous = *entry.result;
    if (!sameRuns(entry.runs, runs) || entry.userData.size() != userData.size()) return FailStructure;
    if (previous.PipelineVariantId() != current.PipelineVariantId() || previous.variantId != current.variantId) return FailVariant;
    if (previous.bindings.size() != current.bindings.size()) return FailBindings;
    if (previous.pushConstants.size() != current.pushConstants.size()) return FailPushSize;
    if (previous.shaderDataUserWords != current.shaderDataUserWords) return FailForwarded;
    if (entry.vertex.has_value() != vertex.has_value() || vertexWords(entry.vertex) != vertexWords(vertex) || previous.vertexAttributes.size() != current.vertexAttributes.size()) return FailVertex;
    for (std::size_t a = 0; a < current.vertexAttributes.size(); ++a) {
        const auto& was = previous.vertexAttributes[a];
        const auto& is = current.vertexAttributes[a];
        if (was.location != is.location || was.components != is.components || was.formatComponents != is.formatComponents || was.resource.fields[3] != is.resource.fields[3] || (was.resource.fields[1] >> 16u) != (is.resource.fields[1] >> 16u)) return FailVertex;
    }
    for (std::uint32_t b = 0; b < current.bindings.size(); ++b) {
        if (!sameBindingShape(previous.bindings[b], current.bindings[b])) return FailBindings;
        if (imageRole(current.bindings[b].role) && previous.bindings[b].guestDescriptor != current.bindings[b].guestDescriptor) return FailDescriptor;
    }
    const auto pushWords = current.pushConstants.size() / sizeof(std::uint32_t);
    if (std::memcmp(previous.pushConstants.data() + pushWords * sizeof(std::uint32_t), current.pushConstants.data() + pushWords * sizeof(std::uint32_t), current.pushConstants.size() - pushWords * sizeof(std::uint32_t)) != 0) return FailPushSize;
    const Inputs before{entry.userData, entry.words, &entry.vertex, &entry.draw};
    const Inputs now{userData, words, &vertex, &draw};
    std::vector<Patch> patches;
    for (const auto& patch : entry.patches) {
        if (patch.kind == FieldKind::Address) {
            if (patch.binding >= current.bindings.size() || patch.word >= current.bindings[patch.binding].count || !bufferQuads(current.bindings[patch.binding]) || !pairExists(patch.source, userData.size(), words.size(), vertex)) continue;
            if (addressOf(previous.bindings[patch.binding], patch.word) != addressValue(patch.source, entry.userData, entry.words, entry.vertex, entry.draw)) continue;
            if (addressOf(current.bindings[patch.binding], patch.word) != addressValue(patch.source, userData, words, vertex, draw)) continue;
            auto confirmed = patch;
            ++confirmed.confirmations;
            patches.push_back(confirmed);
            continue;
        }
        if (!fieldExists(previous, patch) || !fieldExists(current, patch) || !sourceExists(patch.source, userData.size(), words.size(), vertex)) continue;
        if (fieldValue(previous, patch) != sourceValue(patch.source, entry.userData, entry.words, entry.vertex, entry.draw)) continue;
        if (fieldValue(current, patch) != sourceValue(patch.source, userData, words, vertex, draw)) continue;
        auto confirmed = patch;
        ++confirmed.confirmations;
        confirmed.overflow = overflows(sourceBase(patch.source, userData, words, vertex, draw), patch.source.delta);
        patches.push_back(confirmed);
    }
    const auto covered = [&](FieldKind kind, std::uint32_t binding, std::uint32_t word) {
        return std::any_of(patches.begin(), patches.end(), [&](const Patch& patch) { return patch.kind == kind && patch.binding == binding && patch.word == word; });
    };
    const auto& forwarded = current.shaderDataUserWords;
    const auto forwardedUser = [&](std::uint32_t slot) -> std::optional<std::uint32_t> {
        if (slot >= forwarded.size() || forwarded[slot] >= userData.size() || forwarded[slot] >= entry.userData.size()) return std::nullopt;
        return forwarded[slot];
    };
    const auto add = [&](FieldKind kind, std::uint32_t binding, std::uint32_t word, std::uint32_t was, std::uint32_t is) -> bool {
        const bool exactOnly = kind == FieldKind::Binding;
        const auto source = findSource(before, now, was, is, exactOnly);
        if (!source) return false;
        Patch patch{kind, binding, word, *source, 0, false};
        patch.overflow = overflows(sourceBase(*source, userData, words, vertex, draw), source->delta);
        patches.push_back(patch);
        return true;
    };
    for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
        const auto user = forwardedUser(slot);
        if (!user) continue;
        if ((static_cast<std::size_t>(slot) + 1u) * sizeof(std::uint32_t) <= current.pushConstants.size() && !covered(FieldKind::Push, 0, slot) && pushWord(previous, slot) == entry.userData[*user] && pushWord(current, slot) == userData[*user]) {
            patches.push_back({FieldKind::Push, 0, slot, Source{SourceKind::User, *user, 0}, 0, false});
        }
        for (std::uint32_t b = 0; b < current.bindings.size(); ++b) {
            const auto& was = previous.bindings[b];
            const auto& is = current.bindings[b];
            if (is.role != ShaderRecompiler::DescriptorRole::ShaderData || slot >= is.guestDescriptor.size() || covered(FieldKind::Binding, b, slot)) continue;
            if (was.guestDescriptor[slot] == entry.userData[*user] && is.guestDescriptor[slot] == userData[*user]) patches.push_back({FieldKind::Binding, b, slot, Source{SourceKind::User, *user, 0}, 0, false});
        }
    }
    for (std::uint32_t b = 0; b < current.bindings.size(); ++b) {
        const auto& was = previous.bindings[b];
        const auto& is = current.bindings[b];
        if (bufferQuads(is)) {
            for (std::uint32_t e = 0; e < is.count; ++e) {
                const auto* x = was.guestDescriptor.data() + static_cast<std::size_t>(e) * 4u;
                const auto* y = is.guestDescriptor.data() + static_cast<std::size_t>(e) * 4u;
                if ((x[1] >> 16u) != (y[1] >> 16u) || x[3] != y[3]) return FailDescriptor;
                const auto before64 = addressOf(was, e);
                const auto now64 = addressOf(is, e);
                if (before64 != now64 && !covered(FieldKind::Address, b, e)) {
                    const auto source = findAddressSource(before, now, before64, now64);
                    if (!source) return FailNoSource;
                    patches.push_back({FieldKind::Address, b, e, *source, 0, false});
                }
                if (x[2] != y[2] && !covered(FieldKind::Binding, b, e * 4u + 2u)) {
                    if (!add(FieldKind::Binding, b, e * 4u + 2u, x[2], y[2])) return FailNoSource;
                }
            }
            continue;
        }
        for (std::uint32_t w = 0; w < is.guestDescriptor.size(); ++w) {
            if (was.guestDescriptor[w] == is.guestDescriptor[w] || covered(FieldKind::Binding, b, w)) continue;
            if (!add(FieldKind::Binding, b, w, was.guestDescriptor[w], is.guestDescriptor[w])) return FailNoSource;
        }
    }
    for (std::uint32_t d = 0; d < pushWords; ++d) {
        const auto was = pushWord(previous, d);
        const auto is = pushWord(current, d);
        if (was == is || covered(FieldKind::Push, 0, d)) continue;
        if (!add(FieldKind::Push, 0, d, was, is)) return FailNoSource;
    }
    entry.patches = std::move(patches);
    for (std::size_t u = 0; u < userData.size(); ++u) {
        if (userData[u] != entry.userData[u]) entry.userObserved[u] = true;
    }
    for (std::size_t w = 0; w < words.size(); ++w) {
        if (words[w] != entry.words[w]) entry.wordObserved[w] = true;
    }
    for (std::size_t v = 0; v < vertexWords(vertex); ++v) {
        if (v % 4u == 3u) continue;
        if (vertexWord(*vertex, static_cast<std::uint32_t>(v)) != vertexWord(*entry.vertex, static_cast<std::uint32_t>(v))) entry.vertexObserved[v] = true;
    }
    for (std::size_t d = 0; d < DrawWords; ++d) {
        if (draw[d] != entry.draw[d]) entry.drawObserved[d] = true;
    }
    return std::nullopt;
}

void store(Entry& entry, bool keepObserved, const std::shared_ptr<const void>& shader, std::vector<Run> runs, std::vector<std::uint32_t> words, std::span<const std::uint32_t> userData, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const Draw& draw, const ShaderRecompiler::SrtUserDataUse& use, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& result) {
    entry.shader = shader;
    entry.runs = std::move(runs);
    entry.words = std::move(words);
    entry.userData.assign(userData.begin(), userData.end());
    entry.vertex = vertex;
    entry.draw = draw;
    entry.use = use;
    entry.result = result;
    if (!keepObserved || entry.userObserved.size() != entry.userData.size() || entry.wordObserved.size() != entry.words.size()) {
        entry.userObserved.assign(entry.userData.size(), false);
        entry.wordObserved.assign(entry.words.size(), false);
        entry.vertexObserved.assign(VertexWords, false);
        entry.drawObserved.fill(false);
    }
    entry.userPinned.assign(entry.userData.size(), false);
    auto reads = entry.use.reads;
    std::sort(reads.begin(), reads.end());
    for (std::size_t r = 0; r < reads.size(); ++r) {
        const auto pin = [&](std::int32_t origin) {
            if (origin >= 0 && static_cast<std::size_t>(origin) < entry.userPinned.size()) entry.userPinned[static_cast<std::size_t>(origin)] = true;
        };
        if (r + 1 < reads.size() && reads[r].first == reads[r + 1].first && reads[r].second != reads[r + 1].second) {
            pin(reads[r].second);
            pin(reads[r + 1].second);
        }
        if ((reads[r].first & 3u) != 0) pin(reads[r].second);
    }
    entry.ready = entry.learned && !entry.blocked && entry.stable >= 2 && std::all_of(entry.patches.begin(), entry.patches.end(), [](const Patch& patch) { return patch.confirmations >= 1; });
}

void report(State& value) {
    const auto now = std::chrono::steady_clock::now();
    if (now - value.reported < std::chrono::seconds(10)) return;
    value.reported = now;
    auto& c = value.counters;
    std::string line;
    char piece[400];
    std::snprintf(piece, sizeof(piece), "[stage-instances] (10 s): %llu lookups, %llu hits (%.1f%%; %llu shared the cached result, %.2f words patched per hit); misses:", static_cast<unsigned long long>(c.lookups), static_cast<unsigned long long>(c.hits), c.lookups != 0 ? 100.0 * static_cast<double>(c.hits) / static_cast<double>(c.lookups) : 0.0, static_cast<unsigned long long>(c.shared), c.hits != 0 ? static_cast<double>(c.patchedWords) / static_cast<double>(c.hits) : 0.0);
    line += piece;
    for (std::size_t m = 0; m < MissCount; ++m) {
        std::snprintf(piece, sizeof(piece), " %s %llu", MissNames[m], static_cast<unsigned long long>(c.misses[m]));
        line += piece;
    }
    std::snprintf(piece, sizeof(piece), "; learned %llu, failed:", static_cast<unsigned long long>(c.learned));
    line += piece;
    for (std::size_t f = 0; f < FailCount; ++f) {
        std::snprintf(piece, sizeof(piece), " %s %llu", LearnFailNames[f], static_cast<unsigned long long>(c.failed[f]));
        line += piece;
    }
    std::snprintf(piece, sizeof(piece), "; verify %llu compared, %llu differing results, %llu differing regions (patched %llu, unpatched %llu, shape %llu); fast %.2f us per hit, full %.2f us per capture; %zu entries", static_cast<unsigned long long>(c.compared), static_cast<unsigned long long>(c.mismatched), static_cast<unsigned long long>(c.regionMismatches), static_cast<unsigned long long>(c.mismatchedPatched), static_cast<unsigned long long>(c.mismatchedUnpatched), static_cast<unsigned long long>(c.mismatchedShape), c.fastTimed != 0 ? c.fastUs / static_cast<double>(c.fastTimed) : 0.0, c.fullTimed != 0 ? c.fullUs / static_cast<double>(c.fullTimed) : 0.0, value.entries.size());
    line += piece;
    std::fprintf(stderr, "%s\n", line.c_str());
    c = Counters{};
}

}

bool StageInstancesEnabled() {
    static const bool enabled = std::getenv("APS5_SHAPE_INSTANCES") != nullptr || std::getenv("APS5_VERIFY_SHAPE_INSTANCES") != nullptr;
    return enabled;
}

bool StageInstancesVerify() {
    static const bool verify = std::getenv("APS5_VERIFY_SHAPE_INSTANCES") != nullptr;
    return verify;
}

void StageInstancesBeginDraw(std::uint64_t shapeKey) {
    currentShape = shapeKey;
    auto& value = state();
    std::lock_guard lock(value.mutex);
    report(value);
}

std::array<std::uint32_t, 5> StageInstanceDrawWords(const ShaderRecompiler::RecompileRequest& request) {
    if (!request.graphics) return {};
    const auto& draw = request.graphics->draw;
    return {static_cast<std::uint32_t>(draw.indexAddress), static_cast<std::uint32_t>(draw.indexAddress >> 32u), draw.indexCount, draw.indexElementBytes, draw.instanceCount};
}

std::uint64_t StageInstanceKey(const void* shader, std::uint64_t codeAddress, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request) {
    if (request.graphics && (request.graphics->mesh || request.graphics->tessellation)) return 0;
    std::uint64_t hash = 0xcbf29ce484222325ull;
    if (request.graphics) mix(hash, request.graphics->firstUserSgpr);
    mix(hash, reinterpret_cast<std::uintptr_t>(shader));
    mix(hash, codeAddress);
    mix(hash, codeOffset);
    mix(hash, currentShape);
    mix(hash, request.layout.pushConstantOffsetBytes);
    mix(hash, request.layout.pushConstantSizeBytes);
    mix(hash, request.context.waveSize);
    mix(hash, request.context.userData.size());
    if (request.context.pixel) {
        const auto& pixel = *request.context.pixel;
        for (const auto mapping : pixel.targetExportMapping) mix(hash, mapping);
        for (const auto packing : pixel.targetExportPacking) mix(hash, static_cast<std::uint64_t>(packing));
        mix(hash, pixel.dualSourceBlend);
        mix(hash, pixel.reversedBlend);
        mix(hash, static_cast<std::uint64_t>(pixel.rasterPrimitive));
        mix(hash, pixel.layerExported);
    } else {
        mix(hash, 1ull << 40u);
    }
    if (request.context.vertex) {
        const auto& vertex = *request.context.vertex;
        const auto count = std::min(vertex.resourcesNum, ShaderRecompiler::ShaderVertexStageInfo::MaxResources);
        mix(hash, count);
        mix(hash, vertex.fetchAttribReg);
        mix(hash, vertex.fetchBufferReg);
        mix(hash, vertex.fetchEmbedded);
        mix(hash, vertex.paClVsOutCntl);
        mix(hash, vertex.depthRemapMode);
        for (std::uint32_t r = 0; r < count; ++r) {
            mix(hash, vertex.resources[r].fields[3]);
            mix(hash, vertex.resources[r].fields[1] >> 16u);
            const auto& destination = vertex.resourcesDst[r];
            mix(hash, static_cast<std::uint32_t>(destination.registerStart) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(destination.registersNum)) << 32u));
            mix(hash, static_cast<std::uint32_t>(destination.attrId) | (static_cast<std::uint64_t>(destination.fetchIndex) << 32u));
        }
    } else {
        mix(hash, 1ull << 41u);
    }
    return hash != 0 ? hash : 1;
}

std::optional<StageInstanceHit> StageInstanceTry(std::uint64_t key, std::span<const std::uint32_t> userData, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const std::array<std::uint32_t, 5>& draw, ShaderMemory& memory) {
    if (key == 0) return std::nullopt;
    const auto started = std::chrono::steady_clock::now();
    auto& value = state();
    std::shared_ptr<const Entry> held;
    {
        std::lock_guard lock(value.mutex);
        ++value.counters.lookups;
        const auto found = value.entries.find(key);
        if (found != value.entries.end()) held = found->second;
    }
    const auto missed = [&](Miss reason) -> std::optional<StageInstanceHit> {
        std::lock_guard lock(value.mutex);
        ++value.counters.misses[reason];
        return std::nullopt;
    };
    if (held == nullptr) return missed(MissNoEntry);
    const auto& entry = *held;
    if (entry.blocked) return missed(MissBlocked);
    if (!entry.learned) return missed(MissNotLearned);
    if (!entry.ready) return missed(MissNotReady);
    if (entry.userData.size() != userData.size()) return missed(MissUser);
    for (std::size_t u = 0; u < userData.size(); ++u) {
        if (userData[u] != entry.userData[u] && (!entry.userObserved[u] || entry.userPinned[u])) return missed(MissUser);
    }
    for (const auto& run : entry.runs) {
        if (run.origin < 0) continue;
        const auto now = userData[static_cast<std::size_t>(run.origin)];
        const auto was = entry.userData[static_cast<std::size_t>(run.origin)];
        if (now == 0 || ((now ^ was) & 3u) != 0) return missed(MissPointerFlags);
    }
    if (entry.vertex.has_value() != vertex.has_value() || vertexWords(entry.vertex) != vertexWords(vertex)) return missed(MissVertex);
    for (std::size_t v = 0; v < vertexWords(vertex); ++v) {
        const auto is = vertexWord(*vertex, static_cast<std::uint32_t>(v));
        const auto was = vertexWord(*entry.vertex, static_cast<std::uint32_t>(v));
        if (is != was && (v % 4u == 3u || !entry.vertexObserved[v])) return missed(MissVertex);
    }
    for (std::size_t d = 0; d < DrawWords; ++d) {
        if (draw[d] != entry.draw[d] && !entry.drawObserved[d]) return missed(MissDraw);
    }
    thread_local std::vector<std::uint32_t> scratch;
    scratch.assign(entry.words.size(), 0u);
    auto& words = scratch;
    for (const auto& run : entry.runs) {
        const auto address = run.origin >= 0 ? static_cast<std::uint64_t>(aligned(userData[static_cast<std::size_t>(run.origin)])) + run.at : run.at;
        if (!memory.ReadWords(address, std::span(words).subspan(run.first, run.words))) return missed(MissRead);
    }
    for (std::size_t w = 0; w < words.size(); ++w) {
        if (words[w] != entry.words[w] && !entry.wordObserved[w]) return missed(MissWord);
    }
    bool changed = false;
    for (const auto& patch : entry.patches) {
        if (patch.kind == FieldKind::Address) {
            if (addressValue(patch.source, userData, words, vertex, draw) != addressOf(entry.result->bindings[patch.binding], patch.word)) changed = true;
            continue;
        }
        if (patch.source.delta != 0 && overflows(sourceBase(patch.source, userData, words, vertex, draw), patch.source.delta) != patch.overflow) return missed(MissCarry);
        if (sourceValue(patch.source, userData, words, vertex, draw) != fieldValue(*entry.result, patch)) changed = true;
    }
    for (const auto& attribute : entry.result->vertexAttributes) {
        if (!vertex.has_value() || attribute.location >= vertex->resources.size()) continue;
        if (vertex->resources[attribute.location].fields != attribute.resource.fields) changed = true;
        if (vertex->fetchEmbedded && vertex->resourcesDst[attribute.location].fetchIndex != attribute.fetchIndex) changed = true;
    }
    StageInstanceHit hit{entry.result, entry.use};
    if (changed) {
        auto result = std::make_shared<ShaderRecompiler::RecompileResult>(*entry.result);
        for (const auto& patch : entry.patches) {
            if (patch.kind == FieldKind::Address) {
                const auto address = addressValue(patch.source, userData, words, vertex, draw);
                auto* quad = result->bindings[patch.binding].guestDescriptor.data() + static_cast<std::size_t>(patch.word) * 4u;
                quad[0] = static_cast<std::uint32_t>(address);
                quad[1] = (quad[1] & 0xffff0000u) | static_cast<std::uint32_t>(address >> 32u);
                continue;
            }
            const auto patched = sourceValue(patch.source, userData, words, vertex, draw);
            if (patch.kind == FieldKind::Binding) result->bindings[patch.binding].guestDescriptor[patch.word] = patched;
            else std::memcpy(result->pushConstants.data() + patch.word * sizeof(std::uint32_t), &patched, sizeof(patched));
        }
        for (auto& attribute : result->vertexAttributes) {
            if (!vertex.has_value() || attribute.location >= vertex->resources.size()) continue;
            attribute.resource = vertex->resources[attribute.location];
            if (vertex->fetchEmbedded) attribute.fetchIndex = vertex->resourcesDst[attribute.location].fetchIndex;
        }
        hit.compiled = std::move(result);
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
    std::lock_guard lock(value.mutex);
    auto& c = value.counters;
    if (changed) c.patchedWords += entry.patches.size();
    else ++c.shared;
    ++c.hits;
    ++c.fastTimed;
    c.fastUs += elapsed;
    return hit;
}

void StageInstanceLearn(std::uint64_t key, const std::shared_ptr<const void>& shader, std::span<const std::uint32_t> userData, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const std::array<std::uint32_t, 5>& draw, const ShaderRecompiler::ResourceCapture& capture, std::span<const ShaderRecompiler::MemoryRegion> regions, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiled) {
    if (compiled == nullptr || key == 0) return;
    std::vector<std::uint32_t> words;
    auto runs = relativeRuns(userData, capture.userDataUse, regions, words);
    auto& value = state();
    std::shared_ptr<const Entry> previous;
    {
        std::lock_guard lock(value.mutex);
        const auto found = value.entries.find(key);
        if (found != value.entries.end()) previous = found->second;
    }
    auto entry = std::make_shared<Entry>();
    std::optional<LearnFail> failure;
    const bool comparable = previous != nullptr && previous->result != nullptr && previous->shader == shader;
    if (comparable) {
        *entry = *previous;
        failure = learn(*entry, runs, words, userData, vertex, draw, *compiled);
        entry->learned = !failure.has_value();
        entry->stable = failure ? 0u : previous->stable + 1u;
        if (failure) entry->patches.clear();
    }
    const auto& snapshot = capture.snapshot;
    entry->blocked = !snapshot.tables.tables.empty() || !snapshot.tables.ranges.empty() || !snapshot.srtPoison.empty() || snapshot.nullRootReads != 0;
    store(*entry, comparable && !failure.has_value(), shader, std::move(runs), std::move(words), userData, vertex, draw, capture.userDataUse, compiled);
    std::lock_guard lock(value.mutex);
    if (comparable) {
        if (failure) ++value.counters.failed[*failure];
        else ++value.counters.learned;
    }
    value.entries[key] = std::move(entry);
}

void StageInstanceCompare(std::uint64_t key, std::uint64_t codeAddress, const ShaderRecompiler::RecompileResult& fast, std::span<const ShaderRecompiler::MemoryRegion> fastRegions, const ShaderRecompiler::RecompileResult& full, std::span<const ShaderRecompiler::MemoryRegion> fullRegions) {
    auto& value = state();
    std::shared_ptr<const Entry> held;
    {
        std::lock_guard lock(value.mutex);
        const auto found = value.entries.find(key);
        if (found != value.entries.end()) held = found->second;
    }
    const auto patchFor = [&](FieldKind kind, std::uint32_t binding, std::uint32_t word) -> const Patch* {
        if (held == nullptr) return nullptr;
        for (const auto& patch : held->patches) {
            if (patch.kind == kind && patch.binding == binding && patch.word == word) return &patch;
        }
        return nullptr;
    };
    enum class Kind { Same, Shape, Patched, Unpatched, Attribute };
    Kind kind = Kind::Same;
    char detail[240] = "";
    if (fast.PipelineVariantId() != full.PipelineVariantId() || fast.bindings.size() != full.bindings.size() || fast.pushConstants.size() != full.pushConstants.size() || fast.vertexAttributes.size() != full.vertexAttributes.size()) {
        kind = Kind::Shape;
        std::snprintf(detail, sizeof(detail), "shape: variant 0x%llx / 0x%llx, bindings %zu / %zu, push %zu / %zu", static_cast<unsigned long long>(fast.PipelineVariantId()), static_cast<unsigned long long>(full.PipelineVariantId()), fast.bindings.size(), full.bindings.size(), fast.pushConstants.size(), full.pushConstants.size());
    }
    for (std::uint32_t b = 0; kind == Kind::Same && b < full.bindings.size(); ++b) {
        const auto& x = fast.bindings[b];
        const auto& y = full.bindings[b];
        if (!sameBindingShape(x, y)) {
            kind = Kind::Shape;
            std::snprintf(detail, sizeof(detail), "binding %u shape (role %u)", b, static_cast<unsigned>(y.role));
            break;
        }
        for (std::uint32_t w = 0; w < y.guestDescriptor.size(); ++w) {
            if (x.guestDescriptor[w] == y.guestDescriptor[w]) continue;
            const auto* patch = patchFor(FieldKind::Binding, b, w);
            if (patch == nullptr && y.role == ShaderRecompiler::DescriptorRole::GuestBuffers && w % 4u < 2u) patch = patchFor(FieldKind::Address, b, w / 4u);
            kind = patch != nullptr ? Kind::Patched : Kind::Unpatched;
            std::snprintf(detail, sizeof(detail), "binding %u (role %u, count %u) word %u: fast 0x%08x full 0x%08x, %s", b, static_cast<unsigned>(y.role), y.count, w, x.guestDescriptor[w], y.guestDescriptor[w], patch == nullptr ? "not patched" : patch->source.kind == SourceKind::User ? "patched from a user word" : patch->source.kind == SourceKind::Word ? "patched from a read word" : patch->source.kind == SourceKind::Draw ? "patched from a draw parameter" : "patched from a vertex word");
            break;
        }
    }
    const auto pushWords = full.pushConstants.size() / sizeof(std::uint32_t);
    for (std::uint32_t d = 0; kind == Kind::Same && d < pushWords; ++d) {
        if (pushWord(fast, d) == pushWord(full, d)) continue;
        const auto* patch = patchFor(FieldKind::Push, 0, d);
        kind = patch != nullptr ? Kind::Patched : Kind::Unpatched;
        std::snprintf(detail, sizeof(detail), "push word %u: fast 0x%08x full 0x%08x, %s", d, pushWord(fast, d), pushWord(full, d), patch == nullptr ? "not patched" : "patched");
    }
    if (kind == Kind::Same && fast.pushConstants != full.pushConstants) {
        kind = Kind::Unpatched;
        std::snprintf(detail, sizeof(detail), "push tail bytes");
    }
    for (std::size_t a = 0; kind == Kind::Same && a < full.vertexAttributes.size(); ++a) {
        const auto& x = fast.vertexAttributes[a];
        const auto& y = full.vertexAttributes[a];
        if (x.location != y.location || x.components != y.components || x.resource.fields != y.resource.fields || x.fetchIndex != y.fetchIndex || x.formatComponents != y.formatComponents) {
            kind = Kind::Attribute;
            std::snprintf(detail, sizeof(detail), "vertex attribute %zu (location %u)", a, y.location);
        }
    }
    const auto flatten = [](std::span<const ShaderRecompiler::MemoryRegion> regions) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> flat;
        for (const auto& region : regions) {
            for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= region.bytes.size(); offset += sizeof(std::uint32_t)) {
                std::uint32_t word = 0;
                std::memcpy(&word, region.bytes.data() + offset, sizeof(word));
                flat.emplace_back(region.guestAddress + offset, word);
            }
        }
        std::sort(flat.begin(), flat.end());
        flat.erase(std::unique(flat.begin(), flat.end()), flat.end());
        return flat;
    };
    const bool sameRegions = flatten(fastRegions) == flatten(fullRegions);
    {
        std::lock_guard lock(value.mutex);
        auto& c = value.counters;
        ++c.compared;
        if (kind != Kind::Same) ++c.mismatched;
        if (kind == Kind::Patched) ++c.mismatchedPatched;
        if (kind == Kind::Unpatched || kind == Kind::Attribute) ++c.mismatchedUnpatched;
        if (kind == Kind::Shape) ++c.mismatchedShape;
        if (!sameRegions) ++c.regionMismatches;
    }
    static std::atomic<int> shownPatched{0};
    static std::atomic<int> shownOther{0};
    auto& shown = kind == Kind::Patched ? shownPatched : shownOther;
    if ((kind != Kind::Same || !sameRegions) && shown.fetch_add(1) < 25) std::fprintf(stderr, "[stage-instances] verify: program 0x%llx: %s%s (entry stable %u, %zu patches)\n", static_cast<unsigned long long>(codeAddress), kind == Kind::Same ? "result equal" : detail, sameRegions ? "" : "; regions differ", held != nullptr ? held->stable : 0u, held != nullptr ? held->patches.size() : 0u);
}

void StageInstanceTimed(double fastMicroseconds, double fullMicroseconds) {
    auto& value = state();
    std::lock_guard lock(value.mutex);
    if (fullMicroseconds > 0.0) {
        ++value.counters.fullTimed;
        value.counters.fullUs += fullMicroseconds;
    }
    static_cast<void>(fastMicroseconds);
}

}
