#include "BrowserPanel.hpp"
#include <neoshared/bif/ResourceDocument.hpp>
#include <wx/wrapsizer.h>
#include "core/ArchiveExport.hpp"
#include <neoshared/bif/KeyBifArchive.hpp>
#include <neoshared/bif/LooseArchiveCatalog.hpp>
#include <neoshared/bif/ResourceQuery.hpp>
#include "core/Version.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoWxUi.hpp"
#include <neoshared/PathUtf8.hpp>
#include <neoshared/game/OverrideAliases.hpp>

#if defined(__EMSCRIPTEN__)
#include "NeoBrowserFiles.hpp"
#include <emscripten.h>
static_assert(neobrowser::kBrowserFileApiVersion >= 11u,
              "NeoBIF requires NeoShared browser API 11 for safe extraction conflict handling.");
#endif

#include <wx/app.h>
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/weakref.h>
#include <wx/progdlg.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/config.h>
#include <wx/utils.h>
#if !defined(__EMSCRIPTEN__)
#include <atomic>
#include <mutex>
#include <thread>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

#if !defined(__EMSCRIPTEN__)
constexpr const char* kArchiveWildcard =
    "Odyssey archive files (*.key;*.bif;*.bzf)|*.key;*.bif;*.bzf|KEY files (*.key)|*.key|BIF files (*.bif;*.bzf)|*.bif;*.bzf|All files (*.*)|*.*";
constexpr const char* kBifWildcard =
    "BIF files (*.bif;*.bzf)|*.bif;*.bzf|All files (*.*)|*.*";
constexpr const char* kAllFilesWildcard = "All files (*.*)|*.*";
constexpr const char* kZipWildcard = "ZIP archives (*.zip)|*.zip|All files (*.*)|*.*";
#endif
constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();


std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool hasExtension(const std::filesystem::path& path, const std::string& extension) {
    const auto actual = lowerAscii(neoshared::pathToUtf8(path.extension()));
    return actual == lowerAscii(extension) || (actual == ".bzf" && extension == ".bif");
}

using neoshared::bif::ResourceSource;
using neoshared::bif::ResourceSelection;

enum class NodeKind {
    Root,
    Bif,
    LooseRoot,
    Directory,
    LooseArchive,
    Type,
    Resource,
    Page,
};

#if !defined(__EMSCRIPTEN__)
struct IndexJob {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::unique_ptr<neoshared::bif::KeyBifArchive> key;
    std::unique_ptr<neoshared::bif::LooseArchiveCatalog> loose;
    neoshared::override_aliases::Game game{};
    std::string progress, error;
    bool finished{};
    std::thread worker;

    ~IndexJob() {
        cancelled.store(true);
        if (worker.joinable()) worker.join();
    }
};
#endif

struct GameSession {
    std::size_t id{};
    neoshared::bif::KeyBifArchive archive;
    neoshared::bif::LooseArchiveCatalog looseArchives;
    std::filesystem::path keyPath, scanRoot;
    neoshared::override_aliases::Game game{neoshared::override_aliases::Game::Auto};
    std::string name, progress, error;
    std::vector<std::filesystem::path> supplementaryFiles;
    std::map<std::size_t, std::filesystem::path> explicitRelocations;
    std::vector<bool> visibleBif, visibleLoose;
    wxTreeItemId treeRoot;
#if !defined(__EMSCRIPTEN__)
    std::unique_ptr<IndexJob> indexing;
#endif
};

class NodeData final : public wxTreeItemData {
public:
    NodeData(GameSession* gameSession, NodeKind nodeKind,
             std::size_t owner = kNoIndex,
             std::size_t resource = kNoIndex,
             std::uint16_t resourceType = 0u,
             neoshared::bif::ResourceSource resourceSource = neoshared::bif::ResourceSource::KeyBif,
             std::string directory = {})
        : session(gameSession), kind(nodeKind), source(resourceSource), ownerIndex(owner),
          resourceIndex(resource), type(resourceType),
          directoryPath(std::move(directory)) {}

    GameSession* session;
    NodeKind kind;
    neoshared::bif::ResourceSource source;
    std::size_t ownerIndex;
    std::size_t resourceIndex;
    std::uint16_t type;
    std::string directoryPath;
    std::shared_ptr<const std::vector<std::size_t>> lazyResources;
    bool populated{};
};

enum {
    ID_OpenArchive = wxID_HIGHEST + 410,
    ID_AddBifs,
    ID_ScanDirectory,
    ID_CloseArchive,
    ID_ExtractSelected,
    ID_ExtractAll,
    ID_ZipSelected,
    ID_ZipAll,
    ID_ExpandAll,
    ID_CollapseAll,
    ID_GroupByType,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_FilterTimer,
    ID_IndexTimer,
    ID_ExtractBranch,
    ID_ZipBranch,
    ID_CopyResref,
    ID_CopySource,
    ID_LocateSource,
    ID_Rescan,
    ID_FindResource,
    ID_ExportByType,
    ID_OpenResource=neobif::ui::kOpenResourceCommandId, ID_ModuleAbout, ID_ModuleExit,
};

struct NeoBIFGuiProbe;
class NeoBIFPanelImpl final : public neobif::ui::BrowserPanel {
    friend struct NeoBIFGuiProbe;
public:
    NeoBIFPanelImpl(wxWindow* parent, neomodules::Context context = {})
        : BrowserPanel(parent,std::move(context)),
          filterTimer_(this, ID_FilterTimer),
          indexTimer_(this, ID_IndexTimer),
          groupByType_(settings_.readBool("View/GroupByType", true)),
          darkMode_(wxui::readDarkMode("NeoBIF")) {
        addSession();
        buildMenus();
        buildUi();
        bindEvents();
        createModuleStatusBar(2);
        int widths[] = {-3, -2};
        moduleStatusBar()->SetStatusWidths(2, widths);
        fontScale_ = settings_.fontScale();
        fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
        updateStatus();
#if !defined(__EMSCRIPTEN__)
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        CallAfter([weak] { if (weak) weak->openInstalledGames(); });
#endif
    }

    ~NeoBIFPanelImpl() override {
        indexTimer_.Stop();
        sessions_.clear();
#if defined(__EMSCRIPTEN__)
        releaseBrowserSessions();
#endif
    }

    void openPath(const std::filesystem::path& path) override {
        if(jobRunning_)return;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec) && !ec) {
            scanDirectory(path);
            return;
        }
        if (hasExtension(path, ".key")) {
            openArchive(path, {}, path.parent_path());
            return;
        }
        wxMessageBox("NeoBIF expects a chitin.key file or a game directory.",
                     "Unsupported Input", wxOK | wxICON_ERROR, this);
    }

    void openInstalledGames() {
#if !defined(__EMSCRIPTEN__)
        try {
            const auto installations = neogames::GameInstallResolver{}.resolveAllInstalls();
            for (const auto& install : installations) {
                if (install.id != "kotor" && install.id != "kotor2" && install.id != "jade") continue;
                if (install.installPath.empty()) continue;
                const auto key = neoshared::game::firstExisting(install.installPath, {"chitin.key"});
                if (key.empty()) {
                    logScan(install.installPath, "Error: chitin.key was not found.");
                    continue;
                }
                openArchive(key, {}, install.installPath);
                const auto* game = neogames::findGame(install.id);
                session().name = game->displayName;
            }
            rebuildTree();
        } catch (const std::exception& ex) {
            logScan({}, std::string("Installation discovery failed: ") + ex.what());
        }
#endif
    }

    void setOpenHandler(neobif::ui::OpenHandler handler) override {openHandler_=std::move(handler);}
    std::size_t resourceCount() const override {return session().archive.resources().size()+session().looseArchives.resources().size();}
    std::optional<neoshared::ResourceDocument> resolveResource(
        const std::string& resref, std::uint16_t type) const override {
        if (!indexComplete()) return neoshared::bif::resolveResourceDocument(session().archive, {}, resref, type);
        return neoshared::bif::resolveResourceDocument(session().archive,session().looseArchives,resref,type);
    }
    neoshared::ResourceResolver captureResourceResolver() const override {
        // A partial index exposes KEY/BIF resources; it must not mix module
        // resources from an older catalog while a rescan is in progress.
        if (!indexComplete()) return neoshared::bif::captureResourceResolver(session().archive, {});
        return neoshared::bif::captureResourceResolver(session().archive, session().looseArchives);
    }
    bool canClose() override {
#if !defined(__EMSCRIPTEN__)
        if(jobRunning_){cancelJob_.store(true);return false;}
#else
        if(jobRunning_||browserIndexing_)return false;
#endif
        return true;
    }
    void setAppearance(bool dark,double scale) override {darkMode_=dark;fontScale_=scale;applyDarkMode();}
    bool openResource(const neoshared::bif::ResourceSelection& selected) override {
        return openResource(selected, {});
    }
    std::vector<neobif::ui::OpenTarget> openTargets(const neoshared::bif::ResourceSelection& selected) const override {
        if (!canOpen(selected) || !openHandler_.openWith) return {};
        if (selected.source == neoshared::bif::ResourceSource::KeyBif) {
            const auto& resource = session().archive.resources()[selected.resourceIndex];
            if (openHandler_.extensionTargets)
                return openHandler_.extensionTargets(resource.extension);
            return openHandler_.targets ? openHandler_.targets(resource.type)
                                        : std::vector<neobif::ui::OpenTarget>{};
        }
        const auto& resource = session().looseArchives.resources()[selected.resourceIndex];
        if (openHandler_.extensionTargets)
            return openHandler_.extensionTargets(resource.extension);
        return openHandler_.targets ? openHandler_.targets(resource.type)
                                    : std::vector<neobif::ui::OpenTarget>{};
    }
    bool openResource(const neoshared::bif::ResourceSelection& selected, const std::string& editor) override {
        if(jobRunning_||!canOpen(selected))return false;
        if (!editor.empty()) {
            const auto choices=openTargets(selected);
            if (std::none_of(choices.begin(),choices.end(),[&](const auto& c){return c.id==editor;})) return false;
        }
        try {
            neoshared::ResourceDocument document;
#if !defined(__EMSCRIPTEN__)
            if(!runJob("Opening resource",[&](const neoshared::bif::JobControl& job){
                job.check();document=neoshared::bif::readResourceDocument(session().archive,session().looseArchives,selected);job.check();
            }))return false;
#else
            document=neoshared::bif::readResourceDocument(session().archive,session().looseArchives,selected);
#endif
            if (editor.empty()) openHandler_.open(std::move(document));
            else openHandler_.openWith(std::move(document),editor);
            return true;
        }catch(const std::exception& ex){wxui::showError(this,ex);return false;}
    }

    std::filesystem::path activeKeyPath() const override { return session().keyPath; }
    bool indexComplete() const override {
        return session().archive.isOpen() && session().progress.empty() && session().error.empty();
    }
    void setArchiveOpenHandler(ArchiveOpenHandler handler) override {archiveOpen_=std::move(handler);}
    std::vector<std::filesystem::path> sourcePaths() const override {return allInputPaths();}
    bool openDiscoveredArchive(std::size_t index) override {
        if(jobRunning_||!archiveOpen_||index>=session().looseArchives.archives().size())return false;
        const auto& entry=session().looseArchives.archives()[index];
        if(!entry.valid||entry.browserBacked||entry.overrideDirectory)return false;
        archiveOpen_(entry.resolvedPath,allInputPaths());return true;
    }
    void appendOpenCommands(wxMenu& menu) override {
        contextOpenSelections_.clear();
        contextOpenTargets_.clear();

        wxArrayTreeItemIds nodes;
        tree_->GetSelections(nodes);
        const auto* node = nodes.size() == 1
            ? static_cast<NodeData*>(tree_->GetItemData(nodes.front()))
            : nullptr;
        if (node && node->kind == NodeKind::LooseArchive && archiveOpen_) {
            const auto index = node->ownerIndex;
            const bool enabled = !jobRunning_ &&
                index < session().looseArchives.archives().size() &&
                session().looseArchives.archives()[index].valid &&
                !session().looseArchives.archives()[index].browserBacked &&
                !session().looseArchives.archives()[index].overrideDirectory;
            if (enabled) {
                menu.Append(neobif::ui::kOpenArchiveEditorCommandId, "Open with NeoERF");
                menu.AppendSeparator();
            }
            return;
        }
        if (!selectedNodesAreResources() || !openHandler_.open) return;

        contextOpenSelections_ = selectedResources();
        const auto& selections = contextOpenSelections_;
        const bool enabled = !jobRunning_ && !selections.empty() &&
            std::all_of(selections.begin(), selections.end(),
                        [this](const auto& resource) { return canOpen(resource); });
        if (!enabled) return;

        contextOpenTargets_ = openTargets(selections.front());
        for (const auto& selected : selections) {
            const auto available = openTargets(selected);
            contextOpenTargets_.erase(
                std::remove_if(contextOpenTargets_.begin(), contextOpenTargets_.end(),
                    [&](const auto& choice) {
                        return std::none_of(available.begin(), available.end(),
                            [&](const auto& other) { return other.id == choice.id; });
                    }),
                contextOpenTargets_.end());
        }
        // DLG/JRL expose the requested two explicit commands, in default-first order.
        // Mixed resource selections can always use the ordinary default-per-resource Open.
        if (contextOpenTargets_.size() < 2) menu.Append(ID_OpenResource, "&Open");
        if (contextOpenTargets_.size() > 1 ||
            (contextOpenTargets_.size() == 1 && selections.size() > 1)) {
            const auto count = std::min(
                contextOpenTargets_.size(),
                static_cast<std::size_t>(neobif::ui::kOpenWithCommandCapacity));
            for (std::size_t i = 0; i < count; ++i) {
                menu.Append(neobif::ui::kOpenWithFirstCommandId + static_cast<int>(i),
                            wxui::toWx(contextOpenTargets_[i].label));
            }
        }
        menu.AppendSeparator();
    }

private:
    ArchiveOpenHandler archiveOpen_;
    neobif::ui::OpenHandler openHandler_;
    std::vector<neobif::ui::OpenTarget> contextOpenTargets_;
    std::vector<neoshared::bif::ResourceSelection> contextOpenSelections_;
    bool canOpen(const neoshared::bif::ResourceSelection& selected) const {
        if(!openHandler_.open||!resourceIsExtractable(selected))return false;
        if (selected.source == neoshared::bif::ResourceSource::KeyBif) {
            const auto& resource = session().archive.resources()[selected.resourceIndex];
            if (openHandler_.supportsExtension)
                return openHandler_.supportsExtension(resource.extension);
            return openHandler_.supports && openHandler_.supports(resource.type);
        }
        const auto& resource = session().looseArchives.resources()[selected.resourceIndex];
        if (openHandler_.supportsExtension)
            return openHandler_.supportsExtension(resource.extension);
        return openHandler_.supports && openHandler_.supports(resource.type);
    }
    std::vector<std::unique_ptr<GameSession>> sessions_;
    GameSession* activeSession_{};
    std::size_t nextSessionId_{};
    GameSession& session() { return *activeSession_; }
    const GameSession& session() const { return *activeSession_; }
#if defined(__EMSCRIPTEN__)
    std::vector<neoshared::bif::BrowserArchiveFile> browserFiles_;
    std::vector<std::uint32_t> browserSessions_;
    std::uint64_t browserGeneration_{};
    std::uint64_t browserSelectionRequest_{};
    bool browserIndexing_{};
#endif
    wxSearchCtrl* filter_{nullptr};
    wxStaticText* searchCount_{nullptr};
    wxSplitterWindow* splitter_{nullptr};
    wxTreeCtrl* tree_{nullptr};
    wxTextCtrl* details_{nullptr};
    wxStaticText* keyLabel_{nullptr};
    wxButton* openDirectoryButton_{nullptr};
    wxButton* openFilesButton_{nullptr};
    wxButton* addBifsButton_{nullptr};
    wxButton* extractButton_{nullptr};
    wxMenuItem* groupByTypeItem_{nullptr};
    wxMenuItem* darkModeItem_{nullptr};
    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;
    neosettings::AppSettings settings_{"NeoBIF"};
    wxTimer filterTimer_;
    wxTimer indexTimer_;
    wxTextCtrl* scanLog_{};
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool groupByType_{true};
    bool darkMode_{};
    bool rebuilding_{};
    bool jobRunning_{};
    std::set<std::string> expandedNodes_,selectedNodes_;
#if !defined(__EMSCRIPTEN__)
    std::atomic<bool> cancelJob_{false};
#endif

    void addSession() {
        auto game = std::make_unique<GameSession>();
        game->id = nextSessionId_++;
        activeSession_ = game.get();
        sessions_.push_back(std::move(game));
    }

    static std::string sessionName(const GameSession& game) {
        if (!game.name.empty()) return game.name;
        using Game = neoshared::override_aliases::Game;
        if (game.game == Game::Kotor2) return "KotOR II";
        if (game.game == Game::Kotor) return "KotOR I";
        if (game.archive.isOpen()) return neoshared::bif::gameProfileName(game.archive.gameProfile());
        return neoshared::pathToUtf8(game.scanRoot.filename());
    }

    static wxString sessionTreeLabel(const GameSession& game) {
        std::string label = sessionName(game) + " - " + neoshared::pathToUtf8(game.scanRoot);
        if (!game.error.empty()) label += " [scan failed]";
        else if (!game.progress.empty()) label += " [loading]";
        return wxui::toWx(label);
    }

    void logScan(const std::filesystem::path& path, const std::string& text) {
        scanLog_->AppendText(wxui::toWx((path.empty() ? std::string{} :
            neoshared::pathToUtf8(path) + ": ") + text + "\n"));
        if (!scanLog_->IsShown()) {
            scanLog_->Show();
            scanLog_->GetParent()->Layout();
        }
    }

    void forgetSessionSelection(const GameSession& game) {
        const auto prefix = std::to_string(game.id) + ":";
        for (auto* keys : {&expandedNodes_, &selectedNodes_}) {
            for (auto it = keys->begin(); it != keys->end();) {
                if (it->compare(0, prefix.size(), prefix) == 0) it = keys->erase(it);
                else ++it;
            }
        }
        // Discard the old numeric tree indices before rebuilding the session.
        if (game.treeRoot.IsOk()) {
            rebuilding_ = true;
            tree_->Delete(game.treeRoot);
            rebuilding_ = false;
        }
    }

    void logSessionIssues(const GameSession& game) {
        std::ostringstream messages;
        for (const auto& message : game.looseArchives.messages()) messages << "Warning: " << message << '\n';
        for (const auto& archive : game.looseArchives.archives()) {
            for (const auto& message : archive.messages)
                messages << (archive.valid ? "Warning: " : "Error: ")
                         << neoshared::pathToUtf8(archive.resolvedPath) << ": " << message << '\n';
        }
        for (const auto& resource : game.looseArchives.resources()) {
            if (!resource.extractable)
                messages << "Error: " << (resource.overrideFile ? neoshared::pathToUtf8(resource.directPath) : resource.fileName())
                 << ": " << resource.status << '\n';
        }
        if (!messages.str().empty()) logScan(game.scanRoot, messages.str());
    }

