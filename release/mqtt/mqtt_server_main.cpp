#include "mqtt_server_app.h"

int main(int argc, char **argv)
{
    yuan::release::mqtt::ReleaseMqttServerApp app;
    return app.start(argc, argv);
}
