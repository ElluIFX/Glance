#pragma once

#include "glance/contracts/component_api.h"
#include <chrono>
#include <semaphore>
#include <type_traits>
#include <filesystem>

namespace glance::components
{
    inline thread_local const contracts::components::PreviewCancellation* current_cancellation{};
    inline thread_local const wchar_t* current_source_path{};
    inline thread_local const wchar_t* current_effective_extension{};

    inline std::wstring preview_extension(const std::filesystem::path& path)
    {
        if (current_source_path && current_effective_extension && *current_effective_extension &&
            path == std::filesystem::path(current_source_path)) return current_effective_extension;
        return path.extension().wstring();
    }

    class FormatScope final
    {
        const wchar_t* previous_path_ = current_source_path;
        const wchar_t* previous_extension_ = current_effective_extension;
    public:
        FormatScope(const wchar_t* path, const wchar_t* extension) noexcept
        {
            current_source_path = path;
            current_effective_extension = extension;
        }
        ~FormatScope()
        {
            current_source_path = previous_path_;
            current_effective_extension = previous_extension_;
        }
    };
    inline std::counting_semaphore<2> preparation_slots{ 2 };
    template<auto CanPreview>
    BOOL WINAPI can_preview_as(const wchar_t* path,
        const contracts::components::PreviewPreparationOptions* options) noexcept
    {
        if (!path || !options || options->size < sizeof(*options) ||
            wcsnlen_s(options->effective_extension, 32) == 32) return FALSE;
        FormatScope scope(path, options->effective_extension);
        return CanPreview(path);
    }

    inline bool preview_cancelled() noexcept
    {
        return current_cancellation != nullptr && current_cancellation->is_cancelled != nullptr &&
            current_cancellation->is_cancelled(current_cancellation->context);
    }

    class PreparationScope final
    {
    public:
        explicit PreparationScope(const contracts::components::PreviewCancellation* cancellation) noexcept
            : previous_(current_cancellation)
        {
            current_cancellation = cancellation;
            while (!preview_cancelled())
            {
                if (preparation_slots.try_acquire_for(std::chrono::milliseconds(50)))
                {
                    acquired_ = true;
                    break;
                }
            }
        }
        ~PreparationScope()
        {
            if (acquired_) preparation_slots.release();
            current_cancellation = previous_;
        }
        PreparationScope(const PreparationScope&) = delete;
        PreparationScope& operator=(const PreparationScope&) = delete;
    private:
        const contracts::components::PreviewCancellation* previous_{};
        bool acquired_{};
    };

    template <auto Prepare>
    contracts::components::PrepareStatus WINAPI prepare_preview_callback(
        const wchar_t* path, const contracts::components::PreviewPreparationOptions* options,
        const contracts::components::PreviewCancellation* cancellation,
        contracts::components::PreparedPreview* preview) noexcept
    {
        using namespace contracts::components;
        PreparationScope scope(cancellation);
        if (preview_cancelled()) return PrepareStatus::cancelled;
        const PreviewPreparationOptions defaults;
        if (options == nullptr) options = &defaults;
        if (options->size < sizeof(*options)) return PrepareStatus::failed;
        FormatScope format(path, options->effective_extension);
        if constexpr (std::is_invocable_v<decltype(Prepare), const wchar_t*,
            const PreviewPreparationOptions*, PreparedPreview*>)
            return Prepare(path, options, preview);
        else
            return Prepare(path, preview);
    }

    template <auto Refine>
    contracts::components::PrepareStatus WINAPI prepare_refined_preview_callback(
        std::uint64_t token, const contracts::components::PreviewPreparationOptions* options,
        const contracts::components::PreviewCancellation* cancellation,
        contracts::components::PreparedPreview* preview) noexcept
    {
        using namespace contracts::components;
        PreparationScope scope(cancellation);
        if (preview_cancelled()) return PrepareStatus::cancelled;
        const PreviewPreparationOptions defaults;
        if (options == nullptr) options = &defaults;
        if (options->size < sizeof(*options)) return PrepareStatus::failed;
        return Refine(token, options, preview);
    }
}
