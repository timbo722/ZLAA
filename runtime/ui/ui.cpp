/*
 * ui.cpp - the menus: the start screen, the settings window and the pause menu.
 *
 * They are RmlUi documents (res/ui: .rml files are HTML-like markup, .rcss files CSS-like style sheets) in our
 * own "Koholint at night" look. RmlUi lays them out, animates them and tells ui_render.cpp what to draw;
 * this file loads them, feeds them input and reacts to clicks. The controller moves the focus
 * between buttons (RmlUi's spatial navigation: the arrow keys) and A "clicks" the focused one
 * (RmlUi clicks the focused element on Enter); touch works as a mouse.
 *
 * Settings are a table (kSettings): each one is an environment variable the runtime already reads
 * (LAA_DOCKED, LAA_SHOW_FPS, ...) and its choices. They are saved in the config file as
 * {"NAME": "value", ...} and set as environment variables when the game starts; a variable that was
 * already set (env.txt, the shell) stays as it is, and the settings window shows it as fixed.
 */
#include "ui.h"
#include "ui_internal.h"
extern "C" {
#include "game_lib.h"
}
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <map>
#include <string>
#include <vector>

#ifndef LAA_VERSION
#define LAA_VERSION "dev"
#endif
#ifndef LAA_COMMIT
#define LAA_COMMIT "unknown"
#endif

namespace {

/* ---- settings ---- */

struct Choice { const char *label, *value; };          /* value NULL: the variable is left unset */
struct Setting {
    const char *tab, *name, *var;
    std::vector<Choice> choices;
    int fallback;                                       /* the choice when the config doesn't say */
    const char *help;
};

const char *const kTabs[] = { "Display", "Audio", "Controls", "Game", "Advanced", "About" };

#define NEXT_START " Takes effect the next time the game starts."

const std::vector<Setting> kSettings = {
    { "Display", "Mode", "LAA_DOCKED", { { "Handheld (720p)", NULL }, { "Docked (1080p)", "1" } }, 0,
      "How the game sees the console. Handheld renders at up to 720p, docked at up to 1080p: sharper, "
      "but the GPU works about twice as hard (more heat and battery)." NEXT_START },
    { "Display", "Frame rate counter", "LAA_SHOW_FPS", { { "Off", NULL }, { "On", "1" } }, 0,
      "Frames per second and the slowest frame of the last half second. Yellow when a frame missed "
      "the 60 Hz display." },
    { "Display", "Counter position", "LAA_FPS_CORNER",
      { { "Bottom left", NULL }, { "Bottom right", "1" }, { "Top left", "2" }, { "Top right", "3" } }, 0,
      "Which corner of the screen the frame rate counter sits in." },
    { "Audio", "Volume", "LAA_VOLUME",
      { { "Off", "0" }, { "25%", "0.75" }, { "50%", "1.5" }, { "75%", "2.25" }, { "100%", "3" }, { "125%", "3.75" },
        { "150%", "4.5" }, { "200%", "6" } }, 4,
      "The game's mix is quiet, so at 100% it is already amplified (about +10 dB); loud peaks are rounded off "
      "instead of clipping." },
    { "Controls", "Button layout", "LAA_SWAP_FACE", { { "As printed", NULL }, { "Swapped", "1" } }, 0,
      "As printed: each button does what its letter says, as on a Switch. Swapped: A and B trade places, and "
      "X and Y, for controllers labelled the Xbox way. The menus follow it too." },
    { "Controls", "Stick dead zone", "LAA_DEADZONE",
      { { "Small", "2000" }, { "Normal", NULL }, { "Large", "8000" } }, 1,
      "How far a stick must move before it counts. Larger if Link drifts while you don't touch the stick, "
      "smaller for finer control." },
    { "Game", "Language", "LAA_LANGUAGE",
      { { "English (US)", NULL }, { "English (UK)", "en-GB" }, { "French", "fr" }, { "French (Canada)", "fr-CA" },
        { "German", "de" }, { "Italian", "it" }, { "Spanish", "es" }, { "Spanish (Latin America)", "es-419" },
        { "Dutch", "nl" }, { "Russian", "ru" }, { "Japanese", "ja" }, { "Korean", "ko" },
        { "Chinese (Simplified)", "zh-Hans" }, { "Chinese (Traditional)", "zh-Hant" } }, 0,
      "The language the console tells the game to use. Japanese, Korean and Chinese need the Switch's own fonts "
      "in the app's fonts folder: the free font that stands in for them has only Latin and Cyrillic letters." NEXT_START },
    { "Advanced", "Fast core for the game", "LAA_NO_PIN", { { "On", NULL }, { "Off", "1" } }, 0,
      "Keeps the game's main thread on the phone's fastest CPU core and the other work off it. Smoother; turn it "
      "off only to compare." NEXT_START },
    { "Advanced", "Performance hints", "LAA_NO_ADPF", { { "On", NULL }, { "Off", "1" } }, 0,
      "Tells Android how long each frame may take (its performance hint API), so it can raise the CPU clock in "
      "time." NEXT_START },
};

std::map<std::string, std::string> config;              /* NAME -> value, as saved */
std::map<std::string, bool> fixed;                      /* set outside the menu (env.txt): not ours to change */
UiPaths paths;                                          /* (pointing into path_copies: the caller's may go away) */
std::string path_copies[6];

int choice_of(const Setting &s) {
    if (fixed[s.var]) {
        const char *v = getenv(s.var);
        for (size_t i = 0; i < s.choices.size(); i++)
            if (s.choices[i].value && v && strcmp(s.choices[i].value, v) == 0) return (int)i;
        return v ? -1 : 0;
    }
    auto it = config.find(s.var);
    if (it == config.end()) return s.fallback;
    for (size_t i = 0; i < s.choices.size(); i++)
        if ((s.choices[i].value ? s.choices[i].value : "") == it->second) return (int)i;
    return s.fallback;
}

/* The config file: one flat JSON object of strings */
void load_config() {
    FILE *f = fopen(paths.config, "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    std::vector<std::string> strings;                   /* every "..." in order: name, value, name, value */
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '"') continue;
        std::string s;
        for (i++; i < text.size() && text[i] != '"'; i++) {
            if (text[i] == '\\' && i + 1 < text.size()) i++;
            s += text[i];
        }
        strings.push_back(s);
    }
    for (size_t i = 0; i + 1 < strings.size(); i += 2) config[strings[i]] = strings[i + 1];
}

