/**
 * @file main.cpp
 * @brief Himalaya renderer application entry point.
 */

#include <himalaya/app/application.h>

#include <string>

/** @brief Default glTF scene loaded when no command-line argument is given. */
constexpr auto kDefaultScenePath = "assets/Sponza/Sponza.gltf";

/**
 * @brief Application entry point.
 *
 * Parses an optional scene path from argv[1], then creates the
 * Application, runs the main loop, and cleans up on exit.
 */
int main(const int argc, char *argv[]) {
    const std::string scene_path = (argc > 1) ? argv[1] : kDefaultScenePath;

    himalaya::app::Application app;
    app.init(scene_path);
    app.run();
    app.destroy();
    return 0;
}
