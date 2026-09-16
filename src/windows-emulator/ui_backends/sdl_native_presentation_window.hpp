#pragma once

#include <platform/native_presentation_window.hpp>
#include <platform/ui_backend.hpp>

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sogen
{
    class sdl_native_presentation_window final : public native_presentation_window_resource
    {
      public:
        sdl_native_presentation_window(SDL_Window* owned_window, SDL_Renderer* owned_renderer) noexcept
            : window(owned_window),
              renderer(owned_renderer)
        {
            this->refresh_native_target();
        }

        void refresh_native_target() noexcept
        {
            this->require_owner();
            this->target_ = {};
#ifdef _WIN32
            if (this->window)
            {
                auto* native_window = static_cast<HWND>(
                    SDL_GetPointerProperty(SDL_GetWindowProperties(this->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
                if (native_window)
                {
                    this->target_.window = reinterpret_cast<uintptr_t>(native_window);
                    this->target_.instance = static_cast<uintptr_t>(GetWindowLongPtrW(native_window, GWLP_HINSTANCE));
                }
            }
#endif
        }

        sdl_native_presentation_window(const sdl_native_presentation_window&) = delete;
        sdl_native_presentation_window& operator=(const sdl_native_presentation_window&) = delete;

        ~sdl_native_presentation_window() override
        {
            this->require_owner();
            this->destroy_legacy_resources();
            if (this->window)
            {
                SDL_DestroyWindow(this->window);
            }
        }

        [[nodiscard]] win32_presentation_target target() const noexcept override
        {
            return this->target_;
        }

        bool suspend_legacy_presentation() noexcept override
        {
            this->require_owner();
            if (this->retired_ || this->target_.window == 0 || this->target_.instance == 0)
            {
                return false;
            }
            this->native_active_ = true;
            this->destroy_legacy_resources();
            this->refresh_native_target();
            return true;
        }

        bool resume_legacy_presentation() noexcept override
        {
            this->require_owner();
            if (this->retired_)
            {
                return false;
            }
            this->native_active_ = false;
            if (!this->renderer)
            {
                this->renderer = SDL_CreateRenderer(this->window, nullptr);
            }
            this->refresh_native_target();
            return this->renderer != nullptr;
        }

        void retire() noexcept override
        {
            this->require_owner();
            this->retired_ = true;
            if (this->window)
            {
                SDL_StopTextInput(this->window);
                SDL_HideWindow(this->window);
            }
        }

        [[nodiscard]] bool allows_legacy_presentation() const noexcept
        {
            return !this->native_active_ && !this->retired_ && this->renderer;
        }

        SDL_Window* window{};
        SDL_Renderer* renderer{};
        SDL_Texture* texture{};
        int texture_width{};
        int texture_height{};
        ui_surface_format texture_format{ui_surface_format::bgra8};
        bool has_surface{};

      private:
        void require_owner() const noexcept
        {
            if (this->owner_ != std::this_thread::get_id())
            {
                std::terminate();
            }
        }

        void destroy_legacy_resources() noexcept
        {
            if (this->texture)
            {
                SDL_DestroyTexture(this->texture);
                this->texture = nullptr;
            }
            if (this->renderer)
            {
                SDL_DestroyRenderer(this->renderer);
                this->renderer = nullptr;
            }
            this->texture_width = 0;
            this->texture_height = 0;
            this->has_surface = false;
        }

        const std::thread::id owner_{std::this_thread::get_id()};
        win32_presentation_target target_{};
        bool native_active_{};
        bool retired_{};
    };

    class shared_sdl_presentation_window final : public native_presentation_window_resource
    {
      public:
        explicit shared_sdl_presentation_window(std::shared_ptr<sdl_native_presentation_window> owner)
            : owner_(std::move(owner))
        {
        }

        win32_presentation_target target() const noexcept override
        {
            return owner_->target();
        }

        bool suspend_legacy_presentation() noexcept override
        {
            return owner_->suspend_legacy_presentation();
        }

        bool resume_legacy_presentation() noexcept override
        {
            return owner_->resume_legacy_presentation();
        }

        void retire() noexcept override
        {
            owner_->retire();
        }

      private:
        std::shared_ptr<sdl_native_presentation_window> owner_;
    };
}
