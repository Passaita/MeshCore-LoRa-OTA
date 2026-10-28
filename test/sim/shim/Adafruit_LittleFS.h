#pragma once
// sim: a per-node file system in RAM (files survive a node reboot, like the nRF52's internal flash file system)
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#define FILE_O_READ  0
#define FILE_O_WRITE 1
namespace Adafruit_LittleFS_Namespace {
class File {
public:
  std::vector<uint8_t>* d = nullptr; size_t pos = 0; bool w = false;
  File() {}
  File(std::vector<uint8_t>* v, bool wr) : d(v), pos(wr ? v->size() : 0), w(wr) {}
  operator bool() const { return d != nullptr; }
  int read(void* buf, size_t n) { if (!d) return -1; size_t k = pos >= d->size() ? 0 : std::min(n, d->size() - pos); memcpy(buf, d->data() + pos, k); pos += k; return (int)k; }
  int read() { uint8_t b; return read(&b, 1) == 1 ? b : -1; }
  size_t write(const uint8_t* buf, size_t n) { if (!d || !w) return 0; d->insert(d->begin() + pos, buf, buf + n); pos += n; return n; }
  size_t write(uint8_t b) { return write(&b, 1); }
  bool seek(uint32_t p) { pos = p; return true; }
  size_t size() const { return d ? d->size() : 0; }
  size_t available() const { return d && pos < d->size() ? d->size() - pos : 0; }
  void close() { d = nullptr; }
  void flush() {}
};
}
class Adafruit_LittleFS {
public:
  std::map<std::string, std::vector<uint8_t>> files;
  bool begin() { return true; }
  bool exists(const char* p) { return files.count(p) != 0; }
  bool remove(const char* p) { return files.erase(p) != 0; }
  bool mkdir(const char*) { return true; }
  Adafruit_LittleFS_Namespace::File open(const char* p, uint8_t mode = FILE_O_READ) {
    if (mode == FILE_O_READ) { auto it = files.find(p); if (it == files.end()) return Adafruit_LittleFS_Namespace::File(); return Adafruit_LittleFS_Namespace::File(&it->second, false); }
    return Adafruit_LittleFS_Namespace::File(&files[p], true);
  }
};
using namespace Adafruit_LittleFS_Namespace;
