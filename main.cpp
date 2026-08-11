#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kBucketCount = 1u << 16;
constexpr uint32_t kVersion = 1;
constexpr char kDatabaseName[] = "storage.dat";
constexpr char kMagic[8] = {'F', 'S', 'D', 'B', '0', '1', '\0', '\0'};

struct Header {
  char magic[8];
  uint32_t version;
  uint32_t bucket_count;
  uint32_t free_head;
  uint32_t record_count;
  uint64_t reserved[5];
};
static_assert(sizeof(Header) == 64);

// Record references are one-based. Zero is the null reference.
struct Record {
  char key[65];
  uint8_t padding[3];
  int32_t value;
  uint32_t previous_by_key;
  uint32_t next_by_key;
  uint32_t next_by_pair;
};
static_assert(sizeof(Record) == 84);

constexpr off_t kKeyBucketsOffset = sizeof(Header);
constexpr off_t kPairBucketsOffset =
    kKeyBucketsOffset + static_cast<off_t>(kBucketCount) * sizeof(uint32_t);
constexpr off_t kRecordsOffset =
    kPairBucketsOffset + static_cast<off_t>(kBucketCount) * sizeof(uint32_t);

bool read_exact(int fd, void *destination, size_t size, off_t offset) {
  auto *bytes = static_cast<unsigned char *>(destination);
  while (size != 0) {
    const ssize_t result = pread(fd, bytes, size, offset);
    if (result <= 0) return false;
    bytes += result;
    size -= static_cast<size_t>(result);
    offset += result;
  }
  return true;
}

bool write_exact(int fd, const void *source, size_t size, off_t offset) {
  const auto *bytes = static_cast<const unsigned char *>(source);
  while (size != 0) {
    const ssize_t result = pwrite(fd, bytes, size, offset);
    if (result <= 0) return false;
    bytes += result;
    size -= static_cast<size_t>(result);
    offset += result;
  }
  return true;
}

uint64_t hash_key(std::string_view key) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char character : key) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  // An avalanche step prevents patterns in the low bits from selecting buckets.
  hash ^= hash >> 33;
  hash *= 0xff51afd7ed558ccdULL;
  hash ^= hash >> 33;
  hash *= 0xc4ceb9fe1a85ec53ULL;
  return hash ^ (hash >> 33);
}

uint32_t key_bucket(std::string_view key) {
  return static_cast<uint32_t>(hash_key(key)) & (kBucketCount - 1);
}

uint32_t pair_bucket(std::string_view key, int32_t value) {
  uint64_t hash = hash_key(key);
  hash ^= static_cast<uint32_t>(value) + 0x9e3779b97f4a7c15ULL +
          (hash << 6) + (hash >> 2);
  hash ^= hash >> 30;
  hash *= 0xbf58476d1ce4e5b9ULL;
  hash ^= hash >> 27;
  hash *= 0x94d049bb133111ebULL;
  return static_cast<uint32_t>(hash ^ (hash >> 31)) & (kBucketCount - 1);
}

class Database {
 public:
  Database() {
    fd_ = open(kDatabaseName, O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) return;

    struct stat information {};
    if (fstat(fd_, &information) != 0) return;
    if (information.st_size == 0) {
      std::memcpy(header_.magic, kMagic, sizeof(kMagic));
      header_.version = kVersion;
      header_.bucket_count = kBucketCount;
      if (ftruncate(fd_, kRecordsOffset) != 0 ||
          !write_exact(fd_, &header_, sizeof(header_), 0)) {
        return;
      }
    } else if (!read_exact(fd_, &header_, sizeof(header_), 0) ||
               std::memcmp(header_.magic, kMagic, sizeof(kMagic)) != 0 ||
               header_.version != kVersion ||
               header_.bucket_count != kBucketCount ||
               information.st_size < kRecordsOffset) {
      return;
    }
    valid_ = true;
  }

  ~Database() {
    if (valid_ && header_dirty_) write_header();
    if (fd_ >= 0) close(fd_);
  }

  bool valid() const { return valid_; }

