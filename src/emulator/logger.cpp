#include "logger.hpp"

#include <utils/async_file_writer.hpp>
#include <utils/finally.hpp>
#include <utils/win.hpp>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

namespace sogen
{

    namespace
    {
#ifdef _WIN32
#define COLOR(win, posix, web) win
        using color_type = WORD;
#elif defined(__EMSCRIPTEN__) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
#define COLOR(win, posix, web) web
        using color_type = const char*;
#else
#define COLOR(win, posix, web) posix
        using color_type = const char*;
#endif

        color_type get_reset_color()
        {
            return COLOR(7, "\033[0m", "</span>");
        }

        color_type get_color_type(const color c)
        {
            using enum color;

            switch (c)
            {
            case black:
                return COLOR(0x8, "\033[0;90m", "<span class=\"terminal-black\">");
            case red:
                return COLOR(0xC, "\033[0;91m", "<span class=\"terminal-red\">");
            case green:
                return COLOR(0xA, "\033[0;92m", "<span class=\"terminal-green\">");
            case yellow:
                return COLOR(0xE, "\033[0;93m", "<span class=\"terminal-yellow\">");
            case blue:
                return COLOR(0x9, "\033[0;94m", "<span class=\"terminal-blue\">");
            case cyan:
                return COLOR(0xB, "\033[0;96m", "<span class=\"terminal-cyan\">");
            case pink:
                return COLOR(0xD, "\033[0;95m", "<span class=\"terminal-pink\">");
            case white:
                return COLOR(0xF, "\033[0;97m", "<span class=\"terminal-white\">");
            case dark_gray:
                return COLOR(0x8, "\033[0;90m", "<span class=\"terminal-dark-gray\">");
            case gray:
            default:
                return get_reset_color();
            }
        }

#ifdef _WIN32
        HANDLE get_console_handle()
        {
            return GetStdHandle(STD_OUTPUT_HANDLE);
        }
#endif

        void set_color(const color_type color)
        {
#ifdef _WIN32
            const auto* force_color = std::getenv("FORCE_COLOR");
            if (force_color && std::string_view(force_color) != "0")
            {
                constexpr std::array ansi_colors{30, 34, 32, 36, 31, 35, 33, 37, 90, 94, 92, 96, 91, 95, 93, 97};
                printf("\033[%dm", color == get_reset_color() ? 0 : ansi_colors[color & 0xF]);
            }
            else
            {
                SetConsoleTextAttribute(get_console_handle(), color);
            }
#else
            printf("%s", color);
#endif
        }

        void reset_color()
        {
            (void)fflush(stdout);
            set_color(get_reset_color());
            (void)fflush(stdout);
        }

        int format_internal(const char* message, std::span<char> buffer, va_list* ap)
        {
            return vsnprintf(buffer.data(), buffer.size(), message, *ap);
        }

        std::string_view format(const char* message, std::string& reserve_buffer, va_list* ap1, va_list* ap2)
        {
            thread_local std::array<char, 0x1000> buffer{};

            auto count = format_internal(message, buffer, ap1);

            if (count < 0)
            {
                return {};
            }

            if (static_cast<size_t>(count) < buffer.size())
            {
                return {buffer.data(), static_cast<size_t>(count)};
            }

            reserve_buffer.resize(count + 1);
            count = format_internal(message, reserve_buffer, ap2);

            if (count < 0)
            {
                return {};
            }

            return {reserve_buffer.data(), std::min(static_cast<size_t>(count), reserve_buffer.size() - 1)};
        }

#define format_to_string(msg, str)                 \
    std::string buf{};                             \
    va_list ap1;                                   \
    va_list ap2;                                   \
    va_start(ap1, msg);                            \
    va_start(ap2, msg);                            \
    const auto str = format(msg, buf, &ap1, &ap2); \
    va_end(ap2);                                   \
    va_end(ap1)

#ifdef _WIN32
        bool use_ansi_colors()
        {
            const auto* value = std::getenv("FORCE_COLOR");
            return value && std::string_view(value) != "0";
        }

