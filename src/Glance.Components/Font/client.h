#pragma once
#include "protocol.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <winrt/base.h>

namespace glance::font
{
class Client
{
  public:
    ~Client();
    void open(const std::wstring &host, const std::wstring &path);
    std::shared_ptr<Metadata> metadata(unsigned face);
    std::pair<Response, std::vector<std::byte>> render(const Request &request);
    void cancel() noexcept;

  private:
    void read(void *data, std::size_t bytes);
    void write(const void *data, std::size_t bytes);
    Response request(const Request &request);
    std::atomic_bool cancelled_{};
    winrt::handle process_, job_, input_, output_, mapping_;
    void *pixels_{};
};
} // namespace glance::font