#if !defined(__EMSCRIPTEN__)
    void pollIndexes() {
        // A foreground extraction/read owns its current resource selection.
        if (jobRunning_) return;
        bool changed = false, activeChanged = false, running = false;
        for (auto& game : sessions_) {
            if (!game->indexing) continue;
            auto& job = *game->indexing;
            std::unique_ptr<neoshared::bif::KeyBifArchive> key;
            std::unique_ptr<neoshared::bif::LooseArchiveCatalog> loose;
            bool finished;
            {
                std::lock_guard<std::mutex> lock(job.mutex);
                key = std::move(job.key);
                loose = std::move(job.loose);
                game->progress = job.progress;
                game->error = job.error;
                if (key) game->game = job.game;
                finished = job.finished;
            }
            if (key || loose || finished) activeChanged |= game.get() == activeSession_;
            if (key) {
                forgetSessionSelection(*game);
                game->treeRoot = {};
                game->archive = std::move(*key);
                game->looseArchives.clear();
                logScan(game->scanRoot, "KEY/BIF resources ready; loading game archives and Override in the background.");
                std::ostringstream issues;
                for (const auto& issue : game->archive.issues())
                    issues << (issue.severity == neoshared::bif::IssueSeverity::Error ? "Error: " : "Warning: ")
                           << issue.message << '\n';
                if (!issues.str().empty()) logScan(game->scanRoot, issues.str());
                changed = true;
            }
            if (loose) {
                game->looseArchives = std::move(*loose);
                logSessionIssues(*game);
                logScan(game->scanRoot, "Indexing complete: " +
                    std::to_string(game->archive.resources().size() + game->looseArchives.resources().size()) + " resources.");
                changed = true;
            }
            if (finished) {
                if (!game->error.empty()) logScan(game->scanRoot, "Error: " + game->error);
                game->indexing.reset();
                game->progress.clear();
                changed = true;
            } else {
                running = true;
            }
            if (game->treeRoot.IsOk()) tree_->SetItemText(game->treeRoot, sessionTreeLabel(*game));
        }
        if (!running) indexTimer_.Stop();
        if (activeChanged) notifySessionChanged();
        if (changed) {
            updateSessionLabel();
            rebuildTree();
            updateStatus();
        } else if (!session().progress.empty()) {
            setModuleStatusText(session().progress, 1);
        }
    }
#endif

    void notifySessionChanged() {
        setModuleTitle(appTitle() + " - " + wxui::toWx(sessionName(session())));
    }

    static wxString appTitle() {
        return wxui::toWx(std::string("NeoBIF v") + neobif::kVersion);
    }

    static std::string countText(
        std::size_t count, const char* singular, const char* plural) {
        return std::to_string(count) + " " + (count == 1u ? singular : plural);
    }

    static std::string resourceCountText(std::size_t count) {
        return countText(count, "resource", "resources");
    }

    static wxString emptyStateText() {
        return "Open a game directory or a chitin.key file to browse its resources.\n\n"
               "Select a resource or branch, then use Extract Selection or save it as a ZIP.";
    }

    void updateSessionLabel() {
        if (keyLabel_ == nullptr) return;
        if (session().keyPath.empty()) {
            keyLabel_->SetLabel("No game session open");
            keyLabel_->UnsetToolTip();
            keyLabel_->GetParent()->Layout();
            return;
        }
        const std::filesystem::path displayPath = !session().scanRoot.empty() ? session().scanRoot : session().keyPath;
        const wxString path = neosettings::pathToWx(displayPath);
        const auto gameName = sessionName(session());
        const std::string prefix = gameName +
            (!session().scanRoot.empty() ? " game directory: " : " archive: ");
        keyLabel_->SetLabel(wxui::toWx(prefix) + path);
        keyLabel_->SetToolTip(path);
        keyLabel_->GetParent()->Layout();
    }

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_ScanDirectory, "Open &Game Directory...");
#if !defined(__EMSCRIPTEN__)
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file,
            [this](const neogames::SavedGameDirectory& directory) {
                scanDirectory(directory.path);
            },
            neogames::GameDirectoryGameIds{"kotor", "kotor2", "jade"},
            "Open Saved &Directory");
#endif
        file->Append(ID_OpenArchive, "Open &chitin.key...\tCtrl+O");
        file->Append(ID_AddBifs, "Add / &Relocate BIF Files...");
        file->AppendSeparator();
#if defined(__EMSCRIPTEN__)
        file->Append(ID_Rescan, "Reopen &Game Directory...\tF5");
#else
        file->Append(ID_Rescan, "&Rescan Current Session\tF5");
#endif
        file->Append(ID_CloseArchive, "&Close Session");
        file->AppendSeparator();
        if(!context_.embedded)file->Append(ID_ModuleExit, "E&xit\tCtrl+Q");

        auto* extract = new wxMenu;
        extract->Append(ID_ExtractSelected, "Extract &Selection...\tCtrl+E",
            "Extract resources represented by the current tree selection; Search narrows branch selections");
        extract->Append(ID_ExtractBranch,"Extract Entire Selection (ignore &Search)...",
            "Extract the complete selected branches without applying Search");
        extract->Append(ID_ExtractAll, "Extract &All Resources in Selected Game...");
        extract->AppendSeparator();
        extract->Append(ID_ZipSelected, "Save Selection as &ZIP...",
            "Save resources represented by the current tree selection; Search narrows branch selections");
        extract->Append(ID_ZipBranch,"Save Entire Selection as ZIP (ignore Search)...",
            "Save the complete selected branches without applying Search");
        extract->Append(ID_ZipAll, "Save All Resources in Selected Game as Z&IP...");
        extract->AppendSeparator();
        extract->Append(ID_ExportByType, "Save File &Type as ZIP...",
            "Choose a resource type and export all its entries from the open game session to a ZIP");

        auto* view = new wxMenu;
        view->Append(ID_FindResource, context_.embedded?"&Find resources...\tCtrl+Shift+F":"&Find resources...\tCtrl+F");
        view->AppendSeparator();
        groupByTypeItem_ = view->AppendCheckItem(ID_GroupByType, "Group Resources by &Type");
        groupByTypeItem_->Check(groupByType_);
        view->AppendSeparator();
        view->Append(ID_ExpandAll, "&Expand Archive/Type Branches");
        view->Append(ID_CollapseAll, "&Collapse All");
        view->AppendSeparator();
        if(!context_.embedded) {
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
        darkModeItem_->Check(darkMode_);
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");
        }

        auto* help = new wxMenu;
        help->Append(ID_ModuleAbout, "&About NeoBIF");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(extract, "&Extract");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        setModuleMenus(bar);
    }

    void buildUi() {
        auto* root = new wxPanel(this);
        auto* outer = new wxBoxSizer(wxVERTICAL);

        auto* actionRow = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
        openDirectoryButton_ = new wxButton(root, ID_ScanDirectory, "Open Game Directory...");
        openFilesButton_ = new wxButton(root, ID_OpenArchive, "Open chitin.key...");
        addBifsButton_ = new wxButton(root, ID_AddBifs, "Add / Relocate BIF Files...");
        extractButton_ = new wxButton(root, ID_ExtractSelected, "Extract Selection...");
        openDirectoryButton_->SetToolTip(
            "Recommended: select a supported game installation to index chitin.key, its BIF files, and game archives.");
        openFilesButton_->SetToolTip(
            "Open chitin.key directly. Relocated BIF files can be selected in the same operation.");
        addBifsButton_->SetToolTip(
            "Associate selected BIF files with entries from the currently open chitin.key.");
        extractButton_->SetToolTip(
            "Extract the resources represented by the current tree selection. Search narrows branch selections.");
        actionRow->Add(openDirectoryButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        actionRow->Add(openFilesButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        actionRow->Add(addBifsButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        actionRow->Add(extractButton_, 0, wxRIGHT | wxBOTTOM, FromDIP(6));
        outer->Add(actionRow, 0, wxEXPAND | wxALL, FromDIP(8));

        keyLabel_ = new wxStaticText(root, wxID_ANY, "No game session open",
                                     wxDefaultPosition, wxDefaultSize,
                                     wxST_ELLIPSIZE_MIDDLE);
        keyLabel_->SetName("Current game session");
        keyLabel_->SetMinSize(FromDIP(wxSize(1, -1)));
        outer->Add(keyLabel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        auto* filterBlock = new wxBoxSizer(wxVERTICAL);
        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(root, wxID_ANY, "Search:"), 0,
                       wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        filter_ = new wxSearchCtrl(root, wxID_ANY);
        filter_->SetName("Search resources");
        filter_->SetDescriptiveText("Resource name, file type, source, ID, or status");
        filter_->SetToolTip("Search names, source paths, IDs or status. File types: .tga, tga, *.tga. "
                            "Filename wildcards: p_*.tpc. Ctrl+F focuses Search; Escape clears it.");
        filter_->ShowCancelButton(true);
        filterRow->Add(filter_, 1, wxEXPAND);
        filterBlock->Add(filterRow, 0, wxEXPAND);
        searchCount_ = new wxStaticText(root, wxID_ANY, "No session");
        searchCount_->SetName("Search result count");
        filterBlock->Add(searchCount_, 0, wxALIGN_RIGHT | wxTOP, FromDIP(3));
        outer->Add(filterBlock, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        splitter_ = new wxSplitterWindow(root, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
        tree_ = new wxTreeCtrl(splitter_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_MULTIPLE |
                                   wxTR_FULL_ROW_HIGHLIGHT | wxTR_HIDE_ROOT);
        details_ = new wxTextCtrl(splitter_, wxID_ANY, emptyStateText(),
                                  wxDefaultPosition,
                                  wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP |
                                      wxBORDER_NONE);
        details_->SetName("Selection details");
        const bool compactLayout = context_.compact;
        const std::string splitterSetting = compactLayout
            ? "View/CompactSplitterRatio" : "View/SplitterRatio";
        const double defaultRatio = compactLayout ? 0.8 : 0.57;
        const double splitterRatio = std::clamp(
            settings_.readDouble(splitterSetting, defaultRatio), 0.2, 0.88);
        if(compactLayout) {
            splitter_->SplitHorizontally(tree_,details_,FromDIP(460));
            splitter_->SetMinimumPaneSize(FromDIP(75));
        } else {
            splitter_->SplitVertically(tree_,details_,FromDIP(620));
            splitter_->SetMinimumPaneSize(FromDIP(260));
        }
        splitter_->SetSashGravity(splitterRatio);
        splitter_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED,
            [this, splitterSetting, compactLayout](wxSplitterEvent& event) {
                const wxSize size = splitter_->GetClientSize();
                const int span = compactLayout ? size.GetHeight() : size.GetWidth();
                if (span > 0) {
                    const double ratio = std::clamp(
                        static_cast<double>(event.GetSashPosition()) /
                            static_cast<double>(span),
                        0.2, 0.88);
                    splitter_->SetSashGravity(ratio);
                    settings_.writeDouble(splitterSetting, ratio);
                }
                event.Skip();
            });
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        CallAfter([weak, splitterRatio, compactLayout]() {
            if (!weak || weak->splitter_ == nullptr || !weak->splitter_->IsSplit()) return;
            const wxSize size = weak->splitter_->GetClientSize();
            const int span = compactLayout ? size.GetHeight() : size.GetWidth();
            if (span > 0) {
                weak->splitter_->SetSashPosition(
                    static_cast<int>(static_cast<double>(span) * splitterRatio));
            }
        });
        tree_->SetName("NeoBIF resource tree");
        outer->Add(splitter_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        scanLog_ = new wxTextCtrl(root, wxID_ANY, wxEmptyString,
            wxDefaultPosition, FromDIP(wxSize(-1, 90)),
            wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
        scanLog_->SetName("Indexing status and errors");
        scanLog_->Hide();
        outer->Add(scanLog_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

        root->SetSizer(outer);
        auto* frameSizer = new wxBoxSizer(wxVERTICAL);
        frameSizer->Add(root, 1, wxEXPAND);
        SetSizer(frameSizer);
        updateCommandState();
    }

    void bindEvents() {
        Bind(wxEVT_MENU,[this](wxCommandEvent&){
            const auto* node=selectedNodeData();if(!node||node->kind!=NodeKind::LooseArchive)return;
            try{openDiscoveredArchive(node->ownerIndex);}catch(const std::exception& ex){wxui::showError(this,ex);}
        },neobif::ui::kOpenArchiveEditorCommandId);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestOpenArchiveFiles(); }, ID_OpenArchive);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestOpenArchiveFiles(); }, ID_OpenArchive);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestAddBifs(); }, ID_AddBifs);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestAddBifs(); }, ID_AddBifs);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestScanDirectory(); }, ID_ScanDirectory);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { requestScanDirectory(); }, ID_ScanDirectory);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { closeArchive(); }, ID_CloseArchive);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestModuleClose(); }, ID_ModuleExit);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){
#if !defined(__EMSCRIPTEN__)
            if(!session().keyPath.empty())openArchive(session().keyPath,session().supplementaryFiles,session().scanRoot);
#else
            requestScanDirectory();
#endif
        },ID_Rescan);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){extractSelected(false,false);},ID_ExtractBranch);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){extractSelected(true,false);},ID_ZipBranch);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){copySelection(false);},ID_CopyResref);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){copySelection(true);},ID_CopySource);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){locateSource();},ID_LocateSource);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractSelected(false); }, ID_ExtractSelected);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { extractSelected(false); }, ID_ExtractSelected);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractAll(false); }, ID_ExtractAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractSelected(true); }, ID_ZipSelected);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { extractAll(true); }, ID_ZipAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { exportByFileType(); }, ID_ExportByType);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            filter_->SetFocus();
            filter_->SelectAll();
        }, ID_FindResource);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { expandStructure(tree_->GetRootItem()); }, ID_ExpandAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            tree_->CollapseAll();
            for (const auto& game : sessions_) if (game->treeRoot.IsOk()) tree_->Expand(game->treeRoot);
        }, ID_CollapseAll);
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            groupByType_ = event.IsChecked();
            settings_.writeBool("View/GroupByType", groupByType_);
            rebuildTree();
        }, ID_GroupByType);
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            darkMode_ = event.IsChecked();
            wxui::writeDarkMode("NeoBIF", darkMode_);
            applyDarkMode();
        }, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoBIFPanelImpl::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoBIFPanelImpl::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoBIFPanelImpl::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { showAbout(); }, ID_ModuleAbout);
        Bind(wxEVT_MENU,[this](wxCommandEvent&){
            const auto selections=selectedResources();
            for(const auto& resource:selections)if(canOpen(resource))openResource(resource);
        },ID_OpenResource);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){
            const auto index=static_cast<std::size_t>(event.GetId()-neobif::ui::kOpenWithFirstCommandId);
            if(index>=contextOpenTargets_.size())return;
            const auto editor=contextOpenTargets_[index].id;
            const auto selections=contextOpenSelections_;
            for(const auto& resource:selections)openResource(resource,editor);
        },neobif::ui::kOpenWithFirstCommandId,
          neobif::ui::kOpenWithFirstCommandId+neobif::ui::kOpenWithCommandCapacity-1);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { rebuildTree(); }, ID_FilterTimer);