void save_config() {
    std::string tmp = std::string(paths.config) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { perror(tmp.c_str()); return; }
    fprintf(f, "{\n");
    size_t k = 0;
    for (auto &kv : config) {
        std::string v;
        for (char c : kv.second) { if (c == '"' || c == '\\') v += '\\'; v += c; }
        fprintf(f, "  \"%s\": \"%s\"%s\n", kv.first.c_str(), v.c_str(), ++k < config.size() ? "," : "");
    }
    fprintf(f, "}\n");
    if (fclose(f) == 0) rename(tmp.c_str(), paths.config);
}

/* ---- RmlUi's view of the system: time, logging, files ---- */

class System : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { return SDL_GetTicksNS() / 1e9; }
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
        if (type <= Rml::Log::LT_WARNING) fprintf(stderr, "ui: %s\n", message.c_str());
        return true;
    }
};

/* Files through SDL: on Android a relative path is read from the APK's assets */
class Files : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String &path) override {
        std::string full = path[0] == '/' ? path : std::string(paths.res) + "/" + path;
        SDL_IOStream *io = SDL_IOFromFile(full.c_str(), "rb");
        if (!io) fprintf(stderr, "ui: can't open %s: %s\n", full.c_str(), SDL_GetError());
        return (Rml::FileHandle)io;
    }
    void Close(Rml::FileHandle file) override { SDL_CloseIO((SDL_IOStream *)file); }
    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override { return SDL_ReadIO((SDL_IOStream *)file, buffer, size); }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        SDL_IOWhence w = origin == SEEK_SET ? SDL_IO_SEEK_SET : origin == SEEK_CUR ? SDL_IO_SEEK_CUR : SDL_IO_SEEK_END;
        return SDL_SeekIO((SDL_IOStream *)file, offset, w) >= 0;
    }
    size_t Tell(Rml::FileHandle file) override { return (size_t)SDL_TellIO((SDL_IOStream *)file); }
    size_t Length(Rml::FileHandle file) override { return (size_t)SDL_GetIOSize((SDL_IOStream *)file); }
};

