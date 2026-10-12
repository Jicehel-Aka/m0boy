#pragma once
#include "m0boy/system.h"
namespace m0boy::sg1000 {
struct GenerateOptions {
    std::filesystem::path output,meta_templates;
    std::string name;
    TargetId target;
    std::vector<std::filesystem::path> coverage;
    size_t compiled_limit = 1024;
};
bool generate(const std::filesystem::path& rom,const GenerateOptions&,std::string& error);
}