#if !defined(__EMSCRIPTEN__)
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { pollIndexes(); }, ID_IndexTimer);
#endif
        tree_->Bind(wxEVT_TREE_SEL_CHANGING, [this](wxTreeEvent& event) {
            if (jobRunning_) event.Veto();
            else event.Skip();
        });
        filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { filterTimer_.StartOnce(180); });
        filter_->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, [this](wxCommandEvent&) { clearSearch(); });
        filter_->Bind(wxEVT_SEARCHCTRL_SEARCH_BTN, [this](wxCommandEvent&) { flushSearch(); });
        filter_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { flushSearch(); });
        filter_->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_ESCAPE && !filter_->GetValue().empty()) clearSearch();
            else if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) flushSearch();
            else event.Skip();
        });
        tree_->Bind(wxEVT_CHAR, [this](wxKeyEvent& event) {
            event.Skip();
            const auto key = event.GetUnicodeKey();
            if (key <= WXK_SPACE || key == WXK_DELETE ||
                event.ControlDown() || event.AltDown() || event.MetaDown()) return;
            tree_->CallAfter([tree = tree_]() {
                const auto item = tree->GetFocusedItem();
                if (item.IsOk()) {
                    tree->UnselectAll();
                    tree->SelectItem(item);
                }
            });
        });
        tree_->Bind(wxEVT_TREE_ITEM_EXPANDING,[this](wxTreeEvent& event){populateNode(event.GetItem());});
        tree_->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent& event) {
            if (rebuilding_) {
                event.Skip();
                return;
            }
            const wxTreeItemId item = event.GetItem();
            if (item.IsOk() && tree_->IsSelected(item)) {
                const auto* node = static_cast<NodeData*>(tree_->GetItemData(item));
                if (node && node->session != activeSession_) {
                    activeSession_ = node->session;
                    // Extraction and editor commands always belong to one installation.
                    wxArrayTreeItemIds selected;
                    tree_->GetSelections(selected);
                    for (const auto& other : selected) {
                        const auto* data = static_cast<NodeData*>(tree_->GetItemData(other));
                        if (data->session != activeSession_) tree_->UnselectItem(other);
                    }
                    notifySessionChanged();
                    updateSessionLabel();
                    updateStatus();
                }
                tree_->EnsureVisible(item);
            }
            wxWeakRef<NeoBIFPanelImpl> weak(this);
            CallAfter([weak]() {
                if (!weak || weak->tree_ == nullptr) return;
                wxArrayTreeItemIds selections;
                weak->tree_->GetSelections(selections);
                if (!selections.empty()) weak->tree_->EnsureVisible(selections.back());
            });
            selectedNodes_.clear();
            showSelectedDetails();
            updateCommandState();
            event.Skip();
        });
        tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent& event) {
            const wxTreeItemId item = event.GetItem();
            if (!item.IsOk()) return;
            tree_->UnselectAll();tree_->SelectItem(item);
            const auto* archiveNode=static_cast<NodeData*>(tree_->GetItemData(item));
            if(archiveNode&&archiveNode->kind==NodeKind::LooseArchive&&archiveOpen_) {
                try{openDiscoveredArchive(archiveNode->ownerIndex);}catch(const std::exception& ex){wxui::showError(this,ex);}return;
            }
            if (tree_->ItemHasChildren(item)) {
                if (tree_->IsExpanded(item)) tree_->Collapse(item);
                else tree_->Expand(item);
                return;
            }
            const NodeData* data = selectedNodeData();
            if (data != nullptr && data->kind == NodeKind::Resource) {
                const neoshared::bif::ResourceSelection selection{data->source,data->resourceIndex};
                if(canOpen(selection)) openResource(selection);
                else extractSelected(false);
            }
        });
        tree_->Bind(wxEVT_TREE_ITEM_MENU, &NeoBIFPanelImpl::onTreeItemMenu, this);
    }

    void onTreeItemMenu(wxTreeEvent& event) {
        const wxTreeItemId item = event.GetItem();
        if (!item.IsOk()) return;

        if (!tree_->IsSelected(item)) {
            tree_->UnselectAll();
            tree_->SelectItem(item);
        }
        showSelectedDetails();
        updateCommandState();

        const NodeData* data = selectedNodeData();
        if (data == nullptr) return;

        const auto selections = selectedResources();
        const auto entireSelections = selectedResources(false);
        const auto hasExtractable = [this](const auto& resources) {
            return std::any_of(resources.begin(), resources.end(),
                [this](const neoshared::bif::ResourceSelection& resource) {
                    return resourceIsExtractable(resource);
                });
        };

        wxArrayTreeItemIds selectedItems;
        tree_->GetSelections(selectedItems);
        const auto sourcePaths = selectedSourceArchivePaths();
        const bool multipleNodes = selectedItems.size() > 1;
        const bool resourceNodes = selectedNodesAreResources();
        const bool filterActive = !filter_->GetValue().empty();
        const bool scopeNarrowed = selections.size() < entireSelections.size();
        bool branchNode = false;
        for (const auto& selectedItem : selectedItems) {
            const auto* selectedData =
                dynamic_cast<NodeData*>(tree_->GetItemData(selectedItem));
            if (selectedData != nullptr && selectedData->kind != NodeKind::Resource &&
                selectedData->kind != NodeKind::Page) {
                branchNode = true;
                break;
            }
        }

        wxString extractLabel = "Extract Selection...";
        wxString zipLabel = "Save Selection as ZIP...";
        wxString entireLabel = "Extract Entire Selection...";
        wxString entireZipLabel = "Save Entire Selection as ZIP...";
        switch (data->kind) {
        case NodeKind::Root:
            extractLabel = "Extract All Resources...";
            zipLabel = "Save All Resources as ZIP...";
            entireLabel = extractLabel;
            entireZipLabel = zipLabel;
            break;
        case NodeKind::Bif:
            extractLabel = "Extract BIF...";
            zipLabel = "Save BIF as ZIP...";
            entireLabel = "Extract Entire BIF...";
            entireZipLabel = "Save Entire BIF as ZIP...";
            break;
        case NodeKind::LooseRoot:
            extractLabel = "Extract Game Archives...";
            zipLabel = "Save Game Archives as ZIP...";
            entireLabel = "Extract All Game Archives...";
            entireZipLabel = "Save All Game Archives as ZIP...";
            break;
        case NodeKind::Directory:
            extractLabel = "Extract Archive Folder...";
            zipLabel = "Save Archive Folder as ZIP...";
            entireLabel = "Extract Entire Archive Folder...";
            entireZipLabel = "Save Entire Archive Folder as ZIP...";
            break;
        case NodeKind::LooseArchive:
            extractLabel = "Extract Archive...";
            zipLabel = "Save Archive as ZIP...";
            entireLabel = "Extract Entire Archive...";
            entireZipLabel = "Save Entire Archive as ZIP...";
            break;
        case NodeKind::Type:
            extractLabel = "Extract Resource Type...";
            zipLabel = "Save Resource Type as ZIP...";
            entireLabel = "Extract Entire Resource Type...";
            entireZipLabel = "Save Entire Resource Type as ZIP...";
            break;
        case NodeKind::Page:
            extractLabel = "Extract Page...";
            zipLabel = "Save Page as ZIP...";
            break;
        case NodeKind::Resource:
            extractLabel = "Extract Resource...";
            zipLabel = "Save Resource as ZIP...";
            break;
        }

        if (multipleNodes && !resourceNodes) {
            entireLabel = "Extract Entire Selection...";
            entireZipLabel = "Save Entire Selection as ZIP...";
        }

        if (resourceNodes) {
            extractLabel = selections.size() == 1
                ? "Extract Resource..." : "Extract Selected Resources...";
            zipLabel = selections.size() == 1
                ? "Save Resource as ZIP..." : "Save Selected Resources as ZIP...";
        } else if (filterActive && scopeNarrowed) {
            extractLabel = "Extract Matching Resources...";
            zipLabel = "Save Matching Resources as ZIP...";
        } else if (multipleNodes) {
            extractLabel = "Extract Selected Items...";
            zipLabel = "Save Selected Items as ZIP...";
        }

        if (data->kind == NodeKind::Root && !indexComplete()) {
            extractLabel = "Extract Indexed Resources...";
            zipLabel = "Save Indexed Resources as ZIP...";
        }
        wxMenu menu;
        appendOpenCommands(menu);
        menu.Append(ID_ExtractSelected, extractLabel)->Enable(hasExtractable(selections));
        menu.Append(ID_ZipSelected, zipLabel)->Enable(hasExtractable(selections));

        // Without a Search filter, these would duplicate the extraction commands above.
        if (filterActive && scopeNarrowed && branchNode) {
            menu.AppendSeparator();
            menu.Append(ID_ExtractBranch, entireLabel)->Enable(hasExtractable(entireSelections));
            menu.Append(ID_ZipBranch, entireZipLabel)->Enable(hasExtractable(entireSelections));
        }

        const bool showResourceNames = resourceNodes && !selections.empty();
        const bool showSourceActions = !sourcePaths.empty();
        if (showResourceNames || showSourceActions) {
            menu.AppendSeparator();
            if (showResourceNames) {
                menu.Append(ID_CopyResref,
                            selections.size() == 1
                                ? "Copy ResRef" : "Copy ResRefs");
            }
            if (showSourceActions) {
                menu.Append(ID_CopySource,
                            sourcePaths.size() == 1
                                ? "Copy source archive path" : "Copy source archive paths");
            }
#if !defined(__EMSCRIPTEN__)
            if (sourcePaths.size() == 1) {
                menu.Append(ID_LocateSource, "Show in File Manager");
            }
#endif
        }
        PopupMenu(&menu);
    }

    void requestOpenArchiveFiles() {
        if(jobRunning_)return;
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        neobrowser::requestRetainedFiles(
            "Open chitin.key and optional relocated BIF files", ".key,.bif,.bzf", true,
            [weak, request](neobrowser::RetainedFileSetResult result) mutable {
                if (!weak) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                if (weak->browserSelectionRequest_ != request) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                acceptBrowserFileSet(weak, std::move(result), false, request);
            });
#else
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        wxui::requestOpenFiles(
            this, "Open chitin.key and optional relocated BIF files", kArchiveWildcard,
            [weak](std::vector<std::filesystem::path> paths) mutable {
                if (!weak || paths.empty()) return;
                weak->openSelectedFiles(std::move(paths));
            });
#endif
    }

    void openSelectedFiles(std::vector<std::filesystem::path> paths) {
#if !defined(__EMSCRIPTEN__)
        std::vector<std::filesystem::path> keys;
        std::vector<std::filesystem::path> bifs;
        for (const auto& path : paths) {
            if (hasExtension(path, ".key")) keys.push_back(path);
            else if (hasExtension(path, ".bif")) bifs.push_back(path);
        }
        if (keys.empty()) {
            if (session().archive.isOpen() && !bifs.empty()) {
                addSupplementaryFiles(std::move(bifs));
                return;
            }
            wxMessageBox("Select chitin.key. You may select the referenced BIF files in the same operation.",
                         "KEY File Required", wxOK | wxICON_ERROR, this);
            return;
        }
        std::filesystem::path key = keys.front();
        if (keys.size() > 1u) {
            wxArrayString choices;
            int preferred = 0;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                choices.Add(neosettings::pathToWx(keys[i]));
                if (lowerAscii(neoshared::pathToUtf8(keys[i].filename())) == "chitin.key") {
                    preferred = static_cast<int>(i);
                }
            }
            wxSingleChoiceDialog dialog(this,
                "More than one KEY file was selected. Choose the game archive to open:",
                "Select KEY File", choices);
            dialog.SetSelection(preferred);
            if (dialog.ShowModal() != wxID_OK) return;
            key = keys[static_cast<std::size_t>(dialog.GetSelection())];
        }
        openArchive(key, std::move(bifs), key.parent_path());
#else
        (void)paths;