System sys;
Files files;
SDL_Window *window;
Rml::Context *context;
Rml::ElementDocument *start_doc, *settings_doc, *pause_doc;
int showing;                                             /* 0: no menu, 1: start screen, 2: pause menu */
int result;                                              /* what ui_frame returns next */
int game_ready;                                          /* the start screen's Play works */
int settings_changed;                                    /* (ui_settings_changed) */
std::string tab = "Display";
uint64_t held_since[UI_ACTIONS], repeat_at[UI_ACTIONS];  /* (ns) direction presses repeat while held */

/* LAA_UI_SCRIPT="frame:WHAT,...": presses at menu frames (UP DOWN LEFT RIGHT A B L R MENU), or SHOT (the frame
 * as a PNG in LAA_UI_SHOTS, default "."): menu tests without hands. Frames count from the first menu frame. */
struct ScriptStep { unsigned frame; std::string what; };
std::vector<ScriptStep> script;
unsigned menu_frames, shots;

void load_script() {
    const char *s = getenv("LAA_UI_SCRIPT");
    for (const char *p = s; p && *p;) {
        char *end;
        unsigned f = (unsigned)strtoul(p, &end, 10);
        if (*end != ':') break;
        const char *w = end + 1, *comma = strchr(w, ',');
        script.push_back({ f, std::string(w, comma ? (size_t)(comma - w) : strlen(w)) });
        p = comma ? comma + 1 : NULL;
    }
}

std::string esc(const std::string &s) {                  /* text -> RML */
    std::string o;
    for (char c : s) o += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : std::string(1, c);
    return o;
}

/* ---- the start screen ---- */

void update_start_status() {
    char id[17] = "";
    std::string status, label, version;
    struct stat st;
    int lib = game_lib_find(paths.exefs, paths.game_dir, paths.game_import, id);
    game_ready = 0;
    if (lib < 0) {
        status = "bad", label = "No game files";
        version = "Put your dump's exefs and romfs folders in the app's folder";
    } else if (stat(paths.romfs, &st) != 0) {
        status = "bad", label = "The game's romfs folder is missing";
        version = std::string("Expected at ") + paths.romfs;
    } else {
        const char *v = strncmp(id, "AE16F71E002AF8CB", 16) == 0 ? "1.0.0"
                      : strncmp(id, "909E904AF78AC1B8", 16) == 0 ? "1.0.1 / 1.1.0" : "unknown version";
        version = std::string("Link's Awakening ") + v;
        if (lib == 0) status = "good", label = "Ready", game_ready = 1;
        else status = "pending", label = "Game code not built yet";
    }
    Rml::Element *e = start_doc->GetElementById("game-status");
    e->SetAttribute("status", status);
    start_doc->GetElementById("game-status-label")->SetInnerRML(esc(label));
    start_doc->GetElementById("game-version")->SetInnerRML(esc(version));
    Rml::Element *play = start_doc->GetElementById("play");
    if (game_ready) play->RemoveAttribute("disabled");
    else play->SetAttribute("disabled", "");
}

/* ---- the settings window ---- */

void focus_first(Rml::ElementDocument *doc, const char *selector) {
    if (Rml::Element *e = doc->QuerySelector(selector)) e->Focus(true);
}

