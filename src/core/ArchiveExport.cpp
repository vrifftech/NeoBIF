#include "core/ArchiveExport.hpp"
#include "core/SafeOutput.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace neobif {
namespace {
namespace fs=std::filesystem;
std::string key(const fs::path& p) {
    auto text=p.lexically_normal().generic_u8string();
    for(auto& ch:text) if(ch>='A'&&ch<='Z') ch=static_cast<char>(ch+'a'-'A');
    return text;
}
std::vector<fs::path> protectedInputs(const std::vector<ExportItem>& items,const ExportOptions& options) {
    std::vector<fs::path> result=options.protectedInputs;
    for(const auto& item:items) result.insert(result.end(),item.sourcePaths.begin(),item.sourcePaths.end());
    std::sort(result.begin(),result.end()); result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}
fs::path numbered(const fs::path& path,std::size_t ordinal) {
    return path.parent_path()/(path.stem().u8string()+"__"+std::to_string(ordinal)+path.extension().u8string());
}
bool consume(const ExportItem& item,const ByteSink& sink,std::string& error,const JobControl& job) {
    std::uint64_t count=0;
    const ByteSink checked=[&](const std::uint8_t* bytes,std::size_t size,std::string& reason) {
        job.check();
        if(count>item.expectedSize || size>item.expectedSize-count) { reason="Reader exceeded the indexed byte count";return false; }
        if(!sink(bytes,size,reason))return false;
        count+=size;return true;
    };
    if(item.stream) { if(!item.stream(checked,error,job))return false; }
    else {
        std::vector<std::uint8_t> bytes;
        if(!item.read || !item.read(bytes,error))return false;
        constexpr std::size_t chunk=256u*1024u;
        for(std::size_t offset=0;offset<bytes.size();) {
            const auto size=std::min(chunk,bytes.size()-offset);
            if(!checked(bytes.data()+offset,size,error))return false;
            offset+=size;
        }
    }
    if(count!=item.expectedSize) { error="Reader returned an unexpected byte count";return false; }
    return true;
}
void u16(std::ostream& s,std::uint16_t x) { const char b[]={char(x),char(x>>8u)};s.write(b,2); }
void u32(std::ostream& s,std::uint32_t x) { const char b[]={char(x),char(x>>8u),char(x>>16u),char(x>>24u)};s.write(b,4); }
std::uint32_t crcUpdate(std::uint32_t crc,const std::uint8_t* data,std::size_t size) {
    static const auto table=[] {
        std::array<std::uint32_t,256> t{};
        for(std::uint32_t i=0;i<t.size();++i) {auto c=i;for(int n=0;n<8;++n)c=(c&1)?0xEDB88320u^(c>>1u):c>>1u;t[i]=c;}return t;
    }();
    for(std::size_t i=0;i<size;++i)crc=table[(crc^data[i])&255u]^(crc>>8u);
    return crc;
}
} // namespace

std::vector<fs::path> makeUniqueExportPaths(const std::vector<fs::path>& paths,const std::vector<std::string>&) {
    std::vector<fs::path> out;std::set<std::string> used;
    for(const auto& path:paths) {auto candidate=path;std::size_t n=2;while(!used.insert(key(candidate)).second)candidate=numbered(path,n++);out.push_back(candidate);}return out;
}