#endif
    }

    void requestAddBifs() {
        if(jobRunning_)return;
        if (!session().archive.isOpen()) {
            requestOpenArchiveFiles();
            return;
        }
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        neobrowser::requestRetainedFiles(
            "Add or relocate BIF files", ".bif,.bzf", true,
            [weak, request](neobrowser::RetainedFileSetResult result) mutable {
                if (!weak) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                if (weak->browserSelectionRequest_ != request) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                acceptBrowserFileSet(weak, std::move(result), true, request);
            });
#else
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        wxui::requestOpenFiles(
            this, "Add or relocate BIF files", kBifWildcard,
            [weak](std::vector<std::filesystem::path> paths) mutable {
                if (!weak || paths.empty()) return;
                weak->addSupplementaryFiles(std::move(paths));
            });
#endif
    }

    void addSupplementaryFiles(std::vector<std::filesystem::path> paths) {
#if !defined(__EMSCRIPTEN__)
        auto replacements=session().explicitRelocations;
        for(const auto& path:paths) {
            if(!hasExtension(path,".bif"))continue;
            wxArrayString choices;
            int suggested = wxNOT_FOUND;
            int firstMissing = wxNOT_FOUND;
            const std::string selectedName = lowerAscii(
                neoshared::pathToUtf8(path.filename()));
            for (std::size_t i = 0; i < session().archive.bifs().size(); ++i) {
                const auto& bif = session().archive.bifs()[i];
                choices.Add(wxui::toWx(std::to_string(bif.index)+": "+bif.storedPath+
                    (bif.available?" [currently available]":" [missing]") ));
                if (!bif.available && firstMissing == wxNOT_FOUND) {
                    firstMissing = static_cast<int>(i);
                }
                std::string portableStoredPath = bif.storedPath;
                std::replace(portableStoredPath.begin(), portableStoredPath.end(), '\\', '/');
                const std::string storedName = lowerAscii(neoshared::pathToUtf8(
                    std::filesystem::path(portableStoredPath).filename()));
                if (storedName == selectedName && suggested == wxNOT_FOUND) {
                    suggested = static_cast<int>(i);
                }
            }
            wxSingleChoiceDialog dialog(this,
                "Which KEY entry should use this exact file?\n"+neosettings::pathToWx(path),
                "Relocate BIF",choices);
            if (suggested != wxNOT_FOUND) dialog.SetSelection(suggested);
            else if (firstMissing != wxNOT_FOUND) dialog.SetSelection(firstMissing);
            else if (!choices.IsEmpty()) dialog.SetSelection(0);
            if(dialog.ShowModal()!=wxID_OK)return;
            const std::size_t selection = static_cast<std::size_t>(dialog.GetSelection());
            if (selection >= session().archive.bifs().size()) return;
            replacements[session().archive.bifs()[selection].index]=path;
        }
        session().explicitRelocations=std::move(replacements);
        openArchive(session().keyPath,session().supplementaryFiles,session().scanRoot);
        // openArchive keeps the old session on failure; a later explicit rescan
        // can retry a missing relocated file without guessing another candidate.
#else
        (void)paths;
#endif
    }

    void requestScanDirectory() {
        if(jobRunning_)return;
#if defined(__EMSCRIPTEN__)
        if (browserIndexing_) return;
        const std::uint64_t request = ++browserSelectionRequest_;
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        neobrowser::requestRetainedDirectory(
            "Select a game directory",
            ".key,.bif,.bzf,.erf,.mod,.mzd,.sav,.hak,.nwm,.rim",
            [weak, request](neobrowser::RetainedFileSetResult result) mutable {
                if (!weak) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                if (weak->browserSelectionRequest_ != request) {
                    if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                acceptBrowserFileSet(weak, std::move(result), false, request);
            });
#else
        std::filesystem::path initialDirectory = session().scanRoot;
        if (initialDirectory.empty()) {
            initialDirectory = settings_.readPath("Paths/LastGameDirectory").value_or(
                std::filesystem::path{});
        }
        wxDirDialog dialog(this, "Select a game directory",
                           neosettings::pathToWx(initialDirectory),
                           wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto directory = neosettings::pathFromWx(dialog.GetPath());
        settings_.writePath("Paths/LastGameDirectory", directory);
        scanDirectory(directory);
#endif
    }

    void scanDirectory(const std::filesystem::path& directory) {
#if !defined(__EMSCRIPTEN__)
        const auto directKey = neoshared::game::firstExisting(directory, {"chitin.key"});
        if (!directKey.empty()) {
            openArchive(directKey, {}, directory);
            return;
        }
        std::vector<std::filesystem::path> keys;
        if(!runJob("Finding chitin.key",[&](const neoshared::bif::JobControl& job){keys=neoshared::bif::KeyBifArchive::scanForKeyFiles(directory,128u,job);}))return;
        if (keys.empty()) {
            logScan(directory, "Error: no chitin.key file was found under this directory.");
            wxMessageBox("No chitin.key file was found under the selected directory.",
                         "No KEY Archive Found", wxOK | wxICON_ERROR, this);
            return;
        }
        std::filesystem::path selected = keys.front();
        if (keys.size() > 1u) {
            wxArrayString choices;
            for (const auto& key : keys) choices.Add(neosettings::pathToWx(key));
            wxSingleChoiceDialog dialog(this, "Select the game archive to open:",
                                        "Multiple KEY Archives", choices);
            if (dialog.ShowModal() != wxID_OK) return;
            selected = keys[static_cast<std::size_t>(dialog.GetSelection())];
        }
        openArchive(selected, {}, selected.parent_path());
#else
        (void)directory;
#endif
    }

#if defined(__EMSCRIPTEN__)
    static std::size_t browserPathDepth(const std::string& path) {
        return static_cast<std::size_t>(std::count(path.begin(), path.end(), '/'));
    }

    static std::optional<neoshared::bif::BrowserArchiveFile> selectBrowserKey(
        const std::vector<neoshared::bif::BrowserArchiveFile>& files,
        std::string& error) {
        std::vector<neoshared::bif::BrowserArchiveFile> candidates;
        for (const auto& file : files) {
            const std::filesystem::path path(file.relativePath);
            if (hasExtension(path, ".key")) candidates.push_back(file);
        }
        if (candidates.empty()) {
            error = "Select chitin.key together with the required BIF files, or scan the game directory.";
            return std::nullopt;
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            const bool leftChitin = lowerAscii(neoshared::pathToUtf8(
                std::filesystem::path(left.relativePath).filename())) == "chitin.key";
            const bool rightChitin = lowerAscii(neoshared::pathToUtf8(
                std::filesystem::path(right.relativePath).filename())) == "chitin.key";
            if (leftChitin != rightChitin) return leftChitin;
            const std::size_t leftDepth = browserPathDepth(left.relativePath);
            const std::size_t rightDepth = browserPathDepth(right.relativePath);
            if (leftDepth != rightDepth) return leftDepth < rightDepth;
            return lowerAscii(left.relativePath) < lowerAscii(right.relativePath);
        });
        if (candidates.size() > 1u) {
            const auto& first = candidates[0];
            const auto& second = candidates[1];
            const bool firstChitin = lowerAscii(neoshared::pathToUtf8(
                std::filesystem::path(first.relativePath).filename())) == "chitin.key";
            const bool secondChitin = lowerAscii(neoshared::pathToUtf8(
                std::filesystem::path(second.relativePath).filename())) == "chitin.key";
            if (firstChitin == secondChitin &&
                browserPathDepth(first.relativePath) == browserPathDepth(second.relativePath)) {
                error = "More than one equally likely KEY file was selected. Select one game installation at a time.";
                return std::nullopt;
            }
        }
        return candidates.front();
    }

    static void acceptBrowserFileSet(wxWeakRef<NeoBIFPanelImpl> weak,
                                     neobrowser::RetainedFileSetResult result,
                                     bool appendBifs,
                                     std::uint64_t request) {
        if (!weak) {
            if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
            return;
        }
        if (!result.error.empty()) {
            if (result.sessionId != 0) neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(wxui::toWx(result.error), "Browser Archive Selection Failed",
                         wxOK | wxICON_ERROR, weak.get());
            return;
        }
        if (result.cancelled()) return;

        std::vector<neoshared::bif::BrowserArchiveFile> incoming;
        incoming.reserve(result.files.size());
        for (const auto& file : result.files) {
            const std::filesystem::path path(file.relativePath);
            if (!hasExtension(path, ".key") && !hasExtension(path, ".bif") &&
                !neoshared::bif::LooseArchiveCatalog::hasSupportedExtension(path)) {
                continue;
            }
            neoshared::bif::BrowserArchiveFile selected;
            selected.sessionId=result.sessionId; selected.fileId=file.fileId;
            selected.relativePath=file.relativePath; selected.size=file.size;
            incoming.push_back(std::move(selected));
        }
        if (incoming.empty()) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox("The selection contains no KEY, BIF, ERF, MOD, SAV, HAK, NWM, or RIM files.",
                         "No Supported Archives Found", wxOK | wxICON_ERROR, weak.get());
            return;
        }

        std::vector<neoshared::bif::BrowserArchiveFile> combined;
        const std::uint64_t generation = weak->browserGeneration_;
        if (appendBifs) {
            combined = weak->browserFiles_;
            for (auto& file : combined) file.preferred = false;
            for (auto file : incoming) {
                if (hasExtension(std::filesystem::path(file.relativePath), ".bif")) {
                    file.preferred = true;
                    combined.push_back(std::move(file));
                }
            }
        } else {
            const bool hasKey = std::any_of(incoming.begin(), incoming.end(), [](const auto& file) {
                return hasExtension(std::filesystem::path(file.relativePath), ".key");
            });
            if (!hasKey && weak->session().archive.isOpen()) {
                combined = weak->browserFiles_;
                for (auto& file : combined) file.preferred = false;
                for (auto file : incoming) {
                    if (hasExtension(std::filesystem::path(file.relativePath), ".bif")) {
                        file.preferred = true;
                        combined.push_back(std::move(file));
                    }
                }
                appendBifs = true;
            } else {
                combined = incoming;
            }
        }

        if(appendBifs && weak->session().archive.isOpen()) {
            wxArrayString choices;
            for(const auto& bif:weak->session().archive.bifs()) {
                choices.Add(wxui::toWx("["+std::to_string(bif.index)+"] "+bif.storedPath+
                    (bif.available ? " [currently available]" : " [missing]")));
            }
            for(auto& selected:combined) if(selected.sessionId==result.sessionId && hasExtension(std::filesystem::path(selected.relativePath),".bif")) {
                int suggested = wxNOT_FOUND;
                int firstMissing = wxNOT_FOUND;
                const std::string selectedName = lowerAscii(neoshared::pathToUtf8(
                    std::filesystem::path(selected.relativePath).filename()));
                for (std::size_t i = 0; i < weak->session().archive.bifs().size(); ++i) {
                    const auto& bif = weak->session().archive.bifs()[i];
                    if (!bif.available && firstMissing == wxNOT_FOUND) {
                        firstMissing = static_cast<int>(i);
                    }
                    std::string portableStoredPath = bif.storedPath;
                    std::replace(portableStoredPath.begin(), portableStoredPath.end(), '\\', '/');
                    const std::string storedName = lowerAscii(neoshared::pathToUtf8(
                        std::filesystem::path(portableStoredPath).filename()));
                    if (storedName == selectedName && suggested == wxNOT_FOUND) {
                        suggested = static_cast<int>(i);
                    }
                }
                wxSingleChoiceDialog dialog(weak.get(),
                    wxui::toWx("Choose the KEY entry supplied by " + selected.relativePath),
                    "Relocate BIF Explicitly",choices);
                if (suggested != wxNOT_FOUND) dialog.SetSelection(suggested);
                else if (firstMissing != wxNOT_FOUND) dialog.SetSelection(firstMissing);
                else if (!choices.IsEmpty()) dialog.SetSelection(0);
                if(dialog.ShowModal()!=wxID_OK) { neobrowser::releaseRetainedFileSet(result.sessionId); return; }
                const auto choice=static_cast<std::size_t>(dialog.GetSelection());
                if (choice >= weak->session().archive.bifs().size()) {
                    neobrowser::releaseRetainedFileSet(result.sessionId);
                    return;
                }
                const std::size_t bifIndex = weak->session().archive.bifs()[choice].index;
                for(auto& previous:combined) if(&previous!=&selected && previous.explicitBifIndex==bifIndex) previous.explicitBifIndex.reset();
                selected.explicitBifIndex=bifIndex;
            }
        }

        std::string keyError;
        const auto key = selectBrowserKey(combined, keyError);
        if (!key) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(wxui::toWx(keyError), "KEY File Required",
                         wxOK | wxICON_ERROR, weak.get());
            return;
        }

        if (weak->filterTimer_.IsRunning()) weak->flushSearch();
        weak->browserIndexing_ = true;
        weak->Enable(false);
        neoshared::bif::KeyBifArchive loaded;
        neoshared::bif::LooseArchiveCatalog loadedLoose;
        const auto reader = [](const neoshared::bif::BrowserArchiveFile& file,
                               std::uint64_t offset,
                               std::size_t length,
                               std::vector<std::uint8_t>& bytes,
                               std::string& error) {
            return neobrowser::readRetainedFileRange(
                file.sessionId, file.fileId, offset, length, bytes, error);
        };
        bool opened = false;
        std::string openError;
        try {
            wxProgressDialog progress("Indexing game archives","Reading KEY/BIF and game-directory archive tables...",100,weak.get(),wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_ELAPSED_TIME);
            const auto yield = [&progress] {
                if(!progress.Pulse())throw neoshared::bif::JobCancelled();
                emscripten_sleep(0);
            };
            opened = loaded.openBrowser(*key, combined, reader, yield);
            if (!opened) {
                openError = loaded.lastError();
            } else {
                const std::filesystem::path browserRoot =
                    std::filesystem::path(key->relativePath).parent_path();
                opened = loadedLoose.openBrowser(browserRoot, combined, reader, yield, loaded.gameProfile());
                if (!opened) openError = loadedLoose.lastError();
            }
        } catch (const std::exception& exception) {
            openError = exception.what();
        } catch (...) {
            openError = "An unknown error occurred while indexing the browser archive.";
        }

        if (!weak) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            return;
        }
        NeoBIFPanelImpl* frame = weak.get();
        frame->browserIndexing_ = false;
        frame->Enable(true);
        if (frame->browserGeneration_ != generation ||
            frame->browserSelectionRequest_ != request) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            return;
        }
        if (!opened) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(wxui::toWx(openError.empty() ?
                             "Unable to index the selected browser archive." : openError),
                         "Unable to Open Browser Archive",
                         wxOK | wxICON_ERROR, frame);
            return;
        }

        // Keep only the KEY and the exact BIF candidates selected by the
        // completed index. This prevents a previously replaced BIF from
        // competing with its replacement during a later Add BIF operation.
        using BrowserFileKey = std::pair<std::uint32_t, std::uint32_t>;
        std::set<BrowserFileKey> activeFileKeys;
        activeFileKeys.emplace(key->sessionId, key->fileId);
        for (const auto& bif : loaded.bifs()) {
            if (bif.browserBacked && bif.browserSessionId != 0 && bif.browserFileId != 0) {
                activeFileKeys.emplace(bif.browserSessionId, bif.browserFileId);
            }
        }
        for (const auto& looseArchive : loadedLoose.archives()) {
            if (looseArchive.browserBacked && looseArchive.browserSessionId != 0 &&
                looseArchive.browserFileId != 0) {
                activeFileKeys.emplace(looseArchive.browserSessionId,
                                       looseArchive.browserFileId);
            }
        }

        bool incomingBifUsed = false;
        for (const auto& file : incoming) {
            if (hasExtension(std::filesystem::path(file.relativePath), ".bif") &&
                activeFileKeys.count(BrowserFileKey{file.sessionId, file.fileId}) != 0u) {
                incomingBifUsed = true;
                break;
            }
        }
        if (appendBifs && !incomingBifUsed) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox(
                "None of the selected BIF files matched a BIF referenced by the active chitin.key.",
                "No Matching BIF Files", wxOK | wxICON_WARNING, frame);
            return;
        }

        std::vector<neoshared::bif::BrowserArchiveFile> activeFiles;
        std::set<BrowserFileKey> emittedFiles;
        std::map<std::uint32_t, std::vector<std::uint32_t>> retainedIdsBySession;
        for (auto file : combined) {
            const BrowserFileKey fileKey{file.sessionId, file.fileId};
            if (activeFileKeys.count(fileKey) == 0u ||
                !emittedFiles.insert(fileKey).second) {
                continue;
            }
            file.preferred = false;
            retainedIdsBySession[file.sessionId].push_back(file.fileId);
            activeFiles.push_back(std::move(file));
        }
        if (emittedFiles.count(BrowserFileKey{key->sessionId, key->fileId}) == 0u) {
            neobrowser::releaseRetainedFileSet(result.sessionId);
            wxMessageBox("The selected KEY file was lost while reconciling the browser archive session.",
                         "Unable to Open Browser Archive", wxOK | wxICON_ERROR, frame);
            return;
        }

        std::set<std::uint32_t> candidateSessions(
            frame->browserSessions_.begin(), frame->browserSessions_.end());
        candidateSessions.insert(result.sessionId);
        for (const auto& file : combined) candidateSessions.insert(file.sessionId);
        for (const std::uint32_t sessionId : candidateSessions) {
            const auto retained = retainedIdsBySession.find(sessionId);
            if (retained == retainedIdsBySession.end()) {
                neobrowser::releaseRetainedFileSet(sessionId);
            } else {
                neobrowser::retainOnlyRetainedFiles(sessionId, retained->second);
            }
        }

        frame->browserFiles_ = std::move(activeFiles);
        frame->browserSessions_.clear();
        for (const auto& [sessionId, fileIds] : retainedIdsBySession) {
            if (!fileIds.empty()) frame->browserSessions_.push_back(sessionId);
        }
        if (!appendBifs) {
            frame->filterTimer_.Stop();
            frame->filter_->ChangeValue(wxEmptyString);
        }
        frame->expandedNodes_.clear();frame->selectedNodes_.clear();frame->tree_->DeleteAllItems();
        frame->session().archive = std::move(loaded);
        frame->session().looseArchives = std::move(loadedLoose);
        frame->session().keyPath = std::filesystem::path(key->relativePath);
        frame->session().scanRoot = frame->session().keyPath.parent_path();
        frame->session().supplementaryFiles.clear();
        ++frame->browserGeneration_;
        frame->updateSessionLabel();
        frame->setModuleTitle(appTitle() + " - " +
                        wxui::toWx(neoshared::pathToUtf8(frame->session().keyPath.filename())));
        frame->rebuildTree();
        frame->updateStatus();
        frame->updateCommandState();
    }

    void releaseBrowserSessions() {
        ++browserSelectionRequest_;
        for (const std::uint32_t sessionId : browserSessions_) {
            neobrowser::releaseRetainedFileSet(sessionId);
        }
        browserSessions_.clear();
        browserFiles_.clear();
        ++browserGeneration_;
    }
#endif

#if !defined(__EMSCRIPTEN__)
    template<class Work>
    bool runJob(const wxString& title, Work work) {
        if(jobRunning_)return false;
        if (filterTimer_.IsRunning()) flushSearch();
        jobRunning_=true;cancelJob_.store(false);
        struct BusyReset { bool& flag; ~BusyReset(){flag=false;} } busyReset{jobRunning_};
        std::atomic<bool> finished{false};
        std::mutex mutex;std::string detail="Preparing...";std::size_t done=0,total=0;
        std::exception_ptr failure;
        neoshared::bif::JobControl control;
        control.cancelled=[this]{return cancelJob_.load();};
        control.progress=[&](std::size_t current,std::size_t count,const std::string& text){std::lock_guard<std::mutex> lock(mutex);done=current;total=count;detail=text;};
        wxProgressDialog dialog(title,"Preparing...",1000,this,wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_ELAPSED_TIME|wxPD_AUTO_HIDE);
        std::thread worker([&]{try{work(control);}catch(...){failure=std::current_exception();}finished.store(true);});
        struct JoinWorker { std::thread& thread; std::atomic<bool>& cancel;
            ~JoinWorker(){if(thread.joinable()){cancel.store(true);thread.join();}}
        } joinWorker{worker,cancelJob_};
        while(!finished.load()) {
            std::string text;std::size_t current,count;
            {std::lock_guard<std::mutex> lock(mutex);text=detail;current=done;count=total;}
            bool proceed=count?dialog.Update(static_cast<int>(std::min<std::size_t>(999,current*1000/std::max<std::size_t>(1,count))),wxui::toWx(text)):
                               dialog.Pulse(wxui::toWx(text));
            if(!proceed)cancelJob_.store(true);
            wxMilliSleep(10);
        }
        worker.join();dialog.Update(1000);jobRunning_=false;
        if(failure) {
            try{std::rethrow_exception(failure);}
            catch(const neoshared::bif::JobCancelled&){setModuleStatusText("Operation cancelled; current session retained");return false;}
            catch(const std::exception& ex){logScan(session().scanRoot, wxui::toStd(title) + ": " + ex.what());wxMessageBox(wxui::toWx(ex.what()),title,wxOK|wxICON_ERROR,this);return false;}
        }
        return true;
    }
#endif

    void openArchive(const std::filesystem::path& key,
                     std::vector<std::filesystem::path> supplementary,
                     std::filesystem::path scanRoot) {
#if !defined(__EMSCRIPTEN__)
        if(jobRunning_)return;
        const auto keyPath = std::filesystem::absolute(key).lexically_normal();
        const auto existing = std::find_if(sessions_.begin(), sessions_.end(), [&](const auto& game) {
            return !game->keyPath.empty() && neosettings::samePathForMru(game->keyPath, keyPath);
        });
        if (existing != sessions_.end()) activeSession_ = existing->get();
        else if (!session().keyPath.empty()) addSession();
        auto& game = session();
        game.indexing.reset();
        if (!neosettings::samePathForMru(game.keyPath, keyPath)) {
            forgetSessionSelection(game);
            game.treeRoot = {};
            game.archive.clear();
            game.looseArchives.clear();
            game.explicitRelocations.clear();
            game.name.clear();
            game.game = neoshared::override_aliases::Game::Auto;
        }
        if(scanRoot.empty())scanRoot=keyPath.parent_path();
        game.keyPath = keyPath;
        game.scanRoot = std::filesystem::absolute(scanRoot).lexically_normal();
        game.supplementaryFiles = std::move(supplementary);
        game.error.clear();
        game.progress = "Loading KEY/BIF resources...";
        auto work = std::make_unique<IndexJob>();
        work->progress = game.progress;
        auto* job = work.get();
        // The worker owns unpublished results. Only the UI thread changes a live tree/catalog.
        work->worker = std::thread([job, keyPath, root = game.scanRoot,
                                   files = game.supplementaryFiles, mappings = game.explicitRelocations] {
            neoshared::bif::JobControl control;
            control.cancelled = [job] { return job->cancelled.load(); };
            control.progress = [job](std::size_t, std::size_t, const std::string& text) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->progress = text;
            };
            try {
                auto key = std::make_unique<neoshared::bif::KeyBifArchive>();
                if (!key->open(keyPath, files, mappings, control)) throw std::runtime_error(key->lastError());
                const auto profile = key->gameProfile();
                const auto detected = neoshared::override_aliases::detectGame(root);
                std::map<std::string, std::uint16_t> overrideTypes;
                const auto collectTypes = [&](const auto& resources) {
                    for (const auto& resource : resources) {
                        auto extension = lowerAscii(resource.extension);
                        if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
                        if (!extension.empty()) overrideTypes.emplace(std::move(extension), resource.type);
                    }
                };
                collectTypes(key->resources());
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->key = std::move(key);
                    job->game = detected;
                    job->progress = "Finding game-directory archives...";
                }
                auto loose = std::make_unique<neoshared::bif::LooseArchiveCatalog>();
                if (!loose->scan(root, 8192u, control, profile)) throw std::runtime_error(loose->lastError());
                collectTypes(loose->resources());
                const auto roots = neoshared::override_aliases::discover(root, detected);
                if (!loose->addOverrideDirectories(roots, overrideTypes, control, true)) throw std::runtime_error(loose->lastError());
                control.check();
                std::lock_guard<std::mutex> lock(job->mutex);
                job->loose = std::move(loose);
            } catch (const neoshared::bif::JobCancelled&) {
                // A rescan, closed session, or shutdown cancels and joins this worker.
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->error = ex.what();
            }
            std::lock_guard<std::mutex> lock(job->mutex);
            job->finished = true;
        });
        game.indexing = std::move(work);
        tree_->UnselectAll();
        selectedNodes_.clear();
        logScan(game.scanRoot, game.progress);
        indexTimer_.Start(150);
        notifySessionChanged();
        updateSessionLabel();
        rebuildTree();
        updateStatus();
#else
        (void)key;
        (void)supplementary;
        (void)scanRoot;
