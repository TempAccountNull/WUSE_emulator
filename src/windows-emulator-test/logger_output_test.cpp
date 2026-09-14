#include "std_include.hpp"

#include <gtest/gtest.h>
#include <logger.hpp>
#include <utils/finally.hpp>
#include <array>
#include <cstdlib>
#include <string>

#ifdef _WIN32
namespace sogen
{
    TEST(LoggerOutput, PreservesAllColorsAndEmbeddedHighlights)
    {
        const auto* previous = std::getenv("FORCE_COLOR");
        const std::string saved = previous ? previous : "";
        ASSERT_EQ(_putenv_s("FORCE_COLOR", "1"), 0);
        const auto restore = utils::finally([&] { _putenv_s("FORCE_COLOR", saved.c_str()); });
        const std::array colors{color::black, color::red,  color::green, color::yellow,    color::blue,
                                color::cyan,  color::pink, color::white, color::dark_gray, color::gray};
        const std::array codes{90, 91, 92, 93, 94, 96, 95, 97, 90, 0};
        std::string expected;
        testing::internal::CaptureStdout();
        {
            logger output;
            for (size_t i = 0; i < 50000; ++i)
            {
                const auto index = i % colors.size();
                const auto line = std::to_string(i) + " \033[43mhighlight\033[0m\n";
                output.print(colors[index], line);
                expected += "\033[" + std::to_string(codes[index]) + "m" + line + "\033[0m";
            }
        }
        auto actual = testing::internal::GetCapturedStdout();
        std::erase(actual, '\r');
        EXPECT_EQ(actual, expected);
    }

    TEST(LoggerOutput, SilentLoggerStillDeliversSinkRecords)
    {
        std::string observed;
        testing::internal::CaptureStdout();
        {
            logger output;
            output.set_silent(true);
            output.set_sink([&](color, std::string_view text) { observed += text; });
            output.log("ordinary\n");
            output.error("forced\n");
        }
        EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
        EXPECT_EQ(observed, "ordinary\nforced\n");
    }
}
#endif
