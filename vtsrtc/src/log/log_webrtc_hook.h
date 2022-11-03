#include "rtc_base/logging.h"
#include "rtc_base/critical_section.h"
#ifdef _WIN32
#include <io.h>
#elif __linux__
#include <sys/stat.h>
#endif
#include <fcntl.h>
#include <stdio.h>

class FileLog : public rtc::LogSink
{
public:
    FileLog(const std::string &LogPath);

    virtual ~FileLog();

    inline void FileDate();

    inline size_t Size();

    inline void Start(void);

    inline void Close(void);

    virtual void OnLogMessage(const std::string &message);

    std::string get_date_time();

private:
    FILE *logfile_;
    const std::string log_path_;
    std::string logfileName_;
    rtc::CriticalSection log_crit_;
};