        void print_ansi_colored(const std::string_view line, const color_type base_color, utils::async_file_writer* writer,
                                std::FILE* fallback)
        {
            constexpr std::array ansi_colors{30, 34, 32, 36, 31, 35, 33, 37, 90, 94, 92, 96, 91, 95, 93, 97};
            std::array<char, 16> prefix{};
            const auto count =
                snprintf(prefix.data(), prefix.size(), "\033[%dm", base_color == get_reset_color() ? 0 : ansi_colors[base_color & 0xF]);
            thread_local std::string record;
            record.assign(prefix.data(), static_cast<size_t>(count));
            record.append(line);
            record.append("\033[0m");
            if (writer)
            {
                writer->write(record);
            }
            else
            {
                (void)fwrite(record.data(), 1, record.size(), fallback);
                (void)fflush(fallback);
            }
        }
#endif

        void print_colored(const std::string_view& line, const color_type base_color)
        {
#ifdef _WIN32
            if (use_ansi_colors())
            {
                print_ansi_colored(line, base_color, nullptr, stdout);
                return;
            }
#endif
            const auto _ = utils::finally(&reset_color);
            set_color(base_color);
            (void)fwrite(line.data(), 1, line.size(), stdout);
        }
    }

#ifdef _WIN32
    logger::logger()
    {
        old_cp = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
        if (use_ansi_colors() && GetFileType(get_console_handle()) == FILE_TYPE_DISK)
        {
            this->console_output_ = std::make_unique<utils::async_file_writer>(stdout);
        }
    }

    logger::~logger()
    {
        this->console_output_.reset();
        SetConsoleOutputCP(old_cp);
    }

    void logger::set_console_output(std::unique_ptr<utils::async_file_writer> writer)
    {
        const std::scoped_lock lock(this->print_mutex_);
        this->console_output_ = std::move(writer);
        this->console_output_failure_.clear();
    }
#endif

    void logger::print_message(const color c, const std::string_view message, const bool force) const
    {
        const std::scoped_lock lock(this->print_mutex_);

        // Sinks observe all log activity, regardless of disable_output_. That lets
        // consumers capture a full structured log even when they've silenced the
        // terminal (e.g. --silent, or a Python wrapper capturing via callback).
        this->sink_(c, message);

        if (this->silent_ || (!force && this->disable_output_))
        {
            return;
        }

#ifdef _WIN32
        if (this->console_output_)
        {
            try
            {
                print_ansi_colored(message, get_color_type(c), this->console_output_.get(), stdout);
                if (force)
                {
                    this->console_output_->flush();
                }
                return;
            }
            catch (const std::exception& e)
            {
                // The writer thread stopped on a failed write (exhausted volume, closed handle).
                // Logging must never throw: this path also runs from destructors during stack
                // unwinding, where a second exception terminates the process without any record
                // (analyzer.exe.64244.dmp: terminate -> abort -> FAST_FAIL_FATAL_APP_EXIT). Retire the
                // writer once and continue synchronously on stderr so evidence keeps flowing.
                this->console_output_failure_ = e.what();
                this->console_output_.reset();
                (void)fprintf(stderr, "\033[91m[logger] console output writer failed: %s; continuing on stderr\033[0m\n", e.what());
                (void)fflush(stderr);
            }
        }

        if (!this->console_output_failure_.empty())
        {
            print_ansi_colored(message, get_color_type(c), nullptr, stderr);
            return;
        }
#endif
        print_colored(message, get_color_type(c));
    }

    void logger::print(const color c, const std::string_view message)
    {
        this->print_message(c, message);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void logger::print(const color c, const char* message, ...)
    {
        format_to_string(message, data);
        this->print_message(c, data);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void logger::force_print(const color c, const char* message, ...)
    {
        format_to_string(message, data);
        this->print_message(c, data, true);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void logger::info(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::cyan, data);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void logger::warn(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::yellow, data);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void logger::error(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::red, data, true);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void logger::success(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::green, data);
    }

    // NOLINTNEXTLINE(cert-dcl50-cpp)
    void logger::log(const char* message, ...) const
    {
        format_to_string(message, data);
        this->print_message(color::gray, data);
    }

} // namespace sogen
