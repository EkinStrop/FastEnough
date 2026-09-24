#pragma once
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

inline bool copyLocalFile(const std::filesystem::path& source, const std::filesystem::path& destination,
    uint64_t expectedBytes, bool& destinationTouched, const std::function<bool(uint64_t,uint64_t)>& progress) {
    std::error_code equivalentError;
    if (std::filesystem::equivalent(source,destination,equivalentError))
        throw std::runtime_error("Source and destination refer to the same file");
    auto error=[](const char* operation) {
        return std::runtime_error(std::string(operation)+": "+std::generic_category().message(errno));
    };
    FILE* input=nullptr;
    if (_wfopen_s(&input,source.c_str(),L"rb")!=0 || !input) throw error("Cannot open source");
    std::unique_ptr<FILE,decltype(&fclose)> in(input,&fclose);
    FILE* output=nullptr;
    if (_wfopen_s(&output,destination.c_str(),L"wb")!=0 || !output) throw error("Cannot open destination");
    destinationTouched=true;
    std::unique_ptr<FILE,decltype(&fclose)> out(output,&fclose);
    std::vector<char> buffer(4*1024*1024);
    uint64_t copied=0;
    while (true) {
        size_t count=fread(buffer.data(),1,buffer.size(),in.get());
        if (count==0) {
            if (ferror(in.get())) throw error("Reading source failed");
            break;
        }
        if (fwrite(buffer.data(),1,count,out.get())!=count) throw error("Writing destination failed");
        copied+=count;
        if (!progress(copied,expectedBytes)) return false;
    }
    if (fflush(out.get())!=0) throw error("Flushing destination failed");
    if (fclose(out.release())!=0) throw error("Closing destination failed");
    if (copied!=expectedBytes) throw std::runtime_error("Source size changed during the copy");
    return true;
}