#endif
    }

    void closeArchive() {
        if(jobRunning_)return;
#if defined(__EMSCRIPTEN__)
        releaseBrowserSessions();
#endif
        forgetSessionSelection(session());
        sessions_.erase(std::find_if(sessions_.begin(), sessions_.end(),
            [this](const auto& game) { return game.get() == activeSession_; }));
        if (sessions_.empty()) addSession();
        else activeSession_ = sessions_.front().get();
        updateSessionLabel();
        setModuleTitle(appTitle());
        rebuildTree();
        updateStatus();
    }

    void clearSearch() {
        filterTimer_.Stop();
        filter_->ChangeValue(wxEmptyString);
        rebuildTree();
    }

    void flushSearch() {
        filterTimer_.Stop();
        rebuildTree();
    }

    bool resourceMatches(const GameSession& game, const neoshared::bif::ResourceInfo& resource,
                         const neoshared::bif::BifInfo& bif,
                         const neoshared::bif::ResourceQuery& query) const {
        return query.matches(resource.fileName(), resource.extension,
            {resource.resref, neoshared::bif::resourceTypeLabel(resource.type, game.archive.gameProfile()),
             neoshared::bif::hexResourceId(resource.resourceId), resource.status, bif.storedPath});
    }

    bool looseResourceMatches(const GameSession& game, const neoshared::bif::LooseResourceInfo& resource,
                              const neoshared::bif::LooseArchiveInfo& archive,
                              const neoshared::bif::ResourceQuery& query) const {
        return query.matches(resource.fileName(), resource.extension,
            {resource.resref, neoshared::bif::resourceTypeLabel(resource.type, game.archive.gameProfile()),
             neoshared::bif::hexResourceId(resource.resourceId), resource.status,
             neoshared::genericPathToUtf8(archive.relativePath),
             neoshared::bif::looseArchiveKindName(archive.kind)});
    }

    void updateSearchCount() {
        std::size_t total = 0, matches = 0;
        for (const auto& game : sessions_) {
            total += game->archive.resources().size() + game->looseArchives.resources().size();
            matches += std::count(game->visibleBif.begin(), game->visibleBif.end(), true) +
                       std::count(game->visibleLoose.begin(), game->visibleLoose.end(), true);
        }
        const auto text = filter_->GetValue().empty() ? resourceCountText(total) :
            std::to_string(matches) + " of " + std::to_string(total) + " resources match";
        searchCount_->SetLabel(wxui::toWx(text));
        searchCount_->SetToolTip("Search covers all open installations. Extraction uses the selected installation.");
        searchCount_->GetParent()->Layout();
    }

    void revealSearchResults(const wxTreeItemId& item, std::size_t& budget) {
        const auto* data = dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if (!data) return;
        if (data->lazyResources) {
            // Reveal small result sets, but do not undo lazy pagination on
            // queries matching tens of thousands of resources.
            if (data->lazyResources->size() > budget) return;
            budget -= data->lazyResources->size();
            populateNode(item);
            tree_->Expand(item);
            return;
        }
        tree_->Expand(item);
        wxTreeItemIdValue cookie;
        for (auto child = tree_->GetFirstChild(item, cookie); child.IsOk();
             child = tree_->GetNextChild(item, cookie)) revealSearchResults(child, budget);
    }

    std::string nodeKey(const NodeData& data) const {
        return std::to_string(data.session->id)+":"+std::to_string(static_cast<int>(data.kind))+":"+
            std::to_string(static_cast<int>(data.source))+":"+
            std::to_string(data.ownerIndex)+":"+std::to_string(data.resourceIndex)+":"+
            std::to_string(data.type)+":"+data.directoryPath;
    }

    void captureTreeState(const wxTreeItemId& item) {
        if(!item.IsOk()) return;
        const auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if(data) {
            const auto key=nodeKey(*data);
            if(tree_->IsExpanded(item)) expandedNodes_.insert(key);
            else expandedNodes_.erase(key);
            if(tree_->IsSelected(item)) selectedNodes_.insert(key);
            else selectedNodes_.erase(key);
        }
        wxTreeItemIdValue cookie;
        for(auto child=tree_->GetFirstChild(item,cookie);child.IsOk();child=tree_->GetNextChild(item,cookie))captureTreeState(child);
    }

    void attachResourceList(const wxTreeItemId& item, const std::vector<std::size_t>& indices) {
        auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        data->lazyResources=std::make_shared<const std::vector<std::size_t>>(indices);
        tree_->SetItemHasChildren(item,!indices.empty());
    }

    void populateNode(const wxTreeItemId& item) {
        auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if(!data || data->populated || !data->lazyResources) return;
        auto& game = *data->session;
        data->populated=true;
        const auto& indices=*data->lazyResources;
        constexpr std::size_t pageSize=512;
        if(indices.size()>pageSize) {
            for(std::size_t start=0;start<indices.size();start+=pageSize) {
                const auto end=std::min(indices.size(),start+pageSize);
                auto* page=new NodeData(&game, NodeKind::Page,data->ownerIndex,start,data->type,data->source);
                const auto child=tree_->AppendItem(item,wxui::toWx("Resources "+std::to_string(start+1)+"-"+std::to_string(end)), -1,-1,page);
                attachResourceList(child,std::vector<std::size_t>(indices.begin()+static_cast<std::ptrdiff_t>(start),indices.begin()+static_cast<std::ptrdiff_t>(end)));
            }
        } else {
            for(auto index:indices) {
                if(data->source==neoshared::bif::ResourceSource::KeyBif)appendBifResourceNode(game,item,index);
                else appendLooseResourceNode(game,item,index);
            }
        }
    }

    void restoreTreeState(const wxTreeItemId& item) {
        if(!item.IsOk())return;
        const auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if(data) {
            auto& game = *data->session;
            const auto key=nodeKey(*data);
            bool reveal=expandedNodes_.count(key)!=0;
            if(data->lazyResources) for(auto index:*data->lazyResources) {
                const auto type=data->source==neoshared::bif::ResourceSource::KeyBif?game.archive.resources()[index].type:game.looseArchives.resources()[index].type;
                const NodeData resource(&game, NodeKind::Resource,data->ownerIndex,index,type,data->source);
                if(selectedNodes_.count(nodeKey(resource))){reveal=true;break;}
            }
            if(reveal) { populateNode(item);tree_->Expand(item); }
            if(selectedNodes_.count(key))tree_->SelectItem(item);
        }
        wxTreeItemIdValue cookie;
        for(auto child=tree_->GetFirstChild(item,cookie);child.IsOk();child=tree_->GetNextChild(item,cookie))restoreTreeState(child);
    }

    void addResourceGroups(GameSession& game, const wxTreeItemId& parent,neoshared::bif::ResourceSource source,std::size_t owner,
                           const std::vector<std::size_t>& visible) {
        if(!groupByType_) {attachResourceList(parent,visible);return;}
        std::map<std::uint16_t,std::vector<std::size_t>> groups;
        for(auto index:visible)groups[source==neoshared::bif::ResourceSource::KeyBif?game.archive.resources()[index].type:game.looseArchives.resources()[index].type].push_back(index);
        for(const auto& [type,indices]:groups) {
            const auto node=tree_->AppendItem(parent,wxui::toWx("."+neoshared::bif::resourceTypeExtension(type, game.archive.gameProfile())+" ("+std::to_string(indices.size())+")"), -1,-1,
                new NodeData(&game, NodeKind::Type,owner,kNoIndex,type,source));
            attachResourceList(node,indices);
        }
    }

    void expandStructure(const wxTreeItemId& item) {
        if(!item.IsOk()) return;
        const auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
        if(data && data->kind==NodeKind::Page) return;
        populateNode(item);
        if (data) tree_->Expand(item);
        wxTreeItemIdValue cookie;
        for(auto child=tree_->GetFirstChild(item,cookie);child.IsOk();child=tree_->GetNextChild(item,cookie)) {
            const auto* value=dynamic_cast<NodeData*>(tree_->GetItemData(child));
            if(!value || value->kind!=NodeKind::Resource) expandStructure(child);
        }
    }

    void rebuildTree() {
        if(rebuilding_ || jobRunning_)return;
        rebuilding_=true;
        captureTreeState(tree_->GetRootItem());
        tree_->Freeze();tree_->DeleteAllItems();
        const auto root = tree_->AddRoot("Installations");
        const neoshared::bif::ResourceQuery query(wxui::toStd(filter_->GetValue()));
        for (auto& game : sessions_) {
            game->treeRoot = {};
            game->visibleBif.assign(game->archive.resources().size(), false);
            game->visibleLoose.assign(game->looseArchives.resources().size(), false);
            if (game->keyPath.empty()) continue;
            game->treeRoot = tree_->AppendItem(root, sessionTreeLabel(*game), -1, -1,
                                              new NodeData(game.get(), NodeKind::Root));
            buildSessionTree(*game, game->treeRoot, query);
            restoreTreeState(game->treeRoot);
            tree_->Expand(game->treeRoot);
            if (!query.empty()) {
                std::size_t revealBudget = 256;
                revealSearchResults(game->treeRoot, revealBudget);
            }
        }
        if (selectedNodes_.empty() && session().treeRoot.IsOk())
            tree_->SelectItem(session().treeRoot);
        tree_->Thaw();rebuilding_=false;
        showSelectedDetails();updateCommandState();updateSearchCount();
    }

    void buildSessionTree(GameSession& game, const wxTreeItemId& root,
                          const neoshared::bif::ResourceQuery& query) {
        for(const auto& bif:game.archive.bifs()) {
            std::vector<std::size_t> visible;
            const bool ownerMatches=query.empty()||query.matchesContext(bif.storedPath)||
                std::any_of(bif.messages.begin(),bif.messages.end(),[&](const std::string& message){return query.matchesContext(message);});
            for(auto index:bif.resourceIndices)if(index<game.archive.resources().size()&&(ownerMatches||resourceMatches(game,game.archive.resources()[index],bif,query))) {
                visible.push_back(index);game.visibleBif[index]=true;
            }
            if(visible.empty()&&!ownerMatches)continue;
            const std::string label=bif.storedPath+" ("+std::to_string(visible.size())+
                (query.empty()?" resources)":" matches)")+
                (!bif.available?" [missing]":(!bif.valid?" [invalid]":""));
            const auto node=tree_->AppendItem(root,wxui::toWx(label),-1,-1,new NodeData(&game, NodeKind::Bif,bif.index));
            addResourceGroups(game,node,neoshared::bif::ResourceSource::KeyBif,bif.index,visible);
        }
        wxTreeItemId looseRoot;
        std::map<std::string,wxTreeItemId> directories;
        for(const auto& archive:game.looseArchives.archives()) {
            std::vector<std::size_t> visible;
            const bool ownerMatches=query.empty()||query.matchesContext(neoshared::genericPathToUtf8(archive.relativePath))||
                query.matchesContext(neoshared::bif::looseArchiveKindName(archive.kind))||
                std::any_of(archive.messages.begin(),archive.messages.end(),[&](const std::string& message){return query.matchesContext(message);});
            for(auto index:archive.resourceIndices)if(index<game.looseArchives.resources().size()&&(ownerMatches||looseResourceMatches(game,game.looseArchives.resources()[index],archive,query))) {
                visible.push_back(index);game.visibleLoose[index]=true;
            }
            if(visible.empty()&&!ownerMatches)continue;
            if(!looseRoot.IsOk())looseRoot=tree_->AppendItem(root,"Game Archives",-1,-1,
                new NodeData(&game, NodeKind::LooseRoot,kNoIndex,kNoIndex,0,neoshared::bif::ResourceSource::LooseArchive));
            auto parent=looseRoot;std::filesystem::path accumulated;
            for(const auto& component:archive.relativePath.parent_path()) {
                const auto part=neoshared::genericPathToUtf8(component);if(part.empty()||part=="."||part=="..")continue;
                accumulated/=component;const auto pathKey=lowerAscii(neoshared::genericPathToUtf8(accumulated));
                auto found=directories.find(pathKey);
                if(found==directories.end()) {
                    parent=tree_->AppendItem(parent,wxui::toWx(part),-1,-1,new NodeData(&game, NodeKind::Directory,kNoIndex,kNoIndex,0,neoshared::bif::ResourceSource::LooseArchive,neoshared::genericPathToUtf8(accumulated)));
                    directories.emplace(pathKey,parent);
                }else parent=found->second;
            }
            const auto label=neoshared::pathToUtf8(archive.relativePath.filename())+" ("+
                std::to_string(visible.size())+(query.empty()?" resources)":" matches)")+
                (!archive.valid?" [invalid]":"");
            const auto node=tree_->AppendItem(parent,wxui::toWx(label),-1,-1,new NodeData(&game, NodeKind::LooseArchive,archive.index,kNoIndex,0,neoshared::bif::ResourceSource::LooseArchive));
            addResourceGroups(game,node,neoshared::bif::ResourceSource::LooseArchive,archive.index,visible);
        }
    }

    void appendBifResourceNode(GameSession& game, const wxTreeItemId& parent, std::size_t resourceIndex) {
        const auto& resource = game.archive.resources()[resourceIndex];
        std::ostringstream label;
        label << resource.fileName() << " - " << neoshared::bif::formatByteSize(resource.size);
        if (!resource.extractable) label << " [unavailable]";
        if (!resource.keyed) label << " [not indexed]";
        tree_->AppendItem(parent, wxui::toWx(label.str()), -1, -1,
                          new NodeData(&game, NodeKind::Resource, resource.bifIndex,
                                       resourceIndex, resource.type,
                                       neoshared::bif::ResourceSource::KeyBif));
    }

    void appendLooseResourceNode(GameSession& game, const wxTreeItemId& parent,
                                 std::size_t resourceIndex) {
        const auto& resource = game.looseArchives.resources()[resourceIndex];
        std::ostringstream label;
        label << resource.fileName() << " - " << neoshared::bif::formatByteSize(resource.size);
        if (!resource.extractable) label << " [unavailable]";
        tree_->AppendItem(parent, wxui::toWx(label.str()), -1, -1,
                          new NodeData(&game, NodeKind::Resource, resource.archiveIndex,
                                       resourceIndex, resource.type,
                                       neoshared::bif::ResourceSource::LooseArchive));
    }

    NodeData* selectedNodeData() const {
        wxArrayTreeItemIds selections;tree_->GetSelections(selections);
        return selections.empty()?nullptr:dynamic_cast<NodeData*>(tree_->GetItemData(selections.front()));
    }

    bool selectedNodesAreResources() const {
        wxArrayTreeItemIds selections;
        tree_->GetSelections(selections);
        if (selections.empty()) return false;
        for (const auto& item : selections) {
            const auto* data = dynamic_cast<NodeData*>(tree_->GetItemData(item));
            if (data == nullptr || data->kind != NodeKind::Resource) return false;
        }
        return true;
    }

    static bool archiveIsUnderDirectory(const std::filesystem::path& archivePath,
                                        const std::string& directoryPath) {
        std::string parent = lowerAscii(neoshared::genericPathToUtf8(
            archivePath.parent_path().lexically_normal()));
        std::string directory = lowerAscii(neoshared::genericPathToUtf8(
            neoshared::pathFromUtf8(directoryPath).lexically_normal()));
        while (!parent.empty() && parent.back() == '/') parent.pop_back();
        while (!directory.empty() && directory.back() == '/') directory.pop_back();
        return parent == directory ||
               (!directory.empty() && parent.size() > directory.size() &&
                parent.compare(0u, directory.size(), directory) == 0 &&
                parent[directory.size()] == '/');
    }

    std::vector<neoshared::bif::ResourceSelection> nodeResources(const NodeData* data) const {
        if (data == nullptr || data->session != activeSession_ || !session().archive.isOpen()) return {};
        std::vector<neoshared::bif::ResourceSelection> selected;
        const auto appendAllBif = [&] {
            selected.reserve(selected.size() + session().archive.resources().size());
            for (const auto& resource : session().archive.resources()) {
                selected.push_back({neoshared::bif::ResourceSource::KeyBif, resource.index});
            }
        };
        const auto appendAllLoose = [&] {
            selected.reserve(selected.size() + session().looseArchives.resources().size());
            for (const auto& resource : session().looseArchives.resources()) {
                selected.push_back({neoshared::bif::ResourceSource::LooseArchive, resource.index});
            }
        };

        switch (data->kind) {
        case NodeKind::Root:
            appendAllBif();
            appendAllLoose();
            break;
        case NodeKind::Bif:
            if (data->ownerIndex < session().archive.bifs().size()) {
                for (const std::size_t index : session().archive.bifs()[data->ownerIndex].resourceIndices) {
                    selected.push_back({neoshared::bif::ResourceSource::KeyBif, index});
                }
            }
            break;
        case NodeKind::LooseRoot:
            appendAllLoose();
            break;
        case NodeKind::Directory:
            for (const auto& looseArchive : session().looseArchives.archives()) {
                if (!archiveIsUnderDirectory(looseArchive.relativePath,
                                             data->directoryPath)) {
                    continue;
                }
                for (const std::size_t index : looseArchive.resourceIndices) {
                    selected.push_back({neoshared::bif::ResourceSource::LooseArchive, index});
                }
            }
            break;
        case NodeKind::LooseArchive:
            if (data->ownerIndex < session().looseArchives.archives().size()) {
                for (const std::size_t index :
                     session().looseArchives.archives()[data->ownerIndex].resourceIndices) {
                    selected.push_back({neoshared::bif::ResourceSource::LooseArchive, index});
                }
            }
            break;
        case NodeKind::Type:
            if (data->source == neoshared::bif::ResourceSource::KeyBif &&
                data->ownerIndex < session().archive.bifs().size()) {
                for (const std::size_t index : session().archive.bifs()[data->ownerIndex].resourceIndices) {
                    if (index < session().archive.resources().size() &&
                        session().archive.resources()[index].type == data->type) {
                        selected.push_back({neoshared::bif::ResourceSource::KeyBif, index});
                    }
                }
            } else if (data->source == neoshared::bif::ResourceSource::LooseArchive &&
                       data->ownerIndex < session().looseArchives.archives().size()) {
                for (const std::size_t index :
                     session().looseArchives.archives()[data->ownerIndex].resourceIndices) {
                    if (index < session().looseArchives.resources().size() &&
                        session().looseArchives.resources()[index].type == data->type) {
                        selected.push_back({neoshared::bif::ResourceSource::LooseArchive, index});
                    }
                }
            }
            break;
        case NodeKind::Page:
            if(data->lazyResources)for(auto index:*data->lazyResources)selected.push_back({data->source,index});
            break;
        case NodeKind::Resource:
            selected.push_back({data->source, data->resourceIndex});
            break;
        }
        return selected;
    }

    std::vector<neoshared::bif::ResourceSelection> selectedResources(bool respectFilter=true) const {
        wxArrayTreeItemIds selectedItems;tree_->GetSelections(selectedItems);
        std::vector<neoshared::bif::ResourceSelection> result;std::set<std::pair<int,std::size_t>> used;
        for(const auto& item:selectedItems) {
            const auto* data=dynamic_cast<NodeData*>(tree_->GetItemData(item));
            for(const auto& resource:nodeResources(data)) {
                const auto& visible=resource.source==neoshared::bif::ResourceSource::KeyBif?session().visibleBif:session().visibleLoose;
                if(respectFilter && (resource.resourceIndex>=visible.size()||!visible[resource.resourceIndex]))continue;
                if(used.emplace(static_cast<int>(resource.source),resource.resourceIndex).second)result.push_back(resource);
            }
        }
        return result;
    }

    std::vector<neoshared::bif::ResourceSelection> allResourceSelections() const {
        std::vector<neoshared::bif::ResourceSelection> selected;
        selected.reserve(session().archive.resources().size() + session().looseArchives.resources().size());
        for (const auto& resource : session().archive.resources()) {
            selected.push_back({neoshared::bif::ResourceSource::KeyBif, resource.index});
        }
        for (const auto& resource : session().looseArchives.resources()) {
            selected.push_back({neoshared::bif::ResourceSource::LooseArchive, resource.index});
        }
        return selected;
    }

    bool resourceIsExtractable(const neoshared::bif::ResourceSelection& selection) const {
        if (selection.source == neoshared::bif::ResourceSource::KeyBif) {
            return selection.resourceIndex < session().archive.resources().size() &&
                   session().archive.resources()[selection.resourceIndex].extractable;
        }
        return selection.resourceIndex < session().looseArchives.resources().size() &&
               session().looseArchives.resources()[selection.resourceIndex].extractable;
    }

    std::string resourceFileName(const neoshared::bif::ResourceSelection& selection) const {
        if (selection.source == neoshared::bif::ResourceSource::KeyBif) {
            return selection.resourceIndex < session().archive.resources().size()
                ? session().archive.resources()[selection.resourceIndex].fileName()
                : std::string{};
        }
        return selection.resourceIndex < session().looseArchives.resources().size()
            ? session().looseArchives.resources()[selection.resourceIndex].fileName()
            : std::string{};
    }

    std::string resourceDisplayName(const neoshared::bif::ResourceSelection& selection) const {
        if (selection.source == neoshared::bif::ResourceSource::KeyBif) {
            if (selection.resourceIndex >= session().archive.resources().size()) return "Invalid BIF resource";
            const auto& resource = session().archive.resources()[selection.resourceIndex];
            const std::string archivePath = resource.bifIndex < session().archive.bifs().size()
                ? session().archive.bifs()[resource.bifIndex].storedPath : "<invalid BIF>";
            return archivePath + "/" + resource.fileName();
        }
        if (selection.resourceIndex >= session().looseArchives.resources().size()) {
            return "Invalid game-archive resource";
        }
        const auto& resource = session().looseArchives.resources()[selection.resourceIndex];
        const std::string archivePath = resource.archiveIndex < session().looseArchives.archives().size()
            ? neoshared::genericPathToUtf8(
                  session().looseArchives.archives()[resource.archiveIndex].relativePath)
            : "<invalid archive>";
        return archivePath + "/" + resource.fileName();
    }

    std::uint64_t resourceSize(const neoshared::bif::ResourceSelection& selection) const {
        if (selection.source == neoshared::bif::ResourceSource::KeyBif) {
            return selection.resourceIndex < session().archive.resources().size()
                ? session().archive.resources()[selection.resourceIndex].size : 0u;
        }
        return selection.resourceIndex < session().looseArchives.resources().size()
            ? session().looseArchives.resources()[selection.resourceIndex].size : 0u;
    }

    std::string resourceIdentity(const neoshared::bif::ResourceSelection& selection) const {
        std::ostringstream identity;
        if (selection.source == neoshared::bif::ResourceSource::KeyBif &&
            selection.resourceIndex < session().archive.resources().size()) {
            identity << "bif_" << neoshared::bif::hexResourceId(
                session().archive.resources()[selection.resourceIndex].resourceId).substr(2);
        } else if (selection.source == neoshared::bif::ResourceSource::LooseArchive &&
                   selection.resourceIndex < session().looseArchives.resources().size()) {
            const auto& resource = session().looseArchives.resources()[selection.resourceIndex];
            identity << "arc_" << resource.archiveIndex << '_'
                     << neoshared::bif::hexResourceId(resource.resourceId).substr(2);
        } else {
            identity << "resource_" << selection.resourceIndex;
        }
        return identity.str();
    }

    bool readResourceSelection(const neoshared::bif::ResourceSelection& selection,
                               std::vector<std::uint8_t>& bytes,
                               std::string& error) const {
        if (selection.source == neoshared::bif::ResourceSource::KeyBif) {
            return session().archive.readResource(selection.resourceIndex, bytes, error);
        }
        return session().looseArchives.readResource(selection.resourceIndex, bytes, error);
    }

    std::filesystem::path resourceOutputPath(
        const neoshared::bif::ResourceSelection& selection,
        neobif::ExtractionLayout layout) const {
        if (selection.source == neoshared::bif::ResourceSource::KeyBif) {
            const auto paths = neobif::archiveOutputPaths(session().archive, {selection.resourceIndex}, layout);
            return paths.empty() ? std::filesystem::path{} : paths.front();
        }
        return neobif::archiveOutputPath(session().looseArchives, selection.resourceIndex, layout);
    }

    std::vector<std::filesystem::path> resourceOutputPaths(
        const std::vector<neoshared::bif::ResourceSelection>& selections,
        neobif::ExtractionLayout layout) const {
        std::vector<std::filesystem::path> desired;
        desired.reserve(selections.size());
        for (const auto& selection : selections) {
            desired.push_back(resourceOutputPath(selection, layout));
        }
        return desired; // Only explicit Keep both is allowed to rename outputs.
    }

