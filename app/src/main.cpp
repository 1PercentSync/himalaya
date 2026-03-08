/**
 * @file main.cpp
 * @brief Himalaya renderer application entry point.
 */

#include <himalaya/app/application.h>

/**
 * @brief Application entry point.
 *
 * Creates the Application, runs the main loop, and cleans up on exit.
 */
int main() {
    himalaya::app::Application app;
    app.init();
    app.run();
    app.destroy();
    return 0;
}
