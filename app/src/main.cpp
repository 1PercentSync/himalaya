/**
 * @file main.cpp
 * @brief Himalaya renderer application entry point.
 */

#include <himalaya/app/application.h>

#include <string>

#include <CLI/CLI.hpp>

/**
 * @brief Application entry point.
 *
 * Parses command-line arguments via CLI11, then creates the
 * Application, runs the main loop, and cleans up on exit.
 */
int main(int argc, char *argv[]) {
    CLI::App cli{"Himalaya Renderer"};
    argv = cli.ensure_utf8(argv);

    std::string scene_path = "assets/Sponza/Sponza.gltf";
    cli.add_option("--scene", scene_path, "Path to glTF scene file");

    std::string env_path = "assets/environment.hdr";
    cli.add_option("--env", env_path, "Path to HDR environment map");

    CLI11_PARSE(cli, argc, argv);

    himalaya::app::Application app;
    app.init({.scene_path = scene_path, .env_path = env_path});
    app.run();
    app.destroy();
    return 0;
}
