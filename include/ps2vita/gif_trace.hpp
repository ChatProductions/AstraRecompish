#pragma once

// Astra's fresh-instance, reset-starting serialized GIF submissions, NOT a
// PCSX2 GS dump or hardware timing trace. Gif::reset on a reused Gs alone is
// insufficient: host raster/depth state is not in this stream. Host tools only:
// no codec/stream I/O in the core path. All subsequent graphics changes must
// occur through the recorded Gif::submit calls.
#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <utility>
#include <vector>

namespace ps2vita {

struct GifTraceRecord {
  std::uint64_t ordinal = 0, ee_cycle = 0;
  std::uint32_t next_ee_pc = 0;
  std::vector<std::uint8_t> payload;
};

namespace gif_trace_detail {
inline constexpr std::array<char, 8> magic{{'A','S','T','R','A','G','F','1'}};
inline constexpr std::uint64_t max_payload = 64u * 1024u * 1024u;
inline constexpr std::uint32_t max_record = 16u * 1024u * 1024u;
inline constexpr std::uint64_t max_records = 100000u;
inline constexpr std::uint64_t hash_seed = 1469598103934665603ull;
inline void put(std::uint8_t* bytes, std::uint64_t value, unsigned count) {
  for (unsigned b = 0; b < count; ++b) bytes[b] = value >> (b * 8u);
}
inline std::uint64_t get(const std::uint8_t* bytes, unsigned count) {
  std::uint64_t value = 0;
  for (unsigned b = 0; b < count; ++b) value |= std::uint64_t{bytes[b]} << (b * 8u);
  return value;
}
inline void hash(std::uint64_t& value, const std::uint8_t* bytes, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    value ^= bytes[i]; value *= 1099511628211ull;
  }
}
inline std::array<std::uint8_t, 32> header(std::uint64_t ordinal,
    std::uint64_t cycle, std::uint32_t pc, std::uint32_t size, std::uint64_t flag) {
  std::array<std::uint8_t, 32> bytes{};
  put(bytes.data(), ordinal, 8); put(bytes.data() + 8, cycle, 8);
  put(bytes.data() + 16, pc, 4); put(bytes.data() + 20, size, 4);
  put(bytes.data() + 24, flag, 8);
  return bytes;
}
} // namespace gif_trace_detail

// Wire format: magic[8]; repeated little-endian 32-byte headers
// {u64 ordinal,u64 EE cycle,u32 next-PC,u32 payload-size,u64 flag=0} + payload.
// End: {ordinal=0,final cycle,final PC,size=0,flag=1}, followed by u64 record
// count, payload bytes, rolling hash (all headers/payload, including end header).
// Magic is versioned; checksum is corruption detection, not authentication.
class GifTraceWriter {
public:
  explicit GifTraceWriter(std::ostream& output) : output_(output) {
    output_.write(gif_trace_detail::magic.data(), gif_trace_detail::magic.size());
    if (!output_) fail("could not write GIF trace header");
  }
  bool write(std::uint64_t ordinal, std::uint64_t cycle, std::uint32_t pc,
             const std::uint8_t* data, std::size_t size) {
    using namespace gif_trace_detail;
    if (!good()) return false;
    if (finished_) return fail("GIF trace already finished");
    if (ordinal != records_ + 1u || cycle < last_cycle_)
      return fail("nonmonotonic GIF trace ordinal/cycle");
    if (records_ == max_records || size > max_record ||
        size > max_payload - payload_bytes_)
      return fail("GIF trace capacity exceeded");
    if (size != 0u && data == nullptr) return fail("null GIF trace payload");
    const auto bytes = header(ordinal, cycle, pc, static_cast<std::uint32_t>(size), 0);
    output_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (size != 0u) output_.write(reinterpret_cast<const char*>(data), size);
    if (!output_) return fail("could not write GIF trace record");
    hash(hash_, bytes.data(), bytes.size()); hash(hash_, data, size);
    ++records_; payload_bytes_ += size; last_cycle_ = cycle;
    return true;
  }
  bool finish(std::uint64_t cycle, std::uint32_t pc) {
    using namespace gif_trace_detail;
    if (!good()) return false;
    if (finished_ || cycle < last_cycle_) return fail("invalid GIF trace finish");
    const auto bytes = header(0, cycle, pc, 0, 1);
    hash(hash_, bytes.data(), bytes.size());
    std::array<std::uint8_t, 24> footer{};
    put(footer.data(), records_, 8); put(footer.data() + 8, payload_bytes_, 8);
    put(footer.data() + 16, hash_, 8);
    output_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output_.write(reinterpret_cast<const char*>(footer.data()), footer.size());
    output_.flush();
    if (!output_) return fail("could not finish GIF trace");
    finished_ = true; return true;
  }
  bool good() const { return error_ == nullptr; }
  const char* error() const { return error_ ? error_ : ""; }
  std::uint64_t record_count() const { return records_; }
  std::uint64_t payload_bytes() const { return payload_bytes_; }
private:
  bool fail(const char* error) { error_ = error; return false; }
  std::ostream& output_;
  const char* error_ = nullptr;
  bool finished_ = false;
  std::uint64_t records_ = 0, payload_bytes_ = 0, last_cycle_ = 0;
  std::uint64_t hash_ = gif_trace_detail::hash_seed;
};

