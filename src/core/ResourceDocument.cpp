#include "ResourceDocument.hpp"
#include <neoshared/PathUtf8.hpp>
#include <sstream>
#include <stdexcept>
namespace neobif {
neoshared::ResourceDocument readResourceDocument(const KeyBifArchive& key,
    const LooseArchiveCatalog& loose, const ResourceSelection& selected) {
    neoshared::ResourceDocument result;
    if(!key.isOpen()) throw std::runtime_error("Open a KEY/game session before opening a resource.");
    std::filesystem::path source;
    std::uint32_t id{};
    std::size_t memberIndex{};
    bool ready=false;
    result.protectedInputs=key.inputPaths();
    for(const auto& archive:loose.archives())if(!archive.resolvedPath.empty())result.protectedInputs.push_back(archive.resolvedPath);
    if(selected.source==ResourceSource::KeyBif) {
        if(selected.resourceIndex>=key.resources().size())throw std::out_of_range("Invalid BIF resource selection.");
        const auto& item=key.resources()[selected.resourceIndex];
        if(item.bifIndex>=key.bifs().size())throw std::out_of_range("Invalid BIF owner.");
        const auto& owner=key.bifs()[item.bifIndex];
        source=owner.browserBacked?std::filesystem::path(owner.browserRelativePath):owner.resolvedPath;
        result.fileName=item.fileName();result.type=item.type;id=item.resourceId;ready=item.extractable;memberIndex=item.tableIndex;
    } else {
        if(selected.resourceIndex>=loose.resources().size())throw std::out_of_range("Invalid archive resource selection.");
        const auto& item=loose.resources()[selected.resourceIndex];
        if(item.archiveIndex>=loose.archives().size())throw std::out_of_range("Invalid archive owner.");
        const auto& owner=loose.archives()[item.archiveIndex];
        source=owner.browserBacked?std::filesystem::path(owner.browserRelativePath):owner.resolvedPath;
        result.fileName=item.fileName();result.type=item.type;id=item.resourceId;ready=item.extractable;memberIndex=item.archiveResourceIndex;
    }
    if(!ready)throw std::runtime_error("The selected resource is unavailable or inconsistent. Rescan before opening it.");
    // Length-prefix components: separators inside a path cannot collide with
    // another tuple. Include the archive, not just ResRef or global row index.
    const auto component=[](const std::string& value){return std::to_string(value.size())+":"+value;};
    const auto normalized=[](const std::filesystem::path& p){
        std::error_code ec;auto result=std::filesystem::weakly_canonical(p,ec);
        return neoshared::genericPathToUtf8(ec?p.lexically_normal():result);
    };
    result.identity="neobif:"+component(normalized(key.keyPath()))+
        component(normalized(source))+component(std::to_string(int(selected.source)))+
        component(std::to_string(memberIndex))+component(std::to_string(id))+component(std::to_string(result.type))+
        component(result.fileName);
    result.sourceDescription=neoshared::genericPathToUtf8(source)+" :: "+result.fileName+" (archive snapshot; Save As creates a separate file)";
    std::string error;
    const bool ok=selected.source==ResourceSource::KeyBif?
        key.readResource(selected.resourceIndex,result.bytes,error):
        loose.readResource(selected.resourceIndex,result.bytes,error);
    if(!ok)throw std::runtime_error(error.empty()?"Unable to read the selected archive resource.":error);
    return result;
}
}
