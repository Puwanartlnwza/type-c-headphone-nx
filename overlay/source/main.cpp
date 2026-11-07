// Tesla overlay for the usbaudio sysmodule.
// Talks to the sysmodule only through files in sdmc:/config/usbaudio/:
//   enabled   "1"/"0"            (written here, read by the sysmodule every ~0.5 s)
//   config.ini  gain=NN          (written here, applied live)
//   status.txt  one word         (written by the sysmodule)
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <vector>
#include <memory>
#include <sys/stat.h>

#define CFG_DIR "sdmc:/config/usbaudio"

static void ensure_dir() {
    mkdir("sdmc:/config", 0777);
    mkdir(CFG_DIR, 0777);
}

static bool read_enabled() {
    FILE *f = fopen(CFG_DIR "/enabled", "r");
    if (!f) return true;
    int c = fgetc(f);
    fclose(f);
    return c != '0';
}

static void write_enabled(bool on) {
    ensure_dir();
    FILE *f = fopen(CFG_DIR "/enabled", "w");
    if (!f) return;
    fputc(on ? '1' : '0', f);
    fclose(f);
}

static int read_gain() {
    int gain = 50;
    FILE *f = fopen(CFG_DIR "/config.ini", "r");
    if (!f) return gain;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        unsigned v;
        if (sscanf(line, "gain=%u", &v) == 1 && v <= 100) gain = (int)v;
    }
    fclose(f);
    return gain;
}

static void write_gain(int gain) {
    ensure_dir();
    FILE *f = fopen(CFG_DIR "/config.ini", "w");
    if (!f) return;
    fprintf(f, "gain=%d\n", gain);
    fclose(f);
}

static std::string read_status() {
    FILE *f = fopen(CFG_DIR "/status.txt", "r");
    if (!f) return "not running";
    char buf[32] = {0};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return "unknown"; }
    fclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s.empty() ? "unknown" : s;
}

static std::vector<std::string> read_log_tail(size_t max_lines) {
    std::deque<std::string> q;
    FILE *f = fopen(CFG_DIR "/log.txt", "r");
    if (!f) f = fopen("sdmc:/usbaudio_log.txt", "r");
    if (!f) return { "log.txt not found" };
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        std::string l(buf);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.size() > 60) l.resize(60);
        q.push_back(l);
        if (q.size() > max_lines) q.pop_front();
    }
    fclose(f);
    if (q.empty()) q.push_back("(log is empty)");
    return std::vector<std::string>(q.begin(), q.end());
}

class GuiLog : public tsl::Gui {
public:
    virtual tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("USB Audio", "log (last lines)");
        auto lines = std::make_shared<std::vector<std::string>>(read_log_tail(30));
        frame->setContent(new tsl::elm::CustomDrawer([lines](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
            s32 ly = y + 18;
            for (const auto &l : *lines) {
                r->drawString(l.c_str(), false, x + 6, ly, 13, tsl::style::color::ColorText);
                ly += 16;
            }
        }));
        return frame;
    }
};

class GuiMain : public tsl::Gui {
public:
    virtual tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("USB Audio", "EarPods bridge");
        auto *list  = new tsl::elm::List();

        auto *toggle = new tsl::elm::ToggleListItem("Enabled", read_enabled());
        toggle->setStateChangedListener([](bool on) { write_enabled(on); });
        list->addItem(toggle);

        auto *status = new tsl::elm::ListItem("Status");
        status->setValue(read_status());
        list->addItem(status);

        auto *gain = new tsl::elm::TrackBar("\uE13C");
        gain->setProgress((u8)read_gain());
        gain->setValueChangedListener([](u8 value) { write_gain(value); });
        list->addItem(gain);

        auto *logItem = new tsl::elm::ListItem("View log");
        logItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) { tsl::changeTo<GuiLog>(); return true; }
            return false;
        });
        list->addItem(logItem);

        frame->setContent(list);
        return frame;
    }
};

class OverlayMain : public tsl::Overlay {
public:
    virtual void initServices() override { fsdevMountSdmc(); }
    virtual void exitServices() override { fsdevUnmountAll(); }
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<GuiMain>(); }
};

int main(int argc, char **argv) {
    return tsl::loop<OverlayMain>(argc, argv);
}
