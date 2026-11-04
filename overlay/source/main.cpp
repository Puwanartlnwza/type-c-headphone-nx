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
    int gain = 20;
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
