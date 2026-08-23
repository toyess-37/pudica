#include "logger.h"
#include <chrono>
#include <cstdio>
#include <sstream>

using namespace std::chrono;

uint64_t logger_now_us()
{
  return (uint64_t)(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void Logger::open(const std::string &path)
{
  std::lock_guard<std::mutex> lk(mtx_);
  file_.open(path, std::ios::out | std::ios::trunc);
  if (!file_.is_open())
    return;

  uint64_t ts = logger_now_us();
  // mandatory header line
  file_ << "{\"type\":\"LOG_HEADER\",\"version\":1,\"ts_microsecs\":" << ts << "}\n";
  file_.flush();
}

void Logger::close()
{
  std::lock_guard<std::mutex> lk(mtx_);
  if (file_.is_open())
    file_.close();
}

void Logger::write(const std::string &json_line)
{
  std::lock_guard<std::mutex> lk(mtx_);
  if (!file_.is_open())
    return;
  file_ << json_line << '\n';
  file_.flush();
}

void Logger::log(const std::string &type, uint32_t fid, const std::vector<std::pair<std::string, std::string>> &fields)
{
  if (!is_open())
    return;

  std::ostringstream oss;
  oss << "{ \"type\": \"" << type << "\", \"ts_microsecs\": " << logger_now_us() << ", \"fid\": " << fid;
  for (const auto &f : fields)
  {
    oss << ", \"" << f.first << "\": " << f.second;
  }
  oss << " }";

  write(oss.str());
}

void Logger::log(const std::string &type, const std::vector<std::pair<std::string, std::string>> &fields)
{
  if (!is_open())
    return;

  std::ostringstream oss;
  oss << "{ \"type\": \"" << type << "\", \"ts_microsecs\": " << logger_now_us();
  for (const auto &f : fields)
  {
    oss << ", \"" << f.first << "\": " << f.second;
  }
  oss << " }";

  write(oss.str());
}