  bool insert(std::string_view key, int32_t value) {
    const uint32_t key_slot = key_bucket(key);
    const uint32_t pair_slot = pair_bucket(key, value);
    uint32_t old_key_head = 0;
    uint32_t old_pair_head = 0;
    if (!read_bucket(kKeyBucketsOffset, key_slot, old_key_head) ||
        !read_bucket(kPairBucketsOffset, pair_slot, old_pair_head)) {
      return false;
    }

    // Behave like a map if an already-present pair is inserted again.
    for (uint32_t current = old_pair_head; current != 0;) {
      Record existing {};
      if (!read_record(current, existing)) return false;
      if (existing.value == value && matches(existing, key)) return true;
      current = existing.next_by_pair;
    }

    uint32_t identifier;
    if (header_.free_head != 0) {
      identifier = header_.free_head;
      Record free_record {};
      if (!read_record(identifier, free_record)) return false;
      header_.free_head = free_record.next_by_pair;
    } else {
      if (header_.record_count == UINT32_MAX) return false;
      identifier = ++header_.record_count;
    }

    Record record {};
    std::memcpy(record.key, key.data(), key.size());
    record.value = value;
    record.next_by_key = old_key_head;
    record.next_by_pair = old_pair_head;

    if (!write_record(identifier, record)) return false;
    if (old_key_head != 0) {
      Record old_head {};
      if (!read_record(old_key_head, old_head)) return false;
      old_head.previous_by_key = identifier;
      if (!write_record(old_key_head, old_head)) return false;
    }
    if (!write_bucket(kKeyBucketsOffset, key_slot, identifier) ||
        !write_bucket(kPairBucketsOffset, pair_slot, identifier)) {
      return false;
    }
    header_dirty_ = true;
    return true;
  }

  bool erase(std::string_view key, int32_t value) {
    const uint32_t pair_slot = pair_bucket(key, value);
    uint32_t current = 0;
    if (!read_bucket(kPairBucketsOffset, pair_slot, current)) return false;

    uint32_t previous_in_pair_chain = 0;
    Record record {};
    while (current != 0) {
      if (!read_record(current, record)) return false;
      if (record.value == value && matches(record, key)) break;
      previous_in_pair_chain = current;
      current = record.next_by_pair;
    }
    // Deleting a missing pair is explicitly allowed.
    if (current == 0) return true;

    if (previous_in_pair_chain == 0) {
      if (!write_bucket(kPairBucketsOffset, pair_slot, record.next_by_pair))
        return false;
    } else {
      Record previous {};
      if (!read_record(previous_in_pair_chain, previous)) return false;
      previous.next_by_pair = record.next_by_pair;
      if (!write_record(previous_in_pair_chain, previous)) return false;
    }

    if (record.previous_by_key == 0) {
      if (!write_bucket(kKeyBucketsOffset, key_bucket(key),
                        record.next_by_key)) {
        return false;
      }
    } else {
      Record previous {};
      if (!read_record(record.previous_by_key, previous)) return false;
      previous.next_by_key = record.next_by_key;
      if (!write_record(record.previous_by_key, previous)) return false;
    }
    if (record.next_by_key != 0) {
      Record next {};
      if (!read_record(record.next_by_key, next)) return false;
      next.previous_by_key = record.previous_by_key;
      if (!write_record(record.next_by_key, next)) return false;
    }

    record.next_by_pair = header_.free_head;
    header_.free_head = current;
    if (!write_record(current, record)) return false;
    header_dirty_ = true;
    return true;
  }

  bool find(std::string_view key, std::vector<int32_t> &values) const {
    values.clear();
    uint32_t current = 0;
    if (!read_bucket(kKeyBucketsOffset, key_bucket(key), current)) return false;
    while (current != 0) {
      Record record {};
      if (!read_record(current, record)) return false;
      if (matches(record, key)) values.push_back(record.value);
      current = record.next_by_key;
    }
    std::sort(values.begin(), values.end());
    return true;
  }

 private:
  bool matches(const Record &record, std::string_view key) const {
    return std::memcmp(record.key, key.data(), key.size()) == 0 &&
           record.key[key.size()] == '\0';
  }

