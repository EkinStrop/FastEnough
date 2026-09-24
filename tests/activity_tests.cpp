#include "activity_log.h"
#include "local_copy.h"
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
static std::string utf8(const std::filesystem::path& path) {
    auto value=path.u8string();return {value.begin(),value.end()};
}

int main() {
    auto root=std::filesystem::temp_directory_path()/
        (L"FastEnough-activity-test-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(root);
        auto& logger=DebugLog::instance();
        auto logPath=root/L"日志"/L"activity.log";
        require(logger.setFilePath(utf8(logPath),512),"Unicode log path could not be opened");
        for (int i=0;i<50;++i) logger.log(LogLevel::Info,"Files","Rename event "+std::to_string(i));
        require(std::filesystem::exists(logPath.wstring()+L".3"),"Log history was not rotated");
        require(!std::filesystem::exists(logPath.wstring()+L".4"),"Log history exceeded retention");
        logger.log(LogLevel::Error,"Files","write failed\nsecond line\r\nlast line");
        auto current=read(logPath);
        require(current.find("[ERROR] [Files] write failed | second line | last line")!=std::string::npos,"Multiline errors were not preserved as one record");
        auto before=std::filesystem::file_size(logPath);
        logger.clear();
        require(logger.snapshot().empty()&&std::filesystem::file_size(logPath)==before,"Clear view removed saved logs");
        require(!logger.setFilePath(utf8(root)),"Opening a directory as a log should fail");
        require(!logger.fileError().empty(),"Log open failure was hidden");
        logger.log(LogLevel::Error,"Files","still visible without a file");
        require(!logger.snapshot().empty(),"Disk failure prevented in-app logging");
        require(logger.setFilePath(utf8(root/L"concurrent.log")),"Logger did not recover");
        logger.clear();
        std::vector<std::thread> writers;
        for (int worker=0;worker<4;++worker) writers.emplace_back([&logger,worker] {
            for (int i=0;i<1500;++i)logger.log(LogLevel::Info,"Worker",std::to_string(worker)+":"+std::to_string(i));
        });
        for (auto& writer:writers)writer.join();
        auto entries=logger.snapshot();
        require(entries.size()<=5000,"In-app history grew without a limit");
        for (size_t i=1;i<entries.size();++i) require(entries[i].sequence>entries[i-1].sequence,"Concurrent log events lost ordering");
        auto all=read(root/L"concurrent.log");
        require(std::count(all.begin(),all.end(),'\n')==6000,"Concurrent log events were lost on disk");

        auto source=root/L"源文件.bin",destination=root/L"copied.bin";
        std::string payload(5*1024*1024,'x');
        {std::ofstream file(source,std::ios::binary);file<<payload;}
        bool touched=false;
        require(copyLocalFile(source,destination,payload.size(),touched,[](uint64_t,uint64_t){return true;}),"Copy failed");
        require(touched&&read(destination)==payload&&read(source)==payload,"Copy did not preserve source and exact contents");
        touched=false;
        require(!copyLocalFile(source,root/L"cancelled.bin",payload.size(),touched,[](uint64_t,uint64_t){return false;}),"Cancellation was ignored");
        require(read(source)==payload,"Cancellation modified source");
        bool failed=false;touched=false;
        try {copyLocalFile(root/L"missing.bin",destination,1,touched,[](uint64_t,uint64_t){return true;});}
        catch (const std::exception&) {failed=true;}
        require(failed&&!touched&&read(destination)==payload,"A missing source damaged the destination");
        failed=false;touched=false;
        try {copyLocalFile(source,source,payload.size(),touched,[](uint64_t,uint64_t){return true;});}
        catch (const std::exception&) {failed=true;}
        require(failed&&!touched&&read(source)==payload,"Copying to the same file damaged its contents");
        failed=false;touched=false;
        try {copyLocalFile(source,root/L"missing-folder"/L"out.bin",payload.size(),touched,[](uint64_t,uint64_t){return true;});}
        catch (const std::exception&) {failed=true;}
        require(failed&&!touched,"Destination failure was not reported");
        failed=false;touched=false;
        try {copyLocalFile(source,destination,payload.size()+1,touched,[](uint64_t,uint64_t){return true;});}
        catch (const std::exception&) {failed=true;}
        require(failed&&std::filesystem::exists(source),"Changed source size appeared successful");
        logger.setFilePath("");
        std::filesystem::remove_all(root);
        std::cout<<"Activity logging and checked local copy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<"\nTest files: "<<utf8(root)<<'\n';
        return 1;
    }
}
