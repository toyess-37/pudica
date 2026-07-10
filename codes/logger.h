#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// json logger for the controller
// opens a file and writes one json object per line
class Logger
{
public:
  Logger() = default;
  ~Logger() { close(); }

  // don't copy (me: ???)
  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  void open(const std::string &path, const std::string &git_commit = "");
  void close();
  bool is_open() const { return file_.is_open(); }

  void write(const std::string &json_line);

  // simple key-value logger to avoid huge parameter lists
  void log(const std::string &type, uint32_t fid, const std::vector<std::pair<std::string, std::string>> &fields);
  void log(const std::string &type, const std::vector<std::pair<std::string, std::string>> &fields);

private:
  std::ofstream file_;
  mutable std::mutex mtx_;
};

uint64_t logger_now_us();