  off_t record_offset(uint32_t identifier) const {
    return kRecordsOffset +
           static_cast<off_t>(identifier - 1) * sizeof(Record);
  }

  bool read_record(uint32_t identifier, Record &record) const {
    return identifier != 0 &&
           read_exact(fd_, &record, sizeof(record), record_offset(identifier));
  }

  bool write_record(uint32_t identifier, const Record &record) {
    return identifier != 0 &&
           write_exact(fd_, &record, sizeof(record), record_offset(identifier));
  }

  bool read_bucket(off_t table_offset, uint32_t bucket,
                   uint32_t &value) const {
    return read_exact(fd_, &value, sizeof(value),
                      table_offset + static_cast<off_t>(bucket) * sizeof(value));
  }

  bool write_bucket(off_t table_offset, uint32_t bucket, uint32_t value) {
    return write_exact(fd_, &value, sizeof(value),
                       table_offset + static_cast<off_t>(bucket) * sizeof(value));
  }

  bool write_header() {
    return write_exact(fd_, &header_, sizeof(header_), 0);
  }

  int fd_ = -1;
  Header header_ {};
  bool valid_ = false;
  bool header_dirty_ = false;
};

class FastInput {
 public:
  bool read_word(char *result, size_t capacity) {
    int character;
    do {
      character = next();
      if (character == EOF) return false;
    } while (character <= ' ');

    size_t length = 0;
    do {
      if (length + 1 < capacity) result[length++] = static_cast<char>(character);
      character = next();
    } while (character > ' ');
    result[length] = '\0';
    return true;
  }

  bool read_uint(uint32_t &result) {
    int character;
    do {
      character = next();
      if (character == EOF) return false;
    } while (character <= ' ');
    result = 0;
    do {
      result = result * 10 + static_cast<uint32_t>(character - '0');
      character = next();
    } while (character >= '0' && character <= '9');
    return true;
  }

 private:
  int next() {
    if (position_ == length_) {
      length_ = std::fread(buffer_, 1, sizeof(buffer_), stdin);
      position_ = 0;
      if (length_ == 0) return EOF;
    }
    return static_cast<unsigned char>(buffer_[position_++]);
  }

  char buffer_[1u << 16] {};
  size_t position_ = 0;
  size_t length_ = 0;
};

class FastOutput {
 public:
  ~FastOutput() { flush(); }

  void text(const char *value) {
    while (*value != '\0') character(*value++);
  }

  void number(uint32_t value) {
    char digits[10];
    size_t length = 0;
    do {
      digits[length++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value != 0);
    while (length != 0) character(digits[--length]);
  }

  void character(char value) {
    if (position_ == sizeof(buffer_)) flush();
    buffer_[position_++] = value;
  }

 private:
  void flush() {
    if (position_ != 0) {
      std::fwrite(buffer_, 1, position_, stdout);
      position_ = 0;
    }
  }

  char buffer_[1u << 16] {};
  size_t position_ = 0;
};

}  // namespace

int main() {
  FastInput input;
  FastOutput output;
  Database database;
  if (!database.valid()) return 1;

  uint32_t command_count;
  if (!input.read_uint(command_count)) return 0;

  char command[16];
  char key[65];
  std::vector<int32_t> values;
  for (uint32_t i = 0; i < command_count; ++i) {
    if (!input.read_word(command, sizeof(command)) ||
        !input.read_word(key, sizeof(key))) {
      return 1;
    }
    const std::string_view key_view(key);
    if (command[0] == 'i') {
      uint32_t value;
      if (!input.read_uint(value) ||
          !database.insert(key_view, static_cast<int32_t>(value))) {
        return 1;
      }
    } else if (command[0] == 'd') {
      uint32_t value;
      if (!input.read_uint(value) ||
          !database.erase(key_view, static_cast<int32_t>(value))) {
        return 1;
      }
    } else {
      if (!database.find(key_view, values)) return 1;
      if (values.empty()) {
        output.text("null\n");
      } else {
        for (size_t j = 0; j < values.size(); ++j) {
          if (j != 0) output.character(' ');
          output.number(static_cast<uint32_t>(values[j]));
        }
        output.character('\n');
      }
    }
  }
  return 0;
}