#if defined(__EMSCRIPTEN__)
    bool browserRangeForSelection(const neoshared::bif::ResourceSelection& selection,
                                  std::uint32_t& sessionId,
                                  std::uint32_t& fileId,
                                  std::uint64_t& offset,
                                  std::uint64_t& size) const {
        if (selection.source == neoshared::bif::ResourceSource::KeyBif) {
            if (selection.resourceIndex >= session().archive.resources().size()) return false;
            const auto& resource = session().archive.resources()[selection.resourceIndex];
            if (resource.bifIndex >= session().archive.bifs().size()) return false;
            const auto& bif = session().archive.bifs()[resource.bifIndex];
            if (!bif.browserBacked) return false;
            sessionId = bif.browserSessionId;
            fileId = bif.browserFileId;
            offset = resource.offset;
            size = resource.size;
            return sessionId != 0u && fileId != 0u;
        }
        if (selection.resourceIndex >= session().looseArchives.resources().size()) return false;
        const auto& resource = session().looseArchives.resources()[selection.resourceIndex];
        if (resource.archiveIndex >= session().looseArchives.archives().size()) return false;
        const auto& looseArchive = session().looseArchives.archives()[resource.archiveIndex];
        if (!looseArchive.browserBacked) return false;
        sessionId = looseArchive.browserSessionId;
        fileId = looseArchive.browserFileId;
        offset = resource.offset;
        size = resource.storedSize;
        return sessionId != 0u && fileId != 0u;
    }
