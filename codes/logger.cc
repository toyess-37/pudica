#include "logger.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <stdexcept>
uint64_t logger_now_us(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
std::string Logger::quote(const std::string& value) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:value) {
        if(c=='"'||c=='\\')out<<'\\'<<c;
        else if(c<0x20)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<unsigned>(c);
        else out<<c;
    }
    out<<'"';return out.str();
}
void Logger::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(file_.is_open())file_.close();
    file_.open(path,std::ios::out|std::ios::trunc);
    if(!file_)throw std::runtime_error("cannot open log: "+path);
    file_<<"{\"type\":\"LOG_HEADER\",\"version\":2,\"protocol\":2,\"clock\":\"steady\",\"ts_microsecs\":"<<logger_now_us()<<"}\n";
}
bool Logger::is_open()const {std::lock_guard<std::mutex> lock(mtx_);return file_.is_open();}
void Logger::close(){std::lock_guard<std::mutex> lock(mtx_);if(file_.is_open())file_.close();}
void Logger::write(const std::string& line) {
    std::lock_guard<std::mutex> lock(mtx_);
    if(file_.is_open()){file_<<line<<'\n';if(!file_)throw std::runtime_error("log write failed");}
}
void Logger::log(const std::string& type,uint32_t fid,const std::vector<std::pair<std::string,std::string>>& fields){
    std::ostringstream out;out<<"{\"type\":"<<quote(type)<<",\"ts_microsecs\":"<<logger_now_us()<<",\"fid\":"<<fid;
    for(const auto& f:fields)out<<','<<quote(f.first)<<':'<<f.second;
    out<<'}';write(out.str());
}
void Logger::log(const std::string& type,const std::vector<std::pair<std::string,std::string>>& fields){
    std::ostringstream out;out<<"{\"type\":"<<quote(type)<<",\"ts_microsecs\":"<<logger_now_us();
    for(const auto& f:fields)out<<','<<quote(f.first)<<':'<<f.second;
    out<<'}';write(out.str());
}
