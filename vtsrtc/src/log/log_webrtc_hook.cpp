#include "log_webrtc_hook.h"
#include <chrono>

FileLog::FileLog(const std::string &LogPath)
    : logfile_(NULL),
      log_path_(LogPath)
{
    // 关闭WebRTC debug日志，只保留WARNING和ERROR级别
    rtc::LogMessage::LogToDebug(rtc::LS_NONE);
    // 关闭stderr输出，只输出到文件
    rtc::LogMessage::SetLogToStderr(false);
}

FileLog::~FileLog()
{
    if (logfile_)
    {
        fclose(logfile_);
        logfile_ = NULL;
    }
}

inline void FileLog::FileDate()
{
    logfileName_ = log_path_ + "/webrtc" + get_date_time() + ".log";
}

inline size_t FileLog::Size()
{
    size_t size = 0;
    if (logfile_ != NULL)
    {
#ifdef _WIN32
		size = _filelength(_fileno(logfile_));
#elif __linux__
		struct stat statbuf;
		stat(logfileName_.c_str(), &statbuf);
		size = statbuf.st_size;
#endif
    }
    return size;
}

inline void FileLog::Start(void)
{
#define MAX_LOG_FILE_SIZE (1024 * 1024 * 1024)

    if (NULL == logfile_)
    {
        FileDate();
        logfile_ = fopen(logfileName_.c_str(), "w");
    }
    else if (Size() > MAX_LOG_FILE_SIZE)
    {
        Close();
        FileDate();
        logfile_ = fopen(logfileName_.c_str(), "w");
    }
}

inline void FileLog::Close(void)
{
    if (logfile_)
    {
        fclose(logfile_);
        logfile_ = NULL;
    }
}

void FileLog::OnLogMessage(const std::string &message)
{
    rtc::CritScope lock(&log_crit_);
    Start();

    if (NULL == logfile_)
        return;

    std::string msgString = message;
    if (fwrite(msgString.c_str(), 1, msgString.length(), logfile_) < 0)
    {
        Close();
    }
    else if (fflush(logfile_) < 0)
    {
        Close();
    }
}

std::string FileLog::get_date_time()
{
    auto to_string = [](const std::chrono::system_clock::time_point &t) -> std::string
    {
        auto as_time_t = std::chrono::system_clock::to_time_t(t);
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &as_time_t); // win api，线程安全，而std::localtime线程不安全
#elif __linux__
        localtime_r(&as_time_t, &tm); // linux api，线程安全
#endif

        std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch());
        char buf[128];
        snprintf(buf, sizeof(buf), "-%04d%02d%02d%02d%02d%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        return buf;
    };

    std::chrono::system_clock::time_point t = std::chrono::system_clock::now();
    return to_string(t);
}
