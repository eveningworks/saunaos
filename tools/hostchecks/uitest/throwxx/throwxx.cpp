// The uixx exception guard's fixture (throwxx_app_uitest.c): a button
// whose handler throws. The throw must not unwind through the C toolkit,
// whose frames have no unwind tables -- it ends the loop and run()
// rethrows it here, in C++.
#include <stdexcept>

#include "base/ulog.h"
#include "uixx/app.hpp"

namespace {

class Boom : public ui::Window {
public:
    Boom() : Window("Boom", "throwxx") {
        set_root(ui::Column{boom_.name("boom")});
        boom_.on_click([] { throw std::runtime_error("from on_click"); });
    }

private:
    ui::Button boom_{"Throw"};
};

}  // namespace

int main() {
    try {
        return ui::run<Boom>();
    } catch (const std::exception &e) {
        ulogf("throwxx: caught %s\n", e.what());
        return 7;
    }
}
