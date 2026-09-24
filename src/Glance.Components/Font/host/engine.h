#pragma once
#include "../protocol.h"
#include <memory>
#include <string>
#include <vector>

namespace glance::font
{
struct Raster
{
    Response info;
    std::vector<std::byte> pixels;
};
class Engine
{
  public:
    explicit Engine(const std::wstring &path);
    ~Engine();
    Metadata metadata(unsigned index);
    Raster render(const Request &request);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace glance::font
