#pragma once
#include "appearance_pack.h"

#ifdef CS2_CAMERA_SOURCE2_CVARS
#include "filesystem.h"
#include "interfaces/interfaces.h"
#include <dlfcn.h>

namespace cs2_live {
inline AppearancePack::Callbacks Source2AppearanceBackend(CreateInterfaceFn factory,
        const std::function<void(const std::string&)>& execute,
        const std::function<bool()>& refresh={}) {
    AppearancePack::Callbacks callbacks;
    auto* filesystem=factory?static_cast<IFileSystem*>(factory(FILESYSTEM_INTERFACE_VERSION,nullptr)):nullptr;
    if(!filesystem) {
        if(void* module=dlopen("libfilesystem_stdio.so",RTLD_NOW|RTLD_NOLOAD)) {
            auto query=reinterpret_cast<CreateInterfaceFn>(dlsym(module,"CreateInterface"));
            if(query)filesystem=static_cast<IFileSystem*>(query(FILESYSTEM_INTERFACE_VERSION,nullptr));
            dlclose(module);
        }
    }
    if(filesystem) {
        callbacks.mount=[filesystem](const std::string& path) {
            // Current Linux FS sorts priority groups descending. Loose1 is
            // always behind shipped VPK2 even at HEAD. Priority2 puts this
            // directory before that group; it does not change its path type.
            filesystem->AddSearchPath(path.c_str(),"GAME",PATH_ADD_TO_HEAD,SEARCH_PATH_PRIORITY_VPK,0);
            return true; // Apply verifies actual GAME reads, not this void API.
        };
        callbacks.unmount=[filesystem](const std::string& path) {return filesystem->RemoveSearchPath(path.c_str(),"GAME");};
        callbacks.readGame=[filesystem](const std::string& path) {
            FileHandle_t file=filesystem->Open(path.c_str(),"rb","GAME");
            if(!file)throw std::runtime_error("GAME cannot open appearance resource: "+path);
            try {
                const auto size=filesystem->Size(file);
                if(!size || size>AppearancePack::MaxModelBytes)throw std::runtime_error("GAME appearance resource exceeds its read limit");
                AppearancePack::Bytes bytes(size);std::size_t offset=0;
                while(offset<bytes.size()) {
                    const auto count=filesystem->Read(bytes.data()+offset,int(bytes.size()-offset),file);
                    if(count<=0)throw std::runtime_error("GAME appearance resource read was incomplete: "+path);
                    offset+=std::size_t(count);
                }
                filesystem->Close(file);return bytes;
            }catch(...){filesystem->Close(file);throw;}
        };
        callbacks.describe=[filesystem](const std::string& path) {
            AppearancePack::Json result={{"path_id","GAME"},{"add_type",int(PATH_ADD_TO_HEAD)},
                {"priority",int(SEARCH_PATH_PRIORITY_VPK)}};
            CBufferString paths;
            result["search_paths_available"]=filesystem->GetSearchPath("GAME",GET_SEARCH_PATH_ALL,paths,128);
            result["search_paths"]=std::string(paths.Get()).substr(0,16384);
            CBufferString resolved;PathTypeQuery_t type=PATH_IS_NORMAL;
            const auto* selected=filesystem->RelativePathToFullPath(path.c_str(),"GAME",resolved,FILTER_NONE,&type);
            result["selected_path"]=selected?std::string(selected).substr(0,4096):std::string();
            result["selected_type"]=int(type);
            CBufferString loose;
            const auto* selectedLoose=filesystem->RelativePathToFullPath(path.c_str(),"GAME",loose,FILTER_CULLPACK,nullptr);
            result["selected_loose_path"]=selectedLoose?std::string(selectedLoose).substr(0,4096):std::string();
            return result;
        };
    }
    (void)execute;
    // Actual retail proof crashed in reload_model at mesh+0x5b07d because its
    // resource-system global is null. A registered tool command alone cannot
    // establish a safe runtime refresh path. Keep filesystem probing separate
    // until a real resource refresh/demo reload adapter has been validated.
    callbacks.reloadReason="reload_model is unsafe in the validated retail build; resource refresh adapter unavailable";
    callbacks.refresh=refresh;
    if(refresh)callbacks.reloadReason="appearance refresh reopens the current demo in the same game process; rendered result requires verification";
    return callbacks;
}
} // namespace cs2_live
#endif