void build_settings() {
    std::string tabs, rows;
    for (const char *t : kTabs)
        tabs += std::string("<tab id=\"tab-") + t + "\"" + (tab == t ? " selected" : "") + ">" + t + "</tab>";
    for (size_t i = 0; i < kSettings.size(); i++) {
        const Setting &s = kSettings[i];
        if (tab != s.tab) continue;
        int c = choice_of(s);
        std::string value = c >= 0 ? s.choices[(size_t)c].label : getenv(s.var) ? getenv(s.var) : "";
        rows += "<row id=\"row-" + std::to_string(i) + "\"" + (fixed[s.var] ? " class=\"fixed\"" : "") + ">"
                "<key>" + esc(s.name) + "</key><value>" +
                (fixed[s.var] ? "<icon class=\"lock\"/><span>" + esc(value) + "</span>"
                               : "<icon class=\"left\"/><span>" + esc(value) + "</span><icon class=\"right\"/>") +
                "</value></row>";
    }
    if (tab == "About")
        rows = "<about>"
               "<about-head><about-title>LAARecomp</about-title><about-version>Version " LAA_VERSION
               " \xC2\xB7 " LAA_COMMIT "</about-version></about-head>"
               "<p>A static recompilation of The Legend of Zelda: Link's Awakening (Nintendo Switch, 2019) for "
               "Android. The game's code was translated ahead of time into native code for this device, and the "
               "Switch's system and graphics are rebuilt around it. It is not an emulator.</p>"
               "<card><heading>Your game, your files</heading>"
               "<p>Nothing of the game comes with this app: no code, art, music or fonts. It runs only from the "
               "files of your own copy; the game's code is built from them, never shipped.</p></card>"
               "<card><heading>Made with</heading>"
               "<p>SDL 3 (zlib) \xC2\xB7 RmlUi (MIT) \xC2\xB7 FreeType (FTL) \xC2\xB7 Nunito (SIL OFL) \xC2\xB7 "
               "Material Symbols (Apache 2.0)</p></card>"
               "<card><heading>Thanks</heading>"
               "<p>Ryujinx (its shader decoder was the reference for ours), LAS Recomp and Dusklight (ideas and "
               "inspiration), and the Switchbrew wiki.</p></card>"
               "<p class=\"hint legal\">A fan project, not affiliated with or endorsed by Nintendo. The Legend of Zelda "
               "and Link's Awakening are trademarks of Nintendo.</p>"
               "</about>";
    settings_doc->GetElementById("tab-bar")->SetInnerRML(tabs);
    settings_doc->GetElementById("rows")->SetInnerRML(rows);
    settings_doc->GetElementById("help")->SetInnerRML(tab == "About" ? "" : "<p class=\"hint\">Choose a setting to see what it does.</p>");
    settings_doc->GetElementById("window")->SetClass("about", tab == "About");   /* (the About page has no help pane) */
}

void open_settings() {
    build_settings();
    settings_doc->SetClass("over-game", showing == 2);   /* (the paused game is dimmed already) */
    settings_doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::None);
    settings_doc->GetElementById("window")->SetAttribute("open", "");
    if (!settings_doc->QuerySelector("row")) settings_doc->Focus();
    else focus_first(settings_doc, "row");
}

void close_settings() {
    settings_doc->GetElementById("window")->RemoveAttribute("open");
    settings_doc->Hide();
    if (showing == 1) { start_doc->Focus(); focus_first(start_doc, "#settings"); }
    else { pause_doc->Focus(); focus_first(pause_doc, "#settings"); }
}

void switch_tab(int step) {
    size_t n = sizeof kTabs / sizeof *kTabs, k = 0;
    while (k < n && tab != kTabs[k]) k++;
    tab = kTabs[(k + n + (size_t)step) % n];
    build_settings();
    if (Rml::Element *e = settings_doc->QuerySelector("row")) e->Focus(true);
}

int row_of(Rml::Element *e) {
    for (; e; e = e->GetParentNode())
        if (e->GetId().rfind("row-", 0) == 0) return atoi(e->GetId().c_str() + 4);
    return -1;
}

int focused_row() { return row_of(context->GetFocusElement()); }

void change_setting(int row, int step) {
    const Setting &s = kSettings[(size_t)row];
    if (fixed[s.var]) return;
    int n = (int)s.choices.size(), c = (choice_of(s) + step + n) % n;
    const char *v = s.choices[(size_t)c].value;
    config[s.var] = v ? v : "";
    save_config();
    settings_changed = 1;
    build_settings();
    if (Rml::Element *e = settings_doc->GetElementById("row-" + std::to_string(row))) e->Focus(true);
}

void show_help(int row) {
    if (row < 0) return;
    const Setting &s = kSettings[(size_t)row];
    std::string h = "<heading>" + esc(s.name) + "</heading><p>" + esc(s.help) + "</p>";
    if (fixed[s.var]) h += "<p class=\"hint\">Set in env.txt (" + std::string(s.var) + "): remove the line there to choose it here.</p>";
    settings_doc->GetElementById("help")->SetInnerRML(h);
}

/* ---- clicks ---- */