ExportPlan planExportItems(const std::vector<ExportItem>& items,const fs::path& root,const ExportOptions& options) {
    ExportPlan plan;const auto inputs=protectedInputs(items,options);
    std::map<std::string,std::vector<std::size_t>> owners;
    for(std::size_t i=0;i<items.size();++i)owners[key(items[i].relativePath)].push_back(i);
    std::set<std::size_t> clashes;
    for(const auto& [name,indices]:owners) {
        if(indices.size()>1 && options.existing!=ExistingPolicy::KeepBoth)clashes.insert(indices.begin(),indices.end());
        for(auto parent=fs::path(name).parent_path();!parent.empty();parent=parent.parent_path()) {
            const auto other=owners.find(parent.generic_string());
            if(other!=owners.end()) {clashes.insert(indices.begin(),indices.end());clashes.insert(other->second.begin(),other->second.end());}
        }
    }
    std::set<std::string> used;
    for(std::size_t i=0;i<items.size();++i) {
        options.job.check();
        const auto& item=items[i];PlannedExport entry{i,item.relativePath,ExportAction::Conflict,{}};
        try {
            if((!item.read&&!item.stream)||!validExportRelativePath(item.relativePath))throw std::runtime_error("Invalid export item/path");
            if(clashes.count(i))throw std::runtime_error("Several resources claim this output; choose Keep both or a hierarchy that separates them");
            auto target=checkedExportDestination(root,entry.relativePath,inputs);
            if(options.existing==ExistingPolicy::KeepBoth) {
                std::size_t ordinal=2;
                while(used.count(key(entry.relativePath))||fs::exists(target)) {
                    entry.relativePath=numbered(item.relativePath,ordinal++);
                    target=checkedExportDestination(root,entry.relativePath,inputs);
                }
            }
            used.insert(key(entry.relativePath));
            // Use the existing spelling; case-insensitive discovery must never
            // produce an extra same-name file on case-sensitive hosts.
            entry.relativePath=target.lexically_relative(fs::weakly_canonical(fs::absolute(root.empty()?fs::path("."):root)));
            if(fs::exists(target)&&options.existing==ExistingPolicy::Skip) {
                entry.action=ExportAction::Skip;entry.message="Already exists; skipped";
            } else {
                entry.action=ExportAction::Write;
                if(entry.relativePath!=item.relativePath) entry.message="Output name: "+entry.relativePath.generic_string();
            }
        } catch(const std::exception& ex) {entry.message=ex.what();}
        if(entry.action==ExportAction::Write)++plan.writes;else if(entry.action==ExportAction::Skip)++plan.skipped;else ++plan.conflicts;
        plan.entries.push_back(std::move(entry));
    }
    return plan;
}

ExtractionReport extractExportItems(const std::vector<ExportItem>& items,const fs::path& root,const ExportOptions& options) {
    ExtractionReport report;
    try {
        const auto plan=planExportItems(items,root,options);
        const auto inputs=protectedInputs(items,options);
        std::size_t done=0;
        for(const auto& entry:plan.entries) {
            options.job.update(done,items.size(),items[entry.itemIndex].displayName);
            const auto& item=items[entry.itemIndex];
            if(entry.action==ExportAction::Skip)++report.skipped;
            else if(entry.action==ExportAction::Conflict)++report.failed;
            else {
                try {
                    SafeOutput output(root,entry.relativePath,options.existing==ExistingPolicy::Replace,inputs);
                    std::string error;
                    if(!consume(item,[&](const std::uint8_t* data,std::size_t count,std::string& reason) {
                        output.stream().write(reinterpret_cast<const char*>(data),static_cast<std::streamsize>(count));
                        if(!output.stream()){reason="Unable to write staged output";return false;}return true;
                    },error,options.job))throw std::runtime_error(error);
                    options.job.check(); output.commit(); ++report.written;
                } catch(const JobCancelled&) {throw;}
                catch(const std::exception& ex) {++report.failed;report.messages.push_back(item.displayName+": "+ex.what());}
            }
            if(!entry.message.empty())report.messages.push_back(item.displayName+": "+entry.message);
            ++done;options.job.update(done,items.size(),item.displayName);
        }
    } catch(const JobCancelled&) {report.cancelled=true;report.messages.push_back("Cancelled; completed files retained, unfinished output discarded");}
    catch(const std::exception& ex) {++report.failed;report.messages.push_back(ex.what());}
    return report;
}
ExtractionReport extractExportItems(const std::vector<ExportItem>& items,const fs::path& root,bool overwrite) {
    ExportOptions options;options.existing=overwrite?ExistingPolicy::Replace:ExistingPolicy::Skip;
    return extractExportItems(items,root,options);
}

