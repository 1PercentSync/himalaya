/**
 * @file main.cpp
 * @brief Himalaya renderer entry point with CLI conversion mode.
 */

#include <himalaya/app/application.h>
#include <himalaya/framework/ply_converter.h>

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>

/**
 * @brief Runs CLI PLY-to-glTF conversion without starting the renderer.
 * @return 0 on success, 1 on failure.
 */
int run_convert(const std::string &input, const std::string &output) {
    std::filesystem::path ply_path(input);
    if (!std::filesystem::exists(ply_path)) {
        spdlog::error("Input file does not exist: {}", input);
        return 1;
    }

    std::filesystem::path output_path;
    if (output.empty()) {
        output_path = ply_path;
        output_path.replace_extension(".gltf");
    } else {
        output_path = output;
    }

    try {
        himalaya::framework::convert_ply_to_gltf(ply_path, output_path);
        spdlog::info("Output: {}", output_path.string());
    } catch (const std::exception &e) {
        spdlog::error("Conversion failed: {}", e.what());
        return 1;
    }

    return 0;
}

/**
 * @brief Application entry point.
 *
 * No arguments: launches the GUI renderer.
 * --convert: runs CLI PLY-to-glTF conversion and exits.
 */
int main(int argc, char **argv) {
    CLI::App cli{"Himalaya Renderer"};

    std::string convert_input;
    std::string convert_output;

    auto *convert_opt = cli.add_option("--convert", convert_input,
                                        "Convert a PLY file to glTF and exit");
    cli.add_option("--output,-o", convert_output,
                   "Output .gltf path (default: same directory/name as input)")
        ->needs(convert_opt);

    CLI11_PARSE(cli, argc, argv);

    if (!convert_input.empty()) {
        return run_convert(convert_input, convert_output);
    }

    himalaya::app::Application app;
    app.init();
    app.run();
    app.destroy();
    return 0;
}
