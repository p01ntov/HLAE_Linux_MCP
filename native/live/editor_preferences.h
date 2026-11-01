#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace cs2_live {
inline nlohmann::json EditorPreferenceDefaults() {
    return {{"time_unit","seconds"},{"frame_rate",60},{"snap_mode","tick"},{"resize_mode","keep"},
        {"compact_transport",true},{"ui_scale",1.0},{"inspector_width",430},{"panel_opacity",.94},
        {"blur_enabled",true},{"show_key_labels",true},{"show_tooltips",true},
        {"timeline_position",true},{"timeline_rotation",true},{"timeline_fov",true},{"timeline_height",240},
        {"workspace_source","camera"},{"workspace_tool","timeline"},{"workspace_open",true},
        {"inspector_section","camera"},{"demo_navigation_pinned",false}};
}
inline nlohmann::json ValidateEditorPreferences(const nlohmann::json& patch,const nlohmann::json& base=EditorPreferenceDefaults()) {
    using Json=nlohmann::json;const Json defaults=EditorPreferenceDefaults();Json result=defaults;
    auto apply=[&](const Json& fields) {
        if(!fields.is_object())throw std::invalid_argument("editor preferences must be a flat object");
        for(auto entry=fields.begin();entry!=fields.end();++entry) {
            const auto& name=entry.key();const auto& value=entry.value();
            if(!result.contains(name))throw std::invalid_argument("unknown editor preference: "+name);
            const auto& defaultValue=defaults[name];
            if(defaultValue.is_boolean()) {
                if(!value.is_boolean())throw std::invalid_argument(name+" must be boolean");
            } else if(defaultValue.is_string()) {
                if(!value.is_string())throw std::invalid_argument(name+" must be a string enum");
                const auto text=value.get<std::string>();
                if((name=="time_unit" && text!="seconds" && text!="ticks" && text!="frames") ||
                   (name=="snap_mode" && text!="tick" && text!="frame" && text!="off") ||
                   (name=="resize_mode" && text!="keep" && text!="scale") ||
                   (name=="workspace_source" && text!="demo" && text!="camera") ||
                   (name=="workspace_tool" && text!="timeline" && text!="map" && text!="curves") ||
                   (name=="inspector_section" && text!="camera" && text!="shot" && text!="scene"))throw std::invalid_argument("invalid editor preference enum: "+name);
            } else {
                if(!value.is_number() || (defaultValue.is_number_integer() && !value.is_number_integer()))
                    throw std::invalid_argument(name+" has an invalid numeric type");
                const double number=value.get<double>();double low=0,high=0;
                if(name=="frame_rate"){low=1;high=1000;}
                else if(name=="ui_scale"){low=.75;high=1.5;}
                else if(name=="inspector_width"){low=320;high=640;}
                else if(name=="panel_opacity"){low=.3;high=1;}
                else if(name=="timeline_height"){low=160;high=420;}
                if(!std::isfinite(number) || number<low || number>high)throw std::invalid_argument(name+" is outside its preference range");
            }
            result[name]=value;
        }
    };
    apply(base);apply(patch);return result;
}
inline nlohmann::json LoadEditorPreferencesFile(const char* path) {
    if(!path || !*path)return EditorPreferenceDefaults();
    if(path[0]!='/')throw std::invalid_argument("editor preference file must use an absolute path");
    int descriptor=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(descriptor<0) {
        if(errno==ENOENT)return EditorPreferenceDefaults();
        throw std::runtime_error("cannot open editor preference file");
    }
    std::string contents;
    try {
        struct stat info{};
        if(fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_size<0 || info.st_size>16384)
            throw std::invalid_argument("editor preference file must be a regular file no larger than 16 KiB");
        char bytes[2048];
        for(;;){const auto count=read(descriptor,bytes,sizeof(bytes));if(count<0 && errno==EINTR)continue;
            if(count<0)throw std::runtime_error("cannot read editor preference file");
            if(!count)break;
            contents.append(bytes,std::size_t(count));if(contents.size()>16384)throw std::invalid_argument("editor preference file exceeds 16 KiB");}
        close(descriptor);descriptor=-1;
    } catch(...) {if(descriptor>=0)close(descriptor);throw;}
    const auto document=nlohmann::json::parse(contents);
    if(!document.is_object() || document.size()!=2 || !document.contains("version") || !document["version"].is_number_integer() ||
       document["version"]!=1 || !document.contains("preferences"))throw std::invalid_argument("unsupported editor preference document");
    return ValidateEditorPreferences(document["preferences"]);
}
} // namespace cs2_live
