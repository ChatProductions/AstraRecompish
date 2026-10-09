#include "ps2vita/gif.hpp"
#include "ps2vita/gif_trace.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

// Owned diagnostic/codec fixtures. Rendering checks describe Astra's logical
// linear VRAM model; they are not native PS2 rasterization goldens.
namespace {

unsigned checks = 0, failures = 0;

void check(bool condition, const std::string& name) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", name.c_str());
  }
}

struct Packet {
  std::vector<std::uint8_t> bytes;
  void word(std::uint64_t value) {
    for (unsigned byte = 0; byte < 8u; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
  void ad(std::uint64_t value, unsigned reg) {
    word(0x1000000000008001ull); word(0xEull);
    word(value); word(reg);
  }
};

std::uint64_t xyz(unsigned x, unsigned y, unsigned z = 7u) {
  return (0x8000u + x * 64u) |
      (std::uint64_t{0x8000u + y * 64u} << 16u) |
      (std::uint64_t{z} << 32u);
}

const std::array<std::uint32_t, 3> kColors{
    0x20332211u, 0x40554433u, 0x80665544u};
const std::array<std::uint64_t, 3> kSt{
    0x3E8000003E000000ull, 0x3F0000003E800000ull,
    0x3F4000003F000000ull};
const std::array<std::uint64_t, 3> kUv{
    0x0000000000110009ull, 0x0000000000330021ull,
    0x0000000000550043ull};
const std::array<std::uint32_t, 3> kQ{
    0x3F000000u, 0x3F800000u, 0x40000000u};
const std::array<std::uint64_t, 3> kXyz{
    xyz(2, 2, 11u), xyz(10, 2, 22u), xyz(2, 10, 33u)};

void setup(Packet& packet) {
  packet.ad(10ull << 16u, 0x4Cu); // FRAME0, 640 native pixels, C32.
  packet.ad(0x800000008000ull, 0x18u);
  packet.ad(127ull << 16u | (127ull << 48u), 0x40u);
  packet.ad(0u, 0x47u); packet.ad(1ull << 32u, 0x4Eu);
  packet.ad(0x123456789ABCDEF0ull, 0x06u);
  packet.ad(0x105u, 0x08u); packet.ad(0x4000000044ull, 0x42u);
}

void triangle(Packet& packet) {
  packet.ad(3u, 0x00u); // Flat shading must not rewrite observer attributes.
  for (unsigned vertex = 0; vertex < 3u; ++vertex) {
    packet.ad((std::uint64_t{kQ[vertex]} << 32u) | kColors[vertex], 0x01u);
    packet.ad(kSt[vertex], 0x02u); packet.ad(kUv[vertex], 0x03u);
    packet.ad(kXyz[vertex], 0x05u);
  }
}

Packet owned_scene(unsigned triangles = 1u) {
  Packet packet;
  setup(packet);
  for (unsigned draw = 0; draw < triangles; ++draw) triangle(packet);
  packet.ad(0u, 0x00u); packet.ad(xyz(12, 3), 0x05u); // Point.
  packet.ad(1u, 0x00u); packet.ad(xyz(12, 4), 0x05u);
  packet.ad(xyz(16, 4), 0x05u); // Line.
  packet.ad(6u, 0x00u); packet.ad(xyz(12, 6), 0x05u);
  packet.ad(xyz(16, 9), 0x05u); // Sprite.
  packet.ad((0x2000ull << 32u) | (1ull << 48u), 0x50u);
  packet.ad(0u, 0x51u); packet.ad(2u | (2ull << 32u), 0x52u);
  packet.ad(0u, 0x53u);
  packet.word(0x0800000000008001ull); packet.word(0u);
  packet.word(0x1122334455667788ull); packet.word(0xAABBCCDDEEFF0011ull);
  // An owned REGLIST NOP tag exercises a second parser mode.
  packet.word(0x1400000000008001ull); packet.word(0xFull);
  packet.word(0x0123456789ABCDEFull); packet.word(0u);
  return packet;
}

std::array<std::uint64_t, 17> counters(const ps2vita::Gif& gif) {
  return {gif.packets_submitted(), gif.packets_rejected(), gif.sprites_emitted(),
      gif.points_emitted(), gif.lines_emitted(), gif.triangles_emitted(),
      gif.packed_tags(), gif.reglist_tags(), gif.image_tags(), gif.image_bytes(),
      gif.pending_bytes(), gif.local_bytes_written(), gif.first_unsupported_tag(),
      gif.first_image_bitbltbuf(), gif.first_image_trxpos(),
      gif.first_image_trxreg(), gif.first_image_trxdir()};
}

bool same_vram(const ps2vita::Gif& a, const ps2vita::Gif& b) {
  for (std::uint32_t at = 0u; at < 0x400000u; at += 4u)
    if (a.read_local32(at) != b.read_local32(at)) return false;
  return true;
}

bool same_preview(const ps2vita::Gs& a, const ps2vita::Gs& b) {
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x)
      if (a.pixel(x, y) != b.pixel(x, y)) return false;
  return true;
}

void test_observers() {
  ps2vita::Gs baseline_gs, observed_gs;
  ps2vita::Gif baseline(baseline_gs), observed(observed_gs);
  const auto packet = owned_scene(70u);
  std::vector<std::vector<std::uint8_t>> fragments;
  std::vector<std::uint64_t> ordinals, triangles_before;
  std::vector<std::size_t> pending_before;
  std::vector<ps2vita::GifTriangleRecord> triangles;
  observed.set_submission_observer([&](std::uint64_t ordinal,
      const std::uint8_t* data, std::size_t size) {
    ordinals.push_back(ordinal);
    fragments.emplace_back();
    if (size != 0u) fragments.back().assign(data, data + size);
    triangles_before.push_back(observed.triangles_emitted());
    pending_before.push_back(observed.pending_bytes());
  });
  observed.set_triangle_observer([&](const ps2vita::GifTriangleRecord& record) {
    triangles.push_back(record);
  });
  check(baseline.submit(nullptr, 0u) && observed.submit(nullptr, 0u) &&
        fragments.size() == 1u && fragments.back().empty(),
        "empty null submission is observed without reading payload bytes");

  const std::array<std::size_t, 8> pieces{1u, 7u, 8u, 3u, 13u, 31u, 32u, 4096u};
  std::size_t cursor = 0u, piece = 0u;
  bool accepted = true, raw_equal = true, before_equal = true;
  while (cursor < packet.bytes.size()) {
    const auto size = std::min(pieces[piece++ % pieces.size()],
                               packet.bytes.size() - cursor);
    const auto prior_triangles = observed.triangles_emitted();
    const auto prior_pending = observed.pending_bytes();
    accepted &= baseline.submit(packet.bytes.data() + cursor, size);
    accepted &= observed.submit(packet.bytes.data() + cursor, size);
    raw_equal &= fragments.back().size() == size &&
        std::equal(fragments.back().begin(), fragments.back().end(),
                   packet.bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
    before_equal &= triangles_before.back() == prior_triangles &&
                    pending_before.back() == prior_pending;
    cursor += size;
  }
  check(accepted && raw_equal, "raw submission bytes preserve every fragment boundary");
  check(before_equal, "submission observer runs before buffering/parsing its fragment");
  bool consecutive = ordinals.size() == observed.packets_submitted();
  for (std::size_t i = 0; i < ordinals.size(); ++i) consecutive &= ordinals[i] == i + 1u;
  check(consecutive, "submission ordinals start at one and count calls, not GIFtags");
  check(triangles.size() == 70u && observed.triangle_records().empty(),
        "triangle observer is not gated by optional bounded prefix tracing");
  bool original = triangles.size() == 70u;
  for (std::size_t i = 0; i < triangles.size(); ++i) {
    const auto& record = triangles[i];
    original &= record.sequence == i && record.prim == 3u &&
        record.xyz == kXyz && record.xyoffset == 0x800000008000ull &&
        record.frame == (10ull << 16u) && record.tex0 == 0x123456789ABCDEF0ull &&
        record.clamp == 0x105u && record.alpha == 0x4000000044ull &&
        record.test == 0u && record.zbuf == (1ull << 32u) &&
        record.scissor == (127ull << 16u | (127ull << 48u));
    for (unsigned vertex = 0; vertex < 3u; ++vertex)
      original &= record.vertices[vertex].color == kColors[vertex] &&
          record.vertices[vertex].st == kSt[vertex] &&
          record.vertices[vertex].uv == kUv[vertex] &&
          record.vertices[vertex].q == kQ[vertex] &&
          record.vertices[vertex].z == (vertex + 1u) * 11u;
  }
  check(original, "triangle observer retains original latched raw XYZ/color/ST/UV/Q/state");
  check(counters(baseline) == counters(observed), "observers leave every parser/raster counter unchanged");
  check(same_vram(baseline, observed), "observer on/off produces identical complete 4MiB VRAM");
  check(same_preview(baseline_gs, observed_gs), "observer on/off produces identical complete preview");
  check(baseline.image_records().size() == 1u && observed.image_records().size() == 1u &&
        baseline.image_records()[0].hash == observed.image_records()[0].hash &&
        baseline.image_records()[0].bytes == observed.image_records()[0].bytes &&
        baseline.image_records()[0].first_qword == observed.image_records()[0].first_qword,
        "observers preserve IMAGE provenance hash/size/first qword");
  check(observed.pending_bytes() == 0u && observed.image_tags() == 1u &&
        observed.reglist_tags() == 1u && observed.sprites_emitted() == 1u &&
        observed.points_emitted() == 1u && observed.lines_emitted() == 1u,
        "owned scene exercises IMAGE, REGLIST, triangle, sprite, line and point");

  const auto old_submissions = ordinals.size(), old_triangles = triangles.size();
  baseline.reset();
  observed.reset();
  const auto small = owned_scene();
  check(baseline.submit(small.bytes.data(), small.bytes.size()) &&
        observed.submit(small.bytes.data(), small.bytes.size()),
        "reset observed/baseline parsers accept owned scene");
  check(ordinals.size() == old_submissions + 1u && ordinals.back() == 1u &&
        triangles.size() == old_triangles + 1u && triangles.back().sequence == 0u,
        "reset preserves listeners while restarting submission/triangle sequences");
  check(counters(baseline) == counters(observed) && same_vram(baseline, observed) &&
        same_preview(baseline_gs, observed_gs),
        "reset listeners retain full state/counter noninterference");
  observed.set_submission_observer({}); observed.set_triangle_observer({});
  check(observed.submit(small.bytes.data(), small.bytes.size()), "disabled observers keep parser usable");
  check(ordinals.size() == old_submissions + 1u && triangles.size() == old_triangles + 1u,
        "empty callbacks disable both observer streams");
}

const std::array<std::uint8_t, 3> kTracePayloadA{0x00u, 0xA5u, 0xFFu};
const std::array<std::uint8_t, 16> kTracePayloadB{
    0x10u, 0x21u, 0x32u, 0x43u, 0x54u, 0x65u, 0x76u, 0x87u,
    0x98u, 0xA9u, 0xBAu, 0xCBu, 0xDCu, 0xEDu, 0xFEu, 0x0Fu};

std::string valid_trace() {
  std::ostringstream output(std::ios::binary);
  ps2vita::GifTraceWriter writer(output);
  const bool valid = writer.write(1u, 7u, 0xBFC00004u, nullptr, 0u) &&
      writer.write(2u, 7u, 0x80001234u, kTracePayloadA.data(), kTracePayloadA.size()) &&
      writer.write(3u, 19u, 0x00001008u, kTracePayloadB.data(), kTracePayloadB.size()) &&
      writer.finish(23u, 0x80001238u);
  check(valid && writer.good(), "writer emits complete multi-record trace");
  check(writer.record_count() == 3u && writer.payload_bytes() == 19u,
        "writer exposes exact record and payload counts");
  return output.str();
}

void put_le(std::string& bytes, std::size_t offset, std::uint64_t value, unsigned size) {
  for (unsigned i = 0; i < size; ++i)
    bytes[offset + i] = static_cast<char>(value >> (i * 8u));
}

std::uint64_t get_le(const std::string& bytes, std::size_t offset, unsigned size) {
  if (offset > bytes.size() || size > bytes.size() - offset)
    return std::numeric_limits<std::uint64_t>::max();
  std::uint64_t value = 0u;
  for (unsigned i = 0; i < size; ++i)
    value |= std::uint64_t{static_cast<unsigned char>(bytes[offset + i])} << (i * 8u);
  return value;
}

std::string raw_header(std::uint64_t ordinal, std::uint64_t cycle,
                       std::uint32_t pc, std::uint32_t size, std::uint64_t flag = 0u) {
  std::string bytes(32u, '\0');
  put_le(bytes, 0u, ordinal, 8u); put_le(bytes, 8u, cycle, 8u);
  put_le(bytes, 16u, pc, 4u); put_le(bytes, 20u, size, 4u);
  put_le(bytes, 24u, flag, 8u);
  return bytes;
}

void test_wire_format() {
  const auto bytes = valid_trace();
  // Explicit independent offsets make endianness and checksum coverage part of
  // the regression contract, rather than asking codec helpers to check themselves.
  constexpr std::size_t end = 123u, footer = 155u;
  check(bytes.size() == 179u && bytes.substr(0u, 8u) == "ASTRAGF1" &&
        get_le(bytes, 8u, 8u) == 1u && get_le(bytes, 16u, 8u) == 7u &&
        get_le(bytes, 24u, 4u) == 0xBFC00004u && get_le(bytes, 28u, 4u) == 0u &&
        get_le(bytes, 32u, 8u) == 0u &&
        get_le(bytes, 40u, 8u) == 2u && get_le(bytes, 60u, 4u) == 3u &&
        get_le(bytes, 75u, 8u) == 3u && get_le(bytes, 95u, 4u) == 16u,
        "wire header/record fields use documented widths, offsets and little endian");
  check(get_le(bytes, end, 8u) == 0u && get_le(bytes, end + 8u, 8u) == 23u &&
        get_le(bytes, end + 16u, 4u) == 0x80001238u &&
        get_le(bytes, end + 20u, 4u) == 0u && get_le(bytes, end + 24u, 8u) == 1u &&
        get_le(bytes, footer, 8u) == 3u && get_le(bytes, footer + 8u, 8u) == 19u,
        "end marker and footer have independent documented fields");
  std::uint64_t hash = 1469598103934665603ull;
  for (std::size_t i = 8u; i < footer; ++i) {
    hash ^= static_cast<unsigned char>(bytes[i]);
    hash *= 1099511628211ull;
  }
  check(get_le(bytes, footer + 16u, 8u) == hash,
        "rolling checksum independently covers every record and the terminal header");
}

void test_roundtrip() {
  const auto bytes = valid_trace();
  std::istringstream input(bytes, std::ios::binary);
  ps2vita::GifTraceReader reader(input);
  ps2vita::GifTraceRecord record;
  check(reader.good() && !reader.finished(), "valid header opens an unfinished reader");
  check(reader.next(record) && record.ordinal == 1u && record.ee_cycle == 7u &&
        record.next_ee_pc == 0xBFC00004u && record.payload.empty(),
        "roundtrip preserves empty payload, ordinal, cycle and high PC bits");
  check(reader.next(record) && record.ordinal == 2u && record.ee_cycle == 7u &&
        record.next_ee_pc == 0x80001234u &&
        record.payload == std::vector<std::uint8_t>(kTracePayloadA.begin(), kTracePayloadA.end()),
        "roundtrip preserves unaligned binary payload and same-cycle submission");
  check(reader.next(record) && record.ordinal == 3u && record.ee_cycle == 19u &&
        record.next_ee_pc == 0x00001008u &&
        record.payload == std::vector<std::uint8_t>(kTracePayloadB.begin(), kTracePayloadB.end()),
        "roundtrip preserves later submission and all binary byte values");
  check(!reader.next(record) && reader.good() && reader.finished() &&
        reader.record_count() == 3u && reader.payload_bytes() == 19u &&
        reader.terminal_cycle() == 23u && reader.terminal_pc() == 0x80001238u,
        "validated footer preserves counts, terminal cycle and terminal PC");
  check(!reader.next(record) && reader.good() && reader.finished(),
        "reading after a validated footer remains cleanly finished");

  std::ostringstream empty_output(std::ios::binary);
  ps2vita::GifTraceWriter empty_writer(empty_output);
  check(empty_writer.finish(0u, 0u), "empty submission trace still has a valid footer");
  std::istringstream empty_input(empty_output.str(), std::ios::binary);
  ps2vita::GifTraceReader empty_reader(empty_input);
  check(!empty_reader.next(record) && empty_reader.finished() && empty_reader.good() &&
        empty_reader.record_count() == 0u && empty_reader.payload_bytes() == 0u &&
        empty_reader.terminal_cycle() == 0u && empty_reader.terminal_pc() == 0u,
        "zero-record trace roundtrips without inventing a submission");
}

bool rejects_trace(const std::string& bytes) {
  std::istringstream input(bytes, std::ios::binary);
  ps2vita::GifTraceReader reader(input);
  ps2vita::GifTraceRecord record;
  std::size_t iterations = 0u;
  // Fixtures have only three records. This independent guard prevents a broken
  // reader from turning malformed input into an unbounded test execution.
  while (reader.good()) {
    const auto previous = record;
    if (!reader.next(record)) {
      if (record.ordinal != previous.ordinal || record.ee_cycle != previous.ee_cycle ||
          record.next_ee_pc != previous.next_ee_pc || record.payload != previous.payload)
        return false;
      break;
    }
    if (++iterations > 4u) return false;
  }
  return !reader.good() && !reader.finished() && reader.error()[0] != '\0';
}

void test_truncation() {
  const auto bytes = valid_trace();
  bool all_rejected = true;
  std::size_t first_accepted = bytes.size();
  for (std::size_t size = 0u; size < bytes.size(); ++size) {
    if (!rejects_trace(bytes.substr(0u, size))) {
      all_rejected = false;
      first_accepted = std::min(first_accepted, size);
    }
  }
  check(all_rejected, "every strict prefix truncation rejects, including header/record/payload/footer cuts");
  if (!all_rejected)
    std::fprintf(stderr, "  first accepted truncation: %zu/%zu bytes\n", first_accepted, bytes.size());
  auto trailing = bytes;
  trailing.push_back('\0');
  check(rejects_trace(trailing), "data after footer is rejected, including a zero byte");
}

void test_corruption() {
  const auto original = valid_trace();
  if (original.size() != 179u) {
    check(false, "corruption fixture has the independently expected wire size");
    return;
  }
  struct Mutation {
    const char* name;
    std::size_t offset;
    std::uint64_t value;
    unsigned width;
  };
  const std::array<Mutation, 15> mutations{
      Mutation{"unsupported magic/version", 7u, '2', 1u},
      Mutation{"zero data ordinal", 8u, 0u, 8u},
      Mutation{"initial ordinal gap", 8u, 2u, 8u},
      Mutation{"reserved flag low bits", 32u, 2u, 8u},
      Mutation{"reserved flag high bits", 32u, 1ull << 63u, 8u},
      Mutation{"record-size cap", 28u, 16u * 1024u * 1024u + 1u, 4u},
      Mutation{"uint32 maximum declared size", 28u, 0xFFFFFFFFu, 4u},
      Mutation{"duplicate subsequent ordinal", 40u, 1u, 8u},
      Mutation{"decreasing record cycle", 48u, 6u, 8u},
      Mutation{"nonzero end ordinal", 123u, 1u, 8u},
      Mutation{"decreasing terminal cycle", 131u, 18u, 8u},
      Mutation{"nonzero end payload size", 143u, 1u, 4u},
      Mutation{"reserved end flag", 147u, 3u, 8u},
      Mutation{"footer record-count mismatch", 155u, 4u, 8u},
      Mutation{"footer total-payload mismatch", 163u, 20u, 8u}};
  for (const auto& mutation : mutations) {
    auto bytes = original;
    put_le(bytes, mutation.offset, mutation.value, mutation.width);
    check(rejects_trace(bytes), std::string{"reader rejects: "} + mutation.name);
  }
  bool every_byte_checked = true;
  for (std::size_t offset = 0; offset < original.size(); ++offset) {
    auto bytes = original;
    bytes[offset] = static_cast<char>(static_cast<unsigned char>(bytes[offset]) ^ 0x80u);
    every_byte_checked &= rejects_trace(bytes);
  }
  check(every_byte_checked,
        "single-bit corruption at every wire byte rejects, including payload/PC/footer hash");

  auto invalid = original;
  put_le(invalid, 28u, 0xFFFFFFFFu, 4u);
  std::istringstream input(invalid, std::ios::binary);
  ps2vita::GifTraceReader reader(input);
  ps2vita::GifTraceRecord record{73u, 91u, 0x12345678u, {0x12u, 0x34u}};
  check(!reader.next(record) && !reader.good() && record.ordinal == 73u &&
        record.ee_cycle == 91u && record.next_ee_pc == 0x12345678u &&
        record.payload == std::vector<std::uint8_t>{0x12u, 0x34u},
        "failed reader next leaves caller record unchanged");
  check(!reader.next(record) && !reader.good() && record.ordinal == 73u &&
        record.payload == std::vector<std::uint8_t>{0x12u, 0x34u},
        "reader failure stays latched and never resumes into remaining bytes");
}

class CountingSink final : public std::streambuf {
public:
  std::uint64_t bytes = 0u;
protected:
  std::streamsize xsputn(const char*, std::streamsize size) override {
    bytes += static_cast<std::uint64_t>(size);
    return size;
  }
  int_type overflow(int_type ch) override {
    if (!traits_type::eq_int_type(ch, traits_type::eof())) ++bytes;
    return traits_type::not_eof(ch);
  }
};

// A tiny generated source describes large *valid preceding* zero payloads
// without allocating a 64MiB fixture or retaining copies of decoded records.
class GeneratedSource final : public std::streambuf {
public:
  struct Part { std::string bytes; std::size_t zeros = 0u; };
  std::vector<Part> parts;
protected:
  std::streamsize xsgetn(char* destination, std::streamsize requested) override {
    std::streamsize copied = 0;
    while (requested > 0 && part_ < parts.size()) {
      const auto& part = parts[part_];
      const auto length = part.zeros != 0u ? part.zeros : part.bytes.size();
      if (offset_ == length) { ++part_; offset_ = 0u; continue; }
      const auto size = std::min<std::size_t>(requested, length - offset_);
      if (part.zeros != 0u) std::memset(destination, 0, size);
      else std::memcpy(destination, part.bytes.data() + offset_, size);
      offset_ += size; destination += size;
      requested -= static_cast<std::streamsize>(size);
      copied += static_cast<std::streamsize>(size);
    }
    return copied;
  }
private:
  std::size_t part_ = 0u, offset_ = 0u;
};

void test_capacities() {
  constexpr std::uint32_t record_size = 16u * 1024u * 1024u;
  {
    // One reusable 16MiB payload, and a sink which retains no wire bytes.
    std::vector<std::uint8_t> payload(record_size, 0u);
    CountingSink sink;
    std::ostream output(&sink);
    ps2vita::GifTraceWriter writer(output);
    bool accepted = true;
    for (unsigned i = 1; i <= 4u; ++i)
      accepted &= writer.write(i, i, i, payload.data(), payload.size());
    check(accepted && writer.payload_bytes() == 64u * 1024u * 1024u,
          "writer accepts exactly the total payload cap with reusable bounded storage");
    const auto prior_bytes = sink.bytes;
    check(!writer.write(5u, 5u, 5u, payload.data(), 1u) &&
          sink.bytes == prior_bytes && writer.record_count() == 4u &&
          writer.payload_bytes() == 64u * 1024u * 1024u,
          "writer rejects cumulative payload overflow transactionally");
  }
  {
    GeneratedSource source;
    source.parts.push_back({"ASTRAGF1", 0u});
    for (unsigned i = 1; i <= 4u; ++i) {
      source.parts.push_back({raw_header(i, i, i, record_size), 0u});
      source.parts.push_back({{}, record_size});
    }
    source.parts.push_back({raw_header(5u, 5u, 5u, 1u), 0u});
    std::istream input(&source);
    ps2vita::GifTraceReader reader(input);
    ps2vita::GifTraceRecord record;
    bool accepted = true;
    for (unsigned i = 1; i <= 4u; ++i) {
      accepted &= reader.next(record) && record.ordinal == i &&
                  record.payload.size() == record_size;
      std::vector<std::uint8_t>{}.swap(record.payload); // At most one 16MiB record retained.
    }
    check(accepted && reader.payload_bytes() == 64u * 1024u * 1024u,
          "reader accepts the exact cumulative limit without a large fixture allocation");
    check(!reader.next(record) && !reader.good() && reader.record_count() == 4u &&
          reader.payload_bytes() == 64u * 1024u * 1024u && record.ordinal == 4u &&
          record.payload.empty(),
          "reader rejects cumulative overflow header before allocating/reading its payload");
  }
  {
    CountingSink sink;
    std::ostream output(&sink);
    ps2vita::GifTraceWriter writer(output);
    std::string raw = "ASTRAGF1";
    raw.reserve(8u + 100001u * 32u);
    bool accepted = true;
    for (unsigned i = 1; i <= 100000u; ++i) {
      accepted &= writer.write(i, i, i, nullptr, 0u);
      raw += raw_header(i, i, i, 0u);
    }
    const auto prior = sink.bytes;
    check(accepted && writer.record_count() == 100000u &&
          !writer.write(100001u, 100001u, 0u, nullptr, 0u) &&
          sink.bytes == prior && writer.record_count() == 100000u,
          "writer enforces exact record-count cap transactionally for empty payloads");
    raw += raw_header(100001u, 100001u, 0u, 0u);
    std::istringstream input(raw, std::ios::binary);
    ps2vita::GifTraceReader reader(input);
    ps2vita::GifTraceRecord record;
    accepted = true;
    for (unsigned i = 1; i <= 100000u; ++i)
      accepted &= reader.next(record) && record.ordinal == i && record.payload.empty();
    check(accepted && !reader.next(record) && !reader.good() &&
          reader.record_count() == 100000u && record.ordinal == 100000u,
          "reader enforces record-count cap without payload allocations");
  }
}

void test_writer_rejections() {
  const std::uint8_t byte = 0x5Au;
  struct BadWrite {
    const char* name;
    std::uint64_t ordinal;
    const std::uint8_t* data;
    std::size_t size;
  };
  const std::array<BadWrite, 5> bad{
      BadWrite{"zero initial ordinal", 0u, &byte, 1u},
      BadWrite{"skipped initial ordinal", 2u, &byte, 1u},
      BadWrite{"null nonempty payload", 1u, nullptr, 1u},
      BadWrite{"oversized record declaration", 1u, &byte, 16u * 1024u * 1024u + 1u},
      BadWrite{"size_t overflow declaration", 1u, &byte, std::numeric_limits<std::size_t>::max()}};
  for (const auto& item : bad) {
    std::ostringstream output(std::ios::binary);
    ps2vita::GifTraceWriter writer(output);
    const auto prior = output.str();
    check(!writer.write(item.ordinal, 0u, 0u, item.data, item.size) &&
          output.str() == prior && writer.record_count() == 0u &&
          writer.payload_bytes() == 0u,
          std::string{"writer rejects transactionally: "} + item.name);
    check(!writer.good() && writer.error()[0] != '\0' &&
          !writer.write(1u, 0u, 0u, &byte, 1u) && output.str() == prior,
          std::string{"writer failure stays latched: "} + item.name);
  }
  std::ostringstream duplicate_output(std::ios::binary);
  ps2vita::GifTraceWriter duplicate_writer(duplicate_output);
  check(duplicate_writer.write(1u, 0u, 0u, &byte, 1u), "writer accepts first ordinal before duplicate test");
  const auto duplicate_prior = duplicate_output.str();
  check(!duplicate_writer.write(1u, 0u, 0u, &byte, 1u) &&
        duplicate_output.str() == duplicate_prior && duplicate_writer.record_count() == 1u &&
        duplicate_writer.payload_bytes() == 1u,
        "duplicate ordinal does not append a partial record or advance counts");
  std::ostringstream finished_output(std::ios::binary);
  ps2vita::GifTraceWriter finished_writer(finished_output);
  check(finished_writer.finish(5u, 9u), "writer finishes before post-footer write check");
  const auto finished_prior = finished_output.str();
  check(!finished_writer.write(1u, 5u, 9u, &byte, 1u) &&
        finished_output.str() == finished_prior && finished_writer.record_count() == 0u,
        "writer cannot append records after footer");
  std::ostringstream cycle_output(std::ios::binary);
  ps2vita::GifTraceWriter cycle_writer(cycle_output);
  check(cycle_writer.write(1u, 8u, 0u, nullptr, 0u), "writer accepts cycle before monotonic test");
  const auto cycle_prior = cycle_output.str();
  check(!cycle_writer.write(2u, 7u, 0u, nullptr, 0u) &&
        cycle_output.str() == cycle_prior && cycle_writer.record_count() == 1u,
        "decreasing cycle is a transactional writer validation failure");
  std::ostringstream end_cycle_output(std::ios::binary);
  ps2vita::GifTraceWriter end_cycle_writer(end_cycle_output);
  check(end_cycle_writer.write(1u, 8u, 0u, nullptr, 0u), "writer accepts cycle before terminal test");
  const auto end_cycle_prior = end_cycle_output.str();
  check(!end_cycle_writer.finish(7u, 0u) && end_cycle_output.str() == end_cycle_prior,
        "decreasing terminal cycle does not append a partial footer");
}

} // namespace

int main() {
  test_observers();
  test_roundtrip();
  test_wire_format();
  test_truncation();
  test_corruption();
  test_writer_rejections();
  test_capacities();
  std::printf("GIF trace: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