bool writeExportZip(const std::vector<ExportItem>& items,const fs::path& path,std::string& error,const ExportOptions& options) {
    error.clear();
    try {
        options.job.check();
        if(items.empty())throw std::runtime_error("No resources selected");
        if(key(path.extension())!=".zip")throw std::runtime_error("ZIP output must have a .zip extension; input archives cannot be ZIP destinations");
        if(items.size()>65535u)throw std::runtime_error("Classic ZIP supports at most 65,535 entries; extract to a folder or select fewer resources");
        std::vector<fs::path> paths;for(const auto& item:items)paths.push_back(item.relativePath);
        if(options.existing==ExistingPolicy::KeepBoth||options.keepDuplicateNames)paths=makeUniqueExportPaths(paths);
        std::set<std::string> names;std::uint64_t estimated=22;
        for(std::size_t i=0;i<items.size();++i) {
            const auto& item=items[i];const auto name=paths[i].generic_u8string();
            if(!validExportRelativePath(paths[i])||name.size()>65535u||(!item.read&&!item.stream))throw std::runtime_error("Invalid ZIP entry: "+item.displayName);
            if(!names.insert(key(paths[i])).second)throw std::runtime_error("Duplicate ZIP resource name; select Keep both or preserve archive hierarchy");
            estimated+=30u+name.size()+item.expectedSize+16u+46u+name.size();
            if(item.expectedSize>0xFFFFFFFFull||estimated>0xFFFFFFFFull)throw std::runtime_error("Classic ZIP exceeds 4 GiB; extract to a folder instead");
        }
        for(const auto& name:names)for(auto parent=fs::path(name).parent_path();!parent.empty();parent=parent.parent_path())
            if(names.count(parent.generic_string()))throw std::runtime_error("ZIP file/directory name collision");
        const auto inputs=protectedInputs(items,options);
        auto target=path;
        // ZIP destination is explicitly chosen; never silently rename it.
        SafeOutput output(target.parent_path(),target.filename(),options.existing==ExistingPolicy::Replace,inputs);
        auto& s=output.stream();
        struct Central {std::string name;std::uint32_t crc,size,offset;};std::vector<Central> central;
        for(std::size_t i=0;i<items.size();++i) {
            const auto& item=items[i];options.job.update(i,items.size(),item.displayName);
            const auto name=paths[i].generic_u8string();const auto offset=static_cast<std::uint32_t>(s.tellp());
            u32(s,0x04034B50u);u16(s,20);u16(s,0x0808);u16(s,0);u16(s,0);u16(s,0x21);
            u32(s,0);u32(s,0);u32(s,0);u16(s,static_cast<std::uint16_t>(name.size()));u16(s,0);s.write(name.data(),static_cast<std::streamsize>(name.size()));
            std::uint32_t crc=0xFFFFFFFFu;
            if(!consume(item,[&](const std::uint8_t* bytes,std::size_t size,std::string& reason) {
                crc=crcUpdate(crc,bytes,size);s.write(reinterpret_cast<const char*>(bytes),static_cast<std::streamsize>(size));
                if(!s){reason="Unable to write ZIP payload";return false;}return true;
            },error,options.job))throw std::runtime_error(item.displayName+": "+error);
            crc^=0xFFFFFFFFu;const auto size=static_cast<std::uint32_t>(item.expectedSize);
            u32(s,0x08074b50);u32(s,crc);u32(s,size);u32(s,size);central.push_back({name,crc,size,offset});
            options.job.update(i+1,items.size(),item.displayName);
        }
        const auto centralOffset=static_cast<std::uint32_t>(s.tellp());
        for(const auto& entry:central) {
            options.job.check();u32(s,0x02014B50);u16(s,20);u16(s,20);u16(s,0x0808);u16(s,0);u16(s,0);u16(s,0x21);
            u32(s,entry.crc);u32(s,entry.size);u32(s,entry.size);u16(s,static_cast<std::uint16_t>(entry.name.size()));
            u16(s,0);u16(s,0);u16(s,0);u16(s,0);u32(s,0);u32(s,entry.offset);s.write(entry.name.data(),static_cast<std::streamsize>(entry.name.size()));
        }
        const auto centralSize=static_cast<std::uint32_t>(s.tellp())-centralOffset;
        u32(s,0x06054B50);u16(s,0);u16(s,0);u16(s,static_cast<std::uint16_t>(central.size()));u16(s,static_cast<std::uint16_t>(central.size()));u32(s,centralSize);u32(s,centralOffset);u16(s,0);
        options.job.check();output.commit();return true;
    } catch(const std::exception& ex) {error=ex.what();return false;}
}
} // namespace neobif
