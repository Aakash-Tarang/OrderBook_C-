#pragma once
/*
 * replay_format.h – Binary market-data replay format + buffered reader.
 *
 * Why a binary replay format
 * ──────────────────────────
 * The fifth-phase pipeline parsed a CSV with std::getline + substr +
 * std::stoull per field.  std::stoull is exception-based (slow even when it
 * does not throw), substr allocates, and getline re-scans the line.  Total
 * parse cost was ~100–1000× the cost of applying an order to the book,
 * i.e. the feed handler, not the matching engine, dominated the pipeline.
 *
 * Format (little-endian):
 *
 *   offset  size  field
 *   ─────────────────────────────────────────────────
 *   0       8     magic  "OBRPLAY"            (8 bytes, no NUL)
 *   8       8     record_count (uint64)
 *   16      8     reserved (0)
 *   24      ...   records, 32 bytes each:
 *
 *   record (packed, 32 bytes):
 *   offset  size  field
 *   0       8     order_id   (uint64)
 *   8       4     price      (int32, ticks)
 *   12      4     qty        (int32)
 *   16      8     ingest_ts  (uint64, producer counter ticks at enqueue time)
 *   24      1     action     (0 = ADD, 1 = CANCEL)
 *   25      1     side       (0 = BID, 1 = ASK)
 *   26      2     flags      (0)
 *   28      4     reserved   (0)
 *
 * A fixed-size, trivially-copyable record means the producer never parses:
 * it reads a 32-byte struct and pushes it onto the ring buffer as-is.
 * The Python generator (scripts/generate_market_data.py) writes this format
 * natively alongside the CSV.
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ob {
namespace replay {

enum Action : uint8_t { kAdd = 0, kCancel = 1 };
enum SideByte : uint8_t { kBid = 0, kAsk = 1 };

#pragma pack(push, 1)
struct Header {              // 24 bytes
    char     magic[8];      // "OBRPLAY"
    uint64_t record_count;  // number of 32-byte records that follow
    uint64_t reserved;      // 0
};

struct Event {               // 32 bytes
    uint64_t order_id;
    int32_t  price;
    int32_t  qty;
    uint64_t ingest_ts;      // producer-side counter ticks (0 if unused)
    uint8_t  action;         // replay::kAdd / replay::kCancel
    uint8_t  side;           // replay::kBid  / replay::kAsk
    uint16_t flags;
    int32_t  reserved;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 24, "replay header must be 24 bytes");
static_assert(sizeof(Event)  == 32, "replay event must be 32 bytes");

inline constexpr char kMagic[8] = {'O', 'B', 'R', 'P', 'L', 'A', 'Y', '\0'};

// ── Sequential reader (stdio, single 1 MiB buffer) ───────────────────────────
class Reader {
public:
    explicit Reader(const std::string& path) : path_(path)
    {
        file_ = std::fopen(path.c_str(), "rb");
        if (!file_) { fail_ = true; return; }

        Header hdr{};
        if (std::fread(&hdr, sizeof(hdr), 1, file_) != 1) { fail_ = true; return; }
        bool magic_ok = true;
        for (int i = 0; i < 8; ++i)
            if (hdr.magic[i] != kMagic[i]) magic_ok = false;
        if (!magic_ok) { fail_ = true; return; }
        remaining_ = hdr.record_count;
        buffer_.resize(kRecordsPerRead * sizeof(Event));
    }

    ~Reader()
    {
        if (file_) std::fclose(file_);
    }

    Reader(const Reader&)            = delete;
    Reader& operator=(const Reader&) = delete;

    bool ok() const noexcept { return !fail_; }
    uint64_t remaining() const noexcept { return remaining_; }

    // Returns false on EOF or malformed data (fail() then reports true).
    bool next(Event& out)
    {
        if (fail_ || remaining_ == 0) return false;
        if (buf_pos_ >= buf_end_) {
            const size_t want = kRecordsPerRead * sizeof(Event);
            const size_t max_read =
                (remaining_ < kRecordsPerRead)
                    ? static_cast<size_t>(remaining_) * sizeof(Event)
                    : want;
            const size_t got = std::fread(buffer_.data(), 1, max_read, file_);
            if (got == 0 || got % sizeof(Event) != 0) { fail_ = true; return false; }
            buf_pos_ = 0;
            buf_end_ = got;
        }
        __builtin_memcpy(&out, buffer_.data() + buf_pos_, sizeof(Event));
        buf_pos_ += sizeof(Event);
        --remaining_;
        return true;
    }

    bool fail() const noexcept { return fail_; }

private:
    static constexpr std::size_t kRecordsPerRead = 32768; // 1 MiB of events

    std::string            path_;
    std::FILE*             file_   = nullptr;
    std::vector<char>      buffer_;
    std::size_t            buf_pos_   = 0;
    std::size_t            buf_end_   = 0;
    uint64_t               remaining_ = 0;
    bool                   fail_      = false;
};

} // namespace replay
} // namespace ob
