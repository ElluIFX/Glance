#pragma once
#include <cstdint>
#include <windows.h>

namespace glance::font
{
constexpr std::uint32_t maximum_file_size = 128 * 1024 * 1024;
constexpr unsigned maximum_faces = 128;
enum class Operation : std::uint32_t
{
    metadata = 1,
    render = 2
};
struct Request
{
    Operation operation{};
    unsigned face{};
    float size{16}, weight{400}, scale{1}, offset{};
    unsigned width{640}, height{480}, color{0xff202020};
    BOOL custom_text{};
    wchar_t text[4096]{};
};
struct Entry
{
    wchar_t key[32]{}, value[2048]{};
};
struct Metadata
{
    unsigned count{}, selected{}, entry_count{};
    BOOL variable{};
    float minimum{}, maximum{}, weight{};
    wchar_t faces[maximum_faces][256]{};
    wchar_t family[256]{}, style[128]{}, sample[2048]{};
    Entry entries[24]{};
};
struct Response
{
    unsigned error{}, bytes{}, width{}, height{};
    float content_height{};
    BOOL missing{};
};
} // namespace glance::font
