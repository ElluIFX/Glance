#include "access_wire.h"
#include "glance/contracts/access_runtime.h"
#include <iostream>

int run_access_tests()
{
    namespace access = glance::contracts::access;
    namespace wire = glance::core::access_wire;
    int failures{};
    const auto expect = [&](bool value, const char* name) {
        if (!value) { std::cerr << "FAILED: " << name << '\n'; ++failures; }
    };
    const auto directory = access::executable_directory();
    expect(access::same_directory(directory, directory.wstring() + L"\\"), "Installation path tolerates trailing separator");
    expect(!access::same_directory(directory, directory / L"missing-directory"), "Missing directory does not match");
    glance::contracts::SelectionSnapshot original;
    original.timestamp_ms = 123;
    original.source_window = 456;
    original.source_process_id = 789;
    original.source_id = L"source";
    original.host_kind = glance::contracts::HostKind::external_source;
    original.accepts_hotkey = true;
    glance::contracts::FileDescriptor file;
    file.display_name = L"文件.txt";
    file.filesystem_path = L"C:\\Test\\文件.txt";
    file.shell_id_list = {0, 1, 255};
    file.is_filesystem = true;
    original.items.push_back(file);
    bool suppress{};
    const auto payload = wire::selection(original, true);
    const auto decoded = wire::selection(payload, suppress);
    expect(suppress && decoded.items.size() == 1 && decoded.items[0].filesystem_path == file.filesystem_path &&
        decoded.items[0].shell_id_list == file.shell_id_list && decoded.source_window == original.source_window,
        "Selection preserves Unicode paths and shell identifiers");
    original.items.resize(600, file);
    original.focused_index = 599;
    const auto many = wire::selection(wire::selection(original, false), suppress);
    expect(many.items.size() == original.items.size() && many.focused_index == original.focused_index,
        "Large selection preserves all files and focus");
    for (const auto& malformed : {payload.substr(0, payload.size() - 1), payload + "x"})
    {
        bool rejected{};
        try { static_cast<void>(wire::selection(malformed, suppress)); } catch (const std::runtime_error&) { rejected = true; }
        expect(rejected, "Malformed selection payload rejected");
    }
    wire::Writer writer;
    bool oversized{};
    try { writer.text(std::wstring(32769, L'x')); } catch (const std::length_error&) { oversized = true; }
    expect(oversized, "Access text payload is bounded");
    glance::core::GalleryCommand command;
    command.operation = glance::core::GalleryOperation::navigate;
    command.window_id = 3; command.request_id = 4; command.session_id = 5;
    command.source_window = 6; command.navigation_steps = -1;
    command.extensions = {L".jpg", L".mp4"};
    const auto decoded_command = wire::gallery_command(wire::gallery(command));
    expect(decoded_command.navigation_steps == -1 && decoded_command.extensions == command.extensions &&
        decoded_command.session_id == command.session_id, "Privileged gallery request retains routing and direction");
    std::cout << "Access regression failures: " << failures << '\n';
    return failures ? 1 : 0;
}