#endif

    std::string selectionBaseName(const std::vector<neoshared::bif::ResourceSelection>& selections) const {
        wxArrayTreeItemIds selectedItems;
        tree_->GetSelections(selectedItems);
        if (selectedItems.size() > 1u) return "selected_resources";

        const NodeData* data = selectedNodeData();
        if (data != nullptr) {
            if (data->kind == NodeKind::Root) {
                return filter_->GetValue().empty() ? "game_resources" : "search_results";
            }
            if (data->kind == NodeKind::Resource) {
                const std::filesystem::path file(resourceFileName(
                    {data->source, data->resourceIndex}));
                const auto stem = neoshared::pathToUtf8(file.stem());
                return stem.empty() ? "resource" : stem;
            }
            if (data->kind == NodeKind::Bif && data->ownerIndex < session().archive.bifs().size()) {
                std::string stored = session().archive.bifs()[data->ownerIndex].storedPath;
                std::replace(stored.begin(), stored.end(), '\\', '/');
                const auto path = std::filesystem::path(stored);
                const auto stem = neoshared::pathToUtf8(path.stem());
                return stem.empty() ? "bif_resources" : stem;
            }
            if (data->kind == NodeKind::LooseArchive &&
                data->ownerIndex < session().looseArchives.archives().size()) {
                const auto stem = neoshared::pathToUtf8(
                    session().looseArchives.archives()[data->ownerIndex].relativePath.stem());
                return stem.empty() ? "archive_resources" : stem;
            }
            if (data->kind == NodeKind::Directory) {
                const auto name = neoshared::pathToUtf8(
                    neoshared::pathFromUtf8(data->directoryPath).filename());
                return name.empty() ? "directory_archives" : name;
            }
            if (data->kind == NodeKind::LooseRoot) return "game_archives";
            if (data->kind == NodeKind::Type) {
                return neoshared::bif::resourceTypeExtension(data->type, session().archive.gameProfile()) + "_resources";
            }
            if (data->kind == NodeKind::Page) {
                return neoshared::bif::resourceTypeExtension(data->type, session().archive.gameProfile()) + "_resource_page";
            }
        }
        const std::size_t total = session().archive.resources().size() + session().looseArchives.resources().size();
        return selections.size() == total ? "game_resources" : "neobif_selection";
    }

    void extractSelected(bool forceZip, bool respectFilter=true) {
        if (jobRunning_) return;
        if (filterTimer_.IsRunning()) flushSearch();
        const auto selections = selectedResources(respectFilter);
        if (selections.empty()) {
            wxMessageBox("Select a resource or branch first.", "Nothing Selected",
                         wxOK | wxICON_INFORMATION, this);
            return;
        }
        extractSelections(selections, selectionBaseName(selections), forceZip);
    }

    void extractAll(bool forceZip) {
        if (!session().archive.isOpen()) return;
        const auto selections = allResourceSelections();
        extractSelections(selections, "game_resources", forceZip);
    }

    void exportByFileType() {
        if (jobRunning_ || !session().archive.isOpen()) return;
        const auto types = neoshared::bif::summarizeResourceTypes(session().archive, session().looseArchives);
        if (types.empty()) {
            wxMessageBox("The open game session has no indexed resource types.",
                         "Nothing to Export", wxOK | wxICON_INFORMATION, this);
            return;
        }
        wxArrayString labels;
        int initial = 0;
        const neoshared::bif::ResourceQuery query(wxui::toStd(filter_->GetValue()));
        bool pickedAvailable = false;
        for (std::size_t i = 0; i < types.size(); ++i) {
            const auto& entry = types[i];
            std::string label = "." + entry.extension + " - " +
                std::to_string(entry.total) +
                (entry.total == 1u ? " resource" : " resources");
            if (entry.available != entry.total) {
                label += " (" + std::to_string(entry.total - entry.available) +
                    " unavailable)";
            }
            labels.Add(wxui::toWx(label));
            if (entry.available && !pickedAvailable) {
                initial = static_cast<int>(i);
                pickedAvailable = true;
            }
        }
        // A type in Search is a convenient initial choice, never an export
        // scope restriction. Hidden pages and unselected archives are included.
        if (query.isTypeQuery()) {
            for (std::size_t i = 0; i < types.size(); ++i) {
                if (lowerAscii(types[i].extension) == query.extension()) initial = static_cast<int>(i);
            }
        }
        wxSingleChoiceDialog dialog(this,
            "Choose the file type to export to one ZIP",
            "Save File Type as ZIP", labels);
        dialog.SetSelection(initial);
        if (auto* button = dynamic_cast<wxButton*>(dialog.FindWindow(wxID_OK))) {
            button->SetLabel("Save ZIP...");
        }
        wxui::applyTheme(&dialog, darkMode_);
        neoview::applyFontScale(&dialog, fontScale_);
        if (dialog.ShowModal() != wxID_OK) return;
        const int chosen = dialog.GetSelection();
        if (chosen == wxNOT_FOUND || static_cast<std::size_t>(chosen) >= types.size()) return;
        const auto& type = types[static_cast<std::size_t>(chosen)];
        const auto resources = neoshared::bif::selectResourceType(session().archive, session().looseArchives, type.type);
        // Route through the existing native/browser ZIP export, including
        // unavailable-resource confirmation, collision and source protection,
        // staging, cancellation and size limits. Even one match is a ZIP.
        extractSelections(resources, "game_" + type.extension + "_resources", true);
    }

    std::vector<neobif::ExportItem> makeNativeExportItems(
        const std::vector<neoshared::bif::ResourceSelection>& selections,
        const std::vector<std::filesystem::path>& paths) {
        std::vector<neobif::ExportItem> items;
        items.reserve(selections.size());
        for (std::size_t index = 0; index < selections.size(); ++index) {
            const neoshared::bif::ResourceSelection selection = selections[index];
            neobif::ExportItem item;
            item.relativePath = paths[index];
            item.displayName = resourceDisplayName(selection);
            item.identitySuffix = resourceIdentity(selection);
            item.expectedSize = resourceSize(selection);
            const auto source=sourceArchivePath(selection);
            if(!source.empty())item.sourcePaths.push_back(source);
            item.sourcePaths.push_back(session().keyPath);
            item.stream=[this,selection](const neoshared::bif::ByteSink& sink,std::string& error,const neoshared::bif::JobControl& job) {
                return selection.source==neoshared::bif::ResourceSource::KeyBif
                    ? session().archive.streamResource(selection.resourceIndex,sink,error,job)
                    : session().looseArchives.streamResource(selection.resourceIndex,sink,error,job);
            };
            item.read = [this, selection](std::vector<std::uint8_t>& bytes,
                                          std::string& error) {
                return readResourceSelection(selection, bytes, error);
            };
            items.push_back(std::move(item));
        }
        return items;
    }

    void extractSelections(const std::vector<neoshared::bif::ResourceSelection>& selections,
                           std::string baseName,
                           bool forceZip) {
        if(jobRunning_)return;
        std::vector<neoshared::bif::ResourceSelection> extractable;
        extractable.reserve(selections.size());
        for (const auto& selection : selections) {
            if (resourceIsExtractable(selection)) extractable.push_back(selection);
        }
        if (extractable.empty()) {
            wxMessageBox("The requested selection contains no extractable resources.",
                         "Nothing to Extract", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (extractable.size() != selections.size()) {
            const std::string message = std::to_string(
                selections.size() - extractable.size()) +
                " unavailable resource(s) will be omitted. Continue?";
            if (wxMessageBox(wxui::toWx(message), "Incomplete Selection",
                             wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
                return;
            }
        }

        const bool singleResource = extractable.size() == 1u && selections.size() == 1u;
        auto outputPaths = resourceOutputPaths(
            extractable, neobif::ExtractionLayout::BifAndType);
        if (outputPaths.size() != extractable.size()) {
            wxMessageBox("NeoBIF could not construct the extraction layout.",
                         "Extraction Failed", wxOK | wxICON_ERROR, this);
            return;
        }

#if defined(__EMSCRIPTEN__)
        if(jobRunning_)return;
        int directoryPolicy=0;
        if(!singleResource&&!forceZip&&neobrowser::retainedDirectoryWriteSupported()) {
            wxArrayString choices;choices.Add("Skip existing files (recommended)");choices.Add("Replace existing files");choices.Add("Keep both with explicit filename suffixes");
            wxSingleChoiceDialog dialog(this,"Choose output conflict behavior", "Extraction conflicts",choices);
            if(dialog.ShowModal()!=wxID_OK)return;
            directoryPolicy=dialog.GetSelection();
        }
        std::set<std::string> names;bool duplicate=false;
        for(const auto& path:outputPaths)if(!names.insert(lowerAscii(neoshared::genericPathToUtf8(path))).second)duplicate=true;
        if(duplicate) {
            const bool keepBothAlreadySelected = directoryPolicy == 2 &&
                !singleResource && !forceZip &&
                neobrowser::retainedDirectoryWriteSupported();
            if (!keepBothAlreadySelected &&
                wxMessageBox(
                    "The selection contains duplicate output paths. Keep every copy by "
                    "adding explicit filename suffixes? Choose No to return and narrow "
                    "the selection.",
                    "Resource Name Conflicts",
                    wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
                return;
            }
            std::vector<std::string> identities;
            for(const auto& selection:extractable)identities.push_back(resourceIdentity(selection));
            outputPaths=neobif::makeUniqueExportPaths(outputPaths,identities);
            std::ostringstream namesText;namesText<<"Keep-both output names:\n";
            for(const auto& path:outputPaths)namesText<<neoshared::genericPathToUtf8(path)<<'\n';
            details_->ChangeValue(wxui::toWx(namesText.str()));
            details_->SetInsertionPoint(0);
        }
        std::vector<neobrowser::RetainedExportEntry> entries;
        entries.reserve(extractable.size());
        for (std::size_t position = 0; position < extractable.size(); ++position) {
            std::uint32_t sessionId = 0u;
            std::uint32_t fileId = 0u;
            std::uint64_t offset = 0u;
            std::uint64_t size = 0u;
            if (!browserRangeForSelection(
                    extractable[position], sessionId, fileId, offset, size) ||
                outputPaths[position].empty()) {
                wxMessageBox("A selected resource is not backed by an active browser file.",
                             "Extraction Failed", wxOK | wxICON_ERROR, this);
                return;
            }
            entries.push_back(neobrowser::RetainedExportEntry{
                sessionId, fileId, offset, size,
                neoshared::genericPathToUtf8(outputPaths[position]), {}});
            const auto selection = extractable[position];
            const bool compressed = selection.source == neoshared::bif::ResourceSource::KeyBif
                ? session().archive.bifs()[session().archive.resources()[selection.resourceIndex].bifIndex].compressed
                : !session().looseArchives.resources()[selection.resourceIndex].directRangeExtractable;
            if (compressed) {
                auto& entry = entries.back();std::string error;
                if (!readResourceSelection(selection, entry.decodedBytes, error)) {
                    wxMessageBox(wxui::toWx(error), "Extraction Failed", wxOK | wxICON_ERROR, this);return;
                }
                entry.offset = 0;entry.size = entry.decodedBytes.size();
            }
        }

        neobrowser::RetainedExportMode mode = neobrowser::RetainedExportMode::DirectDownload;
        std::string outputName;
        if (singleResource && !forceZip) {
            outputName = resourceFileName(extractable.front());
        } else if (forceZip || !neobrowser::retainedDirectoryWriteSupported()) {
            mode = neobrowser::RetainedExportMode::ZipDownload;
            outputName = sanitizeDownloadName(baseName) + ".zip";
        } else {
            mode = directoryPolicy==1?neobrowser::RetainedExportMode::DirectoryReplace:
                (directoryPolicy==2?neobrowser::RetainedExportMode::DirectoryKeepBoth:neobrowser::RetainedExportMode::Directory);
            outputName = sanitizeDownloadName(baseName);
        }

        std::string confirmation;
        if (mode == neobrowser::RetainedExportMode::DirectDownload) {
            confirmation = "Download " + resourceCountText(extractable.size()) +
                " as " + outputName + "?";
        } else if (mode == neobrowser::RetainedExportMode::ZipDownload) {
            confirmation = "Save " + resourceCountText(extractable.size()) +
                " in " + outputName + "?";
        } else {
            confirmation = "Extract " + resourceCountText(extractable.size()) +
                " to a selected browser directory?";
        }
        if (wxMessageBox(wxui::toWx(confirmation), "Confirm Extraction",
                         wxOK | wxCANCEL | wxICON_QUESTION, this) != wxOK) {
            return;
        }

        const std::uint64_t generation = browserGeneration_;
        const std::size_t requestedCount = entries.size();
        const bool directDownload = mode == neobrowser::RetainedExportMode::DirectDownload;
        wxWeakRef<NeoBIFPanelImpl> weak(this);
        setModuleStatusText("Preparing " + resourceCountText(requestedCount) + "...");
        jobRunning_=true;Enable(false);
        neobrowser::requestExportRetainedFiles(
            mode, outputName, entries,
            [weak, generation, directDownload](
                neobrowser::RetainedExportResult result) {
                if (!weak) return;
                NeoBIFPanelImpl* frame = weak.get();frame->jobRunning_=false;frame->Enable(true);
                frame->updateCommandState();
                if(frame->browserGeneration_!=generation)return;
                if (!result.error.empty()) {
                    wxString message = wxui::toWx(result.error);
                    if (!result.details.empty()) {
                        message += "\n\n" + wxui::toWx(result.details);
                    }
                    wxMessageBox(message, "Extraction Failed",
                                 wxOK | wxICON_ERROR, frame);
                    frame->setModuleStatusText("Extraction failed");
                    return;
                }
                if (result.cancelled()) {
                    frame->setModuleStatusText("Extraction cancelled");
                    return;
                }
                if (result.stopped) {
                    frame->setModuleStatusText("Extraction stopped; completed files retained");
                    wxMessageBox(
                        "Extraction stopped. Any completed files were retained.",
                        "Extraction Stopped", wxOK | wxICON_INFORMATION, frame);
                    return;
                }
                if (result.usedDirectory) {
                    frame->setModuleStatusText("Extraction complete");
                    wxMessageBox("Extraction complete.", "Extraction Complete",
                                 wxOK | wxICON_INFORMATION, frame);
                } else if (directDownload) {
                    frame->setModuleStatusText("Download prepared");
                    wxMessageBox("Download prepared.", "Download Ready",
                                 wxOK | wxICON_INFORMATION, frame);
                } else {
                    frame->setModuleStatusText("ZIP download prepared");
                    wxMessageBox("ZIP download prepared.", "ZIP Ready",
                                 wxOK | wxICON_INFORMATION, frame);
                }
            });
#else
        neobif::ExportOptions options;
        options.protectedInputs=allInputPaths();
        auto items=makeNativeExportItems(extractable,outputPaths);
        if(singleResource&&!forceZip) {
            const auto selected=chooseOutputFile("Extract resource",kAllFilesWildcard,resourceFileName(extractable.front()));
            if(!selected)return;
            items[0].relativePath=selected->filename();options.existing=neobif::ExistingPolicy::Replace;
            neobif::ExtractionReport report;
            if(!runJob("Extracting resource",[&](const neoshared::bif::JobControl& job){options.job=job;report=neobif::extractExportItems(items,selected->parent_path(),options);}))return;
            showExtractionResult(report);return;
        }
        if(forceZip) {
            std::set<std::string> names;bool duplicate=false;
            for(const auto& item:items)if(!names.insert(lowerAscii(neoshared::genericPathToUtf8(item.relativePath))).second)duplicate=true;
            if(duplicate) {
                if(wxMessageBox("Several resources have the same output name. Keep both by explicitly adding suffixes inside the ZIP?", "ZIP name conflicts",wxYES_NO|wxICON_WARNING,this)!=wxYES)return;
                options.keepDuplicateNames=true;
            }
            const auto selected=chooseOutputFile("Save resources as ZIP",kZipWildcard,
                sanitizeDownloadName(baseName)+".zip", ".zip");
            if(!selected)return;
            const auto output=*selected;
            std::uint64_t inputBytes = 0u;
            for (const auto& item : items) inputBytes += item.expectedSize;
            const std::string confirmation = "Save " + resourceCountText(items.size()) +
                " (" + neoshared::bif::formatByteSize(inputBytes) + ") to:\n\n" +
                neoshared::pathToUtf8(output);
            if(wxMessageBox(wxui::toWx(confirmation), "Confirm ZIP",
                            wxOK|wxCANCEL|wxICON_QUESTION,this)!=wxOK)return;
            options.existing=neobif::ExistingPolicy::Replace;std::string error;bool written=false;
            if(!runJob("Writing ZIP",[&](const neoshared::bif::JobControl& job){options.job=job;written=neobif::writeExportZip(items,output,error,options);}))return;
            if(!written) {
                if (error.empty()) error = "NeoBIF could not commit the ZIP archive.";
                wxMessageBox(wxui::toWx(error),"ZIP Was Not Saved",wxOK|wxICON_WARNING,this);
            } else {
                setModuleStatusText("ZIP saved");
                wxMessageBox("ZIP saved successfully.", "ZIP Saved",
                             wxOK | wxICON_INFORMATION, this);
            }
            return;
        }
        wxString lastDirectory;
        if(auto* config=wxConfigBase::Get())config->Read("extraction/lastDirectory",&lastDirectory);
        wxDirDialog directory(this,"Select extraction directory",lastDirectory,wxDD_DEFAULT_STYLE|wxDD_DIR_MUST_EXIST);
        if(directory.ShowModal()!=wxID_OK)return;
        const auto output=neosettings::pathFromWx(directory.GetPath());rememberOutputDirectory(output);
        wxArrayString policies;
        policies.Add("Skip existing files; do not write name conflicts (recommended)");
        policies.Add("Replace existing files; do not write name conflicts");
        policies.Add("Keep both by adding explicit filename suffixes");
        wxSingleChoiceDialog policy(this,"How should output name conflicts be handled?", "Extraction conflicts",policies);
        if(policy.ShowModal()!=wxID_OK)return;
        options.existing=policy.GetSelection()==1?neobif::ExistingPolicy::Replace:
            (policy.GetSelection()==2?neobif::ExistingPolicy::KeepBoth:neobif::ExistingPolicy::Skip);
        neobif::ExportPlan plan;
        if(!runJob("Checking output names",[&](const neoshared::bif::JobControl& job){options.job=job;plan=neobif::planExportItems(items,output,options);}))return;
        std::ostringstream summary;
        summary << countText(plan.writes, "file", "files") << " to write\n"
                << countText(plan.skipped, "existing file", "existing files") << " to skip\n"
                << countText(plan.conflicts, "conflicting or invalid file",
                             "conflicting or invalid files") << " not written\n\n"
                << neoshared::pathToUtf8(output);
        const std::size_t messageCount = static_cast<std::size_t>(std::count_if(
            plan.entries.begin(), plan.entries.end(),
            [](const neobif::PlannedExport& entry) { return !entry.message.empty(); }));
        std::size_t shown = 0u;
        for (const auto& entry : plan.entries) {
            if (entry.message.empty() || shown >= 15u) continue;
            summary << "\n" << items[entry.itemIndex].displayName << ": " << entry.message;
            ++shown;
        }
        if (messageCount > shown) {
            summary << "\n... " << (messageCount - shown)
                    << " additional destination notes are not shown.";
        }
        if(plan.writes==0){wxMessageBox(wxui::toWx(summary.str()),"No Files to Write",wxOK|wxICON_INFORMATION,this);return;}
        summary << "\n\nContinue with "
                << (plan.writes == 1u ? "this file?" : "these files?");
        if(wxMessageBox(wxui::toWx(summary.str()),"Confirm extraction scope",wxOK|wxCANCEL|wxICON_QUESTION,this)!=wxOK)return;
        neobif::ExtractionReport report;
        if(!runJob("Extracting resources",[&](const neoshared::bif::JobControl& job){
            options.job=job;
            report=neobif::extractPlannedExportItems(items,plan,output,options);
        }))return;
        showExtractionResult(report);
#endif
    }

    static std::string sanitizeDownloadName(std::string value) {
        for (char& ch : value) {
            if (static_cast<unsigned char>(ch) < 0x20u || ch == '/' || ch == '\\' ||
                ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
                ch == '>' || ch == '|') ch = '_';
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
        return value.empty() ? "neobif_resources" : value;
    }

    std::filesystem::path sourceArchivePath(const neoshared::bif::ResourceSelection& selection) const {
        if(selection.source==neoshared::bif::ResourceSource::KeyBif) {
            if(selection.resourceIndex>=session().archive.resources().size())return {};
            const auto owner=session().archive.resources()[selection.resourceIndex].bifIndex;
            if(owner>=session().archive.bifs().size())return {};
            const auto& bif=session().archive.bifs()[owner];
            return bif.browserBacked?std::filesystem::path(bif.browserRelativePath):bif.resolvedPath;
        }
        if(selection.resourceIndex>=session().looseArchives.resources().size())return {};
        const auto& resource=session().looseArchives.resources()[selection.resourceIndex];
        if(resource.overrideFile)return resource.directPath;
        const auto owner=resource.archiveIndex;
        if(owner>=session().looseArchives.archives().size())return {};
        const auto& archive=session().looseArchives.archives()[owner];
        return archive.browserBacked?std::filesystem::path(archive.browserRelativePath):archive.resolvedPath;
    }

    std::filesystem::path sourceArchivePath(const NodeData& node) const {
        if (node.kind == NodeKind::Resource) {
            return sourceArchivePath(
                neoshared::bif::ResourceSelection{node.source, node.resourceIndex});
        }
        if ((node.kind == NodeKind::Bif || node.kind == NodeKind::Type ||
             node.kind == NodeKind::Page) &&
            node.source == neoshared::bif::ResourceSource::KeyBif &&
            node.ownerIndex < session().archive.bifs().size()) {
            const auto& bif = session().archive.bifs()[node.ownerIndex];
            return bif.browserBacked ? std::filesystem::path(bif.browserRelativePath)
                                     : bif.resolvedPath;
        }
        if ((node.kind == NodeKind::LooseArchive || node.kind == NodeKind::Type ||
             node.kind == NodeKind::Page) &&
            node.source == neoshared::bif::ResourceSource::LooseArchive &&
            node.ownerIndex < session().looseArchives.archives().size()) {
            const auto& archive = session().looseArchives.archives()[node.ownerIndex];
            return archive.browserBacked
                ? std::filesystem::path(archive.browserRelativePath)
                : archive.resolvedPath;
        }
        return {};
    }

    std::vector<std::filesystem::path> selectedSourceArchivePaths() const {
        wxArrayTreeItemIds selectedItems;
        tree_->GetSelections(selectedItems);
        std::vector<std::filesystem::path> paths;
        std::set<std::string> seen;
        for (const auto& item : selectedItems) {
            const auto* data = dynamic_cast<NodeData*>(tree_->GetItemData(item));
            if (data == nullptr) continue;
            const auto path = sourceArchivePath(*data);
            if (path.empty()) continue;
            std::string key = neoshared::genericPathToUtf8(path.lexically_normal());
#if defined(_WIN32)
            key = lowerAscii(std::move(key));
#endif
            if (seen.insert(key).second) paths.push_back(path);
        }
        return paths;
    }

    std::vector<std::filesystem::path> allInputPaths() const {
        std::vector<std::filesystem::path> paths;
        for (const auto& game : sessions_) {
            if (!game->keyPath.empty()) paths.push_back(game->keyPath);
            auto inputs = game->archive.inputPaths();
            paths.insert(paths.end(), inputs.begin(), inputs.end());
            for (const auto& archive : game->looseArchives.archives())
                if (!archive.resolvedPath.empty()) paths.push_back(archive.resolvedPath);
        }
        return paths;
    }
    void copySelection(bool sourcePaths) {
        std::set<std::string> seen;
        std::string text;
        if (sourcePaths) {
            for (const auto& path : selectedSourceArchivePaths()) {
                const auto value = neoshared::pathToUtf8(path);
                if (value.empty() || !seen.insert(value).second) continue;
                if (!text.empty()) text += '\n';
                text += value;
            }
        } else {
            for (const auto& selected : selectedResources()) {
                const auto value = selected.source == neoshared::bif::ResourceSource::KeyBif
                    ? session().archive.resources()[selected.resourceIndex].resref
                    : session().looseArchives.resources()[selected.resourceIndex].resref;
                if (value.empty() || !seen.insert(value).second) continue;
                if (!text.empty()) text += '\n';
                text += value;
            }
        }
        if (text.empty()) return;
        if (!wxTheClipboard->Open()) {
            wxMessageBox("The clipboard is unavailable.", "Copy Failed",
                         wxOK | wxICON_WARNING, this);
            return;
        }
        wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(text)));
        wxTheClipboard->Close();
        setModuleStatusText("Copied " + std::to_string(seen.size()) +
            (sourcePaths ? (seen.size() == 1u ? " source path" : " source paths")
                         : (seen.size() == 1u ? " ResRef" : " ResRefs")));
    }
    void locateSource() {
#if !defined(__EMSCRIPTEN__)
        const auto paths = selectedSourceArchivePaths();
        const std::filesystem::path source = paths.size() == 1u ? paths.front()
                                                               : std::filesystem::path{};
        if(source.empty()||!wxLaunchDefaultApplication(neosettings::pathToWx(source.parent_path())))
            wxMessageBox("The source folder is unavailable.","Show in File Manager",wxOK|wxICON_INFORMATION,this);
#endif
    }
#if !defined(__EMSCRIPTEN__)
    void rememberOutputDirectory(const std::filesystem::path& directory) {
        if(auto* config=wxConfigBase::Get()){config->Write("extraction/lastDirectory",neosettings::pathToWx(directory));config->Flush();}
    }
    std::optional<std::filesystem::path> chooseOutputFile(
        const std::string& title, const std::string& wildcard,
        const std::string& name, std::string requiredExtension = {}) {
        wxString directory;if(auto* config=wxConfigBase::Get())config->Read("extraction/lastDirectory",&directory);
        wxFileDialog dialog(this,wxui::toWx(title),directory,wxui::toWx(name),wxui::toWx(wildcard),wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if(dialog.ShowModal()!=wxID_OK)return std::nullopt;
        auto path=neosettings::pathFromWx(dialog.GetPath());
        if (!requiredExtension.empty()) {
            if (requiredExtension.front() != '.') requiredExtension.insert(requiredExtension.begin(), '.');
            if (lowerAscii(neoshared::pathToUtf8(path.extension())) !=
                lowerAscii(requiredExtension)) {
                path.replace_extension(requiredExtension);
                std::error_code ec;
                if (std::filesystem::exists(path, ec) && !ec &&
                    wxMessageBox(wxString("The ZIP already exists:\n\n") +
                                     neosettings::pathToWx(path) +
                                     "\n\nReplace it?",
                                 "Replace Existing ZIP", wxYES_NO | wxNO_DEFAULT |
                                     wxICON_WARNING, this) != wxYES) {
                    return std::nullopt;
                }
            }
        }
        rememberOutputDirectory(path.parent_path());return path;
    }
    void showExtractionResult(const neobif::ExtractionReport& report) {
        if (report.cancelled) {
            setModuleStatusText("Extraction cancelled");
            wxMessageBox(
                "Extraction was cancelled. Any completed files were retained.",
                "Extraction Cancelled", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxString message;
        if (report.failed != 0u) {
            setModuleStatusText("Extraction finished with warnings");
            message = "Extraction finished, but some files could not be written.";
            wxMessageBox(message, "Extraction Needs Attention",
                         wxOK | wxICON_WARNING, this);
            return;
        }

        setModuleStatusText("Extraction complete");
        message = "Extraction complete.";
        wxMessageBox(message, "Extraction Complete",
                     wxOK | wxICON_INFORMATION, this);
    }
#endif

    void showSelectedDetails() {
        if (rebuilding_) return;
        if (!session().archive.isOpen()) {
            details_->ChangeValue(session().keyPath.empty() ? emptyStateText() :
                wxui::toWx(neoshared::pathToUtf8(session().scanRoot) + "\n\n" +
                    (session().error.empty() ? session().progress : "Error: " + session().error)));
            details_->SetInsertionPoint(0);
            return;
        }

        const std::size_t visibleCount =
            static_cast<std::size_t>(std::count(session().visibleBif.begin(), session().visibleBif.end(), true)) +
            static_cast<std::size_t>(std::count(session().visibleLoose.begin(), session().visibleLoose.end(), true));
        const wxTreeItemId rootItem = session().treeRoot;
        const bool treeHasMatchingBranch = rootItem.IsOk() &&
            tree_->GetChildrenCount(rootItem, false) != 0u;
        if (!filter_->GetValue().empty() && visibleCount == 0u &&
            !treeHasMatchingBranch) {
            details_->ChangeValue(
                wxString("No resources in this installation match \"") + filter_->GetValue() +
                "\".\n\nClear Search or try a broader resource name or file type.");
            details_->SetInsertionPoint(0);
            return;
        }

        const NodeData* data = selectedNodeData();
        if (data == nullptr) {
            details_->ChangeValue(
                "Select a resource or branch to view its details and extraction scope.");
            details_->SetInsertionPoint(0);
            return;
        }

        wxArrayTreeItemIds selectedItems;
        tree_->GetSelections(selectedItems);
        std::ostringstream text;

        const auto writeSelectionSummary = [&](const std::vector<neoshared::bif::ResourceSelection>& selections) {
            std::size_t extractable = 0u;
            std::uint64_t bytes = 0u;
            for (const auto& selection : selections) {
                if (!resourceIsExtractable(selection)) continue;
                ++extractable;
                bytes += resourceSize(selection);
            }
            const bool filteredScope = !filter_->GetValue().empty();
            text << (filteredScope ? "Resources in Search scope: " : "Resources: ")
                 << selections.size() << '\n'
                 << "Extractable: " << extractable << '\n'
                 << "Extractable size: " << neoshared::bif::formatByteSize(bytes) << '\n';
        };

        if (selectedItems.size() > 1u) {
            text << "Selection\n"
                 << "=========\n\n"
                 << "Selected items: " << selectedItems.size() << '\n';
            writeSelectionSummary(selectedResources());
            text << "\nOverlapping branch selections are counted once.";
            details_->ChangeValue(wxui::toWx(text.str()));
            details_->SetInsertionPoint(0);
            return;
        }

        if (data->kind == NodeKind::Root) {
            std::uint64_t extractableBytes = 0u;
            for (const auto& resource : session().archive.resources()) {
                if (resource.extractable) extractableBytes += resource.size;
            }
            for (const auto& resource : session().looseArchives.resources()) {
                if (resource.extractable) extractableBytes += resource.size;
            }
            const std::size_t totalResources =
                session().archive.resources().size() + session().looseArchives.resources().size();
            const std::size_t totalExtractable = session().archive.extractableResourceCount() +
                session().looseArchives.extractableResourceCount();
            text << "Game Session\n"
                 << "============\n\n"
                 << "Game directory: "
                 << neoshared::pathToUtf8(session().scanRoot.empty()
                        ? session().archive.keyPath().parent_path() : session().scanRoot) << '\n'
                 << "KEY file: " << neoshared::pathToUtf8(session().archive.keyPath()) << '\n'
                 << "BIF archives: " << session().archive.bifs().size() << '\n'
                 << "Game archives: " << session().looseArchives.archives().size() << '\n'
                 << "Resources: " << totalResources << '\n'
                 << "Extractable: " << totalExtractable << '\n'
                 << "Extractable size: " << neoshared::bif::formatByteSize(extractableBytes) << '\n';
            if (!filter_->GetValue().empty()) {
                text << "Search matches: " << visibleCount << '\n';
            }
            if (session().archive.missingBifCount() != 0u) {
                text << "Missing BIFs: " << session().archive.missingBifCount() << '\n';
            }
            if (session().looseArchives.invalidArchiveCount() != 0u) {
                text << "Invalid game archives: "
                     << session().looseArchives.invalidArchiveCount() << '\n';
            }

            if (!session().archive.issues().empty()) {
                text << "\nKEY/BIF Issues\n"
                     << "--------------\n";
                const std::size_t limit =
                    std::min<std::size_t>(session().archive.issues().size(), 100u);
                for (std::size_t i = 0; i < limit; ++i) {
                    const auto& issue = session().archive.issues()[i];
                    text << (issue.severity == neoshared::bif::IssueSeverity::Error
                                 ? "Error: " : "Warning: ")
                         << issue.message;
                    if (issue.bifIndex) text << " (BIF " << *issue.bifIndex << ')';
                    text << '\n';
                }
                if (session().archive.issues().size() > limit) {
                    text << session().archive.issues().size() - limit
                         << " additional issues are not shown.\n";
                }
            }

            bool wroteLooseIssues = false;
            std::size_t looseIssueCount = 0u;
            for (const auto& looseArchive : session().looseArchives.archives()) {
                if (looseArchive.valid && looseArchive.messages.empty()) continue;
                if (!wroteLooseIssues) {
                    text << "\nGame Archive Issues\n"
                         << "-------------------\n";
                    wroteLooseIssues = true;
                }
                if (looseIssueCount++ >= 100u) continue;
                text << neoshared::genericPathToUtf8(looseArchive.relativePath) << ":\n";
                if (!looseArchive.valid && looseArchive.messages.empty()) {
                    text << "  Error: archive is invalid\n";
                }
                for (const auto& message : looseArchive.messages) {
                    text << "  " << (looseArchive.valid ? "Warning: " : "Error: ")
                         << message << '\n';
                }
            }
            if (looseIssueCount > 100u) {
                text << looseIssueCount - 100u
                     << " additional archive issue groups are not shown.\n";
            }
        } else if (data->kind == NodeKind::Bif &&
                   data->ownerIndex < session().archive.bifs().size()) {
            const auto& bif = session().archive.bifs()[data->ownerIndex];
            text << "Stored Path: " << bif.storedPath << '\n'
                 << "Size: " << neoshared::bif::formatByteSize(bif.actualFileSize) << '\n'
                 << "Resources: " << bif.resourceIndices.size() << '\n';
        } else if (data->kind == NodeKind::LooseRoot) {
            text << "Game Archives\n"
                 << "=============\n\n"
                 << "Root: " << neoshared::pathToUtf8(session().scanRoot) << '\n'
                 << "Archives: " << session().looseArchives.archives().size() << '\n';
            writeSelectionSummary(selectedResources());
            if (session().looseArchives.invalidArchiveCount() != 0u) {
                text << "Invalid archives: " << session().looseArchives.invalidArchiveCount() << '\n';
            }
            if (!session().looseArchives.messages().empty()) {
                text << "\nScan Messages\n"
                     << "-------------\n";
                const std::size_t limit =
                    std::min<std::size_t>(session().looseArchives.messages().size(), 100u);
                for (std::size_t i = 0; i < limit; ++i) {
                    text << session().looseArchives.messages()[i] << '\n';
                }
                if (session().looseArchives.messages().size() > limit) {
                    text << session().looseArchives.messages().size() - limit
                         << " additional scan messages are not shown.\n";
                }
            }
        } else if (data->kind == NodeKind::Directory) {
            std::size_t archiveCount = 0u;
            for (const auto& looseArchive : session().looseArchives.archives()) {
                if (archiveIsUnderDirectory(looseArchive.relativePath,
                                            data->directoryPath)) {
                    ++archiveCount;
                }
            }
            text << "Archive Folder\n"
                 << "==============\n\n"
                 << "Path: " << data->directoryPath << '\n'
                 << "Archives: " << archiveCount << '\n';
            writeSelectionSummary(selectedResources());
        } else if (data->kind == NodeKind::LooseArchive &&
                   data->ownerIndex < session().looseArchives.archives().size()) {
            const auto& looseArchive = session().looseArchives.archives()[data->ownerIndex];
            text << "Archive\n"
                 << "=======\n\n"
                 << "Path: " << neoshared::genericPathToUtf8(looseArchive.relativePath) << '\n'
                 << "Type: " << neoshared::bif::looseArchiveKindName(looseArchive.kind) << '\n'
                 << "Size: " << neoshared::bif::formatByteSize(looseArchive.actualFileSize) << '\n'
                 << "Resources: " << looseArchive.resourceIndices.size() << '\n';
            if (!looseArchive.valid || !looseArchive.messages.empty()) {
                text << "\nIssues\n"
                     << "------\n";
                if (!looseArchive.valid && looseArchive.messages.empty()) {
                    text << "Archive is invalid.\n";
                }
                const std::size_t limit =
                    std::min<std::size_t>(looseArchive.messages.size(), 100u);
                for (std::size_t i = 0; i < limit; ++i) {
                    text << looseArchive.messages[i] << '\n';
                }
                if (looseArchive.messages.size() > limit) {
                    text << looseArchive.messages.size() - limit
                         << " additional messages are not shown.\n";
                }
            }
        } else if (data->kind == NodeKind::Type || data->kind == NodeKind::Page) {
            text << (data->kind == NodeKind::Page ? "Resource Page\n=============\n\n"
                                                  : "File Type\n=========\n\n")
                 << "Type: ." << neoshared::bif::resourceTypeExtension(data->type, session().archive.gameProfile()) << '\n';
            if (data->source == neoshared::bif::ResourceSource::KeyBif &&
                data->ownerIndex < session().archive.bifs().size()) {
                text << "Source: " << session().archive.bifs()[data->ownerIndex].storedPath << '\n';
            } else if (data->source == neoshared::bif::ResourceSource::LooseArchive &&
                       data->ownerIndex < session().looseArchives.archives().size()) {
                text << "Source: "
                     << neoshared::genericPathToUtf8(
                            session().looseArchives.archives()[data->ownerIndex].relativePath) << '\n';
            }
            writeSelectionSummary(selectedResources());
        } else if (data->kind == NodeKind::Resource) {
            text << "Resource\n"
                 << "========\n\n";
            if (data->source == neoshared::bif::ResourceSource::KeyBif &&
                data->resourceIndex < session().archive.resources().size()) {
                const auto& resource = session().archive.resources()[data->resourceIndex];
                text << "Filename: " << resource.fileName() << '\n'
                     << "ResRef: " << resource.resref << '\n'
                     << "Type: ." << resource.extension << '\n'
                     << "Size: " << neoshared::bif::formatByteSize(resource.size) << '\n';
                if (resource.bifIndex < session().archive.bifs().size()) {
                    text << "Source: " << session().archive.bifs()[resource.bifIndex].storedPath
                         << '\n';
                }
                text << "Status: " << resource.status << '\n';
            } else if (data->source == neoshared::bif::ResourceSource::LooseArchive &&
                       data->resourceIndex < session().looseArchives.resources().size()) {
                const auto& resource = session().looseArchives.resources()[data->resourceIndex];
                text << "Filename: " << resource.fileName() << '\n'
                     << "ResRef: " << resource.resref << '\n'
                     << "Type: ." << resource.extension << '\n'
                     << "Size: " << neoshared::bif::formatByteSize(resource.size) << '\n';
                if (resource.archiveIndex < session().looseArchives.archives().size()) {
                    text << "Source: "
                         << neoshared::genericPathToUtf8(
                                session().looseArchives.archives()[resource.archiveIndex].relativePath) << '\n';
                }
                text << "Status: " << resource.status << '\n';
            }
        }

        details_->ChangeValue(wxui::toWx(text.str()));
        details_->SetInsertionPoint(0);
    }

    void updateStatus() {
        if (!session().archive.isOpen()) {
            setModuleStatusText(
                                "Open a game directory or chitin.key", 0);
            setModuleStatusText(session().keyPath.empty() ? "No session" :
                session().error.empty() ? session().progress : "Scan failed (see log)", 1);
            return;
        }
        const std::size_t totalResources =
            session().archive.resources().size() + session().looseArchives.resources().size();
        const std::size_t totalExtractable =
            session().archive.extractableResourceCount() + session().looseArchives.extractableResourceCount();
        std::ostringstream left;
        left << resourceCountText(totalResources) << "; "
             << totalExtractable << " extractable";
        setModuleStatusText(left.str(), 0);
        std::ostringstream right;
        right << countText(session().archive.bifs().size(), "BIF", "BIFs") << " | "
              << countText(session().looseArchives.archives().size(),
                           "game archive", "game archives");
        if (session().archive.missingBifCount() != 0u) {
            right << " | " << countText(session().archive.missingBifCount(),
                                         "missing BIF", "missing BIFs");
        }
        if (session().looseArchives.invalidArchiveCount() != 0u) {
            right << " | " << session().looseArchives.invalidArchiveCount() << " invalid";
        }
        if (!session().error.empty()) right << " | Scan failed (see log)";
        else if (!session().progress.empty()) right << " | " << session().progress;
        setModuleStatusText(right.str(), 1);
    }

    void updateCommandState() {
        if(rebuilding_)return;
        const bool open = session().archive.isOpen();
        const bool complete = session().progress.empty() && session().error.empty();
        const auto selections = open ? selectedResources()
                                     : std::vector<neoshared::bif::ResourceSelection>{};
        const auto entireSelections = open ? selectedResources(false)
                                           : std::vector<neoshared::bif::ResourceSelection>{};
        const std::size_t extractableSelectionCount = static_cast<std::size_t>(std::count_if(
            selections.begin(), selections.end(),
            [this](const neoshared::bif::ResourceSelection& selection) {
                return resourceIsExtractable(selection);
            }));
        const bool selectionExtractable = extractableSelectionCount != 0u;
        const bool entireSelectionExtractable = std::any_of(
            entireSelections.begin(), entireSelections.end(),
            [this](const neoshared::bif::ResourceSelection& selection) {
                return resourceIsExtractable(selection);
            });
        const bool filterActive = !filter_->GetValue().empty();
        const bool scopeNarrowed = filterActive &&
            selections.size() < entireSelections.size();
        const bool anyExtractable = open &&
            (session().archive.extractableResourceCount() != 0u ||
             session().looseArchives.extractableResourceCount() != 0u);
        if (openDirectoryButton_ != nullptr) openDirectoryButton_->Enable(!jobRunning_);
        if (openFilesButton_ != nullptr) openFilesButton_->Enable(!jobRunning_);
        if (addBifsButton_ != nullptr) addBifsButton_->Enable(open && !jobRunning_);
        if (extractButton_ != nullptr) {
            extractButton_->Enable(selectionExtractable && !jobRunning_);
            if (!open || selections.empty()) {
                extractButton_->SetLabel("Extract Selection...");
                extractButton_->SetToolTip(
                    "Extract resources represented by the current tree selection. "
                    "Search narrows branch selections.");
            } else {
                const std::size_t count = selections.size();
                const std::string noun = scopeNarrowed
                    ? (count == 1u ? " Match" : " Matches")
                    : (count == 1u ? " Resource" : " Resources");
                const std::string countText = extractableSelectionCount == count
                    ? std::to_string(count)
                    : std::to_string(extractableSelectionCount) + " of " +
                        std::to_string(count);
                extractButton_->SetLabel(wxui::toWx(
                    "Extract " + countText + noun + "..."));
                std::string tip = "Extract " + std::to_string(extractableSelectionCount) +
                    (extractableSelectionCount == 1u
                        ? " extractable resource" : " extractable resources") +
                    " from the current selection.";
                if (extractableSelectionCount != count) {
                    tip += " " + std::to_string(count - extractableSelectionCount) +
                        (count - extractableSelectionCount == 1u
                            ? " unavailable resource will be omitted after confirmation."
                            : " unavailable resources will be omitted after confirmation.");
                }
                if (scopeNarrowed) tip += " Search is narrowing this branch.";
                if (!complete) tip += " This selection includes only resources indexed so far; see the scan log.";
                extractButton_->SetToolTip(wxui::toWx(tip));
            }
        }
        {
            enableModuleCommand(ID_Rescan,!session().keyPath.empty() && !jobRunning_);
            enableModuleCommand(ID_ExtractBranch,scopeNarrowed && entireSelectionExtractable && !jobRunning_);
            enableModuleCommand(ID_ZipBranch,scopeNarrowed && entireSelectionExtractable && !jobRunning_);
            enableModuleCommand(ID_AddBifs, open && !jobRunning_);
            enableModuleCommand(ID_CloseArchive, !session().keyPath.empty() && !jobRunning_);
            enableModuleCommand(ID_ExtractSelected, selectionExtractable && !jobRunning_);
            enableModuleCommand(ID_ExtractAll, complete && anyExtractable && !jobRunning_);
            enableModuleCommand(ID_ZipSelected, selectionExtractable && !jobRunning_);
            enableModuleCommand(ID_ZipAll, complete && anyExtractable && !jobRunning_);
            enableModuleCommand(ID_ExportByType, complete && open && !jobRunning_ &&
                (!session().archive.resources().empty() || !session().looseArchives.resources().empty()));
            enableModuleCommand(ID_ExpandAll, open && !jobRunning_);
            enableModuleCommand(ID_CollapseAll, open && !jobRunning_);
        }
    }

    void applyDarkMode() {
        if (darkModeItem_ != nullptr) darkModeItem_->Check(darkMode_);
        wxui::applyTheme(this, darkMode_);
        if (tree_ != nullptr) wxui::applyTreeTheme(*tree_, darkMode_);
        applyFontScale();
    }

    void applyFontScale() {
        neoview::applyFontScale(this, fontScale_);
    }

    void changeFontScaleSteps(int steps) {
        const double next = neoview::steppedFontScale(fontScale_, steps);
        if (neoview::fontScalePercent(next) == neoview::fontScalePercent(fontScale_)) return;
        fontScale_ = next;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void onIncreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(1);
    }

    void onDecreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(-1);
    }

    void onResetFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        fontScale_ = neoview::kDefaultFontScale;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void showAbout() {
        const std::string message = std::string("NeoBIF v") + neobif::kVersion +
            "\n\nKotOR and Jade Empire game archive browser and extractor."
            "\n\nNeoBIF indexes chitin.key/BIFF resources and recursively catalogs "
            "ERF, MOD, SAV, HAK, NWM, and RIM archives under the game directory. "
            "Every archive, resource type, and individual resource can be extracted "
            "to a directory or ZIP package.";
        wxMessageBox(wxui::toWx(message), "About NeoBIF",
                     wxOK | wxICON_INFORMATION, this);
    }
};

} // namespace
namespace neobif::ui {
BrowserPanel* createBrowserPanel(wxWindow* parent, neomodules::Context context) {
    return new NeoBIFPanelImpl(parent,std::move(context));
}
}
