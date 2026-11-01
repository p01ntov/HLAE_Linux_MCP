#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cs2_live {
// Portable project JSON stays unchanged. Recording identity and recency live
// in adjacent .meta files. All IO is explicit; State() only copies cached data.
class ProjectStore {
public:
    using Json=nlohmann::json;
    using Validate=std::function<void(const Json&)>;
    static constexpr std::size_t MaxFile=4*1024*1024,MaxProjects=256,MaxScanBytes=32*1024*1024,MaxStateBytes=128*1024;
    Json State()const{return state_;}
    static std::string Directory() {
        const char* configured=std::getenv("CS2_CAMERA_PROJECTS_DIR");
        if(configured && *configured)return Absolute(configured);
        // Existing installations can already select a custom quick-save file.
        // Keep that explicit storage location when no library directory is set.
        const char* quick=std::getenv("CS2_CAMERA_PROJECT");
        if(quick && *quick)return Absolute(std::filesystem::path(quick).parent_path().string());
        const char* data=std::getenv("XDG_DATA_HOME");
        if(data && *data)return Absolute(std::string(data)+"/cs2-camera/projects");
        const char* home=std::getenv("HOME");
        if(!home || !*home)throw std::runtime_error("project directory requires CS2_CAMERA_PROJECTS_DIR, XDG_DATA_HOME or HOME");
        return Absolute(std::string(home)+"/.local/share/cs2-camera/projects");
    }
    void Refresh(const Json& currentDemo,const Validate& validate) {
        entries_.clear();Json rows=Json::array();std::size_t skipped=0,bytes=0,rowBytes=0,visited=0;bool truncated=false;
        state_={{"directory",""},{"entries",rows},{"error",""},{"revision",++revision_},{"skipped",0},{"truncated",false}};
        int directory=-1;DIR* listing=nullptr;
        try {
            const auto path=Directory();state_["directory"]=path;directory=OpenDirectory(path);
            const int duplicate=dup(directory);if(duplicate<0)throw std::runtime_error("cannot list project directory");
            listing=fdopendir(duplicate);if(!listing){close(duplicate);throw std::runtime_error("cannot list project directory");}
            while(auto* item=readdir(listing)) {
                if(++visited>4096){truncated=true;break;}
                const std::string filename=item->d_name;
                if(filename.size()<6 || filename.substr(filename.size()-5)!=".json" || filename=="last-project.json")continue;
                const std::string id=filename.substr(0,filename.size()-5);if(!ValidId(id)){++skipped;continue;}
                if(entries_.size()>=MaxProjects){truncated=true;break;}
                try {
                    struct stat info{};
                    if(fstatat(directory,filename.c_str(),&info,AT_SYMLINK_NOFOLLOW) || !OwnedFile(info) || info.st_size<=0 || std::uint64_t(info.st_size)>MaxFile)
                        throw std::runtime_error("project is not an owned bounded regular file");
                    if(bytes+std::size_t(info.st_size)>MaxScanBytes){truncated=true;continue;}
                    bytes+=std::size_t(info.st_size);
                    const Json project=Json::parse(Read(directory,filename,MaxFile));validate(project);
                    Json metadata=ReadMetadata(directory,id);
                    const auto saved=Timestamp(metadata,"saved_at_ns",FileTime(info));
                    const auto opened=Timestamp(metadata,"opened_at_ns",0);
                    const Json demo=metadata.value("demo",Json(nullptr));
                    const std::string reason=Compatibility(demo,currentDemo);
                    Json row={{"id",id},{"title",project.value("name",id)},{"demo",demo},{"saved_at_ns",saved},{"opened_at_ns",opened},
                        {"start_tick",project.value("start_tick",0)},{"duration",project.value("duration",0.0)},
                        {"key_count",project.contains("keys") && project["keys"].is_array()?project["keys"].size():0},
                        {"compatible",reason.empty()},{"reason",reason},
                        {"warning",demo.is_null()?"No recording metadata; opens with the current demo.":""}};
                    const auto size=row.dump().size();
                    if(rowBytes+size>MaxStateBytes){truncated=true;continue;}
                    rowBytes+=size;entries_.emplace(id,row);rows.push_back(std::move(row));
                }catch(const std::exception&){++skipped;}
            }
            closedir(listing);listing=nullptr;close(directory);directory=-1;
            std::sort(rows.begin(),rows.end(),[](const Json& a,const Json& b){
                const auto ar=std::max(a["saved_at_ns"].get<std::int64_t>(),a["opened_at_ns"].get<std::int64_t>());
                const auto br=std::max(b["saved_at_ns"].get<std::int64_t>(),b["opened_at_ns"].get<std::int64_t>());
                return ar!=br?ar>br:a["id"].get<std::string>()<b["id"].get<std::string>();
            });
        }catch(const std::exception& error){state_["error"]=std::string(error.what()).substr(0,512);}
        if(listing)closedir(listing);
        if(directory>=0)close(directory);
        state_["entries"]=std::move(rows);state_["skipped"]=skipped;state_["truncated"]=truncated;
    }
    Json Save(const Json& project,std::string id,const Json& demo) {
        const auto path=Directory();const int directory=OpenDirectory(path);
        try {
            if(id.empty())id="project-"+RandomId();
            if(!ValidId(id) || id=="last-project")throw std::invalid_argument("invalid saved project id");
            Json metadata=ReadMetadata(directory,id);metadata["version"]=1;metadata["id"]=id;
            metadata["title"]=project.value("name",id);metadata["demo"]=RecordingMetadata(demo);metadata["saved_at_ns"]=Now();
            if(!metadata.contains("opened_at_ns"))metadata["opened_at_ns"]=0;
            Write(directory,id+".json",project.dump(2)+"\n");
            Write(directory,id+".meta",metadata.dump(2)+"\n");
            close(directory);return {{"saved",true},{"id",id},{"path",path+"/"+id+".json"}};
        }catch(...){close(directory);throw;}
    }
    Json Load(const std::string& id,const Json& currentDemo,const Validate& validate)const {
        RequireKnown(id);const int directory=OpenDirectory(Directory());
        try {
            const auto metadata=ReadMetadata(directory,id);
            const std::string reason=Compatibility(metadata.value("demo",Json(nullptr)),currentDemo);
            if(!reason.empty())throw std::runtime_error(reason);
            Json project=Json::parse(Read(directory,id+".json",MaxFile));validate(project);
            close(directory);return project;
        }catch(...){close(directory);throw;}
    }
    void Opened(const std::string& id)const {
        RequireKnown(id);const int directory=OpenDirectory(Directory());
        try {
            auto metadata=ReadMetadata(directory,id);metadata["version"]=1;metadata["id"]=id;
            if(!metadata.contains("saved_at_ns"))metadata["saved_at_ns"]=0;
            metadata["opened_at_ns"]=Now();Write(directory,id+".meta",metadata.dump(2)+"\n");close(directory);
        }catch(...){close(directory);throw;}
    }
    static bool ValidId(const std::string& id) {
        if(id.empty() || id.size()>128 || id[0]=='.' || id.find("..")!=std::string::npos ||
           !std::all_of(id.begin(),id.end(),[](unsigned char c){return c>=32 && c!=127 && c!='/' && c!='\\';}))return false;
        // Linux permits invalid UTF-8 filenames. Never let one poison the
        // controller's JSON publication; ordinary Unicode/space names work.
        try{Json(id).dump();return true;}catch(const std::exception&){return false;}
    }
private:
    static std::string Absolute(std::string path) {
        if(path.empty() || path[0]!='/' || path.size()>4096)throw std::runtime_error("project directory must be an absolute bounded path");
        try{Json(path).dump();}catch(const std::exception&){throw std::runtime_error("project directory must contain valid UTF-8");}
        while(path.size()>1 && path.back()=='/')path.pop_back();
        return path;
    }
    static bool OwnedFile(const struct stat& info){return S_ISREG(info.st_mode) && info.st_uid==getuid();}
    static int OpenDirectory(const std::string& path) {
        std::filesystem::create_directories(path);
        const int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        struct stat info{};
        if(fd<0 || fstat(fd,&info) || !S_ISDIR(info.st_mode) || info.st_uid!=getuid()) {
            if(fd>=0)close(fd);
            throw std::runtime_error("project directory must be an owned directory, not a symlink");
        }
        return fd;
    }
    static std::string Read(int directory,const std::string& name,std::size_t limit) {
        const int fd=openat(directory,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0)throw std::runtime_error("cannot open owned project file");
        try {
            struct stat info{};
            if(fstat(fd,&info) || !OwnedFile(info) || info.st_size<0 || std::uint64_t(info.st_size)>limit)throw std::runtime_error("project file must be an owned bounded regular file");
            std::string contents;char buffer[8192];
            for(;;){const auto count=read(fd,buffer,sizeof(buffer));if(count<0 && errno==EINTR)continue;if(count<0)throw std::runtime_error("cannot read project file");if(!count)break;contents.append(buffer,std::size_t(count));if(contents.size()>limit)throw std::runtime_error("project file exceeds size limit");}
            close(fd);return contents;
        }catch(...){close(fd);throw;}
    }
    static Json ReadMetadata(int directory,const std::string& id) {
        struct stat info{};
        if(fstatat(directory,(id+".meta").c_str(),&info,AT_SYMLINK_NOFOLLOW)) {
            if(errno==ENOENT)return Json::object();
            throw std::runtime_error("cannot inspect project metadata");
        }
        Json metadata=Json::parse(Read(directory,id+".meta",16384));
        if(!metadata.is_object() || metadata.value("version",0)!=1 || metadata.value("id",std::string())!=id)
            throw std::runtime_error("project metadata id or version is invalid");
        for(const char* field:{"saved_at_ns","opened_at_ns"})if(metadata.contains(field))Timestamp(metadata,field,0);
        metadata["demo"]=RecordingMetadata(metadata.value("demo",Json(nullptr)));
        return metadata;
    }
    static Json RecordingMetadata(const Json& demo) {
        if(!demo.is_null()) {
            if(!demo.is_object() || !demo.contains("path") || !demo["path"].is_string() || demo["path"].get<std::string>().size()>4096 || demo["path"].get<std::string>().empty() || demo["path"].get<std::string>()[0]!='/')
                throw std::runtime_error("project recording metadata is invalid");
            if(demo.contains("fingerprint") && !demo["fingerprint"].is_null() && !demo["fingerprint"].is_object())throw std::runtime_error("project recording fingerprint is invalid");
            Json result={{"path",demo["path"]}};
            if(demo.contains("fingerprint") && demo["fingerprint"].is_object()) {
                const auto& fingerprint=demo["fingerprint"];Json bounded=Json::object();
                if(fingerprint.contains("sha256")) {
                    if(!fingerprint["sha256"].is_string())throw std::runtime_error("project recording SHA256 is invalid");
                    const auto sha=fingerprint["sha256"].get<std::string>();
                    if(sha.size()!=64 || !std::all_of(sha.begin(),sha.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}))throw std::runtime_error("project recording SHA256 is invalid");
                    bounded["sha256"]=sha;
                }
                if(fingerprint.contains("bytes")) {
                    const auto bytes=Timestamp(fingerprint,"bytes",0);if(!bytes)throw std::runtime_error("project recording size is invalid");bounded["bytes"]=bytes;
                }
                if(fingerprint.contains("mtime_ns"))bounded["mtime_ns"]=Timestamp(fingerprint,"mtime_ns",0);
                result["fingerprint"]=std::move(bounded);
            }
            return result;
        }
        return nullptr;
    }
    static std::int64_t Now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    static std::int64_t FileTime(const struct stat& info) {
        if(info.st_mtim.tv_sec<0)return 0;
        const auto seconds=std::uint64_t(info.st_mtim.tv_sec);
        if(seconds>std::uint64_t(INT64_MAX)/1000000000)return INT64_MAX;
        const auto nanos=seconds*1000000000+std::uint64_t(info.st_mtim.tv_nsec);
        return std::int64_t(std::min(nanos,std::uint64_t(INT64_MAX)));
    }
    static std::int64_t Timestamp(const Json& value,const char* field,std::int64_t fallback) {
        if(!value.contains(field))return fallback;
        const auto& time=value[field];
        if(!time.is_number_integer() || (time.is_number_unsigned() && time.get<std::uint64_t>()>std::uint64_t(INT64_MAX)) || time.get<std::int64_t>()<0)throw std::runtime_error("project recency is invalid");
        return time.get<std::int64_t>();
    }
    static std::string Compatibility(const Json& saved,const Json& current) {
        if(saved.is_null())return "";
        if(!current.is_object() || !current.contains("path"))return "Open this project's recording before loading its cameras.";
        const auto sf=saved.value("fingerprint",Json(nullptr)),cf=current.value("fingerprint",Json(nullptr));
        if(sf.is_object() && sf.contains("sha256")) {
            if(!cf.is_object() || !cf.contains("sha256"))return "Current recording metadata is unavailable; wait for its demo index.";
            if(sf["sha256"]!=cf["sha256"] || (sf.contains("bytes") && cf.contains("bytes") && sf["bytes"]!=cf["bytes"]))return "This project belongs to another recording. Open its demo first.";
        }else if(saved.value("path",std::string())!=current.value("path",std::string()))return "This project belongs to another recording. Open its demo first.";
        return "";
    }
    static std::string RandomId() {
        unsigned char bytes[16];const int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)throw std::runtime_error("cannot create project identity");
        std::size_t offset=0;while(offset<sizeof(bytes)){const auto count=read(fd,bytes+offset,sizeof(bytes)-offset);if(count<0 && errno==EINTR)continue;if(count<=0){close(fd);throw std::runtime_error("cannot create project identity");}offset+=std::size_t(count);}close(fd);
        constexpr char hex[]="0123456789abcdef";std::string id;for(unsigned char byte:bytes){id+=hex[byte>>4];id+=hex[byte&15];}return id;
    }
    static void Write(int directory,const std::string& name,const std::string& contents) {
        struct stat info{};
        const int inspected=fstatat(directory,name.c_str(),&info,AT_SYMLINK_NOFOLLOW);
        if(!inspected && !OwnedFile(info))throw std::runtime_error("refusing to replace an unowned or nonregular project file");
        if(inspected && errno!=ENOENT)throw std::runtime_error("cannot inspect project save destination");
        const std::string temporary=".tmp-"+RandomId();int fd=openat(directory,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)throw std::runtime_error("cannot create project save file");
        try {
            std::size_t offset=0;while(offset<contents.size()){const auto count=write(fd,contents.data()+offset,contents.size()-offset);if(count<0 && errno==EINTR)continue;if(count<=0)throw std::runtime_error("cannot write project save file");offset+=std::size_t(count);}
            if(fsync(fd))throw std::runtime_error("cannot flush project save file");
            close(fd);fd=-1;
            if(renameat(directory,temporary.c_str(),directory,name.c_str()))throw std::runtime_error("cannot replace project save file");
            if(fsync(directory))throw std::runtime_error("cannot flush project directory");
        }catch(...){if(fd>=0)close(fd);unlinkat(directory,temporary.c_str(),0);throw;}
    }
    void RequireKnown(const std::string& id)const {
        if(!ValidId(id) || !entries_.count(id))throw std::invalid_argument("saved project id must come from the refreshed project library");
    }
    Json state_={{"entries",Json::array()},{"error",""},{"revision",0}};
    std::uint64_t revision_=0;std::map<std::string,Json> entries_;
};
} // namespace cs2_live
