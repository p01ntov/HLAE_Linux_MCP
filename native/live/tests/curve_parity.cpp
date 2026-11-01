#include "../live_controller.h"
#include <iostream>

// Offline test helper: JSON on stdin, exact production controller math on
// stdout. No Controller instance, sockets, game launch, filesystem or input.
int main() {
    using namespace cs2_live;
    try {
        Json input;std::cin>>input;Json output={{"cases",Json::array()}};
        for(const auto& entry:input.at("cases")) {
            const auto project=ReadProject(entry.at("project"));
            Json row={{"name",entry.at("name")},{"project",entry.at("project")},{"samples",Json::array()},{"controls",Json::array()}};
            for(const auto& time:entry.at("times")) {
                auto pose=ToJson(Sample(project,time.get<double>()));pose["time"]=time;row["samples"].push_back(pose);
            }
            for(std::size_t i=0;i+1<project.keys.size();++i)
                row["controls"].push_back({{"position",PositionControls(project,i)},
                    {"rotation",RotationControls(project,i)},{"fov",FovControls(project,i)}});
            output["cases"].push_back(std::move(row));
        }
        std::cout<<output.dump(2)<<'\n';
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
