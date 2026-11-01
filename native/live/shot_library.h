#pragma once

#include "live_controller.h"
#include <set>

namespace cs2_live {
// Immutable input read once by the overlay. Each project uses the controller's
// real parser; neither this file nor any library path is read during Draw().
inline Json ReadShotLibrary(const char* path,const char* expectedDemo) {
    if(!path || path[0]!='/' || !expectedDemo || !*expectedDemo)
        throw std::invalid_argument("saved shots require an absolute staged path and owned demo");
    constexpr std::size_t limit=1024*1024;
    const int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)throw std::runtime_error("cannot open staged saved shots");
    std::string contents;
    try {
        struct stat info{};
        if(fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_uid!=getuid() || info.st_size<=0 || info.st_size>ssize_t(limit))
            throw std::invalid_argument("saved shots must be an owned regular file up to 1 MiB");
        char buffer[8192];
        for(;;) {
            const auto count=read(fd,buffer,sizeof(buffer));
            if(count<0 && errno==EINTR)continue;
            if(count<0)throw std::runtime_error("cannot read staged saved shots");
            if(!count)break;
            contents.append(buffer,std::size_t(count));
            if(contents.size()>limit)throw std::invalid_argument("saved shots exceed 1 MiB");
        }
        close(fd);
    }catch(...){close(fd);throw;}
    Json library=Json::parse(contents);
    if(!library.is_object() || library.size()!=3 || !library.contains("version") ||
       !library["version"].is_number_integer() || library["version"]!=1 || !library.contains("demo") || !library["demo"].is_string() || !library.contains("shots"))
        throw std::invalid_argument("saved shots require version 1, demo and shots");
    const auto demo=library["demo"].get<std::string>();
    if(demo.find('\0')!=std::string::npos || !std::filesystem::path(demo).is_absolute() ||
       std::filesystem::canonical(demo)!=std::filesystem::canonical(expectedDemo))
        throw std::invalid_argument("saved shots do not match the owned demo");
    auto& shots=library["shots"];
    if(!shots.is_array() || shots.empty() || shots.size()>64)
        throw std::invalid_argument("saved shots require 1..64 entries");
    std::set<std::string> ids;
    for(auto& shot:shots) {
        if(!shot.is_object() || shot.size()!=3 || !shot.contains("id") || !shot.contains("label") || !shot.contains("project"))
            throw std::invalid_argument("saved shot requires id, label and project");
        for(const auto& field:std::array<std::pair<const char*,std::size_t>,2>{{{"id",128},{"label",256}}}) {
            if(!shot[field.first].is_string())throw std::invalid_argument("saved shot labels must be strings");
            const auto text=shot[field.first].get<std::string>();
            if(text.empty() || text.size()>field.second || text.find('\0')!=std::string::npos)
                throw std::invalid_argument("saved shot label length is invalid");
        }
        if(!ids.insert(shot["id"].get<std::string>()).second)throw std::invalid_argument("duplicate saved shot id");
        auto project=ReadProject(shot["project"]);
        if(project.keys.empty())throw std::invalid_argument("saved shot requires camera keys");
        shot["project"]=ToJson(project);
    }
    return library;
}
} // namespace cs2_live
