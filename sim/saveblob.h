// The SimDLL save blob: read, write, and validate.
//
// SIM_BeginSave hands the game an opaque byte blob and SIM_HandleMessage(Load, ...)
// takes it back. Worldgen round-trips through that pair once per asteroid *before a new
// game can start*, so this is not a persistence detail — nothing boots without it.
//
// The layout here was established by driving the game's SimDLL offline and changing one
// field at a time (driver/src/savefmt.cpp), then checked against a real 9.3 MB blob saved
// by the game.
//
// A replacement sim writes and reads its own blobs, so it does not have to keep this
// format. It does have to read it, or every existing save is dead.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace oni_sim {

// Little-endian, no padding anywhere, no compression. Sizes are asserted below rather
// than assumed, because the game's own message structs are Pack = 4 and mixing the two
// conventions is silent corruption.
#pragma pack(push, 1)

struct SaveCell {
  int32_t elementHash;  // SimHashes value, not an index into the element table
  float temperature;    // K
  float mass;           // kg
  float radiation;      // rads
};

struct SaveDisease {
  int32_t diseaseHash;  // 0 when the cell is clean
  int32_t count;
};

struct SaveBackwall {
  int32_t elementHash;
  float mass;
  float temperature;
};

#pragma pack(pop)

static_assert(sizeof(SaveCell) == 16, "SaveCell layout drift");
static_assert(sizeof(SaveDisease) == 8, "SaveDisease layout drift");
static_assert(sizeof(SaveBackwall) == 12, "SaveBackwall layout drift");

inline constexpr char kSaveMagic[8] = {'S', 'I', 'M', 'S', 'A', 'V', 'E', '\0'};
inline constexpr int32_t kSaveVersion = 15;  // game build 744825

// magic(8) + version(4) + width(4) + height(4) + x(4) + y(4) + one byte that is zero in
// every blob observed, including a real 9.3 MB one from the game.
inline constexpr size_t kSaveHeaderSize = 29;

struct SaveBlob {
  int32_t version = kSaveVersion;
  // Padded dimensions: the sim keeps a one-cell border ring, so these are the game's
  // grid size plus two in each direction and the arrays cover the border as well.
  int32_t width = 0;
  int32_t height = 0;
  int32_t x = 0;  // world offset, straight from SIM_BeginSave's arguments
  int32_t y = 0;
  uint8_t trailing = 0;  // header byte 28, purpose unknown, always zero so far

  std::vector<SaveCell> cells;
  std::vector<SaveDisease> disease;
  std::vector<SaveBackwall> backwall;

  size_t Count() const { return static_cast<size_t>(width) * height; }
  int32_t GameWidth() const { return width - 2; }
  int32_t GameHeight() const { return height - 2; }

  // Padded index of a cell in the game's own coordinates.
  size_t Index(int32_t game_x, int32_t game_y) const {
    return static_cast<size_t>(game_y + 1) * width + (game_x + 1);
  }
};

inline size_t SaveBlobSize(int32_t width, int32_t height) {
  const size_t n = static_cast<size_t>(width) * height;
  return kSaveHeaderSize +
         n * (sizeof(SaveCell) + sizeof(SaveDisease) + sizeof(SaveBackwall));
}

// Decode. Returns false and fills `error` rather than reading past the end of a blob
// that does not describe itself consistently — a truncated save is the one input this
// is guaranteed to meet eventually.
inline bool DecodeSaveBlob(const uint8_t* data, size_t size, SaveBlob* out,
                           std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (size < kSaveHeaderSize) return fail("blob shorter than the header");
  if (memcmp(data, kSaveMagic, sizeof(kSaveMagic)) != 0) return fail("bad magic");

  SaveBlob b;
  memcpy(&b.version, data + 8, 4);
  memcpy(&b.width, data + 12, 4);
  memcpy(&b.height, data + 16, 4);
  memcpy(&b.x, data + 20, 4);
  memcpy(&b.y, data + 24, 4);
  b.trailing = data[28];

  if (b.width <= 2 || b.height <= 2) return fail("degenerate grid dimensions");
  // 2^31 bytes of cells is already absurd; this only has to reject nonsense that would
  // overflow the size computation below.
  if (static_cast<int64_t>(b.width) * b.height > (1 << 28)) {
    return fail("grid dimensions implausibly large");
  }
  if (size != SaveBlobSize(b.width, b.height)) return fail("size does not match w*h");

  const size_t n = b.Count();
  b.cells.resize(n);
  b.disease.resize(n);
  b.backwall.resize(n);
  const uint8_t* p = data + kSaveHeaderSize;
  memcpy(b.cells.data(), p, n * sizeof(SaveCell));
  p += n * sizeof(SaveCell);
  memcpy(b.disease.data(), p, n * sizeof(SaveDisease));
  p += n * sizeof(SaveDisease);
  memcpy(b.backwall.data(), p, n * sizeof(SaveBackwall));

  *out = std::move(b);
  return true;
}

inline std::vector<uint8_t> EncodeSaveBlob(const SaveBlob& b) {
  std::vector<uint8_t> out(SaveBlobSize(b.width, b.height));
  memcpy(out.data(), kSaveMagic, sizeof(kSaveMagic));
  memcpy(out.data() + 8, &b.version, 4);
  memcpy(out.data() + 12, &b.width, 4);
  memcpy(out.data() + 16, &b.height, 4);
  memcpy(out.data() + 20, &b.x, 4);
  memcpy(out.data() + 24, &b.y, 4);
  out[28] = b.trailing;

  const size_t n = b.Count();
  uint8_t* p = out.data() + kSaveHeaderSize;
  memcpy(p, b.cells.data(), n * sizeof(SaveCell));
  p += n * sizeof(SaveCell);
  memcpy(p, b.disease.data(), n * sizeof(SaveDisease));
  p += n * sizeof(SaveDisease);
  memcpy(p, b.backwall.data(), n * sizeof(SaveBackwall));
  return out;
}

}  // namespace oni_sim
