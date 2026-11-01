#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>
#include <string>

namespace cs2_live {
using LensJson=nlohmann::json;
inline LensJson NormalizeLens(const LensJson& input) {
    if(!input.is_object())throw std::invalid_argument("lens must be an object");
    LensJson value={{"enabled",false},{"distortion",0.0},{"vignette",0.0},{"chromatic",0.0}};
    for(auto field=input.begin();field!=input.end();++field) {
        if(!value.contains(field.key()))throw std::invalid_argument("unknown lens setting: "+field.key());
        if(field.key()=="enabled") {
            if(!field->is_boolean())throw std::invalid_argument("lens enabled must be boolean");
        }else {
            if(!field->is_number())throw std::invalid_argument("lens values must be finite numbers");
            const double number=field->get<double>();
            const double low=field.key()=="distortion"?-.75:0;
            const double high=field.key()=="distortion"?.75:field.key()=="vignette"?1:8;
            if(!std::isfinite(number)||number<low||number>high)throw std::invalid_argument("lens value is outside its supported range");
        }
        value[field.key()]=field.value();
    }
    return value;
}
inline bool LensActive(const LensJson& value) {
    const auto lens=NormalizeLens(value);
    return lens["enabled"].get<bool>() && (lens["distortion"].get<double>()!=0 ||
        lens["vignette"].get<double>()!=0 || lens["chromatic"].get<double>()!=0);
}
} // namespace cs2_live
