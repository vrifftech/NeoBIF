#pragma once
#include "NeoModulePanel.hpp"
#include "core/ResourceQuery.hpp"
#include <neoshared/ResourceDocument.hpp>
namespace neobif::ui {
inline constexpr unsigned kBrowserApiVersion=3;
inline constexpr int kOpenResourceCommandId=wxID_HIGHEST+449;
inline constexpr int kOpenWithFirstCommandId=wxID_HIGHEST+470;
inline constexpr int kOpenWithCommandCapacity=16;
inline constexpr int kOpenArchiveEditorCommandId=wxID_HIGHEST+495;
struct OpenTarget {
    std::string id;
    std::string label;
};
struct OpenHandler {
    std::function<bool(std::uint16_t)> supports;
    std::function<void(neoshared::ResourceDocument)> open;
    std::function<std::vector<OpenTarget>(std::uint16_t)> targets;
    std::function<void(neoshared::ResourceDocument, const std::string&)> openWith;
    OpenHandler() = default;
    OpenHandler(std::function<bool(std::uint16_t)> accepts,
                std::function<void(neoshared::ResourceDocument)> defaultOpen,
                std::function<std::vector<OpenTarget>(std::uint16_t)> choices = {},
                std::function<void(neoshared::ResourceDocument, const std::string&)> selectedOpen = {})
        : supports(std::move(accepts)), open(std::move(defaultOpen)),
          targets(std::move(choices)), openWith(std::move(selectedOpen)) {}

};
class BrowserPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual void openPath(const std::filesystem::path& path)=0;
    virtual void setOpenHandler(OpenHandler handler)=0;
    virtual bool openResource(const ResourceSelection& selection)=0;
    virtual bool openResource(const ResourceSelection& selection, const std::string& editor)=0;
    virtual std::vector<OpenTarget> openTargets(const ResourceSelection& selection) const=0;
    // Builds the same Open section used by the context menu; no archive reads.
    virtual void appendOpenCommands(wxMenu& menu)=0;
    virtual std::size_t resourceCount() const=0;
    using ArchiveOpenHandler=std::function<void(const std::filesystem::path&,std::vector<std::filesystem::path>)>;
    virtual void setArchiveOpenHandler(ArchiveOpenHandler handler)=0;
    virtual bool openDiscoveredArchive(std::size_t index)=0;
    virtual std::vector<std::filesystem::path> sourcePaths() const=0;
};
BrowserPanel* createBrowserPanel(wxWindow* parent, neomodules::Context context = {});
} // namespace neobif::ui