class Listener : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event &event) override {
        Rml::Element *e = event.GetTargetElement();
        if (event.GetId() == Rml::EventId::Focus) { show_help(row_of(e)); return; }
        for (; e; e = e->GetParentNode()) {
            const std::string &id = e->GetId();
            if (id.empty()) continue;
            if (id == "play" && game_ready) { result = UI_PLAY; return; }
            if (id == "resume") { result = UI_RESUME; return; }
            if (id == "settings") { open_settings(); return; }
            if (id == "quit") { result = UI_QUIT; return; }
            if (id == "close") { close_settings(); return; }
            if (id.rfind("tab-", 0) == 0 && id != "tab-bar") { tab = id.substr(4); build_settings(); return; }
            if (id.rfind("row-", 0) == 0) { change_setting(atoi(id.c_str() + 4), 1); return; }
        }
    }
} listener;

Rml::ElementDocument *load(const char *name) {
    Rml::ElementDocument *d = context->LoadDocument(std::string("ui/") + name);
    if (!d) { fprintf(stderr, "ui: can't load %s\n", name); return NULL; }
    d->AddEventListener(Rml::EventId::Click, &listener);
    d->AddEventListener(Rml::EventId::Focus, &listener, true);
    return d;
}

void key(Rml::Input::KeyIdentifier k) {
    context->ProcessKeyDown(k, 0);
    context->ProcessKeyUp(k, 0);
}

/* One press of an action (or a repeat of a held direction) */
void act(int action) {
    bool in_settings = settings_doc && settings_doc->IsVisible();
    context->ProcessMouseLeave();                        /* (buttons: only the focus is highlighted, not where a touch was) */
    switch (action) {
    case UI_UP: key(Rml::Input::KI_UP); break;
    case UI_DOWN: key(Rml::Input::KI_DOWN); break;
    case UI_LEFT: case UI_RIGHT:
        if (in_settings && focused_row() >= 0) change_setting(focused_row(), action == UI_LEFT ? -1 : 1);
        else key(action == UI_LEFT ? Rml::Input::KI_LEFT : Rml::Input::KI_RIGHT);
        break;
    case UI_ACCEPT: key(Rml::Input::KI_RETURN); break;
    case UI_BACK:
        if (in_settings) close_settings();
        else if (showing == 2) result = UI_RESUME;
        break;
    case UI_TAB_PREV: case UI_TAB_NEXT: if (in_settings) switch_tab(action == UI_TAB_PREV ? -1 : 1); break;
    case UI_MENU: if (showing == 2 && !in_settings) result = UI_RESUME; break;
    }
}

/* ---- the C side ---- */

extern "C" int ui_init(void *sdl_window, const UiPaths *p) {
    window = (SDL_Window *)sdl_window;
    const char *const in[6] = { p->exefs, p->romfs, p->game_dir, p->game_import, p->config, p->res };
    const char **out[6] = { &paths.exefs, &paths.romfs, &paths.game_dir, &paths.game_import, &paths.config, &paths.res };
    for (int k = 0; k < 6; k++) {
        path_copies[k] = in[k] ? in[k] : "";
        *out[k] = in[k] ? path_copies[k].c_str() : NULL;
    }
    for (const Setting &s : kSettings) fixed[s.var] = getenv(s.var) != NULL;
    load_config();
    load_script();
    if (ui_render_init() != 0) return -1;
    Rml::SetRenderInterface(ui_render_interface());
    Rml::SetSystemInterface(&sys);
    Rml::SetFileInterface(&files);
    if (!Rml::Initialise()) return -1;
    static const char *const fonts[] = { "Nunito-Regular.ttf", "Nunito-Bold.ttf", "Nunito-ExtraBold.ttf",
                                         "MaterialSymbolsRounded.ttf" };
    for (const char *f : fonts)
        if (!Rml::LoadFontFace(std::string("ui/fonts/") + f)) fprintf(stderr, "ui: can't load the font %s\n", f);
    int w = 1280, h = 720;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    context = Rml::CreateContext("main", Rml::Vector2i(w, h));
    if (!context) return -1;
    context->SetDensityIndependentPixelRatio(h / 720.0f);   /* (the layout is 720 dp tall on any screen) */
    start_doc = load("start.rml");
    settings_doc = load("settings.rml");
    pause_doc = load("pause.rml");
    if (!start_doc || !settings_doc || !pause_doc) return -1;
    start_doc->GetElementById("version")->SetInnerRML(LAA_VERSION);
    return 0;
}

extern "C" void ui_start_menu(void) {
    showing = 1;
    result = UI_NONE;
    update_start_status();
    start_doc->Show();
    start_doc->SetClass("animate-in", true);
    focus_first(start_doc, game_ready ? "#play" : "#settings");
}

