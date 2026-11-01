#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>
namespace cs2_live {
// Bounded 2D illustration only. Never accepted as 3D collision proof.
inline nlohmann::json ValidateMapPreview(const nlohmann::json& value){
    using Json=nlohmann::json;
    if(!value.is_object() || value.dump().size()>384*1024)throw std::invalid_argument("map preview exceeds its bounded object format");
    Json result=Json::object();
    for(const auto* field:{"map","status","reason","source_sha256"})if(value.contains(field)){
        if(!value[field].is_string() || value[field].get<std::string>().size()>(std::string(field)=="reason"?1024:256))throw std::invalid_argument("invalid map preview label");
        result[field]=value[field];
    }
    auto numbers=[](const Json& a,std::size_t count){
        if(!a.is_array() || a.size()!=count)throw std::invalid_argument("invalid map preview coordinates");
        for(const auto& n:a)if(!n.is_number() || !std::isfinite(n.get<double>()) || std::abs(n.get<double>())>1000000)throw std::invalid_argument("non-finite map preview coordinates");
    };
    if(value.contains("bounds")){numbers(value["bounds"],4);if(value["bounds"][0].get<double>()>=value["bounds"][2].get<double>() || value["bounds"][1].get<double>()>=value["bounds"][3].get<double>())throw std::invalid_argument("empty map preview bounds");result["bounds"]=value["bounds"];}
    result["layers"]=Json::array();
    if(value.contains("layers")){
        if(!value["layers"].is_array() || value["layers"].size()>8)throw std::invalid_argument("map preview requires at most eight height slices");
        for(const auto& layer:value["layers"]){
            if(!layer.is_object() || !layer.contains("z") || !layer.contains("lines"))throw std::invalid_argument("invalid map preview height slice");
            numbers(Json::array({layer["z"]}),1);
            if(!layer["lines"].is_array() || layer["lines"].size()>1000)throw std::invalid_argument("map preview slice has too many segments");
            for(const auto& line:layer["lines"])numbers(line,4);
            result["layers"].push_back({{"z",layer["z"]},{"lines",layer["lines"]}});
        }
    }
    return result;
}
}
