#pragma once

#include "glance/contracts/component_api.h"
#include <chrono>
#include <semaphore>
#include <type_traits>

namespace glance::components
{
    inline thread_local const contracts::components::PreviewCancellation* current_cancellation{};
    inline std::counting_semaphore<2> preparation_slots{ 2 };

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