extern "C" void ui_pause(void) {
    showing = 2;
    result = UI_NONE;
    pause_doc->Show();
    pause_doc->GetElementById("window")->SetAttribute("open", "");
    focus_first(pause_doc, "#resume");
}

extern "C" int ui_open(void) { return showing != 0; }

extern "C" void ui_input(int action, int down) {
    if (!showing || action < 0 || action >= UI_ACTIONS) return;
    uint64_t now = SDL_GetTicksNS();
    if (!down) { held_since[action] = 0; return; }
    if (action <= UI_RIGHT) { held_since[action] = now; repeat_at[action] = now + 400000000ull; }
    act(action);
}

extern "C" void ui_pointer(const void *event) {
    const SDL_Event *e = (const SDL_Event *)event;
    if (!showing) return;
    float density = SDL_GetWindowPixelDensity(window);
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        context->ProcessMouseMove((int)(e->motion.x * density), (int)(e->motion.y * density), 0);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        context->ProcessMouseMove((int)(e->button.x * density), (int)(e->button.y * density), 0);
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) context->ProcessMouseButtonDown(e->button.button - 1, 0);
        else context->ProcessMouseButtonUp(e->button.button - 1, 0);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        context->ProcessMouseWheel(Rml::Vector2f(-e->wheel.x, -e->wheel.y), 0);
        break;
    }
}

const char *run_script() {                               /* this frame's steps; the PNG path if one is a shot */
    static char shot[4096];
    const char *path = NULL;
    for (const ScriptStep &st : script) {
        if (st.frame != menu_frames) continue;
        static const char *const names[UI_ACTIONS] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "L", "R", "MENU" };
        for (int a = 0; a < UI_ACTIONS; a++)
            if (st.what == names[a]) { act(a); printf("ui: script: %s at %.2f s\n", names[a], SDL_GetTicksNS() / 1e9); }
        if (st.what == "SHOT") {
            snprintf(shot, sizeof shot, "%s/ui_%03u.png", getenv("LAA_UI_SHOTS") ? getenv("LAA_UI_SHOTS") : ".", shots++);
            path = shot;
        }
    }
    menu_frames++;
    return path;
}

}

extern "C" int ui_frame(void) {
    if (!showing) return UI_NONE;
    static uint64_t last_frame;                          /* at most 60 frames a second (a menu needs no more; */
    uint64_t now = SDL_GetTicksNS();                     /*  some displays don't make us wait) */
    if (now - last_frame < 16600000ull) SDL_DelayNS(16600000ull - (now - last_frame));
    last_frame = now = SDL_GetTicksNS();
    const char *shot = run_script();
    for (int a = UI_UP; a <= UI_RIGHT; a++)              /* held directions repeat */
        if (held_since[a] && now >= repeat_at[a]) { repeat_at[a] = now + 90000000ull; act(a); }
    RenderUiTarget t;
    if (render_ui_begin(&t) != 0) return UI_NONE;
    if (context->GetDimensions() != Rml::Vector2i((int)t.width, (int)t.height)) {
        context->SetDimensions(Rml::Vector2i((int)t.width, (int)t.height));
        context->SetDensityIndependentPixelRatio(t.height / 720.0f);
    }
    context->Update();
    ui_render_begin(&t, showing != 2 || !t.last_view);
    if (showing == 2 && t.last_view) ui_render_image(t.last_view, 0.55f);   /* the paused game, darkened */
    context->Render();
    ui_render_end(shot);
    render_ui_end(&t);
    int r = result;
    if (r != UI_NONE) {                                  /* the menu closes */
        result = UI_NONE;
        if (settings_doc->IsVisible()) close_settings();
        start_doc->Hide();
        pause_doc->Hide();
        if (r == UI_PLAY) ui_apply_settings();
        showing = 0;
    }
    return r;
}

extern "C" int ui_settings_changed(void) {
    int c = settings_changed;
    settings_changed = 0;
    return c;
}

extern "C" void ui_apply_settings(void) {
    for (const Setting &s : kSettings) {
        if (fixed[s.var]) continue;
        int c = choice_of(s);
        const char *v = c >= 0 ? s.choices[(size_t)c].value : NULL;
        if (v) setenv(s.var, v, 1);
        else unsetenv(s.var);
        printf("menu: %s=%s\n", s.var, v ? v : "(unset)");
    }
}
