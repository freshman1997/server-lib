#include "eventbus/event_bus.h"

#include <iostream>
#include <string>

int main()
{
    yuan::eventbus::EventBus bus;

    int alpha_calls = 0;
    int beta_calls = 0;
    std::string last_payload;

    const auto alpha = bus.subscribe("alpha", [&](const yuan::eventbus::Event &event) {
        ++alpha_calls;
        if (event.payload.has_value()) {
            last_payload = std::any_cast<std::string>(event.payload);
        }
    });

    const auto beta = bus.subscribe("beta", [&](const yuan::eventbus::Event &) {
        ++beta_calls;
    });

    if (alpha == 0 || beta == 0) {
        std::cerr << "subscribe failed\n";
        return 1;
    }

    bus.publish("alpha", std::string("payload"));
    bus.publish("beta");

    if (alpha_calls != 1 || beta_calls != 1 || last_payload != "payload") {
        std::cerr << "publish dispatch failed\n";
        return 1;
    }

    if (!bus.unsubscribe(alpha)) {
        std::cerr << "unsubscribe alpha failed\n";
        return 1;
    }

    bus.publish("alpha", std::string("after-unsubscribe"));
    if (alpha_calls != 1) {
        std::cerr << "alpha handler still triggered after unsubscribe\n";
        return 1;
    }

    if (bus.unsubscribe(alpha)) {
        std::cerr << "duplicate unsubscribe should fail\n";
        return 1;
    }

    if (!bus.unsubscribe(beta)) {
        std::cerr << "unsubscribe beta failed\n";
        return 1;
    }

    bus.publish("beta");
    if (beta_calls != 1) {
        std::cerr << "beta handler still triggered after unsubscribe\n";
        return 1;
    }

    std::cout << "event bus test passed\n";
    return 0;
}
