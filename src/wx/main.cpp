#include "BrowserPanel.hpp"
#include "core/Version.hpp"
#include "neobif_icon.xpm"
#include "NeoSettings.hpp"
#include <wx/app.h>
#include <wx/iconbndl.h>
namespace {
class NeoBIFFrame final : public wxFrame {
public:
    NeoBIFFrame():wxFrame(nullptr,wxID_ANY,wxui::toWx(std::string("NeoBIF v")+neobif::kVersion)) {
        wxIconBundle icons;
#if defined(__WXMSW__)
        wxIcon native("neobif",wxBITMAP_TYPE_ICO_RESOURCE);if(native.IsOk())icons.AddIcon(native);
#endif
        wxIcon fallback(neobif_icon_xpm);if(fallback.IsOk())icons.AddIcon(fallback);
        SetIcons(icons);
        neomodules::Context context;
        context.titleChanged=[this](const wxString& title){SetTitle(title);};
        context.closeRequested=[this]{Close();};
        panel_=neobif::ui::createBrowserPanel(this,std::move(context));
        SetMenuBar(panel_->takeMenus().release());
        auto* layout=new wxBoxSizer(wxVERTICAL);layout->Add(panel_,1,wxEXPAND);SetSizer(layout);
        Bind(wxEVT_MENU,[this](wxCommandEvent& event){if(!neomodules::routeCommand({panel_},event))event.Skip();});
        Bind(wxEVT_MENU_OPEN,[this](wxMenuEvent& event){neomodules::routeMenuOpen({panel_},event);event.Skip();});
        Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& event){
            if(!panel_->canClose()){if(event.CanVeto()){event.Veto();return;}}
            settings_.saveWindowPlacement(*this);event.Skip();
        });
        wxui::configureResponsiveWindow(*this,wxSize(1220,760),wxSize(820,520));
        settings_.restoreWindowPlacement(*this);
    }
    ~NeoBIFFrame() override {DestroyChildren();}
    void openStartup(const std::filesystem::path& path){try{panel_->openPath(path);}catch(const std::exception& ex){wxui::showError(this,ex);}}
private:
    neobif::ui::BrowserPanel* panel_{};
    neosettings::AppSettings settings_{"NeoBIF"};
};
class NeoBIFApp final:public wxApp {
public:bool OnInit()override {
    SetAppName("NeoBIF");SetVendorName("Neo Tools");wxInitAllImageHandlers();
    auto* frame=new NeoBIFFrame;frame->Show();
    if(argc>1){const auto path=neosettings::pathFromWx(wxString(argv[1]));frame->CallAfter([frame,path]{frame->openStartup(path);});}
    return true;
}};
}
wxIMPLEMENT_APP(NeoBIFApp);