class GifTraceReader {
public:
  explicit GifTraceReader(std::istream& input) : input_(input) {
    std::array<char, 8> magic{};
    input_.read(magic.data(), magic.size());
    if (!input_ || magic != gif_trace_detail::magic) fail("invalid GIF trace header");
  }
  bool next(GifTraceRecord& record) {
    using namespace gif_trace_detail;
    if (!good() || finished_) return false;
    std::array<std::uint8_t, 32> bytes{};
    if (!read(bytes.data(), bytes.size())) return false;
    const auto ordinal = get(bytes.data(), 8), cycle = get(bytes.data() + 8, 8);
    const auto pc = static_cast<std::uint32_t>(get(bytes.data() + 16, 4));
    const auto size = static_cast<std::uint32_t>(get(bytes.data() + 20, 4));
    const auto flag = get(bytes.data() + 24, 8);
    if (cycle < last_cycle_) return fail("nonmonotonic GIF trace cycle");
    if (flag == 1u) {
      if (ordinal != 0u || size != 0u) return fail("invalid GIF trace end record");
      std::array<std::uint8_t, 24> footer{};
      if (!read(footer.data(), footer.size())) return false;
      hash(hash_, bytes.data(), bytes.size());
      if (get(footer.data(), 8) != records_ ||
          get(footer.data() + 8, 8) != payload_bytes_ ||
          get(footer.data() + 16, 8) != hash_)
        return fail("GIF trace footer/checksum mismatch");
      if (input_.peek() != std::char_traits<char>::eof() || input_.bad())
        return fail("data after GIF trace footer");
      terminal_cycle_ = cycle; terminal_pc_ = pc; finished_ = true;
      return false;
    }
    if (flag != 0u || ordinal != records_ + 1u)
      return fail("invalid GIF trace record flag/ordinal");
    if (records_ == max_records || size > max_record ||
        size > max_payload - payload_bytes_)
      return fail("GIF trace capacity exceeded");
    GifTraceRecord candidate{ordinal, cycle, pc, std::vector<std::uint8_t>(size)};
    if (size != 0u && !read(candidate.payload.data(), size)) return false;
    hash(hash_, bytes.data(), bytes.size()); hash(hash_, candidate.payload.data(), size);
    ++records_; payload_bytes_ += size; last_cycle_ = cycle;
    record = std::move(candidate);
    return true;
  }
  bool good() const { return error_ == nullptr; }
  bool finished() const { return finished_; }
  const char* error() const { return error_ ? error_ : ""; }
  std::uint64_t record_count() const { return records_; }
  std::uint64_t payload_bytes() const { return payload_bytes_; }
  std::uint64_t terminal_cycle() const { return terminal_cycle_; }
  std::uint32_t terminal_pc() const { return terminal_pc_; }
private:
  bool read(std::uint8_t* data, std::size_t size) {
    input_.read(reinterpret_cast<char*>(data), size);
    return input_ ? true : fail("truncated GIF trace");
  }
  bool fail(const char* error) { error_ = error; return false; }
  std::istream& input_;
  const char* error_ = nullptr;
  bool finished_ = false;
  std::uint64_t records_ = 0, payload_bytes_ = 0, last_cycle_ = 0, terminal_cycle_ = 0;
  std::uint32_t terminal_pc_ = 0;
  std::uint64_t hash_ = gif_trace_detail::hash_seed;
};

} // namespace ps2